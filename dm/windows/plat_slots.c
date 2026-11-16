// Thread slots in the TEB's TlsSlots (plat_slots.h). dm.h's dm_slot_get / dm_slot_set and the loader's key page values for DM_TARGET=windows.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pthread.h>
#include "plat_slots.h"

int dm_win_slot_alloc(unsigned *index) {
    // TlsAlloc hands out the lowest free index; the process has used only a few at this point (the C runtime, winpthreads). An index past 63 is in the
    // expansion array, which the rewritten code cannot address: give it back and fail
    DWORD i = TlsAlloc();
    if (i == TLS_OUT_OF_INDEXES) return -1;
    if (i >= DM_TEB_TLS_COUNT) { TlsFree(i); return -1; }
    *index = (unsigned)i;
    return 0;
}
// The slots are read and written in the TEB directly, as the rewritten code does: TlsGetValue would also clear the thread's last error
void *dm_slot_get(pthread_key_t k) { return *dm_win_slot_ptr((unsigned)k); }
void dm_slot_set(pthread_key_t k, const void *v) { *dm_win_slot_ptr((unsigned)k) = (void *)v; }
int64_t dm_slot_keypage_value(pthread_key_t k) { return DM_TEB_TLS_SLOTS + 8 * (int64_t)k; }
void dm_slots_thread_init(void) {}    // the TEB is the thread's from its start
void dm_slots_x18_restore(void) {}    // x18 is never moved on Windows
