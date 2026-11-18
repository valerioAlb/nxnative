// A vertex shader's position made safe before it leaves the shader: a component that is NaN or infinite makes the whole position (0,0,0,1), and every
// component is clamped to +-1e15. Maxwell drops or clips such vertices; a tiler's binning pass on Qualcomm's Windows driver is suspected to stall on them
// (Pokémon Pokopia's world draws 85c01ed67ec089e9 and 319aa70662279688 on the Adreno X1-45 hung the GPU when new world meshes streamed in, with or without
// robust buffer access, in passes of those draws alone). Those vertex shaders place each vertex through matrices read at an index taken from the vertex
// data; a stale or garbage index gives garbage matrices. The stores go before each OpReturn of the vertex entry point, on the variable decorated
// BuiltIn Position or the gl_PerVertex block member decorated so. A finite position below the clamp is stored unchanged (invariance holds).
// Self-contained (no libc) for dm/tools/tests/test_spv_possafe.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

// whether the module's (single) entry point is a vertex shader
static int spv_possafe_wanted(const uint32_t *w, size_t n) {
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return 0;
        if (op == 15 && len >= 3) return w[i + 1] == 0;
        if (op != 17 && op != 10 && op != 11 && op != 14) return 0;
        i += len; }
    return 0;
}

#define SPV_POSSAFE_PER 50u   // words added before each OpReturn
#define SPV_POSSAFE_DECL 64u  // words of new types and constants at most

// The module rewritten into o (cap words) → its length; 0 when nothing is done (not a single vertex entry point, no position output, no GLSL.std.450
// import, a malformed module, o too small: n + SPV_POSSAFE_DECL + SPV_POSSAFE_PER * returns words). *nret: the returns guarded
static size_t spv_possafe(const uint32_t *w, size_t n, uint32_t *o, size_t cap, unsigned *nret) {
    *nret = 0;
    if (n < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3];
    uint32_t entry = 0, nentry = 0, glsl = 0, tf = 0, v4 = 0, tb = 0, bv4 = 0, ti = 0, pov4 = 0;
    uint32_t posvar = 0, posblk = 0, posmem = 0;   // the Position variable, or the block type and its member
    size_t first_fn = 0; unsigned rets = 0; int in_entry = 0;
    // pass 1: decorations first (they precede the types), so the block is known when its variable comes
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return 0;
        const uint32_t *a = w + i + 1;
        if (op == 15 && len >= 3) { nentry++; if (a[0] != 0) return 0; entry = a[1]; }
        else if (op == 11 && len >= 3 && a[1] == 0x4c534c47u && a[2] == 0x6474732eu) glsl = a[0];   // OpExtInstImport "GLSL.std..."
        else if (op == 71 && len >= 4 && a[1] == 11 && a[2] == 0) posvar = a[0];   // OpDecorate BuiltIn Position
        else if (op == 72 && len >= 5 && a[2] == 11 && a[3] == 0) { posblk = a[0]; posmem = a[1]; }   // OpMemberDecorate BuiltIn Position
        else if (op == 22 && len == 3 && a[1] == 32 && !tf) tf = a[0];
        else if (op == 23 && len == 4 && tf && a[1] == tf && a[2] == 4 && !v4) v4 = a[0];
        else if (op == 20 && len == 2 && !tb) tb = a[0];
        else if (op == 23 && len == 4 && tb && a[1] == tb && a[2] == 4 && !bv4) bv4 = a[0];
        else if (op == 21 && len == 4 && a[1] == 32 && !ti) ti = a[0];
        else if (op == 32 && len == 4 && a[1] == 3 && v4 && a[2] == v4 && !pov4) pov4 = a[0];
        else if (op == 54 && len >= 5) { if (!first_fn) first_fn = i; in_entry = a[1] == entry; }
        else if (op == 56) in_entry = 0;
        else if (op == 253 && in_entry) rets++;
        i += len;
    }
    if (nentry != 1 || !entry || !glsl || !tf || !v4 || !first_fn || !rets || bound > 0x3fffff00u) return 0;
    // the Output variable of the block type (a pointer to it, then the variable)
    uint32_t blkptr = 0, blkvar = 0;
    if (!posvar && posblk) {
        for (size_t i = 5; i < first_fn; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
            if (op == 32 && len == 4 && a[1] == 3 && a[2] == posblk) blkptr = a[0];
            else if (op == 59 && len >= 4 && blkptr && a[0] == blkptr && a[2] == 3) blkvar = a[1];
            i += len; }
        if (!blkvar) return 0;
    } else if (posvar) {   // the variable must be a float vec4 Output pointer
        int ok = 0;
        for (size_t i = 5; i < first_fn; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
            if (op == 59 && len >= 4 && a[1] == posvar && a[2] == 3)
                for (size_t j = 5; j < first_fn; ) { uint32_t l2 = w[j] >> 16, o2 = w[j] & 0xffff;
                    if (o2 == 32 && l2 == 4 && w[j + 1] == a[0] && w[j + 2] == 3 && w[j + 3] == v4) ok = 1;
                    j += l2 ? l2 : 1; }
            i += len; }
        if (!ok) return 0;
    } else return 0;
    uint32_t nid = bound;
    uint32_t ntb = tb ? 0 : nid++, nbv4 = bv4 ? 0 : nid++, nti = ti ? 0 : nid++, npo = pov4 ? 0 : nid++;
    if (!tb) tb = ntb;
    if (!bv4) bv4 = nbv4;
    if (!ti) ti = nti;
    if (!pov4) pov4 = npo;
    uint32_t cf0 = nid++, cf1 = nid++, cl = nid++, cnl = nid++, csafe = nid++, cvl = nid++, cvnl = nid++, cmem = nid++;
    size_t decl = (ntb ? 2 : 0) + (nbv4 ? 4 : 0) + (nti ? 4 : 0) + (npo ? 4 : 0) + 4 * 4 + 7 * 3 + 4;
    size_t per = (blkvar ? 5 : 0) + 4 + 4 + 4 + 5 + 4 + 7 + 6 + 8 + 3;
    size_t need = n + decl + per * rets;
    if (need > cap) return 0;
    uint32_t base = nid; nid += 9 * rets;
    size_t q = 0;
    for (size_t k = 0; k < 5; k++) o[q++] = w[k];
    o[3] = nid;
    in_entry = 0; unsigned r = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (i == first_fn) {
            if (ntb) { o[q++] = 2u << 16 | 20; o[q++] = ntb; }
            if (nbv4) { o[q++] = 4u << 16 | 23; o[q++] = nbv4; o[q++] = tb; o[q++] = 4; }
            if (nti) { o[q++] = 4u << 16 | 21; o[q++] = nti; o[q++] = 32; o[q++] = 1; }
            if (npo) { o[q++] = 4u << 16 | 32; o[q++] = npo; o[q++] = 3; o[q++] = v4; }
            o[q++] = 4u << 16 | 43; o[q++] = tf; o[q++] = cf0; o[q++] = 0;
            o[q++] = 4u << 16 | 43; o[q++] = tf; o[q++] = cf1; o[q++] = 0x3f800000u;
            o[q++] = 4u << 16 | 43; o[q++] = tf; o[q++] = cl; o[q++] = 0x58635fa9u;    // 1e15
            o[q++] = 4u << 16 | 43; o[q++] = tf; o[q++] = cnl; o[q++] = 0xd8635fa9u;   // -1e15
            o[q++] = 7u << 16 | 44; o[q++] = v4; o[q++] = csafe; o[q++] = cf0; o[q++] = cf0; o[q++] = cf0; o[q++] = cf1;
            o[q++] = 7u << 16 | 44; o[q++] = v4; o[q++] = cvl; o[q++] = cl; o[q++] = cl; o[q++] = cl; o[q++] = cl;
            o[q++] = 7u << 16 | 44; o[q++] = v4; o[q++] = cvnl; o[q++] = cnl; o[q++] = cnl; o[q++] = cnl; o[q++] = cnl;
            o[q++] = 4u << 16 | 43; o[q++] = ti; o[q++] = cmem; o[q++] = posmem;
        }
        if (op == 54 && len >= 5) in_entry = a[1] == entry;
        else if (op == 56) in_entry = 0;
        if (op == 253 && in_entry) {
            uint32_t b = base + 9 * r++, pp = blkvar ? b : posvar, v = b + 1, isn = b + 2, isi = b + 3, bad = b + 4, any = b + 5, bb = b + 6, sel = b + 7, cl4 = b + 8;
            if (blkvar) { o[q++] = 5u << 16 | 65; o[q++] = pov4; o[q++] = pp; o[q++] = blkvar; o[q++] = cmem; }   // OpAccessChain
            o[q++] = 4u << 16 | 61; o[q++] = v4; o[q++] = v; o[q++] = pp;                  // OpLoad
            o[q++] = 4u << 16 | 156; o[q++] = bv4; o[q++] = isn; o[q++] = v;               // OpIsNan
            o[q++] = 4u << 16 | 157; o[q++] = bv4; o[q++] = isi; o[q++] = v;               // OpIsInf
            o[q++] = 5u << 16 | 166; o[q++] = bv4; o[q++] = bad; o[q++] = isn; o[q++] = isi;   // OpLogicalOr
            o[q++] = 4u << 16 | 154; o[q++] = tb; o[q++] = any; o[q++] = bad;              // OpAny
            o[q++] = 7u << 16 | 80; o[q++] = bv4; o[q++] = bb; o[q++] = any; o[q++] = any; o[q++] = any; o[q++] = any;   // OpCompositeConstruct
            o[q++] = 6u << 16 | 169; o[q++] = v4; o[q++] = sel; o[q++] = bb; o[q++] = csafe; o[q++] = v;   // OpSelect
            o[q++] = 8u << 16 | 12; o[q++] = v4; o[q++] = cl4; o[q++] = glsl; o[q++] = 43; o[q++] = sel; o[q++] = cvnl; o[q++] = cvl;   // FClamp
            o[q++] = 3u << 16 | 62; o[q++] = pp; o[q++] = cl4;                             // OpStore
        }
        for (uint32_t j = 0; j < len; j++) o[q++] = w[i + j];
        i += len;
    }
    *nret = r;
    return q == need ? q : 0;
}
