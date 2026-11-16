// The Darwin parts of the host on Windows on Arm (dm_windows.h): Mach threads and memory queries, the hle.h tables' sections, dispatch, CommonCrypto's SHA-256,
// CoreFoundation's pointer dictionary, sysctl's process start time, thread names and priorities, and the save-data file operations (clonefile, RENAME_SWAP).
// The pieces that are plain C are the Android port's (dm/android/plat_android.c), copied as they are.
#include <windows.h>
#include <psapi.h>
#include "dm.h"
#include <stdatomic.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <dispatch/dispatch.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <dirent.h>

// ---- Sections (hle.h tables): COFF grouped sections. Every "name$m" entry lands between this file's "name$a" and "name$z" markers, in one section "name".
// The linker may pad between the pieces (with zeros): the leading and trailing zero words are skipped, and every table's entry starts with a non-null pointer ----
const struct mach_header_64 _mh_execute_header = {0};
#define SECT(n) __attribute__((used, section(#n "$a"))) static const void *const sect_a_##n = 0; __attribute__((used, section(#n "$z"))) static const void *const sect_z_##n = 0;
SECT(swhle) SECT(swforce) SECT(swnvn)
static const void *sect_range(const void *const *a, const void *const *z, unsigned long *size) {
    const void *const *b = a + 1, *const *e = z;
    while (b < e && !*b) b++;
    while (e > b && !e[-1]) e--;
    *size = (unsigned long)((const uint8_t *)e - (const uint8_t *)b);
    return *size ? b : NULL;
}
const void *dm_android_section(const char *name, unsigned long *size) {
    *size = 0;
    if (!strcmp(name, "swhle")) return sect_range(&sect_a_swhle, &sect_z_swhle, size);
    if (!strcmp(name, "swforce")) return sect_range(&sect_a_swforce, &sect_z_swforce, size);
    if (!strcmp(name, "swnvn")) return sect_range(&sect_a_swnvn, &sect_z_swnvn, size);
    return NULL;   // the embedded module images (gen_embed.py) are Apple's: on Windows the modules are files beside the program
}

// ---- Quality of service: thread priorities ----
static __thread qos_class_t t_qos;
int dm_android_qos_set(qos_class_t q) {
    t_qos = q;
    int p = q >= QOS_CLASS_USER_INTERACTIVE ? THREAD_PRIORITY_HIGHEST : q >= QOS_CLASS_USER_INITIATED ? THREAD_PRIORITY_ABOVE_NORMAL : q >= QOS_CLASS_DEFAULT || !q ? THREAD_PRIORITY_NORMAL
          : q >= QOS_CLASS_UTILITY ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
    SetThreadPriority(GetCurrentThread(), p);
    // background work off the performance cores' boost: EcoQoS for the lowest class only
    THREAD_POWER_THROTTLING_STATE s = {THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, q && q <= QOS_CLASS_BACKGROUND ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0};
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &s, sizeof s);
    return 0;
}
qos_class_t dm_android_qos_get(void) { return t_qos ? t_qos : QOS_CLASS_DEFAULT; }
// Time-constraint threads (the audio and frame threads ask): none on Windows; the thread keeps its priority (the WASAPI thread takes MMCSS "Pro Audio" itself)
kern_return_t dm_android_rt_declare(mach_port_t t, uint64_t period_ns, uint64_t comp_ns) { (void)t; (void)period_ns; (void)comp_ns; return KERN_FAILURE; }
int dm_android_rt_declared(mach_port_t t, uint64_t *period_ns, uint64_t *comp_ns) { (void)t; (void)period_ns; (void)comp_ns; return 0; }
// The Android performance hint and thread placement hooks: nothing on Windows
void dm_android_hint_thread(int32_t tid) { (void)tid; }
void dm_android_place_thread(int done) { (void)done; }
void dm_android_hint_frame(uint64_t work_ns, const int32_t *tids, int n, uint64_t hold_ns, int thermal) { (void)work_ns; (void)tids; (void)n; (void)hold_ns; (void)thermal; }
int dm_android_suspend_signal(void) { return SIGUSR1; }   // thread.c leaves it unblocked; thread_suspend below sends no signal

// ---- Thread names ----
#undef pthread_setname_np
int dm_win_setname(pthread_t t, const char *n) {
    HANDLE h = pthread_gethandle(t);
    if (h) { wchar_t w[64]; if (MultiByteToWideChar(CP_UTF8, 0, n, -1, w, 64) > 0) { w[63] = 0; SetThreadDescription(h, w); } }
    return pthread_setname_np(t, n);
}

// ---- The calling thread's stack: the committed part below its base, without the guard pages (posix_windows.c keeps the bounds it had before a guest
// thread's were opened: dm_win_stack_lo / _hi) ----
extern __thread uintptr_t dm_win_stack_lo, dm_win_stack_hi;
size_t dm_android_stack_info(void **top) {
    ULONG_PTR lo, hi;
    if (dm_win_stack_hi) { lo = dm_win_stack_lo; hi = dm_win_stack_hi; } else GetCurrentThreadStackLimits(&lo, &hi);
    uintptr_t a = lo;
    while (a < hi) {   // the reserved and guard pages at the bottom are not readable
        MEMORY_BASIC_INFORMATION mi; if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) break;
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    if (a >= hi) a = lo;
    *top = (void *)hi; return (size_t)(hi - a);
}

// ---- Mach memory ----
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out) {
    (void)task; SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (const void *)addr, (void *)data, size, &got) || got != size) { if (out) *out = 0; return KERN_FAILURE; }
    if (out) *out = size; return KERN_SUCCESS;
}
kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags) {
    (void)task;
    void *p = mmap((flags & VM_FLAGS_ANYWHERE) ? NULL : (void *)*addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | ((flags & VM_FLAGS_ANYWHERE) ? 0 : MAP_FIXED), -1, 0);
    if (p == MAP_FAILED) return KERN_FAILURE;
    *addr = (vm_address_t)p; return KERN_SUCCESS;
}
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size) { (void)task; return munmap((void *)addr, size) ? KERN_FAILURE : KERN_SUCCESS; }
// vm_remap: Windows aliases only section (file mapping) memory, and the host's is private: it fails, as on Android when mremap refuses, and the callers keep
// their own copy (loader.c dies only for the Apple embedded images, which Windows does not use; hle_nvn.c's virtual pools keep the mapping in their page table)
kern_return_t vm_remap(mach_port_t t, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t st, vm_address_t from, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh) {
    (void)t; (void)to; (void)size; (void)mask; (void)flags; (void)st; (void)from; (void)copy; (void)cur; (void)max; (void)inh; return KERN_FAILURE;
}
static vm_prot_t vprot(DWORD p) {
    p &= 0xff;
    switch (p) {
    case PAGE_READONLY: case PAGE_WRITECOPY: return VM_PROT_READ;
    case PAGE_READWRITE: return VM_PROT_READ | VM_PROT_WRITE;
    case PAGE_EXECUTE: return VM_PROT_EXECUTE;
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_WRITECOPY: return VM_PROT_READ | VM_PROT_EXECUTE;
    case PAGE_EXECUTE_READWRITE: return VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    default: return VM_PROT_NONE;
    }
}
// vm_region_64: the region holding *a, or the next one above it; adjacent pieces with the same state and protection are one region, as Mach reports a mapping.
// Reserved-only pages count as a region with no access (Mach reports a reservation so too)
kern_return_t vm_region_64(mach_port_t t, vm_address_t *a, vm_size_t *s, int f, vm_region_info_t info, mach_msg_type_number_t *n, mach_port_t *o) {
    (void)t; (void)f; (void)n; if (o) *o = 0;
    uintptr_t p = *a; MEMORY_BASIC_INFORMATION mi;
    for (;;) {
        if (!VirtualQuery((void *)p, &mi, sizeof mi)) return KERN_FAILURE;
        if (mi.State != MEM_FREE) break;
        p = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (p >= 0x800000000000ull || !p) return KERN_FAILURE;
    }
    uintptr_t lo = (uintptr_t)mi.BaseAddress, hi = lo + mi.RegionSize; DWORD st = mi.State, pr = mi.State == MEM_COMMIT ? mi.Protect : PAGE_NOACCESS;
    MEMORY_BASIC_INFORMATION nx;
    while (VirtualQuery((void *)hi, &nx, sizeof nx) && nx.State == st && (nx.State == MEM_COMMIT ? nx.Protect : PAGE_NOACCESS) == pr && nx.AllocationBase == mi.AllocationBase) hi += nx.RegionSize;
    vm_region_basic_info_data_64_t *bi = (vm_region_basic_info_data_64_t *)info;
    if (bi) { bi->protection = vprot(pr); bi->max_protection = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE; }
    *a = lo; *s = hi - lo;
    return KERN_SUCCESS;
}
kern_return_t task_info(mach_port_t t, int flavor, task_info_t out, mach_msg_type_number_t *n) {
    (void)t; (void)n;
    if (flavor != TASK_VM_INFO) return KERN_FAILURE;
    PROCESS_MEMORY_COUNTERS_EX pm = {.cb = sizeof pm};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm)) return KERN_FAILURE;
    task_vm_info_data_t *vi = (task_vm_info_data_t *)(void *)out;
    vi->virtual_size = pm.PrivateUsage; vi->resident_size = pm.WorkingSetSize; vi->resident_size_peak = pm.PeakWorkingSetSize; vi->phys_footprint = pm.PrivateUsage;
    return KERN_SUCCESS;
}
kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t out, mach_msg_type_number_t *n) {
    if (flavor != THREAD_BASIC_INFO || !out) return KERN_INVALID_ARGUMENT;
    thread_basic_info_data_t *bi = (thread_basic_info_data_t *)(void *)out; memset(bi, 0, sizeof *bi);
    HANDLE h = t == GetCurrentThreadId() ? GetCurrentThread() : OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, t);
    if (!h) return KERN_FAILURE;
    FILETIME c, e, k, u; BOOL ok = GetThreadTimes(h, &c, &e, &k, &u);
    if (h != GetCurrentThread()) CloseHandle(h);
    if (!ok) return KERN_FAILURE;
    uint64_t uu = (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime) / 10, kk = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) / 10;
    bi->user_time.seconds = (int)(uu / 1000000); bi->user_time.microseconds = (int)(uu % 1000000);
    bi->system_time.seconds = (int)(kk / 1000000); bi->system_time.microseconds = (int)(kk % 1000000);
    bi->run_state = TH_STATE_RUNNING;   // Windows gives no state without the native API
    if (n) *n = THREAD_BASIC_INFO_COUNT;
    return KERN_SUCCESS;
}

// ---- Thread suspension (the host's hang reports and sampler): SuspendThread, then GetThreadContext, whose registers are the Darwin state from X0 on ----
#define SUSP_MAX 128
typedef struct { DWORD tid; int count; HANDLE h; } susp_t;
static susp_t g_susp[SUSP_MAX];
static SRWLOCK g_susp_l = SRWLOCK_INIT;
static susp_t *susp_of(DWORD tid) { for (int i = 0; i < SUSP_MAX; i++) if (g_susp[i].tid == tid) return &g_susp[i]; return NULL; }
kern_return_t thread_suspend(mach_port_t t) {
    if (!t || t == GetCurrentThreadId()) return KERN_INVALID_ARGUMENT;
    AcquireSRWLockExclusive(&g_susp_l);
    susp_t *s = susp_of(t);
    if (s) { s->count++; ReleaseSRWLockExclusive(&g_susp_l); return KERN_SUCCESS; }
    for (int i = 0; i < SUSP_MAX && !s; i++) if (!g_susp[i].tid) s = &g_susp[i];
    HANDLE h = s ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, t) : NULL;
    if (!h || SuspendThread(h) == (DWORD)-1) { if (h) CloseHandle(h); ReleaseSRWLockExclusive(&g_susp_l); return KERN_FAILURE; }
    // SuspendThread is asynchronous: GetThreadContext waits until the thread has stopped
    CONTEXT c = {.ContextFlags = CONTEXT_CONTROL}; GetThreadContext(h, &c);
    s->tid = t; s->count = 1; s->h = h;
    ReleaseSRWLockExclusive(&g_susp_l);
    return KERN_SUCCESS;
}
kern_return_t thread_resume(mach_port_t t) {
    AcquireSRWLockExclusive(&g_susp_l);
    susp_t *s = susp_of(t);
    if (!s || s->count <= 0) { ReleaseSRWLockExclusive(&g_susp_l); return KERN_FAILURE; }
    if (--s->count == 0) { ResumeThread(s->h); CloseHandle(s->h); s->h = NULL; s->tid = 0; }
    ReleaseSRWLockExclusive(&g_susp_l);
    return KERN_SUCCESS;
}
kern_return_t thread_get_state(mach_port_t t, int flavor, thread_state_t out, mach_msg_type_number_t *n) {
    if (flavor != ARM_THREAD_STATE64 && flavor != ARM_NEON_STATE64) return KERN_INVALID_ARGUMENT;
    AcquireSRWLockShared(&g_susp_l);
    susp_t *s = susp_of(t); CONTEXT c = {.ContextFlags = CONTEXT_FULL}; int ok = s && GetThreadContext(s->h, &c);
    ReleaseSRWLockShared(&g_susp_l);
    if (!ok) return KERN_FAILURE;
    if (flavor == ARM_THREAD_STATE64) {
        struct __darwin_arm_thread_state64 *ss = (void *)out; memcpy(ss, (uint8_t *)&c + 8, 33 * 8); ss->__cpsr = c.Cpsr; ss->__pad = 0;
        if (n) *n = ARM_THREAD_STATE64_COUNT;
    } else {
        _STRUCT_ARM_NEON_STATE64 *ns = (void *)out; memcpy(ns->__v, &c.V, sizeof ns->__v); ns->__fpcr = c.Fpcr; ns->__fpsr = c.Fpsr;
        if (n) *n = ARM_NEON_STATE64_COUNT;
    }
    return KERN_SUCCESS;
}

// ---- sysctl: only KERN_PROC_PID's start time (main.c), from the process's creation time ----
#undef sysctl
int dm_android_sysctl(int *mib, unsigned n, void *out, size_t *outn, void *in, size_t inn) {
    (void)in; (void)inn;
    if (n != 4 || mib[0] != CTL_KERN || mib[1] != KERN_PROC || mib[2] != KERN_PROC_PID || !out || *outn < sizeof(struct kinfo_proc)) { errno = ENOTSUP; return -1; }
    FILETIME c, e, k, u; if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { errno = EIO; return -1; }
    uint64_t t = (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) - 116444736000000000ull;   // 100 ns since 1970
    struct kinfo_proc *kp = out; memset(kp, 0, sizeof *kp);
    kp->kp_proc.p_starttime.tv_sec = (long)(t / 10000000ull); kp->kp_proc.p_starttime.tv_usec = (long)(t % 10000000ull / 10);
    *outn = sizeof *kp; return 0;
}

// ---- dispatch semaphores (a count under a mutex; the timeout is CLOCK_MONOTONIC nanoseconds, waited as the time left) ----
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) {
    if (when == DISPATCH_TIME_FOREVER) return when;
    uint64_t base = when ? when : clock_gettime_nsec_np(CLOCK_MONOTONIC);
    return delta < 0 && (uint64_t)-delta > base ? 1 : base + (uint64_t)delta;
}
dispatch_semaphore_t dispatch_semaphore_create(long value) {
    dispatch_semaphore_t s = calloc(1, sizeof *s); if (!s) return NULL;
    pthread_mutex_init(&s->m, NULL); pthread_cond_init(&s->c, NULL);
    s->v = value; return s;
}
long dispatch_semaphore_signal(dispatch_semaphore_t s) {
    pthread_mutex_lock(&s->m); long woke = s->v < 0; s->v++; pthread_cond_signal(&s->c); pthread_mutex_unlock(&s->m); return woke;
}
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t timeout) {
    pthread_mutex_lock(&s->m);
    while (s->v <= 0) {
        if (timeout == DISPATCH_TIME_FOREVER) { pthread_cond_wait(&s->c, &s->m); continue; }
        uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        if (now >= timeout) { pthread_mutex_unlock(&s->m); return 1; }
        struct timespec rel = {(time_t)((timeout - now) / 1000000000ull), (long)((timeout - now) % 1000000000ull)};
        pthread_cond_timedwait_relative_np(&s->c, &s->m, &rel);
    }
    s->v--; pthread_mutex_unlock(&s->m); return 0;
}
void dispatch_release(void *o) {
    dispatch_semaphore_t s = o; if (!s) return;
    pthread_mutex_destroy(&s->m); pthread_cond_destroy(&s->c); free(s);
}
struct dm_dispatch_queue { long qos; };
static struct dm_dispatch_queue g_global_q[6];
dispatch_queue_t dispatch_get_global_queue(long qos, unsigned long flags) {
    (void)flags; long i = qos >= QOS_CLASS_USER_INTERACTIVE ? 0 : qos >= QOS_CLASS_USER_INITIATED ? 1 : qos >= QOS_CLASS_DEFAULT ? 2 : qos >= QOS_CLASS_UTILITY ? 3 : qos ? 4 : 5;
    g_global_q[i].qos = qos; return &g_global_q[i];
}
typedef struct { void *ctx; void (*work)(void *); long qos; } dm_async_item;
static void *dm_async_main(void *p) {
    dm_async_item it = *(dm_async_item *)p; free(p);
    if (it.qos) dm_android_qos_set((qos_class_t)it.qos);
    it.work(it.ctx); return NULL;
}
void dispatch_async_f(dispatch_queue_t q, void *ctx, void (*work)(void *)) {
    dm_async_item *it = malloc(sizeof *it); pthread_t th;
    if (!it) { work(ctx); return; }
    *it = (dm_async_item){ctx, work, q ? q->qos : 0};
    if (pthread_create(&th, NULL, dm_async_main, it)) { free(it); work(ctx); return; }
    pthread_detach(th);
}
void qsort_r_dm(void *base, size_t n, size_t size, int (*cmp)(void *, const void *, const void *), void *ctx) { qsort_s(base, n, size, cmp, ctx); }

// ---- mergesort (BSD libc): a stable sort; bionic has none (hle_hid.c sorts DM_KEYS steps with it) ----
int mergesort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    if (n < 2 || !size) return 0;
    char *a = base, *t = malloc(n * size); if (!t) { errno = ENOMEM; return -1; }
    for (size_t w = 1; w < n; w *= 2) {   // bottom-up: runs of w merged into runs of 2w, ties taken from the left run
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi) { if (cmp(a + j * size, a + i * size) < 0) memcpy(t + k++ * size, a + j++ * size, size); else memcpy(t + k++ * size, a + i++ * size, size); }
            if (i < mid) memcpy(t + k * size, a + i * size, (mid - i) * size), k += mid - i;
            if (j < hi) memcpy(t + k * size, a + j * size, (hi - j) * size);
        }
        memcpy(a, t, n * size);
    }
    free(t); return 0;
}

// ---- SHA-256 (FIPS 180-4) ----
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0; return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *d, CC_LONG n) {
    const uint8_t *p = d; c->len += n;
    while (n) {
        uint32_t k = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; n -= k;
        if (c->n == 64) { sha_block(c->h, c->buf); c->n = 0; }
    }
    return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->len * 8; uint8_t pad = 0x80, z = 0;
    CC_SHA256_Update(c, &pad, 1);
    while (c->n != 56) CC_SHA256_Update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(c, lb, 8);
    for (int i = 0; i < 8; i++) { md[4 * i] = (uint8_t)(c->h[i] >> 24); md[4 * i + 1] = (uint8_t)(c->h[i] >> 16); md[4 * i + 2] = (uint8_t)(c->h[i] >> 8); md[4 * i + 3] = (uint8_t)c->h[i]; }
    return 1;
}
unsigned char *CC_SHA256(const void *d, CC_LONG n, unsigned char *md) { CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, d, n); CC_SHA256_Final(md, &c); return md; }

// mach_absolute_time as a real function, for assembly that calls it (nvn_entry.S; tools/asm_elf.py renames the call)
uint64_t dm_mach_absolute_time(void) { return mach_absolute_time(); }

// ---- CFMutableDictionary with pointer keys (open addressing, linear probing, tombstones) ----
struct dm_cfdict { const void **k, **v; size_t cap, n, used; };
#define CF_TOMB ((const void *)1)
static size_t cf_h(const void *k, size_t cap) { uint64_t x = (uint64_t)(uintptr_t)k * 0x9E3779B97F4A7C15ull; return (size_t)(x >> 17) & (cap - 1); }
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb) {
    (void)a; (void)cap; (void)kcb; (void)vcb;
    struct dm_cfdict *d = calloc(1, sizeof *d); d->cap = 64; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); return d;
}
static const void **cf_slot(struct dm_cfdict *d, const void *k, int insert) {
    const void **tomb = NULL;
    for (size_t i = cf_h(k, d->cap);; i = (i + 1) & (d->cap - 1)) {
        if (d->k[i] == k) return &d->k[i];
        if (d->k[i] == CF_TOMB) { if (!tomb) tomb = &d->k[i]; continue; }
        if (!d->k[i]) return insert ? (tomb ? tomb : &d->k[i]) : NULL;
    }
}
static void cf_grow(struct dm_cfdict *d) {
    const void **ok = d->k, **ov = d->v; size_t oc = d->cap;
    d->cap *= 2; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); d->used = d->n;
    for (size_t i = 0; i < oc; i++) if (ok[i] && ok[i] != CF_TOMB) { const void **s = cf_slot(d, ok[i], 1); *s = ok[i]; d->v[s - d->k] = ov[i]; }
    free(ok); free(ov);
}
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *k) { const void **s = cf_slot(d, k, 0); return s ? d->v[s - d->k] : NULL; }
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) {
    if ((d->used + 1) * 4 > d->cap * 3) cf_grow(d);
    const void **s = cf_slot(d, k, 1);
    if (*s != k) { if (!*s) d->used++; *s = k; d->n++; }
    d->v[s - d->k] = v;
}
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k) { const void **s = cf_slot(d, k, 0); if (s) { *s = CF_TOMB; d->v[s - d->k] = NULL; d->n--; } }
CFIndex CFDictionaryGetCount(CFDictionaryRef d) { return (CFIndex)d->n; }
void CFRelease(CFTypeRef o) { struct dm_cfdict *d = (struct dm_cfdict *)o; if (d) { free(d->k); free(d->v); free(d); } }

// ---- Files: clonefile as a copy of the tree (with its guest-name streams), RENAME_SWAP as three renames through <to>.swap (plat_android.c tells why the
// order is safe against a kill); MoveFileEx has no exchange ----
static void copy_xattrs(const char *s, const char *d) {
    char names[1024]; ssize_t n = dm_android_listxattr(s, names, sizeof names, 0);
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(names + i) + 1) {
        char v[1024]; ssize_t k = dm_android_getxattr(s, names + i, v, sizeof v, 0, 0);
        if (k >= 0) dm_android_setxattr(d, names + i, v, (size_t)k, 0, 0);
    }
}
static int copy_tree(const char *s, const char *d) {
    struct stat st; if (lstat(s, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) {
        if (!S_ISREG(st.st_mode)) return 0;
        wchar_t ws[1024], wd[1024];
        MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, 1024); MultiByteToWideChar(CP_UTF8, 0, d, -1, wd, 1024);
        for (wchar_t *c = ws; *c; c++) if (*c == L'/') *c = L'\\';
        for (wchar_t *c = wd; *c; c++) if (*c == L'/') *c = L'\\';
        if (!CopyFileExW(ws, wd, NULL, NULL, NULL, COPY_FILE_FAIL_IF_EXISTS)) { errno = GetLastError() == ERROR_FILE_EXISTS ? EEXIST : EIO; return -1; }
        copy_xattrs(s, d);   // CopyFileEx copies the streams too; again for a volume without (no harm)
        return 0;
    }
    if (mkdir(d, 0755)) return -1;
    copy_xattrs(s, d);
    DIR *dir = opendir(s); if (!dir) return -1;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX], b[PATH_MAX];
        if (snprintf(a, sizeof a, "%s/%s", s, e->d_name) >= (int)sizeof a || snprintf(b, sizeof b, "%s/%s", d, e->d_name) >= (int)sizeof b) { errno = ENAMETOOLONG; r = -1; break; }
        r = copy_tree(a, b);
    }
    closedir(dir); return r;
}
int dm_android_clonefile(const char *src, const char *dst, int flags) {
    (void)flags;
    struct stat st; if (!lstat(dst, &st)) { errno = EEXIST; return -1; }
    return copy_tree(src, dst);
}
static int remove_tree(const char *p) {
    struct stat st; if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    DIR *dir = opendir(p); if (!dir) return -1;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX]; snprintf(a, sizeof a, "%s/%s", p, e->d_name);
        r = remove_tree(a);
    }
    closedir(dir);
    return r ? r : rmdir(p);
}
static int swap_side(const char *to, char *side, size_t n) {
    if (snprintf(side, n, "%s.swap", to) < (int)n) return 0;
    errno = ENAMETOOLONG; return -1;
}
int dm_android_swap_by_renames(const char *from, const char *to) {
    char side[PATH_MAX]; struct stat st;
    if (swap_side(to, side, sizeof side) || lstat(from, &st) || lstat(to, &st)) return -1;
    if (!lstat(side, &st) && remove_tree(side)) return -1;
    if (rename(to, side)) return -1;
    if (rename(from, to)) { int e = errno; rename(side, to); errno = e; return -1; }
    if (rename(side, from)) LOG("[RED] swap: %s holds the new version, but the old one stays in %s (errno=%d) until the next mount deletes it", to, side, errno);
    return 0;
}
void dm_android_swap_recover(const char *p) {
    char side[PATH_MAX]; struct stat st;
    if (swap_side(p, side, sizeof side) || lstat(side, &st)) return;
    if (lstat(p, &st)) {
        if (rename(side, p)) LOG("[RED] swap: %s is missing and moving %s back failed errno=%d", p, side, errno);
        else LOG("swap: %s was cut short by a kill before its second rename: the version from before it is back", p);
    } else if (remove_tree(side)) LOG("[RED] swap: cannot delete %s, which a swap cut short by a kill left (errno=%d)", side, errno);
    else LOG("swap: deleted %s, the version that a swap cut short by a kill had already replaced", side);
}
int dm_android_renamex_np(const char *from, const char *to, unsigned flags) {
    if (!(flags & RENAME_SWAP)) return rename(from, to);
    return dm_android_swap_by_renames(from, to);
}
