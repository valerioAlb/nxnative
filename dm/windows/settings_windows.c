// settings.ini (winfront.h): the player's settings for one game in %LOCALAPPDATA%\nxnative\<title ID>\settings.ini, over the ones for every game in
// %LOCALAPPDATA%\nxnative\settings.ini (a key in the game's file wins), over the environment's DM_<KEY> (env.txt)
#include "winfront.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum { ST_MAX = 256 };
typedef struct { struct { char *key, *val; } e[ST_MAX]; int n; char path[800]; const char *title; } layer;
static layer g_l[2];   // 0 this game's, 1 every game's
static SRWLOCK g_lk = SRWLOCK_INIT;

static char *trim(char *s) { while (*s && isspace((unsigned char)*s)) s++; char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0; return s; }
static int find(const layer *l, const char *k) { for (int i = 0; i < l->n; i++) if (!_stricmp(l->e[i].key, k)) return i; return -1; }
static void put(layer *l, const char *k, const char *v) {
    int i = find(l, k);
    if (!v) { if (i >= 0) { free(l->e[i].key); free(l->e[i].val); l->e[i] = l->e[--l->n]; } return; }
    if (i < 0 && l->n < ST_MAX) { i = l->n++; l->e[i].key = _strdup(k); l->e[i].val = NULL; }
    if (i >= 0) { free(l->e[i].val); l->e[i].val = _strdup(v); }
}
static void read_file(layer *l) {
    FILE *f = fopen(l->path, "rb"); if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line); if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '='); if (!eq) continue; *eq = 0;
        char *k = trim(s), *v = trim(eq + 1); if (*k) put(l, k, v);
    }
    fclose(f);
}
static void save(const layer *l) {   // written whole to a temporary file, then put in place: a crash meanwhile leaves the old file
    if (!l->path[0]) return;
    char tmp[820]; snprintf(tmp, sizeof tmp, "%s.tmp", l->path);
    FILE *f = fopen(tmp, "wb"); if (!f) return;
    fprintf(f, "# nxnative settings %s (the program writes this file; KEY=VALUE, see docs/WINDOWS_ARM.md)\r\n", l->title);
    for (int k = 0; k < l->n; k++) fprintf(f, "%s=%s\r\n", l->e[k].key, l->e[k].val);
    fclose(f);
    wchar_t a[820], b[820]; MultiByteToWideChar(CP_UTF8, 0, tmp, -1, a, 820); MultiByteToWideChar(CP_UTF8, 0, l->path, -1, b, 820);
    MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
}
void st_load(const char *dir) {
    snprintf(g_l[0].path, sizeof g_l[0].path, "%s/settings.ini", dir); g_l[0].title = "for this game";
    char up[800]; snprintf(up, sizeof up, "%s", dir);
    char *s = up + strlen(up); while (s > up && (s[-1] == '/' || s[-1] == '\\')) *--s = 0;
    char *a = strrchr(up, '/'), *b = strrchr(up, '\\'); if (b > a) a = b;
    if (a) { *a = 0; snprintf(g_l[1].path, sizeof g_l[1].path, "%s/settings.ini", up); g_l[1].title = "for every game (a game's own settings.ini wins)"; read_file(&g_l[1]); }
    read_file(&g_l[0]);
}
const char *st_get(const char *key) {
    AcquireSRWLockShared(&g_lk);
    const char *v = NULL; for (int l = 0; l < 2 && !v; l++) { int i = find(&g_l[l], key); if (i >= 0) v = g_l[l].e[i].val; }
    ReleaseSRWLockShared(&g_lk);
    if (v) return v;
    char e[96] = "DM_"; size_t n = 3; for (const char *c = key; *c && n < sizeof e - 1; c++) e[n++] = *c == '.' ? '_' : (char)toupper((unsigned char)*c); e[n] = 0;
    return getenv(e);
}
const char *st_str(const char *key, const char *def) { const char *v = st_get(key); return v && *v ? v : def; }
double st_num(const char *key, double def) { const char *v = st_get(key); char *e; double d = v ? strtod(v, &e) : 0; return v && e != v ? d : def; }
int st_each(int i, const char **key, const char **val) {   // every game's settings first, then this game's (so a caller applying them in turn ends with the game's)
    if (i < 0) return 0;
    if (i < g_l[1].n) { *key = g_l[1].e[i].key; *val = g_l[1].e[i].val; return 1; }
    i -= g_l[1].n; if (i >= g_l[0].n) return 0;
    *key = g_l[0].e[i].key; *val = g_l[0].e[i].val; return 1;
}
void st_put(const char *key, const char *val) { AcquireSRWLockExclusive(&g_lk); put(&g_l[0], key, val ? val : ""); save(&g_l[0]); ReleaseSRWLockExclusive(&g_lk); }
void st_put_scope(const char *key, const char *val, int all) {
    AcquireSRWLockExclusive(&g_lk);
    if (all && g_l[1].path[0]) { put(&g_l[1], key, val); save(&g_l[1]); if (find(&g_l[0], key) >= 0) { put(&g_l[0], key, NULL); save(&g_l[0]); } }
    else { put(&g_l[0], key, val); save(&g_l[0]); }
    ReleaseSRWLockExclusive(&g_lk);
}
