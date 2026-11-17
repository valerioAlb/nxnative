// nxlauncher.exe: the games built for this PC as a grid of covers; a tap, a click, Enter or the controller's A starts one (its dmhost.exe), so no PowerShell is needed.
// A built game is a folder with dmhost.exe and game\game.json (scripts/build_windows_from_nsp.py makes it: game\icon.jpg is the game's own icon from its NACP control data,
// game\control.nacp its names). The launcher looks in the folders of its list: each one itself and the folders right inside it. The list starts with the launcher's own
// folder and the one above it, and is kept in %LOCALAPPDATA%\nxnative\launcher.ini (folder=<path> lines; played.<title ID>=<time> orders the games by when they were last played).
//   nxlauncher.exe [--folder <path>]... [--list <file>] [--dump <file.bmp>]   --folder: also look there (this run only); --list / --dump: write the games / a picture, then exit (CI)
#define WIN32_LEAN_AND_MEAN 1
#define COBJMACROS 1
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../third_party/flip/tool/stb_image.h"

typedef struct { char dir[MAX_PATH * 2], name[256], id[24], ver[48]; uint32_t *icon; int iw, ih; long long played; } game;
static game *g_games; static int g_n, g_cap;
static char g_folders[32][MAX_PATH * 2]; static int g_nf, g_nf_saved;   // the list (the first g_nf_saved are kept in launcher.ini)
static char g_ini[MAX_PATH * 2], g_exe_dir[MAX_PATH * 2];
static struct { char id[24]; long long t; } g_played[256]; static int g_np;
static HWND g_wnd; static int g_sel = 0, g_scroll = 0, g_content_h = 0; static float g_dpi = 1;
static int g_running; static char g_status[300];

static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vsnprintf(g_status, sizeof g_status, fmt, a); va_end(a); if (g_wnd) InvalidateRect(g_wnd, NULL, FALSE); }
static int is_file(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static int is_dir(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = l >= 0 && l < 64 << 20 ? malloc((size_t)l + 1) : NULL;
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
    fclose(f); if (b) { b[l] = 0; if (n) *n = (size_t)l; } return b;
}
// game.json is the build's own small file: "key": "value" or "key": number
static int json_str(const char *j, const char *key, char *out, size_t n) {
    char k[64]; snprintf(k, sizeof k, "\"%s\"", key); const char *p = strstr(j, k); if (!p) return 0;
    p = strchr(p + strlen(k), ':'); if (!p) return 0; p++; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    size_t o = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"' && o + 5 < n; p++) {
            if (*p != '\\' || !p[1]) { out[o++] = *p; continue; }
            p++;
            if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {   // json.dumps writes "Pok\u00e9mon": back to UTF-8 (a surrogate pair too)
                unsigned c = (unsigned)strtoul((char[5]){p[1], p[2], p[3], p[4], 0}, NULL, 16); p += 4;
                if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u') { unsigned lo = (unsigned)strtoul((char[5]){p[3], p[4], p[5], p[6], 0}, NULL, 16); if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); p += 6; } }
                if (c < 0x80) out[o++] = (char)c;
                else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
                else if (c < 0x10000) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
                else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | (c >> 12 & 63)); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
            } else out[o++] = *p == 'n' ? ' ' : *p == 't' ? ' ' : *p;
        }
    }
    else for (; *p && *p != ',' && *p != '}' && *p != '\n' && o + 1 < n; p++) out[o++] = *p;
    while (o && (out[o - 1] == ' ' || out[o - 1] == '\r')) o--;
    out[o] = 0; return o > 0 && strcmp(out, "null");
}
static long long played_of(const char *id) { for (int i = 0; i < g_np; i++) if (!_stricmp(g_played[i].id, id)) return g_played[i].t; return 0; }
static void load_icon(game *g) {
    char p[MAX_PATH * 2 + 32]; size_t n = 0; snprintf(p, sizeof p, "%s\\game\\icon.jpg", g->dir);
    char *b = slurp(p, &n); if (!b) { snprintf(p, sizeof p, "%s\\game\\icon.png", g->dir); b = slurp(p, &n); }
    if (!b) return;
    int w, h, c; unsigned char *px = stbi_load_from_memory((const unsigned char *)b, (int)n, &w, &h, &c, 4); free(b);
    if (!px) return;
    g->icon = malloc((size_t)w * h * 4); g->iw = w; g->ih = h;
    for (int i = 0; i < w * h; i++) { unsigned char *s = px + i * 4; g->icon[i] = (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2]; }   // BGRA for GDI
    stbi_image_free(px);
}
static void check(const char *dir) {
    char exe[MAX_PATH * 2 + 32], js[MAX_PATH * 2 + 32]; snprintf(exe, sizeof exe, "%s\\dmhost.exe", dir); snprintf(js, sizeof js, "%s\\game\\game.json", dir);
    if (!is_file(exe) || !is_file(js)) return;
    char full[MAX_PATH * 2]; if (!GetFullPathNameA(dir, sizeof full, full, NULL)) snprintf(full, sizeof full, "%s", dir);
    for (int i = 0; i < g_n; i++) if (!_stricmp(g_games[i].dir, full)) return;
    if (g_n == g_cap) { g_cap = g_cap ? g_cap * 2 : 16; g_games = realloc(g_games, sizeof(game) * (size_t)g_cap); }
    game *g = &g_games[g_n++]; memset(g, 0, sizeof *g); snprintf(g->dir, sizeof g->dir, "%s", full);
    char *j = slurp(js, NULL);
    if (j) {
        json_str(j, "name", g->name, sizeof g->name); json_str(j, "title_id", g->id, sizeof g->id);
        if (!json_str(j, "display", g->ver, sizeof g->ver)) { char v[32]; if (json_str(j, "version", v, sizeof v)) snprintf(g->ver, sizeof g->ver, "v%s", v); }
        free(j);
    }
    if (!g->name[0]) {   // the NACP: 16 languages of 0x200 name + 0x100 publisher; the first one set
        char np[MAX_PATH * 2 + 32]; size_t n = 0; snprintf(np, sizeof np, "%s\\game\\control.nacp", dir); char *b = slurp(np, &n);
        for (int l = 0; b && l < 16 && !g->name[0]; l++) if ((size_t)l * 0x300 + 0x200 <= n && b[l * 0x300]) snprintf(g->name, sizeof g->name, "%.*s", 0x1ff, b + l * 0x300);
        if (b && !g->ver[0] && n >= 0x3070 && b[0x3060]) snprintf(g->ver, sizeof g->ver, "%.15s", b + 0x3060);
        free(b);
    }
    if (!g->name[0]) { const char *s = strrchr(full, '\\'); snprintf(g->name, sizeof g->name, "%s", s ? s + 1 : full); }
    g->played = played_of(g->id);
    load_icon(g);
}
static int by_played(const void *a, const void *b) {
    const game *x = a, *y = b;
    if (x->played != y->played) return x->played < y->played ? 1 : -1;
    return _stricmp(x->name, y->name);
}
static void scan(void) {
    for (int i = 0; i < g_n; i++) free(g_games[i].icon);
    g_n = 0;
    for (int f = 0; f < g_nf; f++) {
        if (!is_dir(g_folders[f])) continue;
        check(g_folders[f]);
        char pat[MAX_PATH * 2 + 8]; snprintf(pat, sizeof pat, "%s\\*", g_folders[f]);
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            char sub[MAX_PATH * 2 + 300]; snprintf(sub, sizeof sub, "%s\\%s", g_folders[f], fd.cFileName); check(sub);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    qsort(g_games, (size_t)g_n, sizeof(game), by_played);
    if (g_sel >= g_n) g_sel = g_n ? g_n - 1 : 0;
    say(g_n ? "%d game%s" : "No games found in the folders of the list", g_n, g_n == 1 ? "" : "s");
}

// ---- launcher.ini ----
static void trim_slash(char *p) { size_t l = strlen(p); while (l > 3 && (p[l - 1] == '\\' || p[l - 1] == '/')) p[--l] = 0; }
static int add_folder(const char *p) {
    char full[MAX_PATH * 2]; if (!GetFullPathNameA(p, sizeof full, full, NULL)) return 0; trim_slash(full);
    for (int i = 0; i < g_nf; i++) if (!_stricmp(g_folders[i], full)) return 0;
    if (g_nf == 32) return 0;
    snprintf(g_folders[g_nf++], sizeof g_folders[0], "%s", full); return 1;
}
static void save_ini(void) {
    char tmp[MAX_PATH * 2 + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", g_ini);
    FILE *f = fopen(tmp, "wb"); if (!f) return;
    fprintf(f, "# nxnative launcher: the folders it looks in (each one and the folders right inside it), and when each game was last played\r\n");
    for (int i = 0; i < g_nf_saved; i++) fprintf(f, "folder=%s\r\n", g_folders[i]);
    for (int i = 0; i < g_np; i++) fprintf(f, "played.%s=%lld\r\n", g_played[i].id, g_played[i].t);
    fclose(f); MoveFileExA(tmp, g_ini, MOVEFILE_REPLACE_EXISTING);
}
static void load_ini(void) {
    char *b = slurp(g_ini, NULL);
    if (!b) {   // the first start: the launcher's folder and the one above it (where the other games' folders usually sit)
        add_folder(g_exe_dir); char up[MAX_PATH * 2]; snprintf(up, sizeof up, "%s\\..", g_exe_dir); add_folder(up);
        g_nf_saved = g_nf; save_ini(); return;
    }
    for (char *l = strtok(b, "\r\n"); l; l = strtok(NULL, "\r\n")) {
        if (!strncmp(l, "folder=", 7)) add_folder(l + 7);
        else if (!strncmp(l, "played.", 7) && g_np < 256) { char *eq = strchr(l, '='); if (eq) { *eq = 0; snprintf(g_played[g_np].id, sizeof g_played[0].id, "%s", l + 7); g_played[g_np++].t = _atoi64(eq + 1); } }
    }
    free(b); g_nf_saved = g_nf;
}
static void set_played(const char *id) {
    if (!id[0]) return;
    int i = 0; while (i < g_np && _stricmp(g_played[i].id, id)) i++;
    if (i == g_np) { if (g_np == 256) return; g_np++; snprintf(g_played[i].id, sizeof g_played[0].id, "%s", id); }
    g_played[i].t = (long long)time(NULL); save_ini();
}

// ---- starting a game ----
static DWORD WINAPI wait_game(void *h) {
    WaitForSingleObject(h, INFINITE); CloseHandle(h);
    g_running = 0; PostMessageW(g_wnd, WM_APP, 0, 0); return 0;
}
static void start(int i) {
    if (i < 0 || i >= g_n || g_running) return;
    game *g = &g_games[i];
    char exe[MAX_PATH * 2 + 32], cmd[MAX_PATH * 2 + 40]; snprintf(exe, sizeof exe, "%s\\dmhost.exe", g->dir); snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, g->dir, &si, &pi)) { say("Could not start %s (error %lu)", exe, (unsigned long)GetLastError()); return; }
    CloseHandle(pi.hThread); g_running = 1; set_played(g->id);
    HANDLE t = CreateThread(NULL, 0, wait_game, pi.hProcess, 0, NULL); if (t) CloseHandle(t); else { CloseHandle(pi.hProcess); g_running = 0; }
    say("Started %s", g->name); ShowWindow(g_wnd, SW_MINIMIZE);
}
static void pick_folder(void) {
    IFileOpenDialog *d = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return;
    DWORD o = 0; IFileOpenDialog_GetOptions(d, &o); IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(d, L"The folder with your built games (or one game's folder)");
    if (SUCCEEDED(IFileOpenDialog_Show(d, g_wnd))) {
        IShellItem *it = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            PWSTR w = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &w))) {
                char p[MAX_PATH * 2]; WideCharToMultiByte(CP_UTF8, 0, w, -1, p, sizeof p, NULL, NULL); CoTaskMemFree(w);
                // the saved ones first, then this run's --folder ones
                if (add_folder(p)) { char keep[MAX_PATH * 2]; snprintf(keep, sizeof keep, "%s", g_folders[g_nf - 1]);
                    memmove(g_folders[g_nf_saved + 1], g_folders[g_nf_saved], sizeof g_folders[0] * (size_t)(g_nf - 1 - g_nf_saved));
                    snprintf(g_folders[g_nf_saved], sizeof g_folders[0], "%s", keep); g_nf_saved++; save_ini(); scan(); }
            }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
}
static void remove_folder(int i) {
    if (i < 0 || i >= g_nf_saved) return;
    memmove(g_folders[i], g_folders[i + 1], sizeof g_folders[0] * (size_t)(g_nf - i - 1)); g_nf--; g_nf_saved--; save_ini(); scan();
}

// ---- drawing ----
static uint32_t *g_px; static int g_bw, g_bh; static HDC g_mdc; static HBITMAP g_mbmp;
enum { B_ADD = -10, B_FOLDERS = -11, B_REFRESH = -12 };
static int S(float v) { return (int)(v * g_dpi + 0.5f); }
static void layout(int w, int *cols, int *tile, int *gap, int *top) {
    *gap = S(20); *top = S(84); int t = S(200); int c = (w - *gap) / (t + *gap); if (c < 1) c = 1;
    *tile = (w - *gap) / c - *gap; if (*tile > S(260)) *tile = S(260); *cols = c;
}
static void tile_rect(int i, int w, RECT *r) {
    int cols, t, gap, top; layout(w, &cols, &t, &gap, &top);
    int row = i / cols, col = i % cols, x0 = (w - (cols * (t + gap) - gap)) / 2;
    r->left = x0 + col * (t + gap); r->top = top + gap + row * (t + S(70) + gap) - g_scroll; r->right = r->left + t; r->bottom = r->top + t + S(70);
}
static void buttons(int w, RECT r[3]) {
    int h = S(44), y = S(20), bw = S(150), x = w - S(20);
    for (int k = 0; k < 3; k++) { r[k].right = x; r[k].left = x - bw; r[k].top = y; r[k].bottom = y + h; x -= bw + S(12); }
}
static void text(HDC dc, const char *s, RECT *r, int size, int weight, COLORREF c, UINT fmt) {
    wchar_t w[400]; MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 400);
    HFONT f = CreateFontW(-S((float)size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ o = SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT); DrawTextW(dc, w, -1, r, fmt); SelectObject(dc, o); DeleteObject(f);
}
static void fill(HDC dc, RECT *r, COLORREF c, int radius) {
    HBRUSH b = CreateSolidBrush(c); HPEN p = CreatePen(PS_SOLID, 1, c); HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, radius, radius); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
static void paint(HDC dc, int w, int h) {
    RECT all = {0, 0, w, h}; fill(dc, &all, RGB(0x12, 0x15, 0x1A), 0);
    int cols, t, gap, top; layout(w, &cols, &t, &gap, &top);
    SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);
    for (int i = 0; i < g_n; i++) {
        RECT r; tile_rect(i, w, &r); if (r.bottom < top || r.top > h) continue;
        game *g = &g_games[i];
        RECT bg = {r.left - S(6), r.top - S(6), r.right + S(6), r.bottom + S(2)};
        if (i == g_sel) fill(dc, &bg, RGB(0x2E, 0x6E, 0xA6), S(18));
        RECT ic = {r.left, r.top, r.right, r.top + t};
        if (g->icon) { BITMAPINFO bi = {.bmiHeader = {sizeof(BITMAPINFOHEADER), g->iw, -g->ih, 1, 32, BI_RGB}}; StretchDIBits(dc, ic.left, ic.top, t, t, 0, 0, g->iw, g->ih, g->icon, &bi, DIB_RGB_COLORS, SRCCOPY); }
        else { fill(dc, &ic, RGB(0x2A, 0x30, 0x3A), S(14)); char ini[3] = {g->name[0], 0}; text(dc, ini, &ic, 64, FW_BOLD, RGB(0x8F, 0xA0, 0xB5), DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
        RECT nr = {r.left, r.top + t + S(6), r.right, r.top + t + S(44)}; text(dc, g->name, &nr, 16, FW_SEMIBOLD, RGB(0xF0, 0xF0, 0xF0), DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
        char sub[96]; snprintf(sub, sizeof sub, "%s%s%s", g->id, g->ver[0] ? "  \xC2\xB7  " : "", g->ver);
        RECT sr = {r.left, r.top + t + S(46), r.right, r.top + t + S(68)}; text(dc, sub, &sr, 12, FW_NORMAL, RGB(0x9A, 0xA4, 0xB0), DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (!g_n) {
        RECT m = {S(40), top + S(60), w - S(40), top + S(220)};
        text(dc, "No games yet.\nAdd the folder that holds your built games (the folders with dmhost.exe and game\\ that scripts\\build_windows_from_nsp.py makes), or one game's folder.",
             &m, 18, FW_NORMAL, RGB(0xC8, 0xD0, 0xD8), DT_CENTER | DT_WORDBREAK);
    }
    RECT bar = {0, 0, w, top}; fill(dc, &bar, RGB(0x1A, 0x1E, 0x25), 0);
    RECT tl = {S(24), 0, w / 2, top}; text(dc, "nxnative", &tl, 30, FW_BOLD, RGB(0xFF, 0xFF, 0xFF), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT st = {S(170), S(8), w - S(520), top}; text(dc, g_running ? "A game is running" : g_status, &st, 13, FW_NORMAL, RGB(0x9A, 0xA4, 0xB0), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT b[3]; buttons(w, b); static const char *const lab[3] = {"+ Add folder", "Folders", "Refresh"};
    for (int k = 0; k < 3; k++) { fill(dc, &b[k], k ? RGB(0x2A, 0x30, 0x3A) : RGB(0x2E, 0x6E, 0xA6), S(12)); text(dc, lab[k], &b[k], 15, FW_SEMIBOLD, RGB(0xFF, 0xFF, 0xFF), DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
    int rows = (g_n + cols - 1) / cols; g_content_h = top + gap + rows * (t + S(70) + gap);
}
static int hit(int x, int y, int w) {
    RECT b[3]; buttons(w, b); POINT p = {x, y};
    for (int k = 0; k < 3; k++) if (PtInRect(&b[k], p)) return k == 0 ? B_ADD : k == 1 ? B_FOLDERS : B_REFRESH;
    int cols, t, gap, top; layout(w, &cols, &t, &gap, &top); if (y < top) return -1;
    for (int i = 0; i < g_n; i++) { RECT r; tile_rect(i, w, &r); if (PtInRect(&r, p)) return i; }
    return -1;
}
static void clamp_scroll(int h) { int mx = g_content_h + g_scroll - h; if (g_scroll > mx) g_scroll = mx; if (g_scroll < 0) g_scroll = 0; }
static void show_sel(void) {
    RECT c; GetClientRect(g_wnd, &c); if (g_n == 0) return;
    RECT r; tile_rect(g_sel, c.right, &r); int cols, t, gap, top; layout(c.right, &cols, &t, &gap, &top);
    if (r.top < top + gap) g_scroll -= top + gap - r.top; else if (r.bottom > c.bottom - gap) g_scroll += r.bottom - (c.bottom - gap);
    clamp_scroll(c.bottom); InvalidateRect(g_wnd, NULL, FALSE);
}
static void move_sel(int dx, int dy) {
    if (!g_n) return; RECT c; GetClientRect(g_wnd, &c); int cols, t, gap, top; layout(c.right, &cols, &t, &gap, &top);
    int s = g_sel + dx + dy * cols; if (s < 0) s = dy ? g_sel % cols : 0; if (s >= g_n) s = dy ? g_sel : g_n - 1;
    if (dy > 0 && s >= g_n) s = g_n - 1; g_sel = s; show_sel();
}
static void context(int i, POINT sp) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Start"); AppendMenuW(m, MF_STRING, 2, L"Open its folder"); AppendMenuW(m, MF_STRING, 3, L"Open its log (log.txt)");
    AppendMenuW(m, MF_STRING, 4, L"Open its saves and settings (%LOCALAPPDATA%\\nxnative\\<title ID>)");
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, g_wnd, NULL); DestroyMenu(m);
    game *g = &g_games[i]; char p[MAX_PATH * 2 + 64];
    if (c == 1) start(i);
    else if (c == 2) ShellExecuteA(g_wnd, "open", g->dir, NULL, NULL, SW_SHOWNORMAL);
    else if (c == 3) { snprintf(p, sizeof p, "%s\\log.txt", g->dir); if (is_file(p)) ShellExecuteA(g_wnd, "open", p, NULL, NULL, SW_SHOWNORMAL); else say("No log.txt yet in %s", g->dir); }
    else if (c == 4) { char la[MAX_PATH]; if (GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la)) { snprintf(p, sizeof p, "%s\\nxnative\\%s", la, g->id); if (is_dir(p)) ShellExecuteA(g_wnd, "open", p, NULL, NULL, SW_SHOWNORMAL); else say("Not played yet: no %s", p); } }
}
static void folders_menu(POINT sp) {
    HMENU m = CreatePopupMenu(); AppendMenuW(m, MF_STRING, 1000, L"Add a folder...");
    if (g_nf) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (int i = 0; i < g_nf; i++) {
        wchar_t w[MAX_PATH * 2 + 40], p[MAX_PATH * 2]; MultiByteToWideChar(CP_UTF8, 0, g_folders[i], -1, p, MAX_PATH * 2);
        swprintf(w, MAX_PATH * 2 + 40, i < g_nf_saved ? L"Remove from the list: %ls" : L"%ls (this run only, --folder)", p);
        AppendMenuW(m, MF_STRING | (i < g_nf_saved ? 0 : MF_GRAYED), 1 + (UINT)i, w);
    }
    int c = TrackPopupMenu(m, TPM_RETURNCMD, sp.x, sp.y, 0, g_wnd, NULL); DestroyMenu(m);
    if (c == 1000) pick_folder(); else if (c >= 1 && c <= g_nf) remove_folder(c - 1);
}

// ---- input: a tap or click starts (a drag scrolls), right click or a long press (Windows makes it a right click) opens the game's menu ----
static int g_down, g_drag, g_down_y, g_down_x, g_down_scroll;
static void activate(int k, POINT cp) {
    POINT sp = cp; ClientToScreen(g_wnd, &sp);
    if (k == B_ADD) pick_folder(); else if (k == B_FOLDERS) folders_menu(sp); else if (k == B_REFRESH) scan();
    else if (k >= 0) { g_sel = k; InvalidateRect(g_wnd, NULL, FALSE); start(k); }
}
static void pad_poll(void) {
    static WORD was; XINPUT_STATE s; if (XInputGetState(0, &s) != ERROR_SUCCESS || GetForegroundWindow() != g_wnd) { was = 0; return; }
    WORD b = s.Gamepad.wButtons, p = b & ~was; was = b;
    SHORT lx = s.Gamepad.sThumbLX, ly = s.Gamepad.sThumbLY; static int stick; int sd = lx > 16000 ? 1 : lx < -16000 ? 2 : ly > 16000 ? 3 : ly < -16000 ? 4 : 0;
    if (sd && sd != stick) { if (sd == 1) move_sel(1, 0); else if (sd == 2) move_sel(-1, 0); else if (sd == 3) move_sel(0, -1); else move_sel(0, 1); }
    stick = sd;
    if (p & XINPUT_GAMEPAD_DPAD_LEFT) move_sel(-1, 0); if (p & XINPUT_GAMEPAD_DPAD_RIGHT) move_sel(1, 0);
    if (p & XINPUT_GAMEPAD_DPAD_UP) move_sel(0, -1); if (p & XINPUT_GAMEPAD_DPAD_DOWN) move_sel(0, 1);
    if (p & XINPUT_GAMEPAD_A) start(g_sel);
}
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT c; GetClientRect(h, &c);
        if (c.right != g_bw || c.bottom != g_bh || !g_mdc) {
            if (g_mbmp) DeleteObject(g_mbmp); if (!g_mdc) g_mdc = CreateCompatibleDC(dc);
            BITMAPINFO bi = {.bmiHeader = {sizeof(BITMAPINFOHEADER), c.right, -c.bottom, 1, 32, BI_RGB}};
            g_mbmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&g_px, NULL, 0); SelectObject(g_mdc, g_mbmp); g_bw = c.right; g_bh = c.bottom;
        }
        paint(g_mdc, c.right, c.bottom); BitBlt(dc, 0, 0, c.right, c.bottom, g_mdc, 0, 0, SRCCOPY); EndPaint(h, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: { RECT c; GetClientRect(h, &c); clamp_scroll(c.bottom); InvalidateRect(h, NULL, FALSE); return 0; }
    case WM_DPICHANGED: { g_dpi = HIWORD(w) / 96.0f; RECT *r = (RECT *)l; SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
    case WM_MOUSEWHEEL: { RECT c; GetClientRect(h, &c); g_scroll -= GET_WHEEL_DELTA_WPARAM(w) * S(120) / WHEEL_DELTA; clamp_scroll(c.bottom); InvalidateRect(h, NULL, FALSE); return 0; }
    case WM_LBUTTONDOWN: SetCapture(h); g_down = 1; g_drag = 0; g_down_x = GET_X_LPARAM(l); g_down_y = GET_Y_LPARAM(l); g_down_scroll = g_scroll; return 0;
    case WM_MOUSEMOVE:
        if (g_down) { int dy = GET_Y_LPARAM(l) - g_down_y, dx = GET_X_LPARAM(l) - g_down_x;
            if (!g_drag && dy * dy + dx * dx > S(12) * S(12)) g_drag = 1;
            if (g_drag) { RECT c; GetClientRect(h, &c); g_scroll = g_down_scroll - dy; clamp_scroll(c.bottom); InvalidateRect(h, NULL, FALSE); } }
        return 0;
    case WM_LBUTTONUP: { ReleaseCapture(); int was = g_down && !g_drag; g_down = 0; if (was) { RECT c; GetClientRect(h, &c); POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; activate(hit(p.x, p.y, c.right), p); } return 0; }
    case WM_RBUTTONUP: { RECT c; GetClientRect(h, &c); POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; int k = hit(p.x, p.y, c.right); if (k >= 0) { g_sel = k; InvalidateRect(h, NULL, FALSE); ClientToScreen(h, &p); context(k, p); } return 0; }
    case WM_KEYDOWN:
        switch (w) {
        case VK_LEFT: move_sel(-1, 0); break; case VK_RIGHT: move_sel(1, 0); break; case VK_UP: move_sel(0, -1); break; case VK_DOWN: move_sel(0, 1); break;
        case VK_RETURN: case VK_SPACE: start(g_sel); break; case VK_F5: scan(); break;
        case VK_APPS: if (g_n) { RECT r, c; GetClientRect(h, &c); tile_rect(g_sel, c.right, &r); POINT p = {r.left + S(20), r.top + S(20)}; ClientToScreen(h, &p); context(g_sel, p); } break;
        }
        return 0;
    case WM_TIMER: pad_poll(); return 0;
    case WM_APP: say("The game ended"); ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); scan(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void utf8(const wchar_t *w, char *o, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, o, n, NULL, NULL); }
static int dump_bmp(const char *path, int w, int h) {   // --dump: the window's picture without a window (CI)
    HDC dc = CreateCompatibleDC(NULL); BITMAPINFO bi = {.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}}; uint32_t *px;
    HBITMAP b = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0); if (!b) return 1;
    SelectObject(dc, b); paint(dc, w, h); GdiFlush();
    FILE *f = fopen(path, "wb"); if (!f) return 1;
    uint32_t isz = (uint32_t)w * (uint32_t)h * 4u, fsz = 54 + isz, off = 54, hdr[10] = {40, (uint32_t)w, (uint32_t)-h, 1u | 32u << 16, 0, isz};
    uint8_t fh[14] = {'B', 'M'}; memcpy(fh + 2, &fsz, 4); memcpy(fh + 10, &off, 4); fwrite(fh, 1, 14, f); fwrite(hdr, 1, 40, f); fwrite(px, 1, isz, f); fclose(f);
    return 0;
}
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cl, int show) {
    (void)prev; (void)cl;
    wchar_t we[MAX_PATH * 2]; GetModuleFileNameW(NULL, we, MAX_PATH * 2); utf8(we, g_exe_dir, sizeof g_exe_dir); { char *s = strrchr(g_exe_dir, '\\'); if (s) *s = 0; }
    char la[MAX_PATH]; if (!GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la)) snprintf(la, sizeof la, "%s", g_exe_dir);
    char nd[MAX_PATH + 16]; snprintf(nd, sizeof nd, "%s\\nxnative", la); CreateDirectoryA(nd, NULL); snprintf(g_ini, sizeof g_ini, "%s\\launcher.ini", nd);
    load_ini();
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc); char list[MAX_PATH * 2] = "", dump[MAX_PATH * 2] = "";
    for (int i = 1; i < argc; i++) {
        char a[MAX_PATH * 2]; if (i + 1 < argc) utf8(argv[i + 1], a, sizeof a); else a[0] = 0;
        if (!wcscmp(argv[i], L"--folder") && a[0]) { add_folder(a); i++; }
        else if (!wcscmp(argv[i], L"--list") && a[0]) { snprintf(list, sizeof list, "%s", a); i++; }
        else if (!wcscmp(argv[i], L"--dump") && a[0]) { snprintf(dump, sizeof dump, "%s", a); i++; }
    }
    scan();
    if (list[0] || dump[0]) {
        if (list[0]) { FILE *f = fopen(list, "wb"); if (f) { fprintf(f, "folders: %d\n", g_nf); for (int i = 0; i < g_nf; i++) fprintf(f, "folder %s\n", g_folders[i]);
            for (int i = 0; i < g_n; i++) fprintf(f, "game %s | %s | %s | %s | icon %dx%d\n", g_games[i].id, g_games[i].name, g_games[i].ver, g_games[i].dir, g_games[i].iw, g_games[i].ih); fclose(f); } }
        return dump[0] ? dump_bmp(dump, 1280, 800) : 0;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_dpi = GetDpiForSystem() / 96.0f;
    WNDCLASSEXW wc = {sizeof wc, CS_HREDRAW | CS_VREDRAW, proc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), NULL, NULL, L"nxnative_launcher", NULL};
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, L"nxnative_launcher", L"nxnative", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1180), S(780), NULL, NULL, inst, NULL);
    { UINT d = GetDpiForWindow(g_wnd); if (d) g_dpi = d / 96.0f; }
    ShowWindow(g_wnd, show); SetTimer(g_wnd, 1, 16, NULL);
    MSG msg; while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
