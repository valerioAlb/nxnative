// settings.ini (winfront.h): the player's settings for one game, in %LOCALAPPDATA%\nxnative\<title ID>\settings.ini
#include "winfront.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum { ST_MAX = 256 };
static struct { char *key, *val; } g_st[ST_MAX];
static int g_n;
static char g_path[800];
static SRWLOCK g_lk = SRWLOCK_INIT;

static char *trim(char *s) { while (*s && isspace((unsigned char)*s)) s++; char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0; return s; }
static int find(const char *k) { for (int i = 0; i < g_n; i++) if (!_stricmp(g_st[i].key, k)) return i; return -1; }
void st_load(const char *dir) {
    snprintf(g_path, sizeof g_path, "%s/settings.ini", dir);
    FILE *f = fopen(g_path, "rb"); if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f) && g_n < ST_MAX) {
        char *s = trim(line); if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '='); if (!eq) continue; *eq = 0;
        char *k = trim(s), *v = trim(eq + 1); if (!*k) continue;
        int i = find(k); if (i < 0) { i = g_n++; g_st[i].key = _strdup(k); g_st[i].val = NULL; }
        free(g_st[i].val); g_st[i].val = _strdup(v);
    }
    fclose(f);
}
const char *st_get(const char *key) {
    AcquireSRWLockShared(&g_lk); int i = find(key); const char *v = i >= 0 ? g_st[i].val : NULL; ReleaseSRWLockShared(&g_lk);
    if (v) return v;
    char e[96] = "DM_"; size_t n = 3; for (const char *c = key; *c && n < sizeof e - 1; c++) e[n++] = *c == '.' ? '_' : (char)toupper((unsigned char)*c); e[n] = 0;
    return getenv(e);
}
const char *st_str(const char *key, const char *def) { const char *v = st_get(key); return v && *v ? v : def; }
double st_num(const char *key, double def) { const char *v = st_get(key); char *e; double d = v ? strtod(v, &e) : 0; return v && e != v ? d : def; }
int st_each(int i, const char **key, const char **val) { if (i < 0 || i >= g_n) return 0; *key = g_st[i].key; *val = g_st[i].val; return 1; }
void st_put(const char *key, const char *val) {
    AcquireSRWLockExclusive(&g_lk);
    int i = find(key);
    if (i < 0 && g_n < ST_MAX) { i = g_n++; g_st[i].key = _strdup(key); g_st[i].val = NULL; }
    if (i >= 0) { free(g_st[i].val); g_st[i].val = _strdup(val); }
    if (g_path[0]) {   // written whole to a temporary file, then put in place: a crash meanwhile leaves the old file
        char tmp[820]; snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            fprintf(f, "# nxnative settings for this game (the program writes this file; KEY=VALUE, see docs/WINDOWS_ARM.md)\r\n");
            for (int k = 0; k < g_n; k++) fprintf(f, "%s=%s\r\n", g_st[k].key, g_st[k].val);
            fclose(f);
            wchar_t a[820], b[820]; MultiByteToWideChar(CP_UTF8, 0, tmp, -1, a, 820); MultiByteToWideChar(CP_UTF8, 0, g_path, -1, b, 820);
            MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
        }
    }
    ReleaseSRWLockExclusive(&g_lk);
}
