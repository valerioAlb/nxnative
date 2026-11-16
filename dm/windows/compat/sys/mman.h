// <sys/mman.h> on Windows (posix_windows.c): mmap over VirtualAlloc / MapViewOfFile.
//   Anonymous PROT_NONE mappings are reservations; a later mmap MAP_FIXED or mprotect inside one commits its pages (4 KB granules). A commit the system refuses
//   (its commit limit: Pokopia reserves 9.75 GB and makes 9 GB of it read-write) falls back to lazy commit: the pages are committed by the exception handler on first
//   touch (DM_WIN_LAZY=1 makes every read-write range lazy; =0 never). File mappings are views of the file (read-only: copy-on-write views are copies).
//   munmap of a whole mapping releases it; of a part, decommits the part (Windows releases only whole reservations): the addresses stay reserved, as no one else may take them.
#pragma once
#include "dm_windows.h"
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void *)-1)
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 8
#define MADV_FREE_REUSABLE 7
#define MADV_FREE_REUSE 8
#define MS_ASYNC 1
#define MS_SYNC 4
#define MS_INVALIDATE 2
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2
void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int madvise(void *addr, size_t len, int advice);
#define posix_madvise madvise
int mincore(void *addr, size_t len, unsigned char *vec);
int msync(void *addr, size_t len, int flags);
int mlock(const void *addr, size_t len);
int munlock(const void *addr, size_t len);
void *mremap(void *old, size_t oldn, size_t newn, int flags, ...);
// The host's own queries (woa_spike and logs): whether [p, p + n) is inside memory mmap made, and the lazily committed bytes so far
int dm_win_mapped(const void *p, size_t n);
uint64_t dm_win_lazy_bytes(void);
