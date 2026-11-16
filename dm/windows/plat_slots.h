// Thread slots on Windows on Arm. The rewritten guest code finds its four slots (the guest's x18, tpidr_el0, tpidrro_el0 and the host gate; dm.h) at
// x18 + its key page value (patch_nso.py DM_TARGET=windows emits mov xt, x18 where Apple's target reads tpidrro_el0). On Windows arm64 x18 always holds the
// thread's TEB, which the kernel keeps there, and the TEB's TlsSlots array (64 pointers at TEB + 0x1480, the 64-bit TEB layout shared with x64) holds the value
// of each TlsAlloc index below 64. So a slot is a TLS index, its key page value 0x1480 + 8 * index, as Apple's is 8 * key from the pthread TSD.
// Nothing is done to x18: the guest never changes it (every guest use of x18 is rewritten), and the host is built with x18 reserved.
#pragma once
#include <stdint.h>
#define DM_TEB_TLS_SLOTS 0x1480   // offsetof(TEB, TlsSlots) on 64-bit Windows
#define DM_TEB_TLS_COUNT 64       // TLS_MINIMUM_AVAILABLE: indices past this live in TlsExpansionSlots, which rewritten code cannot reach in one load
int dm_win_slot_alloc(unsigned *index);                      // a TLS index below 64 (0 = done; -1: none left below 64)
static inline uint64_t dm_win_teb(void) { uint64_t t; __asm__ volatile("mov %0, x18" : "=r"(t)); return t; }
static inline void **dm_win_slot_ptr(unsigned index) { return (void **)(dm_win_teb() + DM_TEB_TLS_SLOTS + 8 * (uint64_t)index); }
