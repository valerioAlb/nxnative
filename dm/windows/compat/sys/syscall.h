// Linux system calls the shared Vulkan replay names in its Android-only paths (gfx_vk_fileback.inc's opt-in pool file backing: mremap of a file mapping):
// every syscall fails with ENOSYS on Windows, so those paths report their failure and keep the pool as it was
#pragma once
#include "dm_windows.h"
#define __NR_mremap 216
intptr_t syscall(long n, ...);   // (long on Linux, 64 bits; intptr_t keeps the casts to pointers whole)
