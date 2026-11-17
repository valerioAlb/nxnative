// The touch controls over the game (winfront.h): a layered window owned by the game's window, the size of its client area, that never takes input
// (WS_EX_TRANSPARENT): the game window gets every pointer and asks touchpad.c whether a control is under it. A touch on no control is the game's touch screen.
//   touch_overlay  auto (default: shown on a touch, hidden when a key or the controller is used; hidden at start on a PC without a touch screen), on, off. F1 switches on / off
//   touch_opacity  0..1 (0.6), touch_scale 0.5..2 (1), touch.<control>=x,y[,r] to move one (units of the window's shorter side, negative from the right / bottom; r 0 hides it):
//   lstick rstick dpad a b x y l r zl zr plus minus ls rs
#include "winfront.h"
#include "touchpad.h"
#include "dm.h"
#include "front.h"
#include "dm_input.h"
#include <windowsx.h>
#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

uint64_t dm_win_mono_ns(void);
static HWND g_owner, g_ov;
static tp_pad g_pad;
static int g_mode;      // 0 auto, 1 on, 2 off
static int g_visible;   // the controls are shown
static int g_w, g_h, g_x, g_y;   // the window's client area on the screen
static HDC g_dc; static HBITMAP g_bmp; static uint32_t *g_px; static int g_bw, g_bh;
static HDC g_tdc; static HBITMAP g_tbmp; static uint32_t *g_tpx;   // the labels' text, drawn white on black, read as coverage
static int g_pic[4];
static char g_badge[160]; static int g_badge_on;
static int g_shown;     // the layered window is showing
static uint64_t g_last_draw;

// The game's touch screen: fingers not on a control (and the mouse's left button)
typedef struct { int used; uint32_t pid; float x, y; } gtouch;
static gtouch g_gt[16];
static void game_touch_send(void) {
    dm_touch t[16]; int n = 0;
    for (int i = 0; i < 16; i++) if (g_gt[i].used) t[n++] = (dm_touch){(uint32_t)i, g_gt[i].x, g_gt[i].y};
    dm_touch_set(n ? t : NULL, n);
}
static void to_game(float px, float py, float *fx, float *fy) {
    int pw = g_pic[2] > 0 ? g_pic[2] : g_w, ph = g_pic[3] > 0 ? g_pic[3] : g_h;
    float x = (px - (float)g_pic[0]) / (float)(pw > 0 ? pw : 1), y = (py - (float)g_pic[1]) / (float)(ph > 0 ? ph : 1);
    *fx = x < 0 ? 0 : x > 0.999f ? 0.999f : x; *fy = y < 0 ? 0 : y > 0.999f ? 0.999f : y;
}
static int gt_find(uint32_t pid) { for (int i = 0; i < 16; i++) if (g_gt[i].used && g_gt[i].pid == pid) return i; return -1; }
static void gt_down(uint32_t pid, float px, float py) {
    int i = gt_find(pid); if (i < 0) for (i = 0; i < 16 && g_gt[i].used; i++) {}
    if (i == 16) return;
    g_gt[i].used = 1; g_gt[i].pid = pid; to_game(px, py, &g_gt[i].x, &g_gt[i].y); game_touch_send();
}
static int gt_move(uint32_t pid, float px, float py) { int i = gt_find(pid); if (i < 0) return 0; to_game(px, py, &g_gt[i].x, &g_gt[i].y); game_touch_send(); return 1; }
static int gt_up(uint32_t pid) { int i = gt_find(pid); if (i < 0) return 0; g_gt[i].used = 0; game_touch_send(); return 1; }

static void load_layout(void) {
    tp_init(&g_pad);
    tp_set(&g_pad, "opacity", st_str("touch_opacity", "0.6"));
    tp_set(&g_pad, "scale", st_str("touch_scale", "1"));
    const char *k, *v;
    for (int i = 0; st_each(i, &k, &v); i++) if (!_strnicmp(k, "touch.", 6)) { if (!tp_set(&g_pad, k + 6, v)) LOG("[RED] settings.ini: %s=%s is not a touch control (x,y[,r])", k, v); }
    const char *m = st_str("touch_overlay", "auto");
    g_mode = !_stricmp(m, "on") || !strcmp(m, "1") ? 1 : !_stricmp(m, "off") || !strcmp(m, "0") ? 2 : 0;
}
static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}
void ovl_reload(void) { int vis = g_visible; load_layout(); g_visible = g_mode == 1 ? 1 : g_mode == 2 ? 0 : vis; g_pad.dirty = 1; }
void ovl_init(HWND owner) {
    g_owner = owner; load_layout();
    int touch = GetSystemMetrics(SM_MAXIMUMTOUCHES) > 0;
    g_visible = g_mode == 1 || (g_mode == 0 && touch);
    WNDCLASSEXW wc = {sizeof wc, 0, ov_proc, 0, 0, GetModuleHandleW(NULL), NULL, LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), NULL, NULL, L"nxnative_touch", NULL};
    RegisterClassExW(&wc);
    g_ov = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"nxnative_touch", L"", WS_POPUP, 0, 0, 1, 1, owner, NULL, GetModuleHandleW(NULL), NULL);
    LOG("touch controls: %s (%s; %d touch points%s), opacity %.2f, scale %.2f (settings.ini touch_overlay / touch_opacity / touch_scale / touch.<control>; F1 shows / hides)",
        g_mode == 1 ? "on" : g_mode == 2 ? "off" : "auto", g_visible ? "shown" : "hidden until a touch", GetSystemMetrics(SM_MAXIMUMTOUCHES),
        g_ov ? "" : ", [RED] no layered window", g_pad.opacity, g_pad.scale);
}
static void buffers(int w, int h) {
    if (w == g_bw && h == g_bh && g_px) return;
    if (g_bmp) DeleteObject(g_bmp); if (g_tbmp) DeleteObject(g_tbmp);
    if (!g_dc) { g_dc = CreateCompatibleDC(NULL); g_tdc = CreateCompatibleDC(NULL); }
    BITMAPINFO bi = {.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    g_bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, (void **)&g_px, NULL, 0);
    g_tbmp = CreateDIBSection(g_tdc, &bi, DIB_RGB_COLORS, (void **)&g_tpx, NULL, 0);
    if (!g_bmp || !g_tbmp) { g_px = NULL; g_bw = g_bh = 0; return; }
    SelectObject(g_dc, g_bmp); SelectObject(g_tdc, g_tbmp); g_bw = w; g_bh = h;
}
// Text over the picture: drawn white on black in the second bitmap, its coverage blended in as white (alpha a)
static void text_at(const char *s, int cx, int cy, int size, float a, int left) {
    if (size < 6 || !*s) return;
    wchar_t w[200]; int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 200) - 1; if (n <= 0) return;
    HFONT f = CreateFontW(-size, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HGDIOBJ o = SelectObject(g_tdc, f); SIZE ts; GetTextExtentPoint32W(g_tdc, w, n, &ts);
    int x0 = left ? cx : cx - ts.cx / 2, y0 = cy - ts.cy / 2, x1 = x0 + ts.cx, y1 = y0 + ts.cy;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > g_bw) x1 = g_bw; if (y1 > g_bh) y1 = g_bh;
    if (x1 > x0 && y1 > y0) {
        RECT r = {x0, y0, x1, y1}; FillRect(g_tdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetTextColor(g_tdc, RGB(255, 255, 255)); SetBkMode(g_tdc, TRANSPARENT); TextOutW(g_tdc, left ? cx : cx - ts.cx / 2, cy - ts.cy / 2, w, n); GdiFlush();
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
            uint32_t t = g_tpx[(size_t)y * g_bw + x]; float c = (float)((t >> 8) & 255) / 255.0f * a; if (c <= 0) continue;
            uint32_t *d = &g_px[(size_t)y * g_bw + x], v = *d; float ia = 1 - c;
            unsigned r8 = (unsigned)(255 * c + (v >> 16 & 255) * ia), g8 = (unsigned)(255 * c + (v >> 8 & 255) * ia), b8 = (unsigned)(255 * c + (v & 255) * ia), al = (unsigned)(255 * c + (v >> 24) * ia);
            *d = al << 24 | r8 << 16 | g8 << 8 | b8;
        }
    }
    SelectObject(g_tdc, o); DeleteObject(f);
}
// ---- the settings menu ----
// A setting: its settings.ini key, the label, its values (the first: what a fresh install has; "" leaves the key unset, so the game's profile decides) and their names.
// live: applied at once; the others when the game starts again ("Restart" in the menu)
typedef struct { const char *key, *label, *vals[6], *names[6]; int live; const char *note; } mitem;
static const mitem k_menu[] = {
    {"vsync", "V-sync", {"1", "0"}, {"on", "off"}, 1, "off: frames as fast as the GPU goes, may tear"},
    {"fps_target", "Frame limit", {"", "30", "60", "0"}, {"game profile", "30 fps", "60 fps", "game's own"}, 0, "30 fps: steadier and cooler on a Surface"},
    {"opmode", "Console mode", {"", "1", "0"}, {"game profile", "docked (TV)", "handheld"}, 0, "handheld: most games render fewer pixels, faster"},
    {"render_scale", "Render scale", {"", "0.75", "0.5"}, {"100%", "75%", "50%"}, 0, "only games with a render-scale patch (see the log)"},
    {"async_shaders", "Shaders in the background", {"", "0"}, {"on", "off"}, 0, "on: fewer stutters, an object may show a moment late"},
    {"filter", "Picture filter", {"linear", "nearest"}, {"smooth", "sharp"}, 1, NULL},
    {"show_fps", "Show FPS", {"0", "1"}, {"off", "on"}, 1, NULL},
    {"touch_overlay", "Touch controls", {"auto", "on", "off"}, {"auto", "on", "off"}, 1, "F1 too"},
    {"touch_opacity", "Touch controls' opacity", {"0.6", "0.8", "0.45", "0.3"}, {"60%", "80%", "45%", "30%"}, 1, NULL},
    {"touch_scale", "Touch controls' size", {"1", "1.2", "0.85"}, {"100%", "120%", "85%"}, 1, NULL},
};
enum { NM = sizeof k_menu / sizeof k_menu[0], M_RESTART = NM, M_CLOSE = NM + 1, M_ROWS = NM + 2 };
static int g_menu, g_sel, g_restart_due;
static char g_fps[48];
static int cur_val(int i) {
    const char *v = st_get(k_menu[i].key); if (!v) v = "";
    for (int k = 0; k < 6 && k_menu[i].vals[k]; k++) if (!strcmp(v, k_menu[i].vals[k])) return k;
    for (int k = 0; k < 6 && k_menu[i].vals[k]; k++) if (k_menu[i].vals[k][0] && strtod(v, NULL) == strtod(k_menu[i].vals[k], NULL) && *v) return k;
    return 0;
}
static int nvals(int i) { int k = 0; while (k < 6 && k_menu[i].vals[k]) k++; return k; }
static void menu_change(int i, int dir) {
    if (i >= NM) return;
    int n = nvals(i), k = (cur_val(i) + dir + n) % n;
    st_put(k_menu[i].key, k_menu[i].vals[k]);
    if (!k_menu[i].live) g_restart_due = 1;
    LOG("settings: %s = %s (%s)%s", k_menu[i].key, k_menu[i].vals[k][0] ? k_menu[i].vals[k] : "(unset)", k_menu[i].names[k], k_menu[i].live ? "" : ", at the next start");
    if (!strncmp(k_menu[i].key, "touch_", 6)) ovl_reload();
    front_setting_changed(k_menu[i].key);
    g_pad.dirty = 1;
}
static void menu_activate(int i) {
    if (i == M_CLOSE) { ovl_menu(0); return; }
    if (i == M_RESTART) { if (g_restart_due) front_restart(); return; }
    menu_change(i, 1);
}
void ovl_menu(int open) { if (g_menu != open) { g_menu = open; g_pad.dirty = 1; tp_release_all(&g_pad); LOG("settings menu %s", open ? "open (F2 / Esc closes)" : "closed"); } }
int ovl_menu_open(void) { return g_menu; }
int ovl_menu_key(WPARAM vk) {
    if (!g_menu) return 0;
    switch (vk) {
    case VK_UP: g_sel = (g_sel + M_ROWS - 1) % M_ROWS; break;
    case VK_DOWN: g_sel = (g_sel + 1) % M_ROWS; break;
    case VK_LEFT: menu_change(g_sel, -1); break;
    case VK_RIGHT: menu_change(g_sel, 1); break;
    case VK_RETURN: case VK_SPACE: menu_activate(g_sel); break;
    case VK_ESCAPE: case VK_F2: case VK_BACK: ovl_menu(0); break;
    default: return 1;   // every other key stays out of the game while the menu is open
    }
    g_pad.dirty = 1; return 1;
}
void ovl_menu_pad(uint64_t p) {
    if (!g_menu || !p) return;
    if (p & DM_BTN_UP) ovl_menu_key(VK_UP);
    if (p & DM_BTN_DOWN) ovl_menu_key(VK_DOWN);
    if (p & DM_BTN_LEFT) ovl_menu_key(VK_LEFT);
    if (p & DM_BTN_RIGHT) ovl_menu_key(VK_RIGHT);
    if (p & DM_BTN_A) ovl_menu_key(VK_RETURN);
    if (p & (DM_BTN_B | DM_BTN_PLUS)) ovl_menu(0);
}
void ovl_fps(const char *t) { if (!t) { if (g_fps[0]) { g_fps[0] = 0; g_pad.dirty = 1; } return; } if (strcmp(t, g_fps)) { snprintf(g_fps, sizeof g_fps, "%s", t); g_pad.dirty = 1; } }
// The panel's geometry: u (a unit), its rectangle and row height
static void menu_geom(float *u, float *x0, float *y0, float *w, float *rh) {
    float s = (float)(g_bw < g_bh ? g_bw : g_bh); *u = s / 30; *rh = *u * 1.55f;
    *w = *u * 30; if (*w > (float)g_bw * 0.94f) *w = (float)g_bw * 0.94f;
    float h = *rh * (M_ROWS + 2.2f); *x0 = ((float)g_bw - *w) / 2; *y0 = ((float)g_bh - h) / 2; if (*y0 < 0) *y0 = 0;
}
static int menu_row_at(float px, float py, int *side) {
    float u, x0, y0, w, rh; menu_geom(&u, &x0, &y0, &w, &rh);
    if (px < x0 || px > x0 + w) return -2;
    int r = (int)floorf((py - (y0 + rh * 1.6f)) / rh);
    if (py < y0 || r >= M_ROWS) return py < y0 || py > y0 + rh * (M_ROWS + 2.2f) ? -2 : -1;
    if (r < 0) return -1;
    *side = px < x0 + w * 0.62f ? 0 : px < x0 + w * 0.77f ? -1 : 1; return r;
}
static void menu_draw(void) {
    float u, x0, y0, w, rh; menu_geom(&u, &x0, &y0, &w, &rh);
    tp_rect(g_px, g_bw, g_bh, 0, 0, (float)g_bw, (float)g_bh, 0, 0x000000, 0.45f);
    tp_rect(g_px, g_bw, g_bh, x0, y0, x0 + w, y0 + rh * (M_ROWS + 2.2f), u * 0.8f, 0x14181E, 0.94f);
    text_at("Settings", (int)(x0 + u), (int)(y0 + rh * 0.8f), (int)(u * 1.05f), 1, 1);
    text_at("F2 / Esc / B closes", (int)(x0 + w - u * 9), (int)(y0 + rh * 0.8f), (int)(u * 0.55f), 0.55f, 1);
    for (int r = 0; r < M_ROWS; r++) {
        float ry = y0 + rh * (1.6f + r), cy = ry + rh / 2;
        if (r == g_sel) tp_rect(g_px, g_bw, g_bh, x0 + u * 0.4f, ry + 2, x0 + w - u * 0.4f, ry + rh - 2, u * 0.4f, 0x2E6EA6, 0.85f);
        if (r < NM) {
            int k = cur_val(r);
            text_at(k_menu[r].label, (int)(x0 + u), (int)cy, (int)(u * 0.72f), 0.95f, 1);
            char v[64]; snprintf(v, sizeof v, "\u2039  %s  \u203a%s", k_menu[r].names[k], k_menu[r].live ? "" : " *");
            text_at(v, (int)(x0 + w * 0.62f), (int)cy, (int)(u * 0.72f), 0.95f, 1);
        } else if (r == M_RESTART) {
            text_at(g_restart_due ? "Restart the game now to apply *" : "* applies at the next start", (int)(x0 + u), (int)cy, (int)(u * 0.72f), g_restart_due ? 1.0f : 0.5f, 1);
        } else text_at("Close", (int)(x0 + u), (int)cy, (int)(u * 0.72f), 0.95f, 1);
    }
    if (g_sel < NM && k_menu[g_sel].note) text_at(k_menu[g_sel].note, (int)(x0 + u), (int)(y0 + rh * (M_ROWS + 1.75f)), (int)(u * 0.55f), 0.65f, 1);
}
static void draw(void) {
    buffers(g_w, g_h); if (!g_px) return;
    memset(g_px, 0, (size_t)g_bw * g_bh * 4);
    if (g_fps[0]) {
        int u = (g_bw < g_bh ? g_bw : g_bh) / 40; if (u < 10) u = 10;
        tp_rect(g_px, g_bw, g_bh, (float)u * 0.5f, (float)u * 0.5f, (float)u * 7.5f, (float)u * 2.1f, (float)u * 0.5f, 0x000000, 0.55f);
        text_at(g_fps, u, (int)((float)u * 1.3f), u, 0.95f, 1);
    }
    if (g_menu) menu_draw();
    else if (g_visible) {
        tp_render(&g_pad, g_px, g_bw, g_bh);
        for (int i = 0; i < g_pad.n; i++) if (g_pad.c[i].r > 0 && g_pad.c[i].label[0]) { int cx, cy, sz; tp_label_box(&g_pad, i, g_bw, g_bh, &cx, &cy, &sz); text_at(g_pad.c[i].label, cx, cy, sz, 0.9f * g_pad.opacity, 0); }
    }
    if (g_badge_on) {   // the note: a dark pill at the top centre, a spinner of eight dots and the text
        int u = (g_bw < g_bh ? g_bw : g_bh) / 22; if (u < 12) u = 12;
        float bw = (float)u * 16, x0 = (float)g_bw / 2 - bw / 2, y0 = (float)u * 0.6f;
        tp_rect(g_px, g_bw, g_bh, x0, y0, x0 + bw, y0 + (float)u * 1.8f, (float)u * 0.9f, 0x101418, 0.78f);
        double t = (double)(dm_win_mono_ns() / 1000000ull) / 1000.0; float cx = x0 + (float)u * 1.1f, cy = y0 + (float)u * 0.9f;
        for (int k = 0; k < 8; k++) {
            double a = k * M_PI / 4; float ph = (float)fmod(t * 1.25 - k / 8.0 + 8, 1.0);
            tp_disc(g_px, g_bw, g_bh, cx + (float)(cos(a) * u * 0.5), cy + (float)(sin(a) * u * 0.5), (float)u * 0.11f, 0x8FD8FF, 0.25f + 0.75f * (1 - ph));
        }
        text_at(g_badge, (int)(x0 + (float)u * 2.1f), (int)cy, (int)((float)u * 0.62f), 0.95f, 1);
    }
    POINT dst = {g_x, g_y}, src = {0, 0}; SIZE sz = {g_bw, g_bh};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_ov, NULL, &dst, &sz, g_dc, &src, 0, &bf, ULW_ALPHA);
    g_last_draw = dm_win_mono_ns();
}
void ovl_follow(void) {
    if (!g_ov) return;
    RECT r; POINT o = {0, 0};
    int want = (g_visible || g_badge_on || g_menu || g_fps[0]) && IsWindowVisible(g_owner) && !IsIconic(g_owner) && GetClientRect(g_owner, &r) && ClientToScreen(g_owner, &o);
    if (!want) { if (g_shown) { ShowWindow(g_ov, SW_HIDE); g_shown = 0; } return; }
    int w = r.right - r.left, h = r.bottom - r.top; if (w <= 0 || h <= 0) return;
    int moved = w != g_w || h != g_h || o.x != g_x || o.y != g_y;
    g_w = w; g_h = h; g_x = o.x; g_y = o.y;
    uint64_t now = dm_win_mono_ns();
    if (moved || !g_shown || (g_pad.dirty && now - g_last_draw > 15000000ull) || (g_badge_on && now - g_last_draw > 66000000ull)) draw();   // at most ~60 redraws a second
    if (!g_shown) { ShowWindow(g_ov, SW_SHOWNOACTIVATE); g_shown = 1; }
}
static void show(int on) { if (g_visible != on) { g_visible = on; g_pad.dirty = 1; if (!on) tp_release_all(&g_pad); } }
void ovl_other_input(void) {
    if (g_mode != 0 || !g_visible) return;
    for (int k = 0; k < TP_MAXFINGER; k++) if (g_pad.f[k].used) return;   // not while a finger is on the controls
    show(0);
}
void ovl_toggle(void) {
    int on = !g_visible; show(on); g_mode = on ? (g_mode == 2 ? 1 : g_mode) : 2;
    st_put("touch_overlay", g_mode == 1 ? "on" : g_mode == 2 ? "off" : "auto");
    LOG("touch controls %s (F1; settings.ini touch_overlay=%s)", on ? "shown" : "hidden", g_mode == 1 ? "on" : g_mode == 2 ? "off" : "auto");
}
void ovl_input(uint64_t *b, int32_t ax[4]) { int on = g_visible && !g_menu; *b = on ? g_pad.btn : 0; for (int i = 0; i < 4; i++) ax[i] = on ? g_pad.ax[i] : 0; }
void ovl_picture(const int pic[4]) { memcpy(g_pic, pic, sizeof g_pic); }
void ovl_badge(const char *text) {
    int on = text != NULL;
    if (on && strcmp(text, g_badge)) { snprintf(g_badge, sizeof g_badge, "%s", text); g_pad.dirty = 1; }
    if (on != g_badge_on) { g_badge_on = on; g_pad.dirty = 1; }
}
// A finger (or the mouse, pid MOUSE) down / moved / up at a client point
enum { MOUSE = 0xFFFF0000u };
static void ptr_down(uint32_t pid, float x, float y, int is_touch) {
    if (g_menu) {   // a tap on a row: the left part changes it forward, the arrows' halves back / forward; outside the panel closes it
        int side = 0, r = menu_row_at(x, y, &side);
        if (r == -2) ovl_menu(0);
        else if (r >= 0) { g_sel = r; if (r < NM) menu_change(r, side < 0 ? -1 : 1); else menu_activate(r); }
        g_pad.dirty = 1; return;
    }
    if (is_touch && g_mode == 0) show(1);
    if (g_visible && tp_down(&g_pad, pid, x, y, g_w ? g_w : 1, g_h ? g_h : 1)) return;
    gt_down(pid, x, y);
}
static void ptr_move(uint32_t pid, float x, float y) { if (g_menu) return; if (!tp_move(&g_pad, pid, x, y, g_w ? g_w : 1, g_h ? g_h : 1)) gt_move(pid, x, y); }
static void ptr_up(uint32_t pid) { if (!tp_up(&g_pad, pid)) gt_up(pid); }
int ovl_wndproc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) {
    *res = 0;
    if (!g_w) { RECT r; if (GetClientRect(h, &r)) { g_w = r.right; g_h = r.bottom; } }
    switch (m) {
    case WM_POINTERDOWN: case WM_POINTERUPDATE: case WM_POINTERUP: case WM_POINTERCAPTURECHANGED: {
        UINT32 id = GET_POINTERID_WPARAM(w); POINTER_INPUT_TYPE t = PT_POINTER;
        if (!GetPointerType(id, &t) || t == PT_MOUSE || t == PT_TOUCHPAD) return 0;   // the mouse comes as WM_LBUTTON*
        if (m == WM_POINTERCAPTURECHANGED) { ptr_up(id); return 1; }
        POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(h, &p);
        if (m == WM_POINTERDOWN) ptr_down(id, (float)p.x, (float)p.y, 1);
        else if (m == WM_POINTERUP) ptr_up(id);
        else if (IS_POINTER_INCONTACT_WPARAM(w)) ptr_move(id, (float)p.x, (float)p.y);
        return 1;
    }
    case WM_LBUTTONDOWN: SetCapture(h); ptr_down(MOUSE, (float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l), 0); return 1;
    case WM_MOUSEMOVE: if (w & MK_LBUTTON) { ptr_move(MOUSE, (float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l)); return 1; } return 0;
    case WM_LBUTTONUP: ReleaseCapture(); ptr_up(MOUSE); return 1;
    }
    return 0;
}
