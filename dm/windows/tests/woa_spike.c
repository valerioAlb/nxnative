// Windows on Arm platform checks (docs/WINDOWS_ARM.md): the parts of the host that Windows does differently from Apple and Android, each tried
// alone, with no game. Run on the device (or a windows-11-arm CI runner): woa_spike.exe prints one line per check and exits non-zero when one fails.
//   slots     the rewritten code's thread slots at x18 + 0x1480 + 8 * TLS index (plat_slots.h), on several threads, with their values kept apart
//   x18       x18 still holds the TEB after a sleep, a fault handled and a thread switch (the guest relies on it at every slot access)
//   reserve   a fixed-address reservation of guest size (40 GB, as a Switch 2 game's), pages committed in it at 4 KiB granularity, protection changed per page
//   alias     the same memory at two addresses: a pagefile section mapped twice with MapViewOfFile3 into placeholders split at 64 KiB (NVN pools, vm_remap)
//   fault     an access violation in a no-access page caught by a vectored handler, the page made accessible, the access resumed (dirty-page tracking, write watches)
//   lazy      a large reservation committed page by page as it is first touched, from the fault handler (the guest heap without the commit charge up front)
//   code      code written at run time and run: read-write-execute memory and FlushInstructionCache (the loader's patched modules, nn::jit)
//   suspend   another thread suspended and its registers read (hang reports, the sampling probe)
//   pac       pointer authentication: whether the A / B / generic keys are on for user code, and whether threads share them (Switch 2 SDKs sign with them)
//   stack     a deep frame on a thread whose stack was committed whole (guest code has no stack probes: an uncommitted page below the guard is a fault)
//   heapstk   a fault taken and resumed while sp is on a stack the host allocated (a guest thread's), with the TEB's stack bounds as Windows set them and as
//             dm_win_guest_thread_init opens them (posix_windows.c): the kernel writes the exception's context on that stack, the vectored handler runs there
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include "plat_slots.h"

static int g_fail;
static void report(const char *name, int ok, const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("%-8s %s  %s\n", name, ok ? "PASS" : "FAIL", b); fflush(stdout);
    if (!ok) g_fail++;
}

// ---- slots ----
static unsigned g_slot[4];
static uint64_t slot_read_asm(uint64_t off) {   // what rewritten code does: mov xt, x18; ldr xt, [xt, off]
    uint64_t v;
    __asm__ volatile("mov x9, x18\n add x9, x9, %1\n ldr %0, [x9]" : "=r"(v) : "r"(off) : "x9", "memory");
    return v;
}
static DWORD WINAPI slot_thread(void *p) {
    uintptr_t id = (uintptr_t)p; int bad = 0;
    for (int k = 0; k < 4; k++) TlsSetValue(g_slot[k], (void *)(id * 16 + k));
    for (int round = 0; round < 1000; round++) {
        for (int k = 0; k < 4; k++) if (slot_read_asm(DM_TEB_TLS_SLOTS + 8 * g_slot[k]) != id * 16 + k) bad++;
        if (round % 100 == 0) Sleep(0);
    }
    return (DWORD)bad;
}
static void check_slots(void) {
    int ok = 1;
    for (int k = 0; k < 4; k++) if (dm_win_slot_alloc(&g_slot[k])) { ok = 0; break; }
    if (!ok) { report("slots", 0, "TlsAlloc gave no index below 64"); return; }
    HANDLE th[8]; DWORD bad = 0;
    for (int i = 0; i < 8; i++) th[i] = CreateThread(NULL, 0, slot_thread, (void *)(uintptr_t)(i + 1), 0, NULL);
    WaitForMultipleObjects(8, th, TRUE, INFINITE);
    for (int i = 0; i < 8; i++) { DWORD c = 0; GetExitCodeThread(th[i], &c); bad += c; CloseHandle(th[i]); }
    report("slots", bad == 0, "indices %u %u %u %u, 8 threads x 4000 reads through x18 + 0x1480 + 8 * index, %lu wrong", g_slot[0], g_slot[1], g_slot[2], g_slot[3], (unsigned long)bad);
}

// ---- fault handler (shared by fault, lazy, x18) ----
static uint8_t *g_fault_page; static _Atomic int g_faults;
static uint8_t *g_lazy; static size_t g_lazy_len; static _Atomic int g_lazy_commits;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *a = (uint8_t *)ep->ExceptionRecord->ExceptionInformation[1];
    if (g_fault_page && a >= g_fault_page && a < g_fault_page + 4096) {
        DWORD old; VirtualProtect(g_fault_page, 4096, PAGE_READWRITE, &old); g_faults++;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_lazy && a >= g_lazy && a < g_lazy + g_lazy_len) {
        uint8_t *pg = (uint8_t *)((uintptr_t)a & ~(uintptr_t)4095);
        if (VirtualAlloc(pg, 4096, MEM_COMMIT, PAGE_READWRITE)) { g_lazy_commits++; return EXCEPTION_CONTINUE_EXECUTION; }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void check_fault(void) {
    g_fault_page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    uint64_t teb0 = dm_win_teb();
    volatile uint8_t *p = g_fault_page; p[100] = 42; uint8_t r = p[100];
    report("fault", g_faults == 1 && r == 42, "%d fault(s) handled, value read back %d", (int)g_faults, r);
    report("x18", dm_win_teb() == teb0 && teb0 == (uint64_t)NtCurrentTeb(), "x18 %#llx, NtCurrentTeb %p", (unsigned long long)dm_win_teb(), NtCurrentTeb());
    VirtualFree(g_fault_page, 0, MEM_RELEASE); g_fault_page = NULL;
}
static void check_lazy(void) {
    g_lazy_len = (size_t)8 << 30;   // 8 GiB: a Switch 2 game's heap, reserved, not committed
    g_lazy = VirtualAlloc(NULL, g_lazy_len, MEM_RESERVE, PAGE_READWRITE);
    if (!g_lazy) { report("lazy", 0, "8 GiB reservation failed (%lu)", GetLastError()); return; }
    volatile uint64_t *q = (volatile uint64_t *)g_lazy; uint64_t sum = 0;
    for (size_t off = 0; off < g_lazy_len; off += (size_t)256 << 20) { q[off / 8] = off; sum += q[off / 8]; }
    int n = g_lazy_commits;
    report("lazy", n == 32, "8 GiB reserved, %d pages committed by the handler on first touch (32 expected)", n);
    VirtualFree(g_lazy, 0, MEM_RELEASE); g_lazy = NULL;
}

// ---- reserve ----
static void check_reserve(void) {
    size_t len = (size_t)40 << 30;
    uint8_t *base = VirtualAlloc(NULL, len, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) { report("reserve", 0, "40 GB reservation failed (%lu)", GetLastError()); return; }
    int ok = 1;
    for (int i = 0; i < 16 && ok; i++) ok = VirtualAlloc(base + 0x123000 + i * 0x1000, 0x1000, MEM_COMMIT, i & 1 ? PAGE_READONLY : PAGE_READWRITE) != NULL;
    MEMORY_BASIC_INFORMATION mi; VirtualQuery(base + 0x124000, &mi, sizeof mi);
    int ro = mi.Protect == PAGE_READONLY && mi.RegionSize == 0x1000;
    DWORD old; int pr = VirtualProtect(base + 0x123000, 0x1000, PAGE_EXECUTE_READ, &old);
    // decommit and commit again: the page comes back zero (mmap MAP_FIXED over anonymous memory)
    *(volatile int *)(base + 0x125000) = 7; VirtualFree(base + 0x125000, 0x1000, MEM_DECOMMIT); VirtualAlloc(base + 0x125000, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    int zero = *(volatile int *)(base + 0x125000) == 0;
    report("reserve", ok && ro && pr && zero, "40 GB at %p; 16 pages committed at 4 KiB steps %s, per-page protection %s, protect %s, recommit zero %s", base, ok ? "ok" : "no", ro ? "ok" : "no", pr ? "ok" : "no", zero ? "ok" : "no");
    VirtualFree(base, 0, MEM_RELEASE);
}

// ---- alias ----
typedef PVOID (WINAPI *VirtualAlloc2_t)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID (WINAPI *MapViewOfFile3_t)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
static void check_alias(void) {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    VirtualAlloc2_t va2 = (VirtualAlloc2_t)(void *)GetProcAddress(kb, "VirtualAlloc2");
    MapViewOfFile3_t mv3 = (MapViewOfFile3_t)(void *)GetProcAddress(kb, "MapViewOfFile3");
    if (!va2 || !mv3) { report("alias", 0, "VirtualAlloc2 / MapViewOfFile3 missing (Windows 10 1803 or later has them)"); return; }
    size_t len = 0x100000, view = 0x10000;   // 1 MiB of placeholder, two 64 KiB views
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)view, NULL);
    uint8_t *ph = va2(NULL, NULL, len, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (!sec || !ph) { report("alias", 0, "section %p placeholder %p (%lu)", sec, ph, GetLastError()); return; }
    // split the placeholder: [0, 64K) [64K, 512K) [512K, 576K) [576K, 1M)
    int s1 = VirtualFree(ph, view, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    int s2 = VirtualFree(ph + 0x80000, view, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    uint8_t *a = mv3(sec, NULL, ph, 0, view, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    uint8_t *b = mv3(sec, NULL, ph + 0x80000, 0, view, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    int ok = s1 && s2 && a == ph && b == ph + 0x80000;
    if (ok) { a[0x1234] = 0x5a; ok = b[0x1234] == 0x5a; }
    // a 4 KiB view is refused: views fall on the 64 KiB allocation granularity (Horizon maps at 4 KiB: the host keeps such aliases in its own page table)
    HANDLE sec2 = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x1000, NULL);
    uint8_t *ph2 = va2(NULL, NULL, 0x20000, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    int small = ph2 && VirtualFree(ph2 + 0x1000, 0x1000, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    report("alias", ok, "64 KiB section seen at %p and %p: %s; a placeholder split at 4 KiB %s", a, b, ok ? "same bytes" : "differ", small ? "is allowed" : "is refused (64 KiB granularity)");
    if (a) UnmapViewOfFile(a); if (b) UnmapViewOfFile(b);
    CloseHandle(sec); if (sec2) CloseHandle(sec2);
}

// ---- code ----
static void check_code(void) {
    uint32_t *c = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!c) { report("code", 0, "no read-write-execute memory (%lu): Arbitrary Code Guard?", GetLastError()); return; }
    c[0] = 0xD2800540;   // mov x0, #42
    c[1] = 0xD65F03C0;   // ret
    FlushInstructionCache(GetCurrentProcess(), c, 8);
    int r = ((int (*)(void))(void *)c)();
    c[0] = 0xD2800E00;   // mov x0, #112: rewritten in place, as the loader's fixups and a JIT do
    FlushInstructionCache(GetCurrentProcess(), c, 8);
    int r2 = ((int (*)(void))(void *)c)();
    report("code", r == 42 && r2 == 112, "generated code returned %d then %d after a rewrite", r, r2);
    VirtualFree(c, 0, MEM_RELEASE);
}

// ---- suspend ----
static _Atomic int g_spin_go = 1;
static DWORD WINAPI spinner(void *p) { (void)p; volatile uint64_t n = 0; while (g_spin_go) n++; return 0; }
static void check_suspend(void) {
    HANDLE t = CreateThread(NULL, 0, spinner, NULL, 0, NULL); Sleep(20);
    DWORD sc = SuspendThread(t);
    CONTEXT ctx; memset(&ctx, 0, sizeof ctx); ctx.ContextFlags = CONTEXT_FULL;
    BOOL got = GetThreadContext(t, &ctx);   // also waits for the suspension to take effect
    ResumeThread(t); g_spin_go = 0; WaitForSingleObject(t, INFINITE); CloseHandle(t);
    report("suspend", sc != (DWORD)-1 && got && ctx.Pc, "pc %#llx sp %#llx x18 %#llx", (unsigned long long)ctx.Pc, (unsigned long long)ctx.Sp, (unsigned long long)ctx.X18);
}

// ---- pac ----
static uint64_t pacia_of(uint64_t p, uint64_t mod) { __asm__ volatile(".arch armv8.3-a\n pacia %0, %1" : "+r"(p) : "r"(mod)); return p; }
static uint64_t pacib_of(uint64_t p, uint64_t mod) { __asm__ volatile(".arch armv8.3-a\n pacib %0, %1" : "+r"(p) : "r"(mod)); return p; }
static uint64_t pacga_of(uint64_t p, uint64_t mod) { uint64_t r; __asm__ volatile(".arch armv8.3-a\n pacga %0, %1, %2" : "=r"(r) : "r"(p), "r"(mod)); return r; }
static volatile uint64_t g_pac_other[3];
static DWORD WINAPI pac_thread(void *p) { (void)p; g_pac_other[0] = pacia_of(0x1000, 7); g_pac_other[1] = pacib_of(0x1000, 7); g_pac_other[2] = pacga_of(0x1000, 7); return 0; }
static void check_pac(void) {
    if (!IsProcessorFeaturePresent(PF_ARM_V83_JSCVT_INSTRUCTIONS_AVAILABLE) && !IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE)) {}
    uint64_t a = pacia_of(0x1000, 7), b = pacib_of(0x1000, 7), g = pacga_of(0x1000, 7);
    HANDLE t = CreateThread(NULL, 0, pac_thread, NULL, 0, NULL); WaitForSingleObject(t, INFINITE); CloseHandle(t);
    int aon = a != 0x1000, bon = b != 0x1000;
    // the guest needs: a pointer signed on one thread authenticates on another (fibers move between threads), so each key it uses must be the same on every thread
    int shared = a == g_pac_other[0] && b == g_pac_other[1] && g == g_pac_other[2];
    report("pac", shared, "A key %s, B key %s, generic key %s; %s across threads", aon ? "on" : "off (NOP)", bon ? "on" : "off (NOP)", g ? "computes" : "zero",
           shared ? "the same" : "DIFFERENT");
}

// ---- stack ----
static DWORD WINAPI deep(void *p) {
    (void)p; volatile uint8_t big[900 * 1024];   // a frame much larger than the guard page, touched from its far end first, as code without probes does
    big[0] = 1; big[sizeof big - 1] = 2;
    return big[0] + big[sizeof big - 1];
}
static void check_stack(void) {
    HANDLE t = CreateThread(NULL, 1 << 20, deep, NULL, 0, NULL);   // 1 MiB committed whole (no STACK_SIZE_PARAM_IS_A_RESERVATION)
    WaitForSingleObject(t, INFINITE); DWORD c = 0; GetExitCodeThread(t, &c); CloseHandle(t);
    report("stack", c == 3, "a 900 KiB frame on a 1 MiB committed stack: exit %lu", (unsigned long)c);
}

// ---- heapstk ----
static _Atomic int g_hs_ok;
static void hs_body(void) {   // on the heap stack: a handled fault, a slot read through x18, a little recursion
    volatile uint8_t *p = g_fault_page; p[8] = 9;
    int ok = p[8] == 9 && slot_read_asm(DM_TEB_TLS_SLOTS + 8 * g_slot[0]) == (uint64_t)(uintptr_t)TlsGetValue(g_slot[0]);
    atomic_store(&g_hs_ok, ok);
}
static void on_stack(void (*fn)(void), uint8_t *top) {
    __asm__ volatile("mov x19, sp\n mov sp, %1\n blr %0\n mov sp, x19"
                     :: "r"(fn), "r"(top) : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                        "x19", "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25",
                        "v26", "v27", "v28", "v29", "v30", "v31", "memory", "cc");
}
static int heapstk_try(int opened) {
    size_t len = 256 << 10; uint8_t *stk = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_fault_page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb(); void *lo = tib->StackLimit, *hi = tib->StackBase;
    if (opened) { tib->StackLimit = (void *)0x10000; tib->StackBase = (void *)0x7ffffffff000ull; }
    int f0 = g_faults; atomic_store(&g_hs_ok, 0);
    on_stack(hs_body, stk + len - 64);
    tib->StackLimit = lo; tib->StackBase = hi;
    int ok = atomic_load(&g_hs_ok) && g_faults == f0 + 1;
    VirtualFree(g_fault_page, 0, MEM_RELEASE); g_fault_page = NULL; VirtualFree(stk, 0, MEM_RELEASE);
    return ok;
}
static void check_heapstk(void) {
    TlsSetValue(g_slot[0], (void *)0x5157);
    int opened = heapstk_try(1);
    report("heapstk", opened, "a fault handled with sp on a 256 KiB VirtualAlloc stack, TEB bounds opened (the host's way): %s", opened ? "resumed" : "FAILED");
}

// aligned_windows.c under dmhost.manifest's segment heap: free() of a large malloc block (page-aligned, nothing mapped below it) must not read below it,
// and posix_memalign blocks of every alignment still free, resize and report their size (Pokopia's 63 MB module buffer crashed free() on the Surface)
int posix_memalign(void **out, size_t align, size_t n); void dm_win_free(void *p); void *dm_win_realloc(void *p, size_t n); size_t malloc_usable_size(void *p);
static void check_heap(void) {
    static const size_t big[] = {0x3f5fe10, 0x2749480, 64u << 20, 1u << 20, 600u << 10, 0x10000, 4096};
    int ok = 1, paged = 0;
    for (int r = 0; r < 3; r++) for (size_t i = 0; i < sizeof big / sizeof *big; i++) {
        uint8_t *p = malloc(big[i]); if (!p) { ok = 0; continue; }
        if (!((uintptr_t)p & 0xFFF)) paged++;
        p[0] = 1; p[big[i] - 1] = 2;
        if (malloc_usable_size(p) < big[i]) ok = 0;
        uint8_t *q = dm_win_realloc(p, big[i] + 100); if (!q || q[0] != 1) ok = 0;
        dm_win_free(q ? q : p);
    }
    static const size_t al[] = {16, 32, 64, 4096, 16384, 65536, 1u << 21};
    void *keep[64]; int nk = 0;
    for (int r = 0; r < 8; r++) for (size_t i = 0; i < sizeof al / sizeof *al; i++) {
        void *p = NULL; size_t n = r & 1 ? 0x3f5fe10 : 100 + r * 4096;
        if (posix_memalign(&p, al[i], n) || ((uintptr_t)p & (al[i] - 1))) { ok = 0; continue; }
        memset(p, 0x5a, 64);
        if (malloc_usable_size(p) != n) ok = 0;
        if (r < 4 && nk < 64) keep[nk++] = p; else { void *q = dm_win_realloc(p, n + 8); if (!q || ((uint8_t *)q)[63] != 0x5a) ok = 0; dm_win_free(q ? q : p); }
    }
    while (nk) dm_win_free(keep[--nk]);
    report("heap", ok, "large blocks (%d of them page-aligned, no header below) and aligned blocks freed, resized and sized: %s", paged, ok ? "ok" : "FAILED");
}

int main(void) {
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    printf("woa_spike: arch %u, %lu processors, page %lu, allocation granularity %lu\n", si.wProcessorArchitecture, si.dwNumberOfProcessors, si.dwPageSize, si.dwAllocationGranularity);
    if (si.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_ARM64) { printf("not an arm64 Windows: nothing checked\n"); return 2; }
    AddVectoredExceptionHandler(1, veh);
    check_slots(); check_fault(); check_lazy(); check_reserve(); check_alias(); check_code(); check_suspend(); check_pac(); check_stack(); check_heapstk(); check_heap();
    printf("woa_spike: %d check(s) failed\n", g_fail);
    return g_fail ? 1 : 0;
}
