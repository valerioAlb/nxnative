// Aligned blocks freed with free(), as on POSIX (dm_windows.h: free / realloc are dm_win_free / dm_win_realloc everywhere in the host).
// posix_memalign: a block from malloc with room for the alignment and a header just below the aligned pointer {raw block, a check word, size}; free / realloc /
// malloc_usable_size see the header's check word (the pointer's own value mixed with a constant) and take the raw block.
// Reading below a pointer that is NOT one of these must stay inside memory that is there. A 16-byte-aligned pointer not at a page's start has its 16 bytes below
// on the same page, so they are readable. A pointer at a page's start may be the first byte of an allocation (the segment heap, which dmhost.manifest selects,
// gives large blocks whole pages and no header in front). The page below may be unmapped: Pokémon Pokopia's 63 MB main.patched.bin buffer (loader.c slurp)
// crashed in free() on the Surface, reading 16 bytes below 0x2e2ef200000. Aligned blocks at a page's start are therefore kept in a table, and other
// pointers at a page's start are never read below.
// No dependency on the rest of the host: woa_spike.exe links it too and checks it under the same manifest.
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#undef free      // the host's build includes dm_windows.h first, whose free / realloc are these functions
#undef realloc

#define AL_MAGIC 0x6d64616c69676e21ull
typedef struct { void *raw; uint64_t check; size_t n; } al_hdr;

// The aligned blocks that start a page: an open-addressed set of pointers (0 empty, 1 removed), under a lock
static SRWLOCK g_pl = SRWLOCK_INIT;
static uintptr_t *g_pt; static size_t g_pcap, g_pn, g_pused;
static size_t slot_of(uintptr_t p, size_t cap) { uint64_t h = (uint64_t)p >> 12; h ^= h >> 29; h *= 0x9e3779b97f4a7c15ull; return (size_t)(h >> 20) & (cap - 1); }
static void pt_grow(void) {
    size_t cap = g_pcap ? g_pcap * 2 : 256; uintptr_t *t = calloc(cap, sizeof *t); if (!t) return;
    for (size_t i = 0; i < g_pcap; i++) if (g_pt[i] > 1) { size_t j = slot_of(g_pt[i], cap); while (t[j]) j = (j + 1) & (cap - 1); t[j] = g_pt[i]; }
    free(g_pt); g_pt = t; g_pcap = cap; g_pused = g_pn;
}
static int pt_add(uintptr_t p) {
    AcquireSRWLockExclusive(&g_pl);
    if ((g_pused + 1) * 2 > g_pcap) pt_grow();
    int ok = g_pcap && (g_pused + 1) * 2 <= g_pcap;
    if (ok) { size_t j = slot_of(p, g_pcap); while (g_pt[j] > 1) j = (j + 1) & (g_pcap - 1); if (!g_pt[j]) g_pused++; g_pt[j] = p; g_pn++; }
    ReleaseSRWLockExclusive(&g_pl);
    return ok;
}
static int pt_has(uintptr_t p, int remove) {
    int found = 0;
    if (remove) AcquireSRWLockExclusive(&g_pl); else AcquireSRWLockShared(&g_pl);
    if (g_pcap) for (size_t j = slot_of(p, g_pcap);; j = (j + 1) & (g_pcap - 1)) {
        if (!g_pt[j]) break;
        if (g_pt[j] == p) { found = 1; if (remove) { g_pt[j] = 1; g_pn--; } break; }
    }
    if (remove) ReleaseSRWLockExclusive(&g_pl); else ReleaseSRWLockShared(&g_pl);
    return found;
}

static al_hdr *al_of(void *p, int remove) {
    uintptr_t u = (uintptr_t)p;
    if (!p || (u & 15)) return NULL;
    if (!(u & 0xFFF) && !pt_has(u, remove)) return NULL;   // a page's first byte: only a block in the table has a header below it
    al_hdr *h = (al_hdr *)p - 1; return h->check == (AL_MAGIC ^ u) ? h : NULL;
}
int posix_memalign(void **out, size_t align, size_t n) {
    if (align < sizeof(void *) || (align & (align - 1))) return EINVAL;
    if (align < 32) align = 32;   // room for the header in front at any alignment
    uint8_t *raw = malloc(n + align + sizeof(al_hdr)); if (!raw) return ENOMEM;
    uintptr_t p = ((uintptr_t)raw + sizeof(al_hdr) + align - 1) & ~(uintptr_t)(align - 1);
    if (!(p & 0xFFF) && !pt_add(p)) { free(raw); return ENOMEM; }
    al_hdr *h = (al_hdr *)p - 1; h->raw = raw; h->check = AL_MAGIC ^ p; h->n = n;
    *out = (void *)p; return 0;
}
void *aligned_alloc_dm(size_t align, size_t n) { void *p; return posix_memalign(&p, align, n) ? NULL : p; }
void dm_win_free(void *p) { al_hdr *h = al_of(p, 1); if (h) { h->check = 0; free(h->raw); } else free(p); }
void *dm_win_realloc(void *p, size_t n) {
    al_hdr *h = al_of(p, 0); if (!h) return realloc(p, n);
    void *q = malloc(n ? n : 1); if (!q) return NULL;
    memcpy(q, p, h->n < n ? h->n : n); dm_win_free(p); return q;
}
size_t malloc_usable_size(void *p) { al_hdr *h = al_of(p, 0); return !p ? 0 : h ? h->n : _msize(p); }
