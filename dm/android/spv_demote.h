// A fragment shader's discards (OpKill) made demotes to a helper invocation followed by a return (SPV_EXT_demote_to_helper_invocation), an A/B for a driver that
// mishandles OpKill in non-uniform control flow (DM_SPV_DEMOTE=1). An invocation that discarded writes no colour, depth or memory either way; with the demote it keeps
// running as a helper only until the return, so its quad's derivatives stay defined. In the entry point that is OpReturn; in a function that returns a value, OpReturnValue
// of an OpUndef of its type (the caller goes on as a helper, its writes dropped). Self-contained (no libc) for dm/tools/tests/test_spv_demote.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SPV_DEMOTE_TYPES 16u   // distinct return types of functions that discard, at most

static const char spv_demote_ext[] = "SPV_EXT_demote_to_helper_invocation";   // 36 bytes with its NUL: 9 words

// whether the module is a fragment shader with an OpKill (the caller's pre-check)
static int spv_demote_wanted(const uint32_t *w, size_t n) {
    int frag = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return 0;
        if (op == 15 && len >= 3) frag = w[i + 1] == 4;
        if (op == 252) return frag;
        i += len; }
    return 0;
}

// The module rewritten into o (cap words; o NULL: only *need) → its length; 0 when nothing is done. *kills: the OpKills made demotes
static size_t spv_demote(const uint32_t *w, size_t n, uint32_t *o, size_t cap, size_t *need, unsigned *kills) {
    *need = 0; *kills = 0;
    if (n < 5 || w[0] != 0x07230203u || !spv_demote_wanted(w, n)) return 0;
    size_t last_cap = 0, first_fn = 0; uint32_t tvoid = 0, rt = 0, types[SPV_DEMOTE_TYPES], ntypes = 0; int has_cap = 0, has_ext = 0;
    unsigned nk = 0, nkv = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (op == 17) { last_cap = i + len; if (len == 2 && a[0] == 5379) has_cap = 1; }
        else if (op == 10) { const char *s = (const char *)(const void *)a; size_t k = 0; while (k < sizeof spv_demote_ext && k < (len - 1) * 4 && s[k] == spv_demote_ext[k]) k++; if (k == sizeof spv_demote_ext) has_ext = 1; }
        else if (op == 19 && len == 2 && !tvoid) tvoid = a[0];
        else if (op == 54 && len == 5) { if (!first_fn) first_fn = i; rt = a[0]; }
        else if (op == 252) { if (rt == tvoid) nk++; else { nkv++; uint32_t k = 0; while (k < ntypes && types[k] != rt) k++; if (k == ntypes) { if (ntypes == SPV_DEMOTE_TYPES) return 0; types[ntypes++] = rt; } } }
        i += len;
    }
    if (!last_cap || !first_fn || !tvoid || !(nk + nkv)) return 0;
    *need = n + (has_cap ? 0 : 2) + (has_ext ? 0 : 1 + 9) + 3 * ntypes + nk + 2 * nkv;
    if (!o) return 0;
    if (cap < *need) return 0;
    uint32_t bound = w[3], undef0 = bound; size_t q = 0;
    for (int k = 0; k < 5; k++) o[q++] = w[k];
    o[3] = bound + ntypes;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (i == last_cap) {
            if (!has_cap) { o[q++] = 2u << 16 | 17; o[q++] = 5379; }   // OpCapability DemoteToHelperInvocation
            if (!has_ext) { o[q++] = 10u << 16 | 10; const uint8_t *s = (const uint8_t *)spv_demote_ext; for (int k = 0; k < 9; k++) { uint32_t v = 0; for (int b = 0; b < 4; b++) { size_t at = (size_t)k * 4 + b; v |= (uint32_t)(at < sizeof spv_demote_ext ? s[at] : 0) << (8 * b); } o[q++] = v; } }
        }
        if (i == first_fn) for (uint32_t k = 0; k < ntypes; k++) { o[q++] = 3u << 16 | 1; o[q++] = types[k]; o[q++] = undef0 + k; }   // OpUndef of each return type
        if (op == 54 && len == 5) rt = a[0];
        if (op == 252) {
            o[q++] = 1u << 16 | 5380;   // OpDemoteToHelperInvocation
            if (rt == tvoid) o[q++] = 1u << 16 | 253;   // OpReturn
            else { uint32_t k = 0; while (types[k] != rt) k++; o[q++] = 2u << 16 | 254; o[q++] = undef0 + k; }   // OpReturnValue undef
            (*kills)++;
        } else for (uint32_t j = 0; j < len; j++) o[q++] = w[i + j];
        i += len;
    }
    return q == *need ? q : 0;
}
