// dlopen / dlsym on LoadLibrary / GetProcAddress; dladdr names the module holding an address and the nearest exported or debug symbol below it
// (posix_windows.c: the host's own functions through DbgHelp when the .exe carries its symbols, -g with a .pdb beside it)
#pragma once
#include "dm_windows.h"
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
#define RTLD_NOLOAD 4
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT ((void *)-1)
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
void *dlopen(const char *path, int flags);
void *dlsym(void *h, const char *name);
int dlclose(void *h);
const char *dlerror(void);
int dladdr(const void *addr, Dl_info *info);
