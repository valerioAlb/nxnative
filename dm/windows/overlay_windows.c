// The touch controls over the game (winfront.h): a layered window owned by the game's window, the size of its client area, that never takes input
// (WS_EX_TRANSPARENT): the game window gets every pointer and asks touchpad.c whether a control is under it. A touch on no control is the game's touch screen.
//   touch_overlay  auto (default: shown on a touch, hidden when a key or the controller is used; hidden at start on a PC without a touch screen), on, off. F1 switches on / off
//   touch_opacity  0..1 (0.6), touch_scale 0.5..2 (1), touch.<control>=x,y[,r] to move one (units of the window's shorter side, negative from the right / bottom; r 0 hides it):
//   lstick rstick dpad a b x y l r zl zr plus minus ls rs
#include "winfront.h"
#include "touchpad.h"
#include "dm.h"
#include "front.h"
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
static void draw(void) {
    buffers(g_w, g_h); if (!g_px) return;
    memset(g_px, 0, (size_t)g_bw * g_bh * 4);
    if (g_visible) {
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
    int want = (g_visible || g_badge_on) && IsWindowVisible(g_owner) && !IsIconic(g_owner) && GetClientRect(g_owner, &r) && ClientToScreen(g_owner, &o);
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
void ovl_input(uint64_t *b, int32_t ax[4]) { *b = g_visible ? g_pad.btn : 0; for (int i = 0; i < 4; i++) ax[i] = g_visible ? g_pad.ax[i] : 0; }
void ovl_picture(const int pic[4]) { memcpy(g_pic, pic, sizeof g_pic); }
void ovl_badge(const char *text) {
    int on = text != NULL;
    if (on && strcmp(text, g_badge)) { snprintf(g_badge, sizeof g_badge, "%s", text); g_pad.dirty = 1; }
    if (on != g_badge_on) { g_badge_on = on; g_pad.dirty = 1; }
}
// A finger (or the mouse, pid MOUSE) down / moved / up at a client point
enum { MOUSE = 0xFFFF0000u };
static void ptr_down(uint32_t pid, float x, float y, int is_touch) {
    if (is_touch && g_mode == 0) show(1);
    if (g_visible && tp_down(&g_pad, pid, x, y, g_w ? g_w : 1, g_h ? g_h : 1)) return;
    gt_down(pid, x, y);
}
static void ptr_move(uint32_t pid, float x, float y) { if (!tp_move(&g_pad, pid, x, y, g_w ? g_w : 1, g_h ? g_h : 1)) gt_move(pid, x, y); }
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
