// A uniform buffer read at a computed index gives 0 past the array the shader declares, as Maxwell's constant buffers give 0 past their bound size (LDC with a register
// offset). The translator (dm/third_party/ryujinx) declares each game constant buffer as an array of 4096 vec4 (64 KB, Maxwell's largest) and indexes it with the
// shader's own arithmetic, unclamped: Ryujinx runs with robustBufferAccess always on, where such a read gives 0 too. Without robustness the read goes wherever the
// index points. Pokémon Pokopia's world shader (vertex 85c01ed67ec089e9) computes its row in c[1] from a vertex attribute's z and w (z mod 256, w / 63, x 16 bytes):
// a garbage attribute turns into a row of up to 2^28, gigabytes past the buffer, and the Adreno X1-45 hung on the read until Windows reset it (runs 9 and 10).
// Each OpAccessChain into a Uniform variable's fixed-length array member at a non-constant index takes the index only when it is below the array's length (else 0, a
// row that exists), and each OpLoad of such a chain gives its scalar or vector type's 0 when the index was out of range. Self-contained (no libc), for
// dm/tools/tests/test_spv_cbbound.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SPV_CBB_TYPES 8u   // distinct index types and load types, at most each

// an opcode whose first two operands are its result type and result (the ones a constant buffer index or load can come from)
static int spv_cbb_typed(uint32_t op) {
    return op == 12 || (op >= 41 && op <= 46) || (op >= 50 && op <= 52) || op == 55 || op == 57 || op == 59 || op == 61 || (op >= 65 && op <= 67) || (op >= 77 && op <= 83) ||
           (op >= 109 && op <= 202) || op == 245 || (op >= 333 && op <= 366);
}

// The module rewritten into o (cap words; o NULL: only *need) → its length, 0 when nothing is done. s: 3 × bound words of scratch.
// *chains: the reads bounded, *loads: the loads given 0 past the array
static size_t spv_cbbound(const uint32_t *w, size_t n, uint32_t *s, uint32_t *o, size_t cap, size_t *need, unsigned *chains, unsigned *loads) {
    *need = 0; *chains = 0; *loads = 0;
    if (n < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3]; uint32_t *def = s, *typ = s + bound, *cm = s + 2 * (size_t)bound;   // defining word; result type; a bounded chain's slot + 1
    for (uint32_t k = 0; k < 3 * bound; k++) s[k] = 0;
    size_t first_fn = 0; uint32_t tbool = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1; if (!len || i + len > n) return 0;
        if (op >= 19 && op <= 39 && len >= 2 && a[0] < bound) def[a[0]] = (uint32_t)i;   // types
        if (op == 20 && !tbool) tbool = a[0];
        if (spv_cbb_typed(op) && len >= 3 && a[1] < bound) { def[a[1]] = (uint32_t)i; typ[a[1]] = a[0]; }
        if (op == 54 && !first_fn) first_fn = i;
        i += len;
    }
    if (!first_fn) return 0;
    uint32_t ity[SPV_CBB_TYPES], nity = 0, lty[SPV_CBB_TYPES], nlty = 0, vn[SPV_CBB_TYPES], nvn = 0; unsigned nc = 0, nl = 0; size_t extra = 0;
    // pass 1: the chains to bound, the loads of them, the types they need
    for (size_t i = first_fn; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if ((op == 65 || op == 66) && len >= 6 && a[2] < bound && a[3] < bound && a[4] < bound) {
            uint32_t vd = def[a[2]], x = a[4];
            if (vd && (w[vd] & 0xffff) == 59 && w[vd + 3] == 2) {   // a Uniform variable
                uint32_t pd = def[w[vd + 1]];
                uint32_t sd = pd && (w[pd] & 0xffff) == 32 && w[pd + 3] < bound ? def[w[pd + 3]] : 0;
                uint32_t md = def[a[3]];
                if (sd && (w[sd] & 0xffff) == 30 && md && (w[md] & 0xffff) == 43 && (w[md] >> 16) == 4 && w[md + 3] + 2 < (w[sd] >> 16)) {
                    uint32_t mt = w[sd + 2 + w[md + 3]], ad = mt < bound ? def[mt] : 0;
                    uint32_t ld = ad && (w[ad] & 0xffff) == 28 ? def[w[ad + 3]] : 0;
                    uint32_t xd = def[x], xt = typ[x], xtd = xt < bound ? def[xt] : 0;
                    int xconst = xd && ((w[xd] & 0xffff) == 43 || (w[xd] & 0xffff) == 46);
                    if (ld && (w[ld] & 0xffff) == 43 && (w[ld] >> 16) == 4 && !xconst && xtd && (w[xtd] & 0xffff) == 21 && w[xtd + 2] == 32) {
                        uint32_t k = 0; while (k < nity && ity[k] != xt) k++;
                        if (k == nity) { if (nity == SPV_CBB_TYPES) return 0; ity[nity++] = xt; }
                        cm[a[1]] = ++nc; extra += 11;
                    }
                }
            }
        }
        if (op == 61 && len >= 4 && a[2] < bound && cm[a[2]] && a[0] < bound) {
            uint32_t td = def[a[0]], top = td ? w[td] & 0xffff : 0, N = 0;
            if (top == 21 || top == 22) N = 1;
            else if (top == 23) { uint32_t cd = def[w[td + 2]]; if (cd && ((w[cd] & 0xffff) == 21 || (w[cd] & 0xffff) == 22)) N = w[td + 3]; }
            if (N) {
                uint32_t k = 0; while (k < nlty && lty[k] != a[0]) k++;
                if (k == nlty) { if (nlty == SPV_CBB_TYPES) return 0; lty[nlty++] = a[0]; }
                if (N > 1) { k = 0; while (k < nvn && vn[k] != N) k++; if (k == nvn) { if (nvn == SPV_CBB_TYPES) return 0; vn[nvn++] = N; } extra += 3 + N; }
                extra += 6; nl++;
            }
        }
        i += len;
    }
    if (!nc) return 0;
    // the bool vectors that exist
    uint32_t bv[SPV_CBB_TYPES]; for (uint32_t k = 0; k < nvn; k++) bv[k] = 0;
    if (tbool) for (size_t i = 5; i < first_fn; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 23 && w[i + 2] == tbool) for (uint32_t k = 0; k < nvn; k++) if (vn[k] == w[i + 3] && !bv[k]) bv[k] = w[i + 1];
        i += len; }
    uint32_t nid = bound; size_t decl = 0;
    uint32_t b = tbool; if (!b) { b = nid++; decl += 2; }
    for (uint32_t k = 0; k < nvn; k++) if (!bv[k]) { bv[k] = nid++; decl += 4; }
    uint32_t inull = nid; nid += nity; uint32_t lnull = nid; nid += nlty; decl += 3 * (size_t)(nity + nlty);
    uint32_t idbase = nid; nid += 2 * nc + 2 * nl;   // per chain: in-bounds, index; per load: the raw value, the condition vector
    *need = n + decl + extra; *chains = nc; *loads = nl;
    if (!o) return 0;
    if (cap < *need) return 0;
    size_t q = 0; for (int k = 0; k < 5; k++) o[q++] = w[k]; o[3] = nid;
    uint32_t lslot = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (i == first_fn) {
            if (!tbool) { o[q++] = 2u << 16 | 20; o[q++] = b; }
            for (uint32_t k = 0; k < nvn; k++) if (bv[k] >= bound) { o[q++] = 4u << 16 | 23; o[q++] = bv[k]; o[q++] = b; o[q++] = vn[k]; }
            for (uint32_t k = 0; k < nity; k++) { o[q++] = 3u << 16 | 46; o[q++] = ity[k]; o[q++] = inull + k; }
            for (uint32_t k = 0; k < nlty; k++) { o[q++] = 3u << 16 | 46; o[q++] = lty[k]; o[q++] = lnull + k; }
        }
        if (i >= first_fn && (op == 65 || op == 66) && len >= 6 && a[1] < bound && cm[a[1]]) {
            uint32_t c = cm[a[1]] - 1, inb = idbase + 2 * c, sx = inb + 1, x = a[4], xt = typ[x], k = 0; while (ity[k] != xt) k++;
            uint32_t ad = def[w[def[w[def[a[2]] + 1]] + 3]];   // the struct
            uint32_t mt = w[ad + 2 + w[def[a[3]] + 3]], lenid = w[def[mt] + 3];
            o[q++] = 5u << 16 | 176; o[q++] = b; o[q++] = inb; o[q++] = x; o[q++] = lenid;   // OpULessThan
            o[q++] = 6u << 16 | 169; o[q++] = xt; o[q++] = sx; o[q++] = inb; o[q++] = x; o[q++] = inull + k;   // OpSelect
            for (uint32_t j = 0; j < len; j++) o[q++] = j == 5 ? sx : w[i + j];
            i += len; continue;
        }
        if (i >= first_fn && op == 61 && len >= 4 && a[2] < bound && cm[a[2]]) {
            uint32_t td = def[a[0]], top = td ? w[td] & 0xffff : 0, N = 0;
            if (top == 21 || top == 22) N = 1;
            else if (top == 23) { uint32_t cd = def[w[td + 2]]; if (cd && ((w[cd] & 0xffff) == 21 || (w[cd] & 0xffff) == 22)) N = w[td + 3]; }
            if (N) {
                uint32_t inb = idbase + 2 * (cm[a[2]] - 1), raw = idbase + 2 * nc + 2 * lslot, cv = raw + 1, k = 0; lslot++;
                while (lty[k] != a[0]) k++;
                o[q++] = w[i]; o[q++] = a[0]; o[q++] = raw; for (uint32_t j = 3; j < len; j++) o[q++] = w[i + j];
                uint32_t cond = inb;
                if (N > 1) { uint32_t m = 0; while (vn[m] != N) m++; o[q++] = (3u + N) << 16 | 80; o[q++] = bv[m]; o[q++] = cv; for (uint32_t j = 0; j < N; j++) o[q++] = inb; cond = cv; }
                o[q++] = 6u << 16 | 169; o[q++] = a[0]; o[q++] = a[1]; o[q++] = cond; o[q++] = raw; o[q++] = lnull + k;
                i += len; continue;
            }
        }
        for (uint32_t j = 0; j < len; j++) o[q++] = w[i + j];
        i += len;
    }
    return q == *need ? q : 0;
}
