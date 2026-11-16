// libc passthrough table: C library functions consistent with AAPCS64 and with no struct layout dependency, connected directly to Apple libc. Goes into the swhle section.
#include "dm.h"
#include <math.h>
#include <ctype.h>
#include <time.h>
typedef struct { const char *name; void *fn; } hle_ent;
#define L(n) __attribute__((used, section(DM_SECT("swhle")))) static const hle_ent hle_lib_##n = { #n, (void *)n };
#ifdef _WIN32
// Windows is LLP64: the guest's long is 64 bits, the host's 32. The functions that take or return a long go to their 64-bit twins under the guest's name
#define L64(n, host) __attribute__((used, section(DM_SECT("swhle")))) static const hle_ent hle_lib_##n = { #n, (void *)host };
L64(strtol, strtoll) L64(strtoul, strtoull) L64(lroundf, llroundf)
#define DM_L_LONG(n)
#else
#define DM_L_LONG(n) L(n)
#endif
L(memcpy) L(memset) L(memmove) L(memcmp) L(memchr) L(strlen) L(strnlen) L(strcmp) L(strncmp)
L(strcpy) L(strncpy) L(strcat) L(strncat) L(strchr) L(strrchr) L(strstr) L(strspn) L(strcspn)
L(strpbrk) L(strcasecmp) L(strncasecmp) L(atoi) L(atof) DM_L_LONG(strtol) DM_L_LONG(strtoul) L(strtoll) L(strtoull)
L(strtod) L(strtof) L(sqrtf) L(cbrtf) L(expf) L(acosf) L(cosf) L(sinf) L(atan2f) L(fmodf) L(cos)
L(sin) L(powf) L(atanf) L(tanf) L(asinf) L(log10) L(pow) L(logf) L(ldexpf) L(log10f) L(log)
L(ldexp) L(frexp) L(atan2) L(modff) L(exp2f) L(exp2) L(log2f) L(log2) DM_L_LONG(lroundf)
L(mktime) L(timegm) L(gmtime_r) L(localtime_r) L(strftime)
