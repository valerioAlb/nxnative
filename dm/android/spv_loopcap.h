// A cap on every loop's iterations in a translated shader, so a loop that never ends (a translation fault, or a constant the replay gives as 0 or garbage) ends
// after cap trips around instead of holding the GPU until the driver's hang recovery: on Windows that recovery (TDR, about 2 s) takes the whole device, and
// the Adreno X1-45 lost it at the same frame of Pokémon Pokopia twice (a menu's draws, 0x05d525ae.fdata's rdb UI shaders). Each structured loop (OpLoopMerge)
// whose continue target block branches back to the header gets a Function-scope counter (started at 0 with the invocation), counted in that block; its branch
// back is taken only while the counter is under the cap, else the loop leaves to its merge block as a break would. The count is per loop and per invocation
// (an inner loop counts across its outer loop's trips), so the cap is set far above what a real loop does. A loop whose back edge is shaped otherwise (a
// continue construct of several blocks, or a branch from it to a block other than the header or the merge) is left alone and counted in *skipped.
// Self-contained (no libc) for dm/tools/tests/test_spv_loopcap.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SPV_LOOPCAP_MAX 1024u   // loops capped in a module at most (the rest are left alone, counted in *skipped)

typedef struct { uint32_t fn, header, merge, cont; size_t term; size_t at; int kind; } spv_lc_loop;   // kind: 1 OpBranch to the header, 2 header on true, 3 header on false

// the module's execution model of its (first) entry point; -1 when there is none
static int spv_loopcap_model(const uint32_t *w, size_t n) {
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return -1;
        if (op == 15 && len >= 3) return (int)w[i + 1];
        if (op == 54) return -1;
        i += len; }
    return -1;
}

// The module rewritten into o (cap words; o NULL: only *need) → its length; 0 when nothing is done (no loop to cap, a malformed module, o too small).
// scratch: 4 * bound bytes. *need: the words the rewrite takes; *capped / *skipped: loops capped / left alone
static size_t spv_loopcap(const uint32_t *w, size_t n, uint32_t capv, uint32_t *scratch, spv_lc_loop *L, uint32_t *o, size_t cap, size_t *need, unsigned *capped, unsigned *skipped) {
    *need = 0; *capped = 0; *skipped = 0;
    if (n < 5 || w[0] != 0x07230203u || !capv) return 0;
    uint32_t bound = w[3], *cont_of = scratch;   // cont_of[label]: 1 + the loop whose continue target it is
    for (uint32_t k = 0; k < bound; k++) cont_of[k] = 0;
    uint32_t tu = 0, tb = 0, pfu = 0, nl = 0, fn = 0, label = 0;
    size_t first_fn = 0;
    // pass 1: the types, the loops and their headers
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return 0;
        const uint32_t *a = w + i + 1;
        if (op == 21 && len == 4 && a[1] == 32 && a[2] == 0 && !tu) tu = a[0];   // OpTypeInt 32 0
        else if (op == 20 && len == 2 && !tb) tb = a[0];   // OpTypeBool
        else if (op == 32 && len == 4 && a[1] == 7 && tu && a[2] == tu && !pfu) pfu = a[0];   // OpTypePointer Function uint
        else if (op == 54) { fn++; if (!first_fn) first_fn = i; }   // OpFunction
        else if (op == 248 && len == 2) label = a[0];   // OpLabel
        else if (op == 246 && len >= 4 && fn) {   // OpLoopMerge merge continue
            if (nl == SPV_LOOPCAP_MAX || a[1] >= bound || cont_of[a[1]]) { (*skipped)++; }
            else { L[nl] = (spv_lc_loop){fn, label, a[0], a[1], 0, 0, 0}; cont_of[a[1]] = ++nl; }
        }
        i += len;
    }
    if (!nl || !first_fn) return 0;
    // pass 2: each continue target block's terminator (and the merge instruction right before it, where the counter goes)
    label = 0; size_t prev = 0, blk_at = 0;
    for (size_t i = first_fn; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (op == 248) { label = a[0]; blk_at = 0; }
        else if (label && (op == 246 || op == 247)) blk_at = i;   // a merge instruction: the counter goes before it
        else if (label && op >= 249 && op <= 255) {   // a terminator
            uint32_t li = label < bound ? cont_of[label] : 0;
            if (li) { spv_lc_loop *l = &L[li - 1];
                if (op == 249 && len == 2 && a[0] == l->header) l->kind = 1;
                else if (op == 250 && len >= 4 && a[1] == l->header && a[2] == l->merge) l->kind = 2;
                else if (op == 250 && len >= 4 && a[2] == l->header && a[1] == l->merge) l->kind = 3;
                if (l->kind) { l->term = i; l->at = blk_at && blk_at == prev ? blk_at : i; } }
            label = 0;
        }
        prev = i; i += len;
    }
    (void)prev;
    uint32_t nid = bound, ntu = tu ? 0 : nid++, ntb = tb ? 0 : nid++, npf = pfu ? 0 : nid++, c0 = nid++, c1 = nid++, cc = nid++;
    if (!tu) tu = ntu;
    if (!tb) tb = ntb;
    if (!pfu) pfu = npf;
    unsigned nc = 0; for (uint32_t k = 0; k < nl; k++) if (L[k].kind) nc++; else (*skipped)++;
    if (!nc) return 0;
    size_t decl = (ntu ? 4 : 0) + (ntb ? 2 : 0) + (npf ? 4 : 0) + 3 * 4;
    size_t per = 5 + 4 + 5 + 3 + 5;   // the variable, load, add, store, compare
    size_t extra = decl;
    for (uint32_t k = 0; k < nl; k++) if (L[k].kind) extra += per + (L[k].kind == 1 ? 2 : L[k].kind == 2 ? 5 : 9);
    *need = n + extra;
    if (!o) return 0;
    if (cap < *need) return 0;
    // ids: per loop the variable, load, add, compare and the condition (and its not)
    uint32_t base = nid; nid += 6 * nl;
    size_t q = 0;
    for (size_t k = 0; k < 5; k++) o[q++] = w[k];
    o[3] = nid;
    fn = 0; int want_vars = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (i == first_fn) {
            if (ntu) { o[q++] = 4u << 16 | 21; o[q++] = ntu; o[q++] = 32; o[q++] = 0; }
            if (ntb) { o[q++] = 2u << 16 | 20; o[q++] = ntb; }
            if (npf) { o[q++] = 4u << 16 | 32; o[q++] = npf; o[q++] = 7; o[q++] = tu; }
            o[q++] = 4u << 16 | 43; o[q++] = tu; o[q++] = c0; o[q++] = 0;
            o[q++] = 4u << 16 | 43; o[q++] = tu; o[q++] = c1; o[q++] = 1;
            o[q++] = 4u << 16 | 43; o[q++] = tu; o[q++] = cc; o[q++] = capv;
        }
        if (op == 54) { fn++; want_vars = 1; }
        for (uint32_t k = 0; k < nl; k++) {   // the counter, before the continue block's merge instruction or terminator
            const spv_lc_loop *l = &L[k]; if (!l->kind || l->at != i) continue;
            uint32_t v = base + 6 * k, ld = v + 1, inc = v + 2, ok = v + 3;
            o[q++] = 4u << 16 | 61; o[q++] = tu; o[q++] = ld; o[q++] = v;   // OpLoad
            o[q++] = 5u << 16 | 128; o[q++] = tu; o[q++] = inc; o[q++] = ld; o[q++] = c1;   // OpIAdd
            o[q++] = 3u << 16 | 62; o[q++] = v; o[q++] = inc;   // OpStore
            o[q++] = 5u << 16 | 176; o[q++] = tb; o[q++] = ok; o[q++] = inc; o[q++] = cc;   // OpULessThan
            uint32_t cond = w[l->term + 1], c2 = v + 4, nok = v + 5;   // the branch's condition with the cap: before the merge instruction too
            if (l->kind == 2) { o[q++] = 5u << 16 | 167; o[q++] = tb; o[q++] = c2; o[q++] = cond; o[q++] = ok; }   // OpLogicalAnd
            else if (l->kind == 3) { o[q++] = 4u << 16 | 168; o[q++] = tb; o[q++] = nok; o[q++] = ok;   // OpLogicalNot
                o[q++] = 5u << 16 | 166; o[q++] = tb; o[q++] = c2; o[q++] = cond; o[q++] = nok; }   // OpLogicalOr
        }
        int done = 0;
        for (uint32_t k = 0; k < nl; k++) {   // the back edge, taken only under the cap
            const spv_lc_loop *l = &L[k]; if (!l->kind || l->term != i) continue;
            uint32_t v = base + 6 * k, ok = v + 3, c2 = v + 4;
            if (l->kind == 1) { o[q++] = 4u << 16 | 250; o[q++] = ok; o[q++] = l->header; o[q++] = l->merge; }
            else for (uint32_t j = 0; j < len; j++) o[q++] = j == 1 ? c2 : w[i + j];
            done = 1;
        }
        if (!done) for (uint32_t j = 0; j < len; j++) o[q++] = w[i + j];
        if (op == 248 && want_vars) {   // the function's first block: its loops' counters
            want_vars = 0;
            for (uint32_t k = 0; k < nl; k++) if (L[k].kind && L[k].fn == fn) { o[q++] = 5u << 16 | 59; o[q++] = pfu; o[q++] = base + 6 * k; o[q++] = 7; o[q++] = c0; }
        }
        i += len;
    }
    *capped = nc;
    return q == *need ? q : 0;
}
