// POSIX on Windows on Arm for the host (dm_windows.h): memory (mmap over VirtualAlloc, with commit on first touch for large ranges), signals (a vectored exception
// handler for SIGSEGV / SIGBUS / SIGILL / SIGTRAP, pthread_kill by suspending the thread and running the handler on it), files with UTF-8 paths, directories, time,
// and the small libc extras the UCRT lacks. Only what the host calls, with the semantics it relies on (each section says which).
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>
#include <psapi.h>
#include <timeapi.h>
#include <share.h>
#include "dm_windows.h"
#include <stdatomic.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <spawn.h>
#include <dlfcn.h>
#undef free
#undef realloc
#undef open
#undef fopen
#undef rename
#undef unlink
#undef rmdir
#undef access
#undef chdir
#undef chmod
#undef stat
#undef fstat
#undef lstat
#undef mkdir
#undef raise
#undef pthread_kill
#undef pthread_sigmask
#undef sigaction
#undef opendir
#undef readdir
#undef closedir
#undef rewinddir
#undef dirfd
#undef fdopendir
#undef dirent
#undef DIR
#undef st_atime
#undef st_mtime
#undef st_ctime
#undef ftruncate
#define PLOG(...) do { fprintf(stderr, "[win] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// ---------------- Errors ----------------
static int err_of(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH: return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: case ERROR_PRIVILEGE_NOT_HELD: return EACCES;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_BUSY: return EBUSY;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_COMMITMENT_LIMIT: return ENOMEM;
    case ERROR_NOACCESS: case ERROR_INVALID_USER_BUFFER: return EFAULT;
    case ERROR_NOT_SUPPORTED: case ERROR_INVALID_FUNCTION: return ENOTSUP;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_NOT_SAME_DEVICE: return EXDEV;
    case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: return EPIPE;
    case ERROR_HANDLE_EOF: return 0;
    default: return EIO;
    }
}
static int fail_win(void) { errno = err_of(GetLastError()); return -1; }

// ---------------- Paths: UTF-8 with '/' (the host's) and UTF-16 with '\' (Win32's) ----------------
#define WP 4096
static __thread wchar_t t_wp[3][WP];
static wchar_t *wpath_n(const char *p, int k) {
    wchar_t *w = t_wp[k];
    if (!p) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, w, WP);
    if (n <= 0) { w[0] = 0; return w; }
    for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
    return w;
}
#define WPATH(p) wpath_n((p), 0)
#define WPATH2(p) wpath_n((p), 1)
static int utf8_of(const wchar_t *w, char *out, size_t n) {
    int k = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
    if (k <= 0) { if (n) out[0] = 0; return -1; }
    for (char *c = out; *c; c++) if (*c == '\\') *c = '/';
    return 0;
}

// ---------------- Start-up ----------------
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep);
static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep);
static BOOL WINAPI console_ctrl(DWORD type);
static uint8_t *g_img_lo, *g_img_hi;   // the host's own image (an exception with the pc there is the host's)
static LARGE_INTEGER g_qpf;
__attribute__((constructor)) static void posix_init(void) {
    _set_fmode(_O_BINARY);
    _setmode(0, _O_BINARY); _setmode(1, _O_BINARY); _setmode(2, _O_BINARY);
    setvbuf(stderr, NULL, _IONBF, 0);
    QueryPerformanceFrequency(&g_qpf);
    HMODULE m = GetModuleHandleW(NULL); MODULEINFO mi;
    if (GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) { g_img_lo = mi.lpBaseOfDll; g_img_hi = g_img_lo + mi.SizeOfImage; }
    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter(unhandled);
    SetConsoleCtrlHandler(console_ctrl, TRUE);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    timeBeginPeriod(1);   // 1 ms sleeps and timed waits (winpthreads' timed waits and Sleep round to the timer's period)
    ULONG g = 65536; SetThreadStackGuarantee(&g);
}

// ---------------- Time ----------------
uint64_t dm_win_mono_ns(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    uint64_t f = (uint64_t)g_qpf.QuadPart ? (uint64_t)g_qpf.QuadPart : 10000000ull, t = (uint64_t)c.QuadPart;
    return t / f * 1000000000ull + t % f * 1000000000ull / f;
}
static __thread HANDLE t_timer;
static void wait_ns(uint64_t ns) {   // a high-resolution waitable timer (Windows 10 1803 on), else Sleep
    if (!ns) { SwitchToThread(); return; }
    if (!t_timer) t_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (t_timer) { LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((ns + 99) / 100); if (SetWaitableTimer(t_timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(t_timer, INFINITE); return; } }
    Sleep((DWORD)((ns + 999999) / 1000000));
}
kern_return_t mach_wait_until(uint64_t deadline) { uint64_t now = dm_win_mono_ns(); if (deadline > now) wait_ns(deadline - now); return KERN_SUCCESS; }
// clock_nanosleep: winpthreads' (its waits round to the timer period, 1 ms after timeBeginPeriod)
struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) ? NULL : out; }
struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }
time_t timegm(struct tm *tm) { return _mkgmtime(tm); }
const char *dm_win_tm_zone(const struct tm *tm) { static char z[2][64]; static int done; if (!done) { size_t n; _get_tzname(&n, z[0], sizeof z[0], 0); _get_tzname(&n, z[1], sizeof z[1], 1); done = 1; } return z[tm->tm_isdst > 0]; }
long dm_win_tm_gmtoff(const struct tm *tm) { long tz = 0, dst = 0; _get_timezone(&tz); _get_dstbias(&dst); return -(tz + (tm->tm_isdst > 0 ? dst : 0)); }
uint64_t dm_win_thread_cpu_ns(pthread_t t) {
    HANDLE h = t ? pthread_gethandle(t) : GetCurrentThread(); FILETIME c, e, k, u;
    if (!h || !GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100;
}

// ---------------- Memory: mmap ----------------
// Every mapping mmap makes is recorded: a region is one VirtualAlloc reservation (or one file view) with a byte per 4 KB page for its pages' protection as the
// host set it (PROT_*, and P_COMMIT once committed). A page is committed when the host makes it accessible, unless the range is lazy: then the exception handler
// commits it on first touch (with the recorded protection), so a 9 GB heap made read-write costs only what the game touches. The guest's memory is one large region.
#define PG 4096ull
#define GRAN 65536ull
#define P_COMMIT 0x80
enum { R_ANON = 1, R_FILE = 2 };
typedef struct { uint8_t *lo, *hi; int kind; uint8_t *pm; void *view; HANDLE map; } region;
static region *g_rg; static int g_nrg, g_crg;
static SRWLOCK g_rgl = SRWLOCK_INIT;
static _Atomic uint64_t g_lazy_bytes;
static int g_lazy_env = -1;   // DM_WIN_LAZY: -1 unread, 0 never, 1 always, 2 ranges of 64 MB and more (the default)
static uint64_t g_lazy_min = 64ull << 20;
static int lazy_mode(void) { if (g_lazy_env < 0) { const char *e = getenv("DM_WIN_LAZY"); g_lazy_env = e && *e ? atoi(e) : 2; } return g_lazy_env; }
static region *rg_find(const void *p) {   // under g_rgl
    int lo = 0, hi = g_nrg - 1; const uint8_t *a = p;
    while (lo <= hi) { int m = (lo + hi) / 2; if (a < g_rg[m].lo) hi = m - 1; else if (a >= g_rg[m].hi) lo = m + 1; else return &g_rg[m]; }
    return NULL;
}
static int rg_add(region r) {   // under g_rgl (exclusive)
    if (g_nrg == g_crg) { int c = g_crg ? 2 * g_crg : 64; region *n = realloc(g_rg, (size_t)c * sizeof *n); if (!n) return -1; g_rg = n; g_crg = c; }
    int i = g_nrg; while (i > 0 && g_rg[i - 1].lo > r.lo) { g_rg[i] = g_rg[i - 1]; i--; }
    g_rg[i] = r; g_nrg++; return 0;
}
static void rg_del(region *r) { int i = (int)(r - g_rg); free(r->pm); memmove(r, r + 1, (size_t)(g_nrg - i - 1) * sizeof *r); g_nrg--; }
int dm_win_mapped(const void *p, size_t n) {
    AcquireSRWLockShared(&g_rgl); region *r = rg_find(p); int ok = r && (const uint8_t *)p + n <= r->hi; ReleaseSRWLockShared(&g_rgl); return ok;
}
uint64_t dm_win_lazy_bytes(void) { return atomic_load(&g_lazy_bytes); }
static DWORD wprot(int prot) {
    if (prot & PROT_EXEC) return prot & PROT_WRITE ? PAGE_EXECUTE_READWRITE : prot & PROT_READ ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    if (prot & PROT_READ) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
// [a, b) of an anonymous region to prot: committed pages change protection, uncommitted ones are committed now, or left for the first touch when lazy.
// fresh (mmap MAP_FIXED): committed pages are decommitted first, so that they come back zeroed as a new mapping's
static int anon_protect(region *r, uint8_t *a, uint8_t *b, int prot, int fresh) {
    int lazy = prot != PROT_NONE && (lazy_mode() == 1 || (lazy_mode() == 2 && (uint64_t)(b - a) >= g_lazy_min));
    size_t i = (size_t)(a - r->lo) / PG, e = (size_t)(b - r->lo) / PG;
    while (i < e) {
        size_t j = i; int com = r->pm[i] & P_COMMIT;
        while (j < e && (r->pm[j] & P_COMMIT) == com) j++;
        uint8_t *p = r->lo + i * PG; size_t n = (j - i) * PG;
        if (com && fresh) {
            if (!VirtualFree(p, n, MEM_DECOMMIT)) return fail_win();
            for (size_t k = i; k < j; k++) r->pm[k] = 0;
            continue;   // the same pages again, now uncommitted
        }
        if (com) {
            DWORD old; if (!VirtualProtect(p, n, wprot(prot), &old)) return fail_win();
            for (size_t k = i; k < j; k++) r->pm[k] = (uint8_t)(P_COMMIT | prot);
        } else if (prot == PROT_NONE || lazy) {
            for (size_t k = i; k < j; k++) r->pm[k] = (uint8_t)prot;
        } else if (!VirtualAlloc(p, n, MEM_COMMIT, wprot(prot))) {
            DWORD le = GetLastError();
            if (le != ERROR_COMMITMENT_LIMIT && le != ERROR_NOT_ENOUGH_MEMORY) return fail_win();
            for (size_t k = i; k < j; k++) r->pm[k] = (uint8_t)prot;   // over the system's commit limit: committed on first touch instead
        } else for (size_t k = i; k < j; k++) r->pm[k] = (uint8_t)(P_COMMIT | prot);
        i = j;
    }
    return 0;
}
// The exception handler's part: a touch of a lazy page commits it (with up to 15 lazy pages after it of the same protection: fewer faults for a sequential fill).
// 1 = committed (resume), 0 = not a lazy page
static int lazy_commit(const void *addr) {
    int done = 0;
    AcquireSRWLockExclusive(&g_rgl);
    region *r = rg_find(addr);
    if (r && r->kind == R_ANON) {
        size_t i = (size_t)((const uint8_t *)addr - r->lo) / PG, n = (size_t)(r->hi - r->lo) / PG; uint8_t m = r->pm[i];
        if (!(m & P_COMMIT) && (m & 7)) {
            size_t e = i + 1; while (e < n && e < i + 16 && r->pm[e] == m) e++;
            if (VirtualAlloc(r->lo + i * PG, (e - i) * PG, MEM_COMMIT, wprot(m & 7))) { for (size_t k = i; k < e; k++) r->pm[k] = (uint8_t)(P_COMMIT | m); atomic_fetch_add(&g_lazy_bytes, (e - i) * PG); done = 1; }
            else if (VirtualAlloc(r->lo + i * PG, PG, MEM_COMMIT, wprot(m & 7))) { r->pm[i] = (uint8_t)(P_COMMIT | m); atomic_fetch_add(&g_lazy_bytes, PG); done = 1; }
            else done = -1;   // out of commit: the fault goes on to the host's handler
        } else if ((m & P_COMMIT) && (m & 7)) {   // committed by another thread between its fault and this lock: retry the access
            MEMORY_BASIC_INFORMATION mi; if (VirtualQuery(addr, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect != PAGE_NOACCESS && (mi.Protect == wprot(m & 7))) done = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_rgl);
    if (done < 0) PLOG("out of memory: no commit for the page at %p (the system's commit limit: a larger page file, or fewer programs open)", addr);
    return done > 0;
}
static void *map_file(void *addr, size_t len, int prot, int flags, int fd, int64_t off) {
    HANDLE fh = (HANDLE)_get_osfhandle(fd); if (fh == INVALID_HANDLE_VALUE) { errno = EBADF; return MAP_FAILED; }
    if (flags & MAP_FIXED) { errno = ENOTSUP; return MAP_FAILED; }   // a file over a fixed range (DM_GUEST_FILE) is not done on Windows: the host keeps anonymous memory
    int wr = (prot & PROT_WRITE) && (flags & MAP_SHARED);
    DWORD fp = prot & PROT_EXEC ? (wr ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_WRITECOPY) : wr ? PAGE_READWRITE : (prot & PROT_WRITE) ? PAGE_WRITECOPY : PAGE_READONLY;
    uint64_t end = (uint64_t)off + len;
    HANDLE mh = CreateFileMappingW(fh, NULL, fp, (DWORD)(end >> 32), (DWORD)end, NULL);
    if (!mh) { fail_win(); return MAP_FAILED; }
    uint64_t aoff = (uint64_t)off & ~(GRAN - 1), pre = (uint64_t)off - aoff;
    DWORD acc = wr ? FILE_MAP_WRITE : (prot & PROT_WRITE) ? FILE_MAP_COPY : FILE_MAP_READ;
    if (prot & PROT_EXEC) acc |= FILE_MAP_EXECUTE;
    uint8_t *v = MapViewOfFile(mh, acc, (DWORD)(aoff >> 32), (DWORD)aoff, (SIZE_T)(len + pre));
    if (!v) { fail_win(); CloseHandle(mh); return MAP_FAILED; }
    (void)addr;
    AcquireSRWLockExclusive(&g_rgl); rg_add((region){v + pre, v + pre + ((len + PG - 1) & ~(PG - 1)), R_FILE, NULL, v, mh}); ReleaseSRWLockExclusive(&g_rgl);
    return v + pre;
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off) {
    if (!len) { errno = EINVAL; return MAP_FAILED; }
    if (!(flags & MAP_ANONYMOUS)) return map_file(addr, len, prot, flags, fd, off);
    size_t n = (len + PG - 1) & ~(PG - 1);
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        AcquireSRWLockExclusive(&g_rgl);
        region *r = rg_find(addr);
        if (r && r->kind == R_ANON && (uint8_t *)addr + n <= r->hi && !((uintptr_t)addr & (PG - 1))) {   // inside a reservation of ours: fresh pages there
            int rc = (flags & MAP_FIXED_NOREPLACE) ? (errno = EEXIST, -1) : anon_protect(r, addr, (uint8_t *)addr + n, PROT_NONE, 1);
            if (!rc && prot != PROT_NONE) rc = anon_protect(r, addr, (uint8_t *)addr + n, prot, 0);
            ReleaseSRWLockExclusive(&g_rgl);
            return rc ? MAP_FAILED : addr;
        }
        ReleaseSRWLockExclusive(&g_rgl);
        if (r || ((uintptr_t)addr & (GRAN - 1))) { errno = ENOMEM; return MAP_FAILED; }   // across regions, or free space off a 64 KB boundary: Windows cannot place it
    }
    size_t rn = (n + GRAN - 1) & ~(GRAN - 1);
    uint8_t *p = VirtualAlloc(addr, rn, MEM_RESERVE, PAGE_NOACCESS);
    if (!p && addr && !(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) p = VirtualAlloc(NULL, rn, MEM_RESERVE, PAGE_NOACCESS);   // a hint, not a demand
    if (!p) { fail_win(); return MAP_FAILED; }
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && p != addr) { VirtualFree(p, 0, MEM_RELEASE); errno = ENOMEM; return MAP_FAILED; }
    uint8_t *pm = calloc(rn / PG, 1);
    if (!pm) { VirtualFree(p, 0, MEM_RELEASE); errno = ENOMEM; return MAP_FAILED; }
    AcquireSRWLockExclusive(&g_rgl);
    int rc = rg_add((region){p, p + rn, R_ANON, pm, NULL, NULL});
    region *r = rc ? NULL : rg_find(p);
    if (!rc && prot != PROT_NONE) rc = anon_protect(r, p, p + n, prot, 0);
    if (rc) { if (r) rg_del(r); else free(pm); VirtualFree(p, 0, MEM_RELEASE); }
    ReleaseSRWLockExclusive(&g_rgl);
    if (!rc && (flags & MAP_POPULATE)) for (size_t k = 0; k < n; k += PG) (void)*(volatile uint8_t *)(p + k);
    return rc ? MAP_FAILED : p;
}
int munmap(void *addr, size_t len) {
    uint8_t *a = addr, *b = a + ((len + PG - 1) & ~(PG - 1));
    AcquireSRWLockExclusive(&g_rgl);
    while (a < b) {
        region *r = rg_find(a);
        if (!r) { a += PG; continue; }   // POSIX: unmapping pages that are not mapped is no error
        uint8_t *e = b < r->hi ? b : r->hi;
        if (r->kind == R_FILE) { if (a == r->lo) { UnmapViewOfFile(r->view); CloseHandle(r->map); a = r->hi; rg_del(r); } else a = e; continue; }
        if (a == r->lo && e == r->hi) { VirtualFree(r->lo, 0, MEM_RELEASE); a = r->hi; rg_del(r); continue; }
        // a part: decommitted; the addresses stay reserved (Windows releases only whole reservations), so nothing else is placed there
        size_t i = (size_t)(a - r->lo) / PG, j = (size_t)(e - r->lo) / PG;
        for (size_t k = i; k < j; k++) if (r->pm[k] & P_COMMIT) { size_t m = k; while (m < j && (r->pm[m] & P_COMMIT)) m++; VirtualFree(r->lo + k * PG, (m - k) * PG, MEM_DECOMMIT); k = m - 1; }
        memset(r->pm + i, 0, j - i);
        a = e;
    }
    ReleaseSRWLockExclusive(&g_rgl);
    return 0;
}
int mprotect(void *addr, size_t len, int prot) {
    uint8_t *a = (uint8_t *)((uintptr_t)addr & ~(PG - 1)), *b = (uint8_t *)((((uintptr_t)addr + len) + PG - 1) & ~(PG - 1));
    AcquireSRWLockExclusive(&g_rgl);
    int rc = 0;
    while (a < b && !rc) {
        region *r = rg_find(a);
        if (!r) {   // memory mmap did not make (the host's image, a heap block): VirtualProtect as it is
            ReleaseSRWLockExclusive(&g_rgl);
            DWORD old; return VirtualProtect(a, (size_t)(b - a), wprot(prot), &old) ? 0 : fail_win();
        }
        uint8_t *e = b < r->hi ? b : r->hi;
        if (r->kind == R_FILE) { DWORD old; if (!VirtualProtect(a, (size_t)(e - a), wprot(prot), &old)) rc = fail_win(); }
        else rc = anon_protect(r, a, e, prot, 0);
        a = e;
    }
    ReleaseSRWLockExclusive(&g_rgl);
    return rc;
}
// MADV_DONTNEED / MADV_FREE: the pages' contents may go. Linux gives zeros back after DONTNEED: decommitted here, and committed again, zeroed, on next touch
int madvise(void *addr, size_t len, int advice) {
    if (advice != MADV_DONTNEED && advice != MADV_FREE && advice != MADV_FREE_REUSABLE) return 0;
    uint8_t *a = addr, *b = a + len;
    AcquireSRWLockExclusive(&g_rgl);
    region *r = rg_find(a);
    if (r && r->kind == R_ANON && b <= r->hi && !((uintptr_t)a & (PG - 1))) {
        size_t i = (size_t)(a - r->lo) / PG, j = (size_t)(b - r->lo + PG - 1) / PG;
        for (size_t k = i; k < j; k++) if (r->pm[k] & P_COMMIT) {
            size_t m = k; while (m < j && r->pm[m] == r->pm[k]) m++;
            uint8_t pr = r->pm[k] & 7;
            if (VirtualFree(r->lo + k * PG, (m - k) * PG, MEM_DECOMMIT)) for (size_t q = k; q < m; q++) r->pm[q] = pr;
            k = m - 1;
        }
    }
    ReleaseSRWLockExclusive(&g_rgl);
    return 0;
}
int mincore(void *addr, size_t len, unsigned char *vec) {
    uint8_t *a = (uint8_t *)((uintptr_t)addr & ~(PG - 1)); size_t n = (size_t)(((uint8_t *)addr + len - a) + PG - 1) / PG;
    for (size_t i = 0; i < n;) {
        MEMORY_BASIC_INFORMATION mi; if (!VirtualQuery(a + i * PG, &mi, sizeof mi)) { errno = ENOMEM; return -1; }
        size_t k = (size_t)((uint8_t *)mi.BaseAddress + mi.RegionSize - (a + i * PG)) / PG; if (!k) k = 1;
        for (size_t j = 0; j < k && i < n; j++, i++) vec[i] = mi.State == MEM_COMMIT ? MINCORE_INCORE : 0;
    }
    return 0;
}
int msync(void *addr, size_t len, int flags) { (void)flags; return FlushViewOfFile(addr, len) ? 0 : fail_win(); }
int mlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
int munlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
void *mremap(void *old, size_t oldn, size_t newn, int flags, ...) { (void)old; (void)oldn; (void)newn; (void)flags; errno = ENOTSUP; return MAP_FAILED; }
void dm_win_icache(void *p, size_t n) { FlushInstructionCache(GetCurrentProcess(), p, n); }

// ---------------- Signals ----------------
typedef struct { struct sigaction sa; int set; } sig_ent;
static sig_ent g_sig[DM_NSIG];
static __thread sigset_t t_mask;
void dm_win_crt_sig(int sig);
int dm_win_sigaction(int sig, const struct sigaction *sa, struct sigaction *old) {
    if (sig <= 0 || sig >= DM_NSIG) { errno = EINVAL; return -1; }
    if (old) *old = g_sig[sig].set ? g_sig[sig].sa : (struct sigaction){0};
    if (sa) {
        g_sig[sig].sa = *sa; g_sig[sig].set = 1;
        if (sig == SIGABRT || sig == SIGINT || sig == SIGFPE)   // the CRT's own signals: abort() raises SIGABRT through it
            signal(sig, sa->sa_handler == SIG_DFL ? SIG_DFL : sa->sa_handler == SIG_IGN ? SIG_IGN : dm_win_crt_sig);
    }
    return 0;
}
int dm_win_sigmask(int how, const sigset_t *set, sigset_t *old) {
    if (old) *old = t_mask;
    if (set) { if (how == SIG_BLOCK) t_mask |= *set; else if (how == SIG_UNBLOCK) t_mask &= ~*set; else t_mask = *set; }
    return 0;
}
// Runs the host's handler for sig with the exception's context (written back when the handler returns); 1 when there was one to run
static int run_handler(int sig, void *addr, CONTEXT *ctx, uint64_t esr, int code) {
    if (sig <= 0 || sig >= DM_NSIG || !g_sig[sig].set) return 0;
    struct sigaction sa = g_sig[sig].sa;
    if (sa.sa_handler == SIG_DFL) return 0;
    if (sa.sa_handler == SIG_IGN) return sig != SIGSEGV && sig != SIGBUS && sig != SIGILL;
    if (t_mask & (1ull << sig)) return 0;   // blocked on this thread (a fault inside the handler itself): the default action
    sigset_t was = t_mask;
    t_mask |= sa.sa_mask; if (!(sa.sa_flags & SA_NODEFER)) t_mask |= 1ull << sig;
    if (sa.sa_flags & SA_RESETHAND) g_sig[sig].set = 0;
    siginfo_t si = {.si_signo = sig, .si_code = code, .si_addr = addr};
    ucontext_t uc = {.uc_ctx = ctx, .uc_esr = esr, .uc_cpsr = ctx ? ctx->Cpsr : 0, .uc_sigmask = was};
    if (sa.sa_flags & SA_SIGINFO) sa.sa_sigaction(sig, &si, &uc); else sa.sa_handler(sig);
    t_mask = was;
    return 1;
}
void dm_win_fatal(const char *what);
void dm_win_crt_sig(int sig) {   // the CRT's signal() handler, which the CRT resets to the default each time: set again
    CONTEXT c; RtlCaptureContext(&c);
    if (sig == SIGABRT && !g_sig[sig].set) dm_win_fatal("abort(): the FATAL line in log.txt says why");
    if (!run_handler(sig, NULL, &c, 0, 0)) { signal(sig, SIG_DFL); raise(sig); return; }
    if (g_sig[sig].set) signal(sig, dm_win_crt_sig);
}
static int in_host(const void *pc) { return (const uint8_t *)pc >= g_img_lo && (const uint8_t *)pc < g_img_hi; }
static int sig_of(DWORD code, int *si_code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: *si_code = SEGV_ACCERR; return SIGSEGV;
    case EXCEPTION_STACK_OVERFLOW: *si_code = SEGV_MAPERR; return SIGSEGV;
    case EXCEPTION_IN_PAGE_ERROR: *si_code = 0; return SIGBUS;
    case EXCEPTION_DATATYPE_MISALIGNMENT: *si_code = BUS_ADRALN; return SIGBUS;
    case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION: *si_code = ILL_ILLOPC; return SIGILL;
    case EXCEPTION_BREAKPOINT: case EXCEPTION_SINGLE_STEP: *si_code = TRAP_BRKPT; return SIGTRAP;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_FLT_DIVIDE_BY_ZERO: case EXCEPTION_FLT_INVALID_OPERATION: *si_code = 0; return SIGFPE;
    default: return 0;
    }
}
// An ESR for the handlers that read one (crash.c; the dirty-page tracker reads WnR): a data abort from a lower level, WnR set for a write
static uint64_t esr_of(const EXCEPTION_RECORD *er) {
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return 0;
    if (er->ExceptionInformation[0] == 8) return (0x20ull << 26) | (1ull << 25);   // an instruction abort (execution of a page without execute)
    return (0x24ull << 26) | (1ull << 25) | (er->ExceptionInformation[0] == 1 ? 1ull << 6 : 0);
}
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord; CONTEXT *c = ep->ContextRecord;
    int code; int sig = sig_of(er->ExceptionCode, &code);
    if (!sig || sig == SIGFPE) return EXCEPTION_CONTINUE_SEARCH;
    int av = er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2;
    void *addr = av ? (void *)er->ExceptionInformation[1] : (void *)er->ExceptionAddress;
    if (av && lazy_commit(addr)) return EXCEPTION_CONTINUE_EXECUTION;
    // Ours: a fault in memory mmap made, or with the pc in the host or in memory it made (guest code). Another (a system DLL probing an address under its own
    // __try) goes on to that DLL's handler; one nobody takes reaches the host's crash handler through the unhandled-exception filter below
    void *pc = (void *)c->Pc;
    int mapped = av && dm_win_mapped(addr, 1);
    if (!in_host(pc) && !dm_win_mapped(pc, 4) && !mapped) return EXCEPTION_CONTINUE_SEARCH;
    if (av) code = mapped ? SEGV_ACCERR : SEGV_MAPERR;
    return run_handler(sig, addr, c, esr_of(er), code) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
// dmhost_cli.exe with no arguments: <program folder>/selftest main when the self test is there (dmhost.exe runs it too without a game). → 1 with argc/argv set
int dm_win_default_args(int *argc, char ***argv) {
    wchar_t w[MAX_PATH]; if (!GetModuleFileNameW(NULL, w, MAX_PATH)) return 0;
    wchar_t *b = wcsrchr(w, L'\\'); if (!b) return 0; *b = 0;
    static char dir[MAX_PATH * 3 + 16], t[MAX_PATH * 3 + 40]; static char *av[4];
    if (utf8_of(w, dir, sizeof dir - 16)) return 0;
    strcat(dir, "/selftest"); snprintf(t, sizeof t, "%s/main.fixups.tsv", dir);
    if (GetFileAttributesW(WPATH(t)) == INVALID_FILE_ATTRIBUTES) return 0;
    fprintf(stderr, "[dm] no arguments: running the self test in %s (dmhost_cli <dir> [mod...] for another module)\n", dir);
    setenv("DM_DBG", "1", 0);
    wchar_t tw[MAX_PATH]; char tp[MAX_PATH * 3 + 40], d[MAX_PATH * 3 + 60];
    if (GetTempPathW(MAX_PATH, tw) && !utf8_of(tw, tp, sizeof tp - 40)) {   // the self test's saves and temp files: %TEMP%\nxnative_selftest
        strcat(tp, "nxnative_selftest"); CreateDirectoryW(WPATH(tp), NULL);
        snprintf(d, sizeof d, "%s/save", tp); CreateDirectoryW(WPATH(d), NULL); setenv("DM_SAVE", d, 0);
        snprintf(d, sizeof d, "%s/temp", tp); CreateDirectoryW(WPATH(d), NULL); setenv("DM_TEMP", d, 0);
    }
    av[0] = (*argv)[0]; av[1] = dir; av[2] = "main"; av[3] = NULL; *argv = av; *argc = 3;
    return 1;
}
// The front end's note of a fatal error (front_windows.c: crash.txt and, with no console, a message box), called once before the process ends
void (*dm_win_fatal_hook)(const char *what);
void dm_win_fatal(const char *what) { void (*h)(const char *) = dm_win_fatal_hook; dm_win_fatal_hook = NULL; if (h) h(what); }
static void where_of(const void *pc, char *out, size_t n) {   // "dmhost.exe+0x1234" / "nvoglv64.dll+0x...": the module an address is in
    HMODULE m = NULL; wchar_t w[MAX_PATH]; char a[MAX_PATH * 3];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)pc, &m) && GetModuleFileNameW(m, w, MAX_PATH)) {
        wchar_t *b = wcsrchr(w, L'\\'); WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : w, -1, a, sizeof a, NULL, NULL);
        snprintf(out, n, "%s+%#llx", a, (unsigned long long)((const uint8_t *)pc - (const uint8_t *)m));
    } else snprintf(out, n, "no module (guest or generated code)");
}
static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord; int code = 0; int sig = sig_of(er->ExceptionCode, &code);
    if (!sig) sig = SIGABRT;
    void *addr = er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 ? (void *)er->ExceptionInformation[1] : (void *)er->ExceptionAddress;
    char where[MAX_PATH * 3 + 32]; where_of((void *)ep->ContextRecord->Pc, where, sizeof where);
    fprintf(stderr, "[win] unhandled exception %#lx at pc %p (%s), address %p, thread %lu\n", (unsigned long)er->ExceptionCode, (void *)ep->ContextRecord->Pc, where, addr, GetCurrentThreadId());
    t_mask &= ~(1ull << sig);
    if (run_handler(sig, addr, ep->ContextRecord, esr_of(er), code)) return EXCEPTION_CONTINUE_EXECUTION;
    char m[MAX_PATH * 3 + 128]; snprintf(m, sizeof m, "exception %#lx at %s (address %p)", (unsigned long)er->ExceptionCode, where, addr);
    dm_win_fatal(m);
    TerminateProcess(GetCurrentProcess(), 128 + sig);
    return EXCEPTION_EXECUTE_HANDLER;
}
// SIGTERM: Ctrl+C, Ctrl+Break, the console's close, log-off and shut-down, and the window's close (front_windows.c, dm_win_post_term) end sigwait with SIGTERM
static HANDLE g_term_ev;
static INIT_ONCE g_term_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK term_init(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; g_term_ev = CreateEventW(NULL, TRUE, FALSE, NULL); return TRUE; }
void dm_win_post_term(void) { InitOnceExecuteOnce(&g_term_once, term_init, NULL, NULL); SetEvent(g_term_ev); }
static BOOL WINAPI console_ctrl(DWORD type) { dm_win_post_term(); if (type == CTRL_CLOSE_EVENT) Sleep(1600); return TRUE; }
int sigwait(const sigset_t *set, int *sig) {
    InitOnceExecuteOnce(&g_term_once, term_init, NULL, NULL);
    if (!(*set & (1ull << SIGTERM))) { Sleep(INFINITE); return EINTR; }
    WaitForSingleObject(g_term_ev, INFINITE); *sig = SIGTERM; return 0;
}
int dm_win_raise(int sig) {
    CONTEXT c; RtlCaptureContext(&c);
    if (run_handler(sig, NULL, &c, 0, 0)) return 0;
    if (sig == SIGTERM) { dm_win_post_term(); return 0; }
    return raise(sig);
}
int kill(pid_t pid, int sig) {
    if (pid == getpid() || pid == 0) return dm_win_raise(sig);
    HANDLE h = OpenProcess(sig ? PROCESS_TERMINATE : PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid); if (!h) { errno = ESRCH; return -1; }
    BOOL ok = sig ? TerminateProcess(h, 128 + sig) : TRUE; CloseHandle(h); return ok ? 0 : fail_win();
}
// alarm: SIGALRM's handler on a timer thread (crash.c's watchdogs, DM_WATCH)
static HANDLE g_alarm;
static VOID CALLBACK alarm_fire(PVOID p, BOOLEAN t) { (void)p; (void)t; CONTEXT c; RtlCaptureContext(&c); if (!run_handler(SIGALRM, NULL, &c, 0, 0)) TerminateProcess(GetCurrentProcess(), 128 + SIGALRM); }
unsigned alarm(unsigned s) {
    if (g_alarm) { DeleteTimerQueueTimer(NULL, g_alarm, NULL); g_alarm = NULL; }
    if (s) CreateTimerQueueTimer(&g_alarm, NULL, alarm_fire, NULL, s * 1000u, 0, WT_EXECUTEONLYONCE);
    return 0;
}
int pause(void) { Sleep(INFINITE); return -1; }

// pthread_kill: the thread is suspended and, when it is in guest code (memory mmap made: not the host's image or a system DLL), its context is changed to run the
// handler on its own stack below its sp, then to resume where it was (RtlRestoreContext). Elsewhere (host code, a system call) the signal is dropped: hle_thread.c's
// pause, its one user, resends until the thread is back in guest code or has entered a gate, where it stops on its own
typedef struct { CONTEXT ctx; int sig; } inject_frame;
__attribute__((noreturn)) static void inject_entry(inject_frame *f) {
    CONTEXT c = f->ctx; int sig = f->sig;
    VirtualFree(f, 0, MEM_RELEASE);
    run_handler(sig, NULL, &c, 0, 0);
    RtlRestoreContext(&c, NULL);
    __builtin_unreachable();
}
int dm_win_pthread_kill(pthread_t t, int sig) {
    if (!sig) return 0;
    HANDLE h = pthread_gethandle(t); if (!h) return ESRCH;
    if (GetThreadId(h) == GetCurrentThreadId()) { dm_win_raise(sig); return 0; }
    if (sig <= 0 || sig >= DM_NSIG || !g_sig[sig].set || g_sig[sig].sa.sa_handler == SIG_DFL || g_sig[sig].sa.sa_handler == SIG_IGN) return 0;
    if (SuspendThread(h) == (DWORD)-1) return ESRCH;
    CONTEXT c = {.ContextFlags = CONTEXT_FULL};
    if (GetThreadContext(h, &c) && !in_host((void *)c.Pc) && dm_win_mapped((void *)c.Pc, 4)) {
        inject_frame *f = VirtualAlloc(NULL, sizeof *f, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   // not malloc: the stopped thread may hold the heap's lock
        if (f) {
            f->ctx = c; f->sig = sig;
            c.Sp = (c.Sp - 1024) & ~15ull;   // below what the guest keeps under its sp (the rewriter parks registers up to 24 bytes below it)
            c.Pc = (DWORD64)(uintptr_t)inject_entry; c.X0 = (DWORD64)(uintptr_t)f; c.Lr = 0;
            if (!SetThreadContext(h, &c)) VirtualFree(f, 0, MEM_RELEASE);
        }
    }
    ResumeThread(h);
    return 0;
}
// A guest thread: the TEB's stack bounds opened to the whole user address space, so that Windows' exception dispatch, which checks frames against them, accepts the
// frames of host code running on a guest stack (an HLE function the game calls) or on a gate stack; and room for the exception handler on a stack overflow
__thread uintptr_t dm_win_stack_lo, dm_win_stack_hi;   // the thread's own stack, kept for plat_windows.c dm_android_stack_info
void dm_win_guest_thread_init(void) {
    if (!dm_win_stack_hi) { ULONG_PTR lo, hi; GetCurrentThreadStackLimits(&lo, &hi); dm_win_stack_lo = lo; dm_win_stack_hi = hi; }
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    tib->StackLimit = (PVOID)(uintptr_t)0x10000; tib->StackBase = (PVOID)(uintptr_t)0x7ffffffff000ull;
    ULONG g = 65536; SetThreadStackGuarantee(&g);
}

// ---------------- Files ----------------
// open: CreateFileW with every share mode (POSIX lets a file be renamed or deleted while open; FILE_SHARE_DELETE is the closest), then a CRT descriptor over the handle.
// A directory opens too (O_DIRECTORY, or a path that is one): its descriptor is kept in a table with its path, for fsync, F_GETPATH, fdopendir and the *at calls
#define DIRFD_MAX 256
static struct { int fd; char *path; } g_dirfd[DIRFD_MAX];
static SRWLOCK g_dirl = SRWLOCK_INIT;
static const char *dirfd_path(int fd) {
    const char *p = NULL; AcquireSRWLockShared(&g_dirl);
    for (int i = 0; i < DIRFD_MAX; i++) if (g_dirfd[i].path && g_dirfd[i].fd == fd) { p = g_dirfd[i].path; break; }
    ReleaseSRWLockShared(&g_dirl); return p;
}
static void dirfd_set(int fd, const char *path) {
    AcquireSRWLockExclusive(&g_dirl);
    for (int i = 0; i < DIRFD_MAX; i++) if (g_dirfd[i].path && g_dirfd[i].fd == fd) { free(g_dirfd[i].path); g_dirfd[i].path = NULL; }
    if (path) for (int i = 0; i < DIRFD_MAX; i++) if (!g_dirfd[i].path) { g_dirfd[i].fd = fd; g_dirfd[i].path = _strdup(path); break; }
    ReleaseSRWLockExclusive(&g_dirl);
}
int dm_win_close(int fd) { dirfd_set(fd, NULL); return _close(fd); }
int dm_win_open(const char *path, int flags, ...) {
    int acc = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    DWORD da = acc == O_WRONLY ? GENERIC_WRITE : acc == O_RDWR ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD cd = (flags & O_CREAT) ? ((flags & O_EXCL) ? CREATE_NEW : (flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS) : (flags & O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    if (flags & O_APPEND) da = (da & ~GENERIC_WRITE) | FILE_APPEND_DATA | (da & GENERIC_READ);
    wchar_t *w = WPATH(path);
    DWORD fa = GetFileAttributesW(w);
    int isdir = fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY);
    if ((flags & O_DIRECTORY) && !isdir) { errno = fa == INVALID_FILE_ATTRIBUTES ? ENOENT : ENOTDIR; return -1; }
    if (isdir && (acc != O_RDONLY)) { errno = EISDIR; return -1; }
    HANDLE h = CreateFileW(w, isdir ? GENERIC_READ : da, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, isdir ? OPEN_EXISTING : cd,
                           isdir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS) { errno = EEXIST; return -1; }
        SetLastError(e); return fail_win();
    }
    int cf = _O_BINARY | (acc == O_WRONLY ? _O_WRONLY : acc == O_RDWR ? _O_RDWR : _O_RDONLY) | (flags & O_APPEND ? _O_APPEND : 0);
    int fd = _open_osfhandle((intptr_t)h, cf);
    if (fd < 0) { CloseHandle(h); errno = EMFILE; return -1; }
    dirfd_set(fd, isdir ? path : NULL);   // (a stale entry of a closed directory with the same number goes)
    return fd;
}
FILE *dm_win_fopen(const char *path, const char *mode) {
    wchar_t wm[16]; int i = 0;
    for (const char *m = mode; *m && i < 12; m++) if (*m != 'b' && *m != 'e' && *m != 'x') wm[i++] = (wchar_t)*m;
    wm[i++] = L'b'; if (strchr(mode, 'x')) wm[i++] = L'x'; wm[i] = 0;
    FILE *f = _wfsopen(WPATH(path), wm, _SH_DENYNO);
    if (!f && errno == 0) errno = ENOENT;
    return f;
}
// pread / pwrite: ReadFile / WriteFile at an offset (these move the descriptor's position, which POSIX's leave: the host does not mix them with read on one descriptor).
// The destination's pages are touched first: a read into lazy memory faults in the kernel's copy (ERROR_NOACCESS) instead of in user mode, where it is committed
static void touch(const void *buf, size_t n, int wr) {
    const volatile uint8_t *p = buf;
    if (!n || !dm_win_mapped(buf, 1)) return;
    for (size_t k = 0; k < n; k += PG - ((uintptr_t)(p + k) & (PG - 1))) { if (wr) { uint8_t v = p[k]; ((volatile uint8_t *)p)[k] = v; } else (void)p[k]; }
    if (wr) { uint8_t v = p[n - 1]; ((volatile uint8_t *)p)[n - 1] = v; } else (void)p[n - 1];
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd); if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    touch(buf, n, 1);
    size_t done = 0;
    while (done < n) {
        OVERLAPPED o = {0}; uint64_t at = (uint64_t)off + done; o.Offset = (DWORD)at; o.OffsetHigh = (DWORD)(at >> 32);
        DWORD want = n - done > 0x40000000u ? 0x40000000u : (DWORD)(n - done), got = 0;
        if (!ReadFile(h, (uint8_t *)buf + done, want, &got, &o)) { if (GetLastError() == ERROR_HANDLE_EOF) break; if (done) break; return fail_win(); }
        done += got; if (got < want) break;
    }
    return (ssize_t)done;
}
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd); if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    touch(buf, n, 0);
    size_t done = 0;
    while (done < n) {
        OVERLAPPED o = {0}; uint64_t at = (uint64_t)off + done; o.Offset = (DWORD)at; o.OffsetHigh = (DWORD)(at >> 32);
        DWORD want = n - done > 0x40000000u ? 0x40000000u : (DWORD)(n - done), got = 0;
        if (!WriteFile(h, (const uint8_t *)buf + done, want, &got, &o)) { if (done) break; return fail_win(); }
        done += got; if (got < want) break;
    }
    return (ssize_t)done;
}
ssize_t readv(int fd, const struct iovec *v, int n) {
    ssize_t t = 0;
    for (int i = 0; i < n; i++) { if (!v[i].iov_len) continue; touch(v[i].iov_base, v[i].iov_len, 1); int r = _read(fd, v[i].iov_base, (unsigned)v[i].iov_len); if (r < 0) return t ? t : -1; t += r; if ((size_t)r < v[i].iov_len) break; }
    return t;
}
ssize_t writev(int fd, const struct iovec *v, int n) {
    ssize_t t = 0;
    for (int i = 0; i < n; i++) { if (!v[i].iov_len) continue; int r = _write(fd, v[i].iov_base, (unsigned)v[i].iov_len); if (r < 0) return t ? t : -1; t += r; if ((size_t)r < v[i].iov_len) break; }
    return t;
}
ssize_t preadv(int fd, const struct iovec *v, int n, off_t off) {
    ssize_t t = 0;
    for (int i = 0; i < n; i++) { ssize_t r = pread(fd, v[i].iov_base, v[i].iov_len, off + t); if (r < 0) return t ? t : -1; t += r; if ((size_t)r < v[i].iov_len) break; }
    return t;
}
ssize_t pwritev(int fd, const struct iovec *v, int n, off_t off) {
    ssize_t t = 0;
    for (int i = 0; i < n; i++) { ssize_t r = pwrite(fd, v[i].iov_base, v[i].iov_len, off + t); if (r < 0) return t ? t : -1; t += r; if ((size_t)r < v[i].iov_len) break; }
    return t;
}
int fsync(int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd); if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (dirfd_path(fd)) return 0;   // a directory: NTFS's metadata is journaled, nothing to flush
    return FlushFileBuffers(h) || GetLastError() == ERROR_ACCESS_DENIED ? 0 : fail_win();   // a read-only handle cannot flush: nothing it wrote
}
int fdatasync(int fd) { return fsync(fd); }
int dm_win_ftruncate(int fd, int64_t n) {
    HANDLE h = (HANDLE)_get_osfhandle(fd); if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    FILE_END_OF_FILE_INFO e = {.EndOfFile.QuadPart = n};
    return SetFileInformationByHandle(h, FileEndOfFileInfo, &e, sizeof e) ? 0 : fail_win();
}
int dm_win_mkdir(const char *path, int mode) { (void)mode; return CreateDirectoryW(WPATH(path), NULL) ? 0 : fail_win(); }
int dm_win_rmdir(const char *path) { return RemoveDirectoryW(WPATH(path)) ? 0 : fail_win(); }
int dm_win_unlink(const char *path) {
    wchar_t *w = WPATH(path);
    DWORD a = GetFileAttributesW(w);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) { errno = EISDIR; return -1; }
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(w, a & ~FILE_ATTRIBUTE_READONLY);
    // POSIX semantics where the file system has them (NTFS on Windows 10 1709 on): the name goes now even while the file is open
    HANDLE h = CreateFileW(w, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO_EX d = {.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
        BOOL ok = SetFileInformationByHandle(h, FileDispositionInfoEx, &d, sizeof d);
        CloseHandle(h);
        if (ok) return 0;
    }
    return DeleteFileW(w) ? 0 : fail_win();
}
int dm_win_rename(const char *from, const char *to) {
    wchar_t *a = WPATH(from), *b = WPATH2(to);
    if (MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    DWORD e = GetLastError();
    // POSIX rename replaces an empty directory with a directory: removed first, then moved
    DWORD fa = GetFileAttributesW(a), ta = GetFileAttributesW(b);
    if (e == ERROR_ACCESS_DENIED && fa != INVALID_FILE_ATTRIBUTES && ta != INVALID_FILE_ATTRIBUTES && (fa & ta & FILE_ATTRIBUTE_DIRECTORY) && RemoveDirectoryW(b) && MoveFileExW(a, b, 0)) return 0;
    SetLastError(e); return fail_win();
}
int dm_win_access(const char *path, int mode) {
    DWORD a = GetFileAttributesW(WPATH(path));
    if (a == INVALID_FILE_ATTRIBUTES) return fail_win();
    if ((mode & W_OK) && (a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_DIRECTORY)) { errno = EACCES; return -1; }
    return 0;
}
int dm_win_chdir(const char *path) { return SetCurrentDirectoryW(WPATH(path)) ? 0 : fail_win(); }
int dm_win_chmod(const char *p, int mode) {
    wchar_t *w = WPATH(p); DWORD a = GetFileAttributesW(w); if (a == INVALID_FILE_ATTRIBUTES) return fail_win();
    a = (mode & 0200) ? a & ~FILE_ATTRIBUTE_READONLY : a | FILE_ATTRIBUTE_READONLY;
    return SetFileAttributesW(w, a) ? 0 : fail_win();
}
int fchmod(int fd, int mode) { (void)fd; (void)mode; return 0; }
int fchown(int fd, uid_t u, gid_t g) { (void)fd; (void)u; (void)g; return 0; }
int chown(const char *p, uid_t u, gid_t g) { (void)p; (void)u; (void)g; return 0; }
// Times: FILETIME is 100 ns since 1601
#define FT_EPOCH 116444736000000000ull
static struct timespec ts_of_ft(FILETIME f) { uint64_t t = ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; t = t > FT_EPOCH ? t - FT_EPOCH : 0; return (struct timespec){(time_t)(t / 10000000ull), (long)(t % 10000000ull * 100)}; }
static FILETIME ft_of_ts(struct timespec t) { uint64_t v = (uint64_t)t.tv_sec * 10000000ull + (uint64_t)t.tv_nsec / 100 + FT_EPOCH; return (FILETIME){(DWORD)v, (DWORD)(v >> 32)}; }
static int set_times(HANDLE h, const struct timespec t[2]) {
    FILETIME now; GetSystemTimeAsFileTime(&now); FILETIME a, m, *pa = &a, *pm = &m;
    if (!t) { a = m = now; }
    else {
        if (t[0].tv_nsec == UTIME_OMIT) pa = NULL; else a = t[0].tv_nsec == UTIME_NOW ? now : ft_of_ts(t[0]);
        if (t[1].tv_nsec == UTIME_OMIT) pm = NULL; else m = t[1].tv_nsec == UTIME_NOW ? now : ft_of_ts(t[1]);
    }
    return SetFileTime(h, NULL, pa, pm) ? 0 : fail_win();
}
int futimens(int fd, const struct timespec t[2]) { HANDLE h = (HANDLE)_get_osfhandle(fd); if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; } return set_times(h, t); }
static const char *at_path(int dirfd, const char *path, char *buf, size_t n) {
    if (dirfd == AT_FDCWD || !path || path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':')) return path;
    const char *d = dirfd_path(dirfd); if (!d) return NULL;
    snprintf(buf, n, "%s/%s", d, path); return buf;
}
int utimensat(int dirfd, const char *path, const struct timespec t[2], int flags) {
    char b[PATH_MAX * 2]; const char *p = at_path(dirfd, path, b, sizeof b); if (!p) { errno = EBADF; return -1; }
    HANDLE h = CreateFileW(WPATH(p), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | ((flags & AT_SYMLINK_NOFOLLOW) ? FILE_FLAG_OPEN_REPARSE_POINT : 0), NULL);
    if (h == INVALID_HANDLE_VALUE) return fail_win();
    int r = set_times(h, t); CloseHandle(h); return r;
}
int utimes(const char *path, const struct timeval t[2]) {
    if (!t) return utimensat(AT_FDCWD, path, NULL, 0);
    struct timespec ts[2] = {{t[0].tv_sec, t[0].tv_usec * 1000}, {t[1].tv_sec, t[1].tv_usec * 1000}};
    return utimensat(AT_FDCWD, path, ts, 0);
}
// stat from a handle's information; a path is opened without following a final link only for lstat
static int stat_handle(HANDLE h, struct dm_stat *st) {
    BY_HANDLE_FILE_INFORMATION fi; if (!GetFileInformationByHandle(h, &fi)) {
        DWORD t = GetFileType(h);   // a console, a pipe: no file information
        if (t == FILE_TYPE_CHAR || t == FILE_TYPE_PIPE) { memset(st, 0, sizeof *st); st->st_mode = t == FILE_TYPE_CHAR ? _S_IFCHR | 0666 : _S_IFIFO | 0666; st->st_nlink = 1; st->st_blksize = 4096; return 0; }
        return fail_win();
    }
    memset(st, 0, sizeof *st);
    st->st_dev = fi.dwVolumeSerialNumber; st->st_ino = ((uint64_t)fi.nFileIndexHigh << 32) | fi.nFileIndexLow; st->st_nlink = fi.nNumberOfLinks;
    st->st_size = (int64_t)(((uint64_t)fi.nFileSizeHigh << 32) | fi.nFileSizeLow);
    int dir = fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    st->st_mode = (dir ? _S_IFDIR | 0755 : _S_IFREG | ((fi.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644));
    if (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        FILE_ATTRIBUTE_TAG_INFO tg; if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tg, sizeof tg) && tg.ReparseTag == IO_REPARSE_TAG_SYMLINK) st->st_mode = S_IFLNK | 0777;
    }
    st->st_blksize = 4096; st->st_blocks = (st->st_size + 511) / 512;
    st->st_atimespec = ts_of_ft(fi.ftLastAccessTime); st->st_mtimespec = ts_of_ft(fi.ftLastWriteTime);
    st->st_ctimespec = st->st_mtimespec; st->st_birthtimespec = ts_of_ft(fi.ftCreationTime);
    FILE_BASIC_INFO bi; if (GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof bi)) { FILETIME c = {(DWORD)bi.ChangeTime.QuadPart, (DWORD)(bi.ChangeTime.QuadPart >> 32)}; st->st_ctimespec = ts_of_ft(c); }
    return 0;
}
static int stat_path(const char *path, struct dm_stat *st, int nofollow) {
    if (!path || !*path) { errno = ENOENT; return -1; }
    HANDLE h = CreateFileW(WPATH(path), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0), NULL);
    if (h == INVALID_HANDLE_VALUE) return fail_win();
    int r = stat_handle(h, st); CloseHandle(h); return r;
}
int dm_stat(const char *path, struct dm_stat *st) { return stat_path(path, st, 0); }
int dm_lstat(const char *path, struct dm_stat *st) { return stat_path(path, st, 1); }
int dm_fstat(int fd, struct dm_stat *st) { HANDLE h = (HANDLE)_get_osfhandle(fd); if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; } return stat_handle(h, st); }
int dm_fstatat(int dirfd, const char *path, struct dm_stat *st, int flags) {
    char b[PATH_MAX * 2]; const char *p = at_path(dirfd, path, b, sizeof b); if (!p) { errno = EBADF; return -1; }
    return stat_path(p, st, flags & AT_SYMLINK_NOFOLLOW);
}
ssize_t readlink(const char *path, char *buf, size_t n) {
    HANDLE h = CreateFileW(WPATH(path), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return fail_win();
    wchar_t w[WP]; DWORD k = GetFinalPathNameByHandleW(h, w, WP, FILE_NAME_NORMALIZED); CloseHandle(h);
    if (!k || k >= WP) { errno = EINVAL; return -1; }
    char u[WP * 3]; utf8_of(wcsncmp(w, L"\\\\?\\", 4) ? w : w + 4, u, sizeof u);
    size_t l = strlen(u); if (l > n) l = n; memcpy(buf, u, l); return (ssize_t)l;
}
int symlink(const char *target, const char *path) {
    DWORD a = GetFileAttributesW(WPATH(target));
    if (CreateSymbolicLinkW(WPATH2(path), WPATH(target), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0))) return 0;
    return fail_win();
}
int link(const char *target, const char *path) { return CreateHardLinkW(WPATH2(path), WPATH(target), NULL) ? 0 : fail_win(); }
char *realpath(const char *path, char *resolved) {
    wchar_t w[WP]; DWORD k = GetFullPathNameW(WPATH(path), WP, w, NULL);
    if (!k || k >= WP) { errno = ENAMETOOLONG; return NULL; }
    if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) { fail_win(); return NULL; }
    char *out = resolved ? resolved : malloc(PATH_MAX);
    if (!out) { errno = ENOMEM; return NULL; }
    if (utf8_of(w, out, PATH_MAX)) { if (!resolved) free(out); errno = ENAMETOOLONG; return NULL; }
    return out;
}
int dm_android_fcntl(int fd, int cmd, ...) {
    va_list ap; va_start(ap, cmd); intptr_t arg = va_arg(ap, intptr_t); va_end(ap);
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    switch (cmd) {
    case F_GETPATH: {
        if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
        wchar_t w[WP]; DWORD k = GetFinalPathNameByHandleW(h, w, WP, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!k || k >= WP) return fail_win();
        return utf8_of(wcsncmp(w, L"\\\\?\\", 4) ? w : w + 4, (char *)arg, PATH_MAX) ? (errno = ENAMETOOLONG, -1) : 0;
    }
    case F_FULLFSYNC: return fsync(fd);
    case F_DUPFD: case F_DUPFD_CLOEXEC: { int n = _dup(fd); if (n >= 0) { const char *d = dirfd_path(fd); if (d) dirfd_set(n, d); } return n; }
    case F_GETFD: return h == INVALID_HANDLE_VALUE ? (errno = EBADF, -1) : 0;
    case F_GETFL: return h == INVALID_HANDLE_VALUE ? (errno = EBADF, -1) : O_RDWR;
    case F_SETFD: case F_SETFL: case F_NOCACHE: case F_RDAHEAD: case F_PREALLOCATE: return 0;
    case F_SETLK: case F_SETLKW: case F_GETLK: {
        struct flock *l = (struct flock *)arg;
        if (cmd == F_GETLK) { l->l_type = F_UNLCK; return 0; }
        OVERLAPPED o = {0}; o.Offset = (DWORD)l->l_start; o.OffsetHigh = (DWORD)((uint64_t)l->l_start >> 32);
        DWORD lo = l->l_len ? (DWORD)l->l_len : MAXDWORD, hi = l->l_len ? (DWORD)((uint64_t)l->l_len >> 32) : MAXDWORD;
        if (l->l_type == F_UNLCK) { UnlockFileEx(h, 0, lo, hi, &o); return 0; }
        DWORD fl = (l->l_type == F_WRLCK ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (cmd == F_SETLK ? LOCKFILE_FAIL_IMMEDIATELY : 0);
        return LockFileEx(h, fl, 0, lo, hi, &o) ? 0 : (errno = EAGAIN, -1);
    }
    default: errno = EINVAL; return -1;
    }
}
int flock(int fd, int op) {
    HANDLE h = (HANDLE)_get_osfhandle(fd); if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED o = {0};
    if (op & LOCK_UN) { UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &o); return 0; }
    DWORD fl = ((op & LOCK_EX) ? LOCKFILE_EXCLUSIVE_LOCK : 0) | ((op & LOCK_NB) ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    return LockFileEx(h, fl, 0, MAXDWORD, MAXDWORD, &o) ? 0 : (errno = EWOULDBLOCK, -1);
}
static int vol_space(const char *path, uint64_t *total, uint64_t *avail, uint64_t *fr) {
    ULARGE_INTEGER a, t, f;
    if (!GetDiskFreeSpaceExW(WPATH(path), &a, &t, &f)) return fail_win();
    *total = t.QuadPart; *avail = a.QuadPart; *fr = f.QuadPart; return 0;
}
int statfs(const char *path, struct statfs *s) {
    uint64_t t, a, f; if (vol_space(path, &t, &a, &f)) return -1;
    memset(s, 0, sizeof *s); s->f_bsize = 4096; s->f_blocks = t / 4096; s->f_bavail = a / 4096; s->f_bfree = f / 4096; s->f_files = 1u << 30; s->f_ffree = 1u << 29;
    strcpy(s->f_fstypename, "ntfs"); return 0;
}
int statvfs(const char *path, struct statvfs *s) {
    uint64_t t, a, f; if (vol_space(path, &t, &a, &f)) return -1;
    memset(s, 0, sizeof *s); s->f_bsize = s->f_frsize = 4096; s->f_blocks = t / 4096; s->f_bavail = a / 4096; s->f_bfree = f / 4096;
    s->f_files = 1u << 30; s->f_ffree = s->f_favail = 1u << 29; s->f_namemax = 255; return 0;
}
int fallocate(int fd, int mode, off_t off, off_t len) { (void)fd; (void)mode; (void)off; (void)len; errno = ENOSYS; return -1; }
int posix_fallocate(int fd, off_t off, off_t len) {
    struct dm_stat st; if (dm_fstat(fd, &st)) return errno;
    if (st.st_size >= off + len) return 0;
    return dm_win_ftruncate(fd, off + len) ? errno : 0;
}
int sync_file_range(int fd, off_t off, off_t n, unsigned flags) { (void)fd; (void)off; (void)n; (void)flags; errno = ENOSYS; return -1; }
intptr_t syscall(long n, ...) { (void)n; errno = ENOSYS; return -1; }
int ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; errno = ENOTTY; return -1; }
int pipe(int fd[2]) { return _pipe(fd, 65536, _O_BINARY); }

// ---------------- Extended attributes: NTFS alternate data streams "<path>:dm.<name>" ----------------
static wchar_t *xattr_path(const char *p, const char *n) {
    char b[PATH_MAX * 2]; snprintf(b, sizeof b, "%s:dm.%s", p, n);
    for (char *c = b + strlen(p) + 4; *c; c++) if (*c == ':' || *c == '/' || *c == '\\') *c = '_';
    return WPATH(b);
}
ssize_t dm_android_getxattr(const char *p, const char *n, void *v, size_t s, uint32_t pos, int opt) {
    (void)pos; (void)opt;
    HANDLE h = CreateFileW(xattr_path(p, n), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); errno = e == ERROR_FILE_NOT_FOUND ? ENOATTR : err_of(e); return -1; }
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if (!v || !s) { CloseHandle(h); return (ssize_t)sz.QuadPart; }
    if ((uint64_t)sz.QuadPart > s) { CloseHandle(h); errno = ERANGE; return -1; }
    DWORD got = 0; BOOL ok = ReadFile(h, v, (DWORD)sz.QuadPart, &got, NULL); CloseHandle(h);
    return ok ? (ssize_t)got : fail_win();
}
int dm_android_setxattr(const char *p, const char *n, const void *v, size_t s, uint32_t pos, int opt) {
    (void)pos; (void)opt;
    HANDLE h = CreateFileW(xattr_path(p, n), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); errno = e == ERROR_INVALID_NAME || e == ERROR_INVALID_PARAMETER ? ENOTSUP : err_of(e); return -1; }   // FAT / exFAT: no streams
    DWORD put = 0; BOOL ok = WriteFile(h, v, (DWORD)s, &put, NULL); CloseHandle(h);
    return ok && put == s ? 0 : fail_win();
}
int dm_android_removexattr(const char *p, const char *n, int opt) {
    (void)opt; if (DeleteFileW(xattr_path(p, n))) return 0;
    DWORD e = GetLastError(); errno = e == ERROR_FILE_NOT_FOUND ? ENOATTR : err_of(e); return -1;
}
ssize_t dm_android_listxattr(const char *p, char *list, size_t s, int opt) {
    (void)opt; WIN32_FIND_STREAM_DATA fd; size_t used = 0;
    HANDLE h = FindFirstStreamW(WPATH(p), FindStreamInfoStandard, &fd, 0);
    if (h == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); if (e == ERROR_HANDLE_EOF) return 0; return fail_win(); }
    do {
        // ":dm.<name>:$DATA"
        if (wcsncmp(fd.cStreamName, L":dm.", 4)) continue;
        char u[MAX_PATH * 3]; wchar_t *e = wcsrchr(fd.cStreamName + 1, L':'); if (e) *e = 0;
        WideCharToMultiByte(CP_UTF8, 0, fd.cStreamName + 4, -1, u, sizeof u, NULL, NULL);
        size_t l = strlen(u) + 1;
        if (list && s) { if (used + l > s) { FindClose(h); errno = ERANGE; return -1; } memcpy(list + used, u, l); }
        used += l;
    } while (FindNextStreamW(h, &fd));
    FindClose(h);
    return (ssize_t)used;
}

// ---------------- Directories ----------------
struct dm_dir { HANDLE h; int first, fd; WIN32_FIND_DATAW fd_w; wchar_t pat[WP]; struct dm_dirent e; char *path; };
DM_DIR *dm_win_opendir(const char *path) {
    DM_DIR *d = calloc(1, sizeof *d); if (!d) { errno = ENOMEM; return NULL; }
    wchar_t *w = WPATH(path); size_t l = wcslen(w);
    if (l + 3 >= WP) { free(d); errno = ENAMETOOLONG; return NULL; }
    wcscpy(d->pat, w); if (l && d->pat[l - 1] != L'\\') d->pat[l++] = L'\\'; d->pat[l++] = L'*'; d->pat[l] = 0;
    d->h = FindFirstFileExW(d->pat, FindExInfoBasic, &d->fd_w, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (d->h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND) { free(d); SetLastError(e); if (e == ERROR_PATH_NOT_FOUND || e == ERROR_DIRECTORY) { DWORD a = GetFileAttributesW(w); errno = a != INVALID_FILE_ATTRIBUTES ? ENOTDIR : ENOENT; return NULL; } fail_win(); return NULL; }
    }
    d->first = 1; d->fd = -1; d->path = _strdup(path);
    return d;
}
struct dm_dirent *dm_win_readdir(DM_DIR *d) {
    if (d->h == INVALID_HANDLE_VALUE) return NULL;
    if (!d->first && !FindNextFileW(d->h, &d->fd_w)) return NULL;
    d->first = 0;
    memset(&d->e, 0, sizeof d->e);
    WideCharToMultiByte(CP_UTF8, 0, d->fd_w.cFileName, -1, d->e.d_name, sizeof d->e.d_name, NULL, NULL);
    d->e.d_namlen = (unsigned short)strlen(d->e.d_name); d->e.d_reclen = sizeof d->e; d->e.d_ino = 1;
    DWORD a = d->fd_w.dwFileAttributes;
    d->e.d_type = (a & FILE_ATTRIBUTE_REPARSE_POINT) && d->fd_w.dwReserved0 == IO_REPARSE_TAG_SYMLINK ? DT_LNK : (a & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->e;
}
int dm_win_closedir(DM_DIR *d) {
    if (!d) return 0;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    if (d->fd >= 0) dm_win_close(d->fd);
    free(d->path); free(d); return 0;
}
void dm_win_rewinddir(DM_DIR *d) {
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    d->h = FindFirstFileExW(d->pat, FindExInfoBasic, &d->fd_w, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH); d->first = 1;
}
int dm_win_dirfd(DM_DIR *d) { if (d->fd < 0) d->fd = dm_win_open(d->path, O_RDONLY | O_DIRECTORY); return d->fd; }
DM_DIR *dm_win_fdopendir(int fd) {
    const char *p = dirfd_path(fd); if (!p) { errno = ENOTDIR; return NULL; }
    DM_DIR *d = dm_win_opendir(p); if (d) d->fd = fd;   // closedir closes fd, as POSIX's
    return d;
}
int alphasort(const struct dm_dirent **a, const struct dm_dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }
int scandir(const char *dir, struct dm_dirent ***list, int (*sel)(const struct dm_dirent *), int (*cmp)(const struct dm_dirent **, const struct dm_dirent **)) {
    DM_DIR *d = dm_win_opendir(dir); if (!d) return -1;
    struct dm_dirent **v = NULL, *e; int n = 0, c = 0;
    while ((e = dm_win_readdir(d))) {
        if (sel && !sel(e)) continue;
        if (n == c) { c = c ? 2 * c : 32; struct dm_dirent **nv = realloc(v, (size_t)c * sizeof *nv); if (!nv) break; v = nv; }
        v[n] = malloc(sizeof *e); if (!v[n]) break; *v[n++] = *e;
    }
    dm_win_closedir(d);
    if (cmp && n) qsort(v, (size_t)n, sizeof *v, (int (*)(const void *, const void *))cmp);
    *list = v; return n;
}

// ---------------- Process and system ----------------
long sysconf(int name) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    switch (name) {
    case _SC_PAGESIZE: return 4096;
    case _SC_NPROCESSORS_ONLN: case _SC_NPROCESSORS_CONF: return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    case _SC_OPEN_MAX: return 8192;
    case _SC_PHYS_PAGES: { MEMORYSTATUSEX m = {.dwLength = sizeof m}; GlobalMemoryStatusEx(&m); return (long)(m.ullTotalPhys / 4096); }
    case _SC_CLK_TCK: return 100;
    default: errno = EINVAL; return -1;
    }
}
int getpagesize(void) { return 4096; }
pid_t gettid(void) { return (pid_t)GetCurrentThreadId(); }
mach_port_t dm_win_thread_id(void) { return (mach_port_t)GetCurrentThreadId(); }
mach_port_t dm_win_pthread_tid(pthread_t t) { HANDLE h = pthread_gethandle(t); return h ? (mach_port_t)GetThreadId(h) : 0; }
int sched_getcpu(void) { PROCESSOR_NUMBER p; GetCurrentProcessorNumberEx(&p); return p.Group * 64 + p.Number; }
uid_t getuid(void) { return 1000; }
uid_t geteuid(void) { return 1000; }
gid_t getgid(void) { return 1000; }
pid_t getppid(void) { return 1; }
int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? (errno = ENOMEM, -1) : 0;
}
int unsetenv(const char *name) { return _putenv_s(name, "") ? -1 : 0; }
int setpriority(int which, id_t who, int prio) {
    (void)which;
    HANDLE h = who ? OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, who) : GetCurrentThread();
    if (!h) return fail_win();
    int p = prio <= -15 ? THREAD_PRIORITY_TIME_CRITICAL : prio <= -8 ? THREAD_PRIORITY_HIGHEST : prio < 0 ? THREAD_PRIORITY_ABOVE_NORMAL : prio == 0 ? THREAD_PRIORITY_NORMAL : prio < 10 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
    BOOL ok = SetThreadPriority(h, p); if (who) CloseHandle(h);
    return ok ? 0 : fail_win();
}
int getpriority(int which, id_t who) {
    (void)which; HANDLE h = who ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, who) : GetCurrentThread(); if (!h) return 0;
    int p = GetThreadPriority(h); if (who) CloseHandle(h);
    return p >= THREAD_PRIORITY_TIME_CRITICAL ? -20 : p >= THREAD_PRIORITY_HIGHEST ? -10 : p > 0 ? -5 : p == 0 ? 0 : p > THREAD_PRIORITY_LOWEST ? 5 : 10;
}
int getrlimit(int r, struct rlimit *l) { (void)r; l->rlim_cur = l->rlim_max = RLIM_INFINITY; return 0; }
int setrlimit(int r, const struct rlimit *l) { (void)r; (void)l; return 0; }
int getrusage(int who, struct rusage *u) {
    (void)who; memset(u, 0, sizeof *u);
    FILETIME c, e, k, us; if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &us)) {
        uint64_t uk = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) / 10, uu = (((uint64_t)us.dwHighDateTime << 32) | us.dwLowDateTime) / 10;
        u->ru_utime.tv_sec = (long)(uu / 1000000); u->ru_utime.tv_usec = (long)(uu % 1000000); u->ru_stime.tv_sec = (long)(uk / 1000000); u->ru_stime.tv_usec = (long)(uk % 1000000);
    }
    PROCESS_MEMORY_COUNTERS pm; if (GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm)) { u->ru_maxrss = (long)(pm.PeakWorkingSetSize / 1024); u->ru_majflt = (long)pm.PageFaultCount; }
    return 0;
}
// Child processes: not run on Windows (the shader translator runs ahead of time, in the packaging script)
int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at, char *const argv[], char *const envp[]) {
    (void)pid; (void)path; (void)fa; (void)at; (void)argv; (void)envp; return ENOSYS;
}
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at, char *const argv[], char *const envp[]) {
    return posix_spawn(pid, file, fa, at, argv, envp);
}
pid_t waitpid(pid_t pid, int *status, int options) { (void)pid; (void)status; (void)options; errno = ECHILD; return -1; }
pid_t wait(int *status) { return waitpid(-1, status, 0); }
// dlopen / dlsym: LoadLibrary / GetProcAddress (RTLD_DEFAULT looks in the host and the modules it has loaded)
static __thread char t_dlerr[128];
void *dlopen(const char *path, int flags) {
    (void)flags; if (!path) return GetModuleHandleW(NULL);
    HMODULE m = LoadLibraryExW(WPATH(path), NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!m) m = LoadLibraryW(WPATH(path));
    if (!m) snprintf(t_dlerr, sizeof t_dlerr, "LoadLibrary %s: error %lu", path, (unsigned long)GetLastError());
    return m;
}
void *dlsym(void *h, const char *name) {
    void *p = NULL;
    if (h == RTLD_DEFAULT || h == RTLD_NEXT) {
        HMODULE mods[512]; DWORD need = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) for (DWORD i = 0; i < need / sizeof *mods && i < 512 && !p; i++) p = (void *)GetProcAddress(mods[i], name);
    } else p = (void *)GetProcAddress((HMODULE)h, name);
    if (!p) snprintf(t_dlerr, sizeof t_dlerr, "no symbol %s", name);
    return p;
}
int dlclose(void *h) { return h && h != GetModuleHandleW(NULL) && !FreeLibrary(h); }
const char *dlerror(void) { if (!t_dlerr[0]) return NULL; static __thread char e[128]; memcpy(e, t_dlerr, sizeof e); t_dlerr[0] = 0; return e; }
int dladdr(const void *addr, Dl_info *info) {
    HMODULE m; static __thread char name[MAX_PATH * 3];
    memset(info, 0, sizeof *info);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m)) return 0;
    wchar_t w[MAX_PATH]; if (GetModuleFileNameW(m, w, MAX_PATH)) utf8_of(w, name, sizeof name);
    info->dli_fname = name; info->dli_fbase = m;
    return 1;
}

// ---------------- Sockets: none (sys/socket.h) ----------------
#define NOSOCK do { errno = EAFNOSUPPORT; return -1; } while (0)
int socket(int d, int t, int p) { (void)d; (void)t; (void)p; NOSOCK; }
int socketpair(int d, int t, int p, int sv[2]) { (void)d; (void)t; (void)p; (void)sv; NOSOCK; }
int bind(int s, const struct sockaddr *a, socklen_t n) { (void)s; (void)a; (void)n; NOSOCK; }
int connect(int s, const struct sockaddr *a, socklen_t n) { (void)s; (void)a; (void)n; NOSOCK; }
int listen(int s, int n) { (void)s; (void)n; NOSOCK; }
int accept(int s, struct sockaddr *a, socklen_t *n) { (void)s; (void)a; (void)n; NOSOCK; }
ssize_t send(int s, const void *b, size_t n, int f) { (void)s; (void)b; (void)n; (void)f; NOSOCK; }
ssize_t recv(int s, void *b, size_t n, int f) { (void)s; (void)b; (void)n; (void)f; NOSOCK; }
ssize_t sendto(int s, const void *b, size_t n, int f, const struct sockaddr *a, socklen_t an) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)an; NOSOCK; }
ssize_t recvfrom(int s, void *b, size_t n, int f, struct sockaddr *a, socklen_t *an) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)an; NOSOCK; }
ssize_t sendmsg(int s, const struct msghdr *m, int f) { (void)s; (void)m; (void)f; NOSOCK; }
ssize_t recvmsg(int s, struct msghdr *m, int f) { (void)s; (void)m; (void)f; NOSOCK; }
int setsockopt(int s, int l, int o, const void *v, socklen_t n) { (void)s; (void)l; (void)o; (void)v; (void)n; NOSOCK; }
int getsockopt(int s, int l, int o, void *v, socklen_t *n) { (void)s; (void)l; (void)o; (void)v; (void)n; NOSOCK; }
int getsockname(int s, struct sockaddr *a, socklen_t *n) { (void)s; (void)a; (void)n; NOSOCK; }
int getpeername(int s, struct sockaddr *a, socklen_t *n) { (void)s; (void)a; (void)n; NOSOCK; }
int shutdown(int s, int how) { (void)s; (void)how; NOSOCK; }
int poll(struct pollfd *f, nfds_t n, int timeout) {
    for (nfds_t i = 0; i < n; i++) f[i].revents = f[i].fd < 0 ? 0 : POLLNVAL;
    if (n == 0 && timeout > 0) Sleep((DWORD)timeout);
    int c = 0; for (nfds_t i = 0; i < n; i++) c += f[i].revents != 0; return c;
}

// ---------------- Strings and streams ----------------
char *strndup(const char *s, size_t n) { size_t l = strnlen(s, n); char *d = malloc(l + 1); if (d) { memcpy(d, s, l); d[l] = 0; } return d; }
char *strsep(char **s, const char *delim) { char *b = *s; if (!b) return NULL; char *e = b + strcspn(b, delim); if (*e) { *e = 0; *s = e + 1; } else *s = NULL; return b; }
char *strcasestr(const char *h, const char *n) { size_t l = strlen(n); for (; *h; h++) if (!_strnicmp(h, n, l)) return (char *)h; return l ? NULL : (char *)h; }
void *memmem(const void *h, size_t hn, const void *n, size_t nn) {
    if (!nn) return (void *)h; if (hn < nn) return NULL;
    const uint8_t *p = h, *e = p + hn - nn + 1;
    for (; p < e; p++) { p = memchr(p, *(const uint8_t *)n, (size_t)(e - p)); if (!p) return NULL; if (!memcmp(p, n, nn)) return (void *)p; }
    return NULL;
}
void *memrchr(const void *s, int c, size_t n) { const uint8_t *p = (const uint8_t *)s + n; while (n--) if (*--p == (uint8_t)c) return (void *)p; return NULL; }
char *stpcpy(char *d, const char *s) { size_t l = strlen(s); memcpy(d, s, l + 1); return d + l; }
size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t k = l < n - 1 ? l : n - 1; memcpy(d, s, k); d[k] = 0; } return l; }
size_t strlcat(char *d, const char *s, size_t n) { size_t dl = strnlen(d, n); if (dl == n) return n + strlen(s); return dl + strlcpy(d + dl, s, n - dl); }
ssize_t getdelim(char **line, size_t *n, int delim, FILE *f) {
    if (!*line || !*n) { *n = 128; *line = malloc(*n); if (!*line) return -1; }
    size_t k = 0; int c;
    while ((c = fgetc(f)) != EOF) {
        if (k + 2 > *n) { size_t nn = *n * 2; char *nl = realloc(*line, nn); if (!nl) return -1; *line = nl; *n = nn; }
        (*line)[k++] = (char)c; if (c == delim) break;
    }
    if (!k && c == EOF) return -1;
    (*line)[k] = 0; return (ssize_t)k;
}
ssize_t getline(char **line, size_t *n, FILE *f) { return getdelim(line, n, '\n', f); }
int vasprintf(char **out, const char *fmt, va_list ap) {
    va_list c; va_copy(c, ap); int n = vsnprintf(NULL, 0, fmt, c); va_end(c);
    if (n < 0 || !(*out = malloc((size_t)n + 1))) { *out = NULL; return -1; }
    return vsnprintf(*out, (size_t)n + 1, fmt, ap);
}
int asprintf(char **out, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vasprintf(out, fmt, ap); va_end(ap); return n; }
int dprintf(int fd, const char *fmt, ...) {
    char b[1024], *p = b; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n < 0) return n;
    if ((size_t)n >= sizeof b) { p = malloc((size_t)n + 1); if (!p) return -1; va_start(ap, fmt); vsnprintf(p, (size_t)n + 1, fmt, ap); va_end(ap); }
    int r = _write(fd, p, (unsigned)n); if (p != b) free(p); return r;
}
static void rand_name(char *x) { static const char a[] = "abcdefghijklmnopqrstuvwxyz0123456789"; uint8_t r[6]; arc4random_buf(r, 6); for (int i = 0; i < 6; i++) x[i] = a[r[i] % 36]; }
char *mkdtemp(char *tmpl) {
    size_t l = strlen(tmpl); if (l < 6 || strcmp(tmpl + l - 6, "XXXXXX")) { errno = EINVAL; return NULL; }
    for (int t = 0; t < 100; t++) { rand_name(tmpl + l - 6); if (!dm_win_mkdir(tmpl, 0700)) return tmpl; if (errno != EEXIST) return NULL; }
    return NULL;
}
int mkostemp(char *tmpl, int flags) {
    size_t l = strlen(tmpl); if (l < 6 || strcmp(tmpl + l - 6, "XXXXXX")) { errno = EINVAL; return -1; }
    for (int t = 0; t < 100; t++) { rand_name(tmpl + l - 6); int fd = dm_win_open(tmpl, O_RDWR | O_CREAT | O_EXCL | (flags & O_APPEND)); if (fd >= 0 || errno != EEXIST) return fd; }
    return -1;
}
int mkstemp(char *tmpl) { return mkostemp(tmpl, 0); }
void arc4random_buf(void *buf, size_t n) { BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG); }
uint32_t arc4random(void) { uint32_t v; arc4random_buf(&v, sizeof v); return v; }
uint32_t arc4random_uniform(uint32_t n) { if (n < 2) return 0; uint32_t m = -n % n, v; do v = arc4random(); while (v < m); return v % n; }

// ---------------- Aligned blocks freed with free() ----------------
// posix_memalign: a block from malloc with room for the alignment and a header just below the aligned pointer {raw block, a check word, size}; free / realloc /
// malloc_usable_size see the header's check word (the pointer's own value mixed with a constant) and take the raw block. A plain malloc block has heap metadata there
#define AL_MAGIC 0x6d64616c69676e21ull
typedef struct { void *raw; uint64_t check; size_t n; } al_hdr;
static al_hdr *al_of(void *p) { if (!p || ((uintptr_t)p & 15)) return NULL; al_hdr *h = (al_hdr *)p - 1; return h->check == (AL_MAGIC ^ (uintptr_t)p) ? h : NULL; }
int posix_memalign(void **out, size_t align, size_t n) {
    if (align < sizeof(void *) || (align & (align - 1))) return EINVAL;
    if (align < 32) align = 32;   // room for the header in front at any alignment
    uint8_t *raw = malloc(n + align + sizeof(al_hdr)); if (!raw) return ENOMEM;
    uintptr_t p = ((uintptr_t)raw + sizeof(al_hdr) + align - 1) & ~(uintptr_t)(align - 1);
    al_hdr *h = (al_hdr *)p - 1; h->raw = raw; h->check = AL_MAGIC ^ p; h->n = n;
    *out = (void *)p; return 0;
}
void *aligned_alloc_dm(size_t align, size_t n) { void *p; return posix_memalign(&p, align, n) ? NULL : p; }
void dm_win_free(void *p) { al_hdr *h = al_of(p); if (h) { h->check = 0; free(h->raw); } else free(p); }
void *dm_win_realloc(void *p, size_t n) {
    al_hdr *h = al_of(p); if (!h) return realloc(p, n);
    void *q = malloc(n ? n : 1); if (!q) return NULL;
    memcpy(q, p, h->n < n ? h->n : n); dm_win_free(p); return q;
}
size_t malloc_usable_size(void *p) { al_hdr *h = al_of(p); return !p ? 0 : h ? h->n : _msize(p); }

// ---------------- pthread extras ----------------
// pthread_cond_timedwait_relative_np: winpthreads'
int pthread_getcpuclockid_dm(pthread_t t, clockid_t *c) { (void)t; *c = CLOCK_THREAD_CPUTIME_ID; return 0; }
char *strptime(const char *s, const char *fmt, struct tm *tm) {   // the %Y-%m-%d %H:%M:%S subset the host parses
    for (; *fmt && *s; fmt++) {
        if (*fmt != '%') { if (*fmt != *s) return NULL; s++; continue; }
        fmt++; char *e; long v = strtol(s, &e, 10); if (e == s) return NULL;
        switch (*fmt) { case 'Y': tm->tm_year = (int)v - 1900; break; case 'm': tm->tm_mon = (int)v - 1; break; case 'd': tm->tm_mday = (int)v; break;
                        case 'H': tm->tm_hour = (int)v; break; case 'M': tm->tm_min = (int)v; break; case 'S': tm->tm_sec = (int)v; break; default: return NULL; }
        s = e;
    }
    return (char *)s;
}
