// Windows on Arm (arm64, llvm-mingw's UCRT and winpthreads) stand-ins for the Darwin and POSIX interfaces the host uses. Force-included into every host source built
// for Windows (CMakeLists.txt: -include dm_windows.h), as dm/android/compat/dm_android.h is on Android, so the shared sources keep their Apple and POSIX calls.
// The Apple header names (mach/mach.h, dispatch/dispatch.h, ...) are the Android port's small files, copied beside this one; the POSIX headers MinGW lacks
// (sys/mman.h, sys/xattr.h, dlfcn.h, ...) are here too. Implementations: posix_windows.c (memory, signals, files, processes), plat_windows.c (the Darwin parts).
// This header never includes <windows.h>: its macros (near, far, small, interface, ERROR, ...) would collide with host identifiers. The .c files that call Win32 include it.
// Data model: Windows is LLP64 (long is 32 bits), where Apple and Android are LP64. Host code that keeps addresses or 64-bit sizes in long is fixed at the source
// (uintptr_t / int64_t / long long); calls the guest makes straight into the C library (hle_table.c) go to 64-bit wrappers where the guest's long is 64 bits.
#pragma once
#ifndef _WIN32
#error "dm_windows.h is for Windows builds only"
#endif
#ifndef DM_WINDOWS
#define DM_WINDOWS 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <direct.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>

#define TARGET_OS_IPHONE 0
#define TARGET_OS_OSX 0
#define TARGET_OS_MAC 0
#define TARGET_OS_ANDROID 0
#define TARGET_OS_WINDOWS 1

typedef int boolean_t;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

// ---- POSIX types and calls the UCRT lacks (posix_windows.c) ----
typedef unsigned int uid_t, gid_t;
typedef int64_t blkcnt_t;
typedef uint32_t id_t;
typedef unsigned int nlink_t_;
typedef int clockid_t_dm;
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
// Files are binary: the host reads and writes bytes (O_BINARY is added to every open, fopen's "b" is implied by _fmode = _O_BINARY, set at start in posix_windows.c)
int dm_win_open(const char *path, int flags, ...);
#define open(...) dm_win_open(__VA_ARGS__)
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x200000   // posix_windows.c: opens a directory (a handle for fsync / F_GETPATH; reads fail)
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_SYNC
#define O_SYNC 0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif
#define O_EVTONLY O_RDONLY
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int fsync(int fd);
int fdatasync(int fd);
// mkdir with a mode (MinGW's takes the path alone); the mode is ignored, as Windows has no permission bits
// Linux file calls of the Android port's opt-in pool file backing (gfx_vk_fileback.inc): they fail (ENOSYS / EINVAL), so the backing is refused and the pool stays as it was
#define O_TMPFILE 0x4000000
#define FALLOC_FL_KEEP_SIZE 1
#define FALLOC_FL_PUNCH_HOLE 2
#define SYNC_FILE_RANGE_WRITE 2
#define SEEK_DATA 3
#define SEEK_HOLE 4
int fallocate(int fd, int mode, off_t off, off_t len);
int posix_fallocate(int fd, off_t off, off_t len);
int sync_file_range(int fd, off_t off, off_t n, unsigned flags);
int mkostemp(char *tmpl, int flags);
int dm_win_mkdir(const char *path, int mode);
#define mkdir(p, ...) dm_win_mkdir((p), 0)
ssize_t readlink(const char *path, char *buf, size_t n);
int symlink(const char *target, const char *path);
int link(const char *target, const char *path);
char *realpath(const char *path, char *resolved);
int dm_win_rename(const char *from, const char *to);   // replaces an existing file, as POSIX rename does (MoveFileEx MOVEFILE_REPLACE_EXISTING)
#define rename(a, b) dm_win_rename((a), (b))
int dm_win_unlink(const char *path);   // a read-only file too
#define unlink(p) dm_win_unlink(p)
int dm_win_rmdir(const char *path);
#define rmdir(p) dm_win_rmdir(p)
int dm_win_access(const char *path, int mode);
#define access(p, m) dm_win_access((p), (m))
int dm_win_chdir(const char *path);
#define chdir(p) dm_win_chdir(p)
FILE *dm_win_fopen(const char *path, const char *mode);   // UTF-8 paths, binary
#define fopen(p, m) dm_win_fopen((p), (m))
int fchmod(int fd, int mode);
int dm_win_chmod(const char *p, int mode);
#define chmod(p, m) dm_win_chmod((p), (m))
int fchown(int fd, uid_t u, gid_t g);
int chown(const char *p, uid_t u, gid_t g);
int futimens(int fd, const struct timespec t[2]);
int utimensat(int dirfd, const char *path, const struct timespec t[2], int flags);
int utimes(const char *path, const struct timeval t[2]);
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
// stat: a struct of its own with Darwin's nanosecond times and 64-bit sizes (MinGW's struct stat has a 32-bit size and whole seconds), filled from the file's
// attributes (GetFileInformationByHandle: st_ino is the NTFS file index, st_nlink the link count). The tag and the call share the name, as in POSIX
struct dm_stat {
    uint64_t st_dev, st_ino; uint32_t st_mode, st_nlink, st_uid, st_gid; uint64_t st_rdev; int64_t st_size, st_blocks; int32_t st_blksize, st_flags;
    struct timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
};
int dm_stat(const char *path, struct dm_stat *st);
int dm_lstat(const char *path, struct dm_stat *st);
int dm_fstat(int fd, struct dm_stat *st);
int dm_fstatat(int dirfd, const char *path, struct dm_stat *st, int flags);
#undef stat
#undef lstat
#undef fstat
#define stat dm_stat
#define lstat dm_lstat
#define fstat dm_fstat
#define fstatat dm_fstatat
#define st_atime st_atimespec.tv_sec
#define st_mtime st_mtimespec.tv_sec
#define st_ctime st_ctimespec.tv_sec
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#undef lseek
#undef ftruncate
#define lseek(f, o, w) _lseeki64((f), (o), (w))
int dm_win_ftruncate(int fd, int64_t n);
#define ftruncate(fd, n) dm_win_ftruncate((fd), (n))
#define ftello _ftelli64
#define fseeko _fseeki64
#ifndef S_IFSOCK
#define S_IFSOCK 0xC000
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) (((m) & S_IFMT) == _S_IFIFO)
#endif
#ifndef S_ISBLK
#define S_ISBLK(m) 0
#endif
#ifndef S_ISLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_IRWXU
#define S_IRWXU 0700
#define S_IRWXG 0070
#define S_IRWXO 0007
#endif
#ifndef S_IRGRP
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#endif
#ifndef S_ISUID
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#endif
static inline int umask_dm(int m) { (void)m; return 022; }
long sysconf(int name);
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_ONLN 84
#define _SC_NPROCESSORS_CONF 83
#define _SC_OPEN_MAX 4
#define _SC_PHYS_PAGES 85
#define _SC_CLK_TCK 2
int getpagesize(void);
pid_t gettid(void);
int sched_getcpu(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
pid_t getppid(void);
int pipe(int fd[2]);
unsigned alarm(unsigned seconds);
int pause(void);
int kill(pid_t pid, int sig);
int dm_win_raise(int sig);
#define raise dm_win_raise
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
char *strndup(const char *s, size_t n);
char *strsep(char **s, const char *delim);
char *strcasestr(const char *h, const char *n);
void *memmem(const void *h, size_t hn, const void *n, size_t nn);
void *memrchr(const void *s, int c, size_t n);
char *stpcpy(char *d, const char *s);
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
ssize_t getline(char **line, size_t *n, FILE *f);
ssize_t getdelim(char **line, size_t *n, int delim, FILE *f);
int asprintf(char **out, const char *fmt, ...);
int vasprintf(char **out, const char *fmt, va_list ap);
char *mkdtemp(char *tmpl);
int mkstemp(char *tmpl);
FILE *fmemopen(void *buf, size_t n, const char *mode);
FILE *open_memstream(char **buf, size_t *n);
int dprintf(int fd, const char *fmt, ...);
// ftrylockfile: the UCRT has no try-lock on a stream; this takes the lock (crash.c's 1 s give-up on a held stderr does not apply on Windows)
static inline int ftrylockfile(FILE *f) { _lock_file(f); return 0; }
#define flockfile _lock_file
#define funlockfile _unlock_file
#define getc_unlocked _getc_nolock
#define putc_unlocked _putc_nolock
#define fwrite_unlocked _fwrite_nolock
#define fread_unlocked _fread_nolock
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t timegm(struct tm *tm);
char *strptime(const char *s, const char *fmt, struct tm *tm);
int clock_nanosleep(clockid_t c, int flags, const struct timespec *req, struct timespec *rem);
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif
int pthread_getcpuclockid_dm(pthread_t t, clockid_t *c);
uint64_t dm_win_thread_cpu_ns(pthread_t t);
// Process start / wait: the host's child processes (the shader translator in a test, the Mac's sampler) are not run on Windows
int posix_memalign(void **out, size_t align, size_t n);
void *aligned_alloc_dm(size_t align, size_t n);
// posix_memalign / malloc_zone_memalign blocks are freed with free(), as on POSIX: free, realloc and malloc_usable_size look for the aligned block's header first
void dm_win_free(void *p);
void *dm_win_realloc(void *p, size_t n);
size_t malloc_usable_size(void *p);
#define free(p) dm_win_free(p)
#define realloc(p, n) dm_win_realloc((p), (n))
void arc4random_buf(void *buf, size_t n);
uint32_t arc4random(void);
uint32_t arc4random_uniform(uint32_t n);
// Directory streams: MinGW's dirent.h (opendir / readdir / closedir); d_type is filled from the attributes posix_windows.c reads (dm_win_readdir)
#include <dirent.h>
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#endif
struct dm_dirent { long d_ino; unsigned short d_reclen; unsigned short d_namlen; unsigned char d_type; char d_name[NAME_MAX * 3 + 1]; };
typedef struct dm_dir DM_DIR;
DM_DIR *dm_win_opendir(const char *path);
struct dm_dirent *dm_win_readdir(DM_DIR *d);
int dm_win_closedir(DM_DIR *d);
void dm_win_rewinddir(DM_DIR *d);
int dm_win_dirfd(DM_DIR *d);
DM_DIR *dm_win_fdopendir(int fd);
#define DIR DM_DIR
#define dirent dm_dirent
#define opendir dm_win_opendir
#define readdir dm_win_readdir
#define closedir dm_win_closedir
#define rewinddir dm_win_rewinddir
#define dirfd dm_win_dirfd
#define fdopendir dm_win_fdopendir
int scandir(const char *dir, struct dm_dirent ***list, int (*sel)(const struct dm_dirent *), int (*cmp)(const struct dm_dirent **, const struct dm_dirent **));
int alphasort(const struct dm_dirent **a, const struct dm_dirent **b);
// Paths: the host's are UTF-8 with '/' separators, as on POSIX; posix_windows.c converts them to UTF-16 for every Win32 call (and the UCRT's narrow calls take UTF-8
// through the process code page, set to UTF-8 by the manifest: dm/windows/dmhost.manifest, activeCodePage)

// ---- Mach VM and threads ----
typedef int kern_return_t;
typedef unsigned int mach_port_t, thread_act_t, mach_msg_type_number_t, vm_prot_t, vm_inherit_t;
typedef uintptr_t vm_address_t, vm_size_t, mach_vm_address_t, mach_vm_size_t, vm_offset_t;
#define vm_page_size ((vm_size_t)4096)   // Windows on Arm pages are 4 KB (allocations are made at 64 KB granularity)
typedef thread_act_t *thread_act_array_t;
typedef int *thread_state_t, *thread_info_t, *vm_region_info_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
#define KERN_INVALID_ARGUMENT 4
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 2
// A thread's "port" is its Windows thread id (GetCurrentThreadId)
mach_port_t dm_win_thread_id(void);
static inline mach_port_t mach_task_self(void) { return 0; }
static inline mach_port_t mach_thread_self(void) { return dm_win_thread_id(); }
static inline kern_return_t mach_port_deallocate(mach_port_t t, mach_port_t p) { (void)t; (void)p; return KERN_SUCCESS; }
// A read that fails instead of faulting (ReadProcessMemory on our own process: an unreadable range fails, no exception is raised)
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out);
kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags);
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size);
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT 68
#define ARM_NEON_STATE64 17
#define ARM_NEON_STATE64_COUNT 132
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT 10
#define MACH_PORT_NULL 0
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 9
typedef struct { vm_prot_t protection, max_protection; } vm_region_basic_info_data_64_t;
// The region holding an address, or the next one above it (VirtualQuery), as plat_android.c answers it from /proc/self/maps
kern_return_t vm_region_64(mach_port_t t, vm_address_t *a, vm_size_t *s, int f, vm_region_info_t i, mach_msg_type_number_t *n, mach_port_t *o);
#define mach_vm_region(t, a, s, f, i, n, o) vm_region_64(t, (vm_address_t *)(a), (vm_size_t *)(s), f, i, n, o)
typedef struct { struct { int seconds, microseconds; } user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
#define TH_STATE_RUNNING 1
#define TH_STATE_STOPPED 2
#define TH_STATE_WAITING 3
#define TH_STATE_UNINTERRUPTIBLE 4
#define TH_STATE_HALTED 5
kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t out, mach_msg_type_number_t *n);   // plat_windows.c: CPU times (GetThreadTimes)
// mach_vm_remap / vm_remap: an alias of private memory. Windows aliases only section memory (MapViewOfFile3), so private memory fails here as on Android, and
// NVN's virtual pools keep such mappings in their page table only (plat_windows.c)
static inline kern_return_t mach_vm_remap(mach_port_t t, mach_vm_address_t *a, mach_vm_size_t s, mach_vm_size_t m, int f, mach_port_t st, mach_vm_address_t sa, boolean_t c, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t i) {
    (void)t; (void)a; (void)s; (void)m; (void)f; (void)st; (void)sa; (void)c; (void)cur; (void)max; (void)i; return KERN_FAILURE; }
typedef int *task_info_t;
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT 87
#define TASK_POWER_INFO_V2 26
#define TASK_POWER_INFO_V2_COUNT 30
typedef struct { uint64_t virtual_size, resident_size, resident_size_peak, phys_footprint; } task_vm_info_data_t;
typedef struct { struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups; } cpu_energy;
                 struct { uint64_t task_gpu_utilisation; } gpu_energy; uint64_t task_ptime, task_energy; } task_power_info_v2_data_t;
kern_return_t task_info(mach_port_t t, int flavor, task_info_t out, mach_msg_type_number_t *n);   // plat_windows.c: GetProcessMemoryInfo
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT 40
typedef struct { uint64_t pth_user_time, pth_system_time; int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
static inline int sysctlbyname(const char *n, void *o, size_t *on, const void *i, size_t in) { (void)n; (void)o; (void)on; (void)i; (void)in; errno = ENOENT; return -1; }
kern_return_t vm_remap(mach_port_t t, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t st, vm_address_t from, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh);   // plat_windows.c
static inline int getdtablesize(void) { return 8192; }
// Suspending another thread (by its id): SuspendThread / GetThreadContext; nesting counts as Mach's does
kern_return_t thread_suspend(mach_port_t t);
kern_return_t thread_resume(mach_port_t t);
kern_return_t thread_get_state(mach_port_t t, int flavor, thread_state_t s, mach_msg_type_number_t *n);
static inline kern_return_t task_threads(mach_port_t t, thread_act_array_t *a, mach_msg_type_number_t *n) { (void)t; *a = NULL; *n = 0; return KERN_FAILURE; }
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
typedef pthread_t dm_cpuclock_t;

// ---- Time ----
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline void mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = 1; t->denom = 1; }
uint64_t dm_win_mono_ns(void);   // QueryPerformanceCounter in nanoseconds (posix_windows.c)
static inline uint64_t mach_absolute_time(void) { return dm_win_mono_ns(); }
kern_return_t mach_wait_until(uint64_t deadline);   // a high-resolution waitable timer (posix_windows.c)
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec ts; if (clock_gettime(c, &ts)) return 0; return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
#define USEC_PER_SEC 1000000ull

// ---- Instruction cache: FlushInstructionCache (the builtin clears through the data cache by line, which Windows also allows from user mode) ----
void dm_win_icache(void *p, size_t n);
static inline void sys_icache_invalidate(void *p, size_t n) { dm_win_icache(p, n); }
static inline void sys_dcache_flush(void *p, size_t n) { (void)p; (void)n; }

// ---- Thread quality of service: Windows thread priorities ----
typedef enum { QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_UTILITY = 0x11,
               QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0 } qos_class_t;
int dm_android_qos_set(qos_class_t q);   // plat_windows.c (the name the shared Vulkan port's code calls)
qos_class_t dm_android_qos_get(void);
static inline int pthread_set_qos_class_self_np(qos_class_t q, int rel) { (void)rel; return dm_android_qos_set(q); }
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) { (void)t; if (q) *q = dm_android_qos_get(); if (rel) *rel = 0; return 0; }
typedef void *pthread_override_t;
static inline pthread_override_t pthread_override_qos_class_start_np(pthread_t t, qos_class_t q, int rel) { (void)t; (void)q; (void)rel; return NULL; }
static inline int pthread_override_qos_class_end_np(pthread_override_t o) { (void)o; return 0; }
mach_port_t dm_win_pthread_tid(pthread_t t);   // the Windows thread id of a pthread (winpthreads keeps its handle)
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { return dm_win_pthread_tid(t); }
size_t dm_android_stack_info(void **top);   // plat_windows.c: the calling thread's stack (GetCurrentThreadStackLimits)
static inline void *pthread_get_stackaddr_np(pthread_t t) { (void)t; void *top; dm_android_stack_info(&top); return top; }
static inline size_t pthread_get_stacksize_np(pthread_t t) { (void)t; void *top; return dm_android_stack_info(&top); }
// Darwin's one-argument form names the calling thread; winpthreads' names any thread (and SetThreadDescription names it for debuggers too: plat_windows.c)
int dm_win_setname(pthread_t t, const char *n);
static inline int dm_setname_self(const char *n) { return dm_win_setname(pthread_self(), n); }
#define pthread_setname_np(...) DM_SETNAME_PICK(__VA_ARGS__, dm_win_setname, dm_setname_self)(__VA_ARGS__)
#define DM_SETNAME_PICK(a, b, f, ...) f
int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel);
static inline int pthread_threadid_np(pthread_t t, uint64_t *id) { *id = t ? dm_win_pthread_tid(t) : dm_win_thread_id(); return 0; }
// Threads are made with their whole stack committed (winpthreads passes the size to _beginthreadex as the commit): guest code runs without stack probes
#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 16384
#endif

// ---- malloc ----
static inline size_t malloc_size(const void *p) { return p ? malloc_usable_size((void *)p) : 0; }
static inline size_t malloc_good_size(size_t n) { return (n + 15) & ~(size_t)15; }
typedef struct malloc_zone { int unused; } malloc_zone_t;
static inline malloc_zone_t *malloc_create_zone(size_t a, unsigned f) { (void)a; (void)f; static malloc_zone_t z; return &z; }
static inline void malloc_set_zone_name(malloc_zone_t *z, const char *n) { (void)z; (void)n; }
// The UCRT's malloc returns 16-byte aligned blocks on arm64, as Darwin's
static inline void *malloc_zone_malloc(malloc_zone_t *z, size_t n) { (void)z; return malloc(n); }
static inline void *malloc_zone_memalign(malloc_zone_t *z, size_t a, size_t n) { (void)z; void *p = NULL; return posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n) ? NULL : p; }
static inline void *malloc_zone_realloc(malloc_zone_t *z, void *p, size_t n) { (void)z; return realloc(p, n); }
static inline void malloc_zone_free(malloc_zone_t *z, void *p) { (void)z; free(p); }

// ---- Signals: the host's handlers (crash.c, hle_thread.c's pause, the dirty-page tracker) run from a vectored exception handler (posix_windows.c) ----
// The guest faults the host handles happen in its own memory and code: an access violation at an address the host mapped (mmap), or with the pc in the host or in
// memory it mapped (guest code), becomes SIGSEGV; another (a system DLL's own probe of an address it then catches) goes on to the DLL's exception handler, and an
// exception nobody handles reaches the host's crash handler through the unhandled-exception filter.
typedef struct {
    int si_signo, si_code, si_errno;
    void *si_addr;
    union { int sival_int; void *sival_ptr; } si_value;
} siginfo_t;
#define SA_SIGINFO 4
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGUSR1
#define SIGUSR1 30
#endif
#ifndef SIGUSR2
#define SIGUSR2 31
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGSYS
#define SIGSYS 12
#endif
#ifndef SIGCONT
#define SIGCONT 18
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif
#define DM_NSIG 32
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define BUS_ADRALN 1
#define ILL_ILLOPC 1
#define TRAP_BRKPT 1
typedef uint64_t dm_sigset_t;
#define sigset_t dm_sigset_t
struct sigaction {
    union { void (*sa_handler)(int); void (*sa_sigaction)(int, siginfo_t *, void *); };
    sigset_t sa_mask;
    int sa_flags;
};
int dm_win_sigaction(int sig, const struct sigaction *sa, struct sigaction *old);
#define sigaction(s, a, o) dm_win_sigaction((s), (a), (o))
static inline int sigemptyset(sigset_t *s) { *s = 0; return 0; }
static inline int sigfillset(sigset_t *s) { *s = ~0ull; return 0; }
static inline int sigaddset(sigset_t *s, int n) { *s |= 1ull << (n & 63); return 0; }
static inline int sigdelset(sigset_t *s, int n) { *s &= ~(1ull << (n & 63)); return 0; }
static inline int sigismember(const sigset_t *s, int n) { return (int)((*s >> (n & 63)) & 1); }
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
// sigwait: SIGTERM comes from the console's close / Ctrl+C / Ctrl+Break and from the window's close (posix_windows.c: SetConsoleCtrlHandler); other signals never arrive
int sigwait(const sigset_t *set, int *sig);
int dm_win_sigmask(int how, const sigset_t *set, sigset_t *old);   // per thread: a blocked signal is not delivered to that thread (the crash handler blocks SIGSEGV)
#undef pthread_sigmask
#define pthread_sigmask dm_win_sigmask
#define sigprocmask dm_win_sigmask
typedef struct { void *ss_sp; int ss_flags; size_t ss_size; } stack_t;
#define SS_ONSTACK 1
#define SS_DISABLE 4
#ifndef MINSIGSTKSZ
#define MINSIGSTKSZ 32768
#define SIGSTKSZ 131072
#endif
// No alternate stack on Windows: a handler runs on the faulting thread's stack (a guard of 64 KB is kept for it, SetThreadStackGuarantee)
static inline int sigaltstack(const stack_t *ss, stack_t *old) { (void)ss; if (old) { old->ss_sp = NULL; old->ss_flags = SS_DISABLE; old->ss_size = 0; } return 0; }
// pthread_kill: a signal with a handler is delivered by suspending the thread and running the handler on it (posix_windows.c dm_win_pthread_kill), only while the
// thread runs guest code (in host code it is left alone; hle_thread.c's pause resends until the thread is back in guest code or in a gate)
int dm_win_pthread_kill(pthread_t t, int sig);
#undef pthread_kill
#define pthread_kill dm_win_pthread_kill

// ---- Signal context ----
// The handler's context is the exception's Windows CONTEXT (arm64): {ContextFlags, Cpsr, X0-X28, Fp, Lr, Sp, Pc, V[32], Fpcr, Fpsr, ...}. Darwin's thread state
// {x0-x28, fp, lr, sp, pc} is the same memory from X0 on (+8), so the Darwin view is the CONTEXT itself (the cpsr after pc does not line up: DM_CPSR reads it). The NEON
// state is V[32] then Fpcr, Fpsr: the fields are named as Darwin's (__v, __fpcr, __fpsr), in Windows' order
struct __darwin_arm_thread_state64 { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; };
typedef struct __darwin_arm_thread_state64 _STRUCT_ARM_THREAD_STATE64, arm_thread_state64_t;
typedef struct { __uint128_t __v[32]; uint32_t __fpcr, __fpsr; } _STRUCT_ARM_NEON_STATE64;
typedef _STRUCT_ARM_NEON_STATE64 arm_neon_state64_t;
typedef struct dm_win_mcontext { struct __darwin_arm_thread_state64 __ss; _STRUCT_ARM_NEON_STATE64 __ns; } *mcontext_t;
typedef struct dm_win_ucontext {
    void *uc_ctx;          // the Windows CONTEXT (written back when the handler returns)
    uint64_t uc_esr;       // an ESR made from the exception (data abort with WnR for a write; 0 otherwise)
    uint32_t uc_cpsr;      // the CONTEXT's Cpsr, which Darwin's layout cannot show in place
    struct dm_win_ucontext *uc_link;
    sigset_t uc_sigmask;
    stack_t uc_stack;
} ucontext_t;
#define DM_SS(uc) (*(struct __darwin_arm_thread_state64 *)(void *)((uint8_t *)(uc)->uc_ctx + 8))
#define DM_NS(uc) (*(_STRUCT_ARM_NEON_STATE64 *)(void *)((uint8_t *)(uc)->uc_ctx + 0x110))
#define DM_UC_ESR(uc) ((uc)->uc_esr)

// ---- Sections: Mach-O "__DATA,name" sections are COFF grouped sections "name$m", bounded by the "$a" and "$z" markers plat_windows.c defines (the linker sorts by suffix) ----
#define DM_SECT(name) name "$m"
const void *dm_android_section(const char *name, unsigned long *size);   // plat_windows.c (the Android port's name)
struct mach_header_64 { uint32_t magic; };
extern const struct mach_header_64 _mh_execute_header;
static inline const uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; (void)seg; return (const uint8_t *)dm_android_section(sect, size);
}

// ---- File system ----
#define XATTR_NOFOLLOW 1
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#ifndef ENODATA
#define ENODATA 120
#endif
#ifndef ENOTSUP
#define ENOTSUP 129
#endif
#ifndef ELOOP
#define ELOOP 114
#endif
#define RENAME_SWAP 2
int dm_android_clonefile(const char *src, const char *dst, int flags);   // plat_windows.c: a recursive copy (no copy-on-write clone; ReFS block clones are not used)
int dm_android_renamex_np(const char *from, const char *to, unsigned flags);   // plat_windows.c: RENAME_SWAP by plain renames through <to>.swap
int dm_android_swap_by_renames(const char *from, const char *to);
void dm_android_swap_recover(const char *path);   // at a save's mount: undoes or finishes a swap of plain renames that a kill cut short
#define clonefile dm_android_clonefile
#define renamex_np dm_android_renamex_np
// Extended attributes (hle_fs.c keeps a guest name in one): an NTFS alternate data stream "<path>:dm.<name>" (posix_windows.c). Darwin's argument order
ssize_t dm_android_getxattr(const char *p, const char *n, void *v, size_t s, uint32_t pos, int opt);
int dm_android_setxattr(const char *p, const char *n, const void *v, size_t s, uint32_t pos, int opt);
int dm_android_removexattr(const char *p, const char *n, int opt);
ssize_t dm_android_listxattr(const char *p, char *list, size_t s, int opt);
#define getxattr dm_android_getxattr
#define setxattr dm_android_setxattr
#define removexattr dm_android_removexattr
#define listxattr dm_android_listxattr
#define F_DUPFD 0
#define F_DUPFD_CLOEXEC 1030
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define FD_CLOEXEC 1
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
struct flock { short l_type, l_whence; off_t l_start, l_len; pid_t l_pid; };
#define F_GETPATH 0x7d01
#define F_FULLFSYNC 0x7d02
#define F_NOCACHE 0x7d03
#define F_RDAHEAD 0x7d04
#define F_PREALLOCATE 0x7d05
int dm_android_fcntl(int fd, int cmd, ...);
#define fcntl dm_android_fcntl
int flock(int fd, int op);
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#include <sys/param.h>
struct statfs { uint64_t f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree; char f_fstypename[16]; };
int statfs(const char *path, struct statfs *s);
struct statvfs { unsigned long f_bsize, f_frsize; uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail; unsigned long f_fsid, f_flag, f_namemax; };
int statvfs(const char *path, struct statvfs *s);

// mincore: bit 0 for a committed page (posix_windows.c: VirtualQuery)
#define MINCORE_INCORE 1

// ---- Code made at run time (hle_jit.c): Windows maps private memory read + write + execute without a JIT flag, as Linux; no per-thread write switch ----
#define MAP_JIT 0
static inline void pthread_jit_write_protect_np(int enabled) { (void)enabled; }

// ---- Sorting: BSD's stable sort (plat_windows.c) ----
int mergesort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void qsort_r_dm(void *base, size_t n, size_t size, int (*cmp)(void *, const void *, const void *), void *ctx);

// ---- The Android port's hooks the shared Vulkan code calls (plat_windows.c: no performance hint sessions, no thermal API) ----
void dm_android_hint_thread(int32_t tid);
void dm_android_place_thread(int done);
void dm_android_hint_frame(uint64_t work_ns, const int32_t *tids, int n, uint64_t hold_ns, int thermal);
int dm_thermal_now(void);
int dm_android_suspend_signal(void);

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/ucontext.h>
