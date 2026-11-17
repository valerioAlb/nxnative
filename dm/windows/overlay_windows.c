// The touch controls over the game (winfront.h): a layered window owned by the game's window, the size of its client area, that never takes input
// (WS_EX_TRANSPARENT): the game window gets every pointer and asks touchpad.c whether a control is under it. A touch on no control is the game's touch screen.
//   touch_overlay  auto (default: shown on a touch, hidden when a key or the controller is used; hidden at start on a PC without a touch screen), on, off. F1 switches on / off
//   touch_opacity  0..1 (0.6), touch_scale 0.5..2 (1), touch.<control>=x,y[,r] to move one (units of the window's shorter side, negative from the right / bottom; r 0 hides it):
//   lstick rstick dpad a b x y l r zl zr plus minus ls rs
// The settings menu (F2) is here too, with its Controls page (remap.h: key.<button>=..., pad.<Xbox input>=...)
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

static void load_map(void);
static void load_layout(void) {
    load_map();
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
    if (st_num("overlay_window", 1) <= 0) {   // DM_OVERLAY_WINDOW=0 (env.txt) / overlay_window=0: no window over the game at all (an A/B: no touch controls, F2 menu, FPS or notes)
        LOG("touch controls, settings menu, FPS and notes: off (overlay_window=0): nothing is drawn over the game; the mouse is the game's touch screen");
        g_visible = 0; g_mode = 2; return;
    }
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
    {"vk_robust", "GPU safe reads (robust access)", {"", "1"}, {"off", "on"}, 0, "on: a shader's stray reads give 0; try it if the GPU hangs"},
    {"rumble", "Controller rumble", {"1", "0.5", "0"}, {"on", "weak", "off"}, 1, "Xbox controllers (XInput)"},
};
enum { NM = sizeof k_menu / sizeof k_menu[0], M_CONTROLS = NM, M_RESTART = NM + 1, M_CLOSE = NM + 2, M_ROWS = NM + 3 };
// The Controls page: the Switch's 16 buttons, the keyboard's two sticks (four keys each, asked in turn), where changes go, reset, back
enum { C_STICK_L = RM_NBTN, C_STICK_R, C_SCOPE, C_RESET, C_BACK, C_ROWS };
static int g_menu, g_sel, g_restart_due, g_page, g_csel;
static int g_cap = -1, g_cap_step, g_scope, g_reset_armed;   // g_cap: the row waiting for a key / button; g_scope 1: changes for every game
static rm_map g_map;
static char g_fps[48];
const rm_map *ovl_map(void) { return &g_map; }
static void load_map(void) {
    const char *ab = st_get("pad_abxy"); rm_defaults(&g_map, ab && !_stricmp(ab, "label"));
    const char *k, *v;
    for (int i = 0; st_each(i, &k, &v); i++) if ((!_strnicmp(k, "key.", 4) || !_strnicmp(k, "pad.", 4)) && !rm_set(&g_map, k, v)) LOG("[RED] settings.ini: %s=%s is not a key / controller mapping (docs/WINDOWS_ARM.md)", k, v);
}
static void save_map_diff(const rm_map *was) {   // the mappings that changed, to this game's settings or every game's
    char v[64];
    for (int a = 0; a < RM_NACT; a++) if (memcmp(was->key[a], g_map.key[a], sizeof g_map.key[a])) {
        char k[40]; snprintf(k, sizeof k, "key.%s", rm_act_name[a]); rm_key_str(&g_map, a, v, sizeof v); st_put_scope(k, v[0] ? v : "none", g_scope);
        LOG("controls: %s = %s (%s)", k, v[0] ? v : "none", g_scope ? "every game" : "this game");
    }
    for (int s = 0; s < RM_NSRC; s++) if (was->pad[s] != g_map.pad[s]) {
        char k[40]; snprintf(k, sizeof k, "pad.%s", rm_src_name[s]); const char *t = g_map.pad[s] >= 0 ? rm_act_name[g_map.pad[s]] : "none"; st_put_scope(k, t, g_scope);
        LOG("controls: %s = %s (%s)", k, t, g_scope ? "every game" : "this game");
    }
}
static void reset_map(void) {
    char k[40];
    for (int a = 0; a < RM_NACT; a++) { snprintf(k, sizeof k, "key.%s", rm_act_name[a]); st_put_scope(k, NULL, g_scope); }
    for (int s = 0; s < RM_NSRC; s++) { snprintf(k, sizeof k, "pad.%s", rm_src_name[s]); st_put_scope(k, NULL, g_scope); }
    load_map(); LOG("controls: %s's mappings reset", g_scope ? "every game" : "this game");
}
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
    if (i == M_CONTROLS) { g_page = 1; g_cap = -1; g_reset_armed = 0; return; }
    menu_change(i, 1);
}
static void controls_activate(int r) {
    if (r != C_RESET) g_reset_armed = 0;
    if (r < C_SCOPE) { g_cap = r; g_cap_step = 0; }
    else if (r == C_SCOPE) g_scope = !g_scope;
    else if (r == C_RESET) { if (g_reset_armed) { reset_map(); g_reset_armed = 0; } else g_reset_armed = 1; }
    else { g_page = 0; g_cap = -1; }
}
static void capture_key(int vk) {   // the key for the row being asked; a stick row asks for up, down, left, right in turn
    int act = g_cap < RM_NBTN ? g_cap : (g_cap == C_STICK_L ? RM_LUP : RM_RUP) + g_cap_step;
    rm_map was = g_map;
    if (vk == VK_DELETE) {
        if (g_cap < RM_NBTN) rm_clear(&g_map, act); else for (int d = 0; d < 4; d++) rm_clear(&g_map, (g_cap == C_STICK_L ? RM_LUP : RM_RUP) + d);
        save_map_diff(&was); g_cap = -1; return;
    }
    if (rm_bind_key(&g_map, act, vk) == -1 && g_map.key[act][0] != vk) return;   // a key of the program's own (F1, F2, F11)
    save_map_diff(&was);
    if (g_cap >= RM_NBTN && ++g_cap_step < 4) return;
    g_cap = -1;
}
void ovl_menu(int open) {
    if (open && !g_ov) { LOG("settings menu: not available with overlay_window=0"); return; }
    if (g_menu != open) { g_menu = open; g_page = 0; g_cap = -1; g_reset_armed = 0; g_pad.dirty = 1; tp_release_all(&g_pad); LOG("settings menu %s", open ? "open (F2 / Esc closes)" : "closed"); }
}
int ovl_menu_open(void) { return g_menu; }
int ovl_menu_key(WPARAM vk) {
    if (!g_menu) return 0;
    g_pad.dirty = 1;
    if (g_page == 1 && g_cap >= 0) {   // waiting for a key: Esc gives up, Delete clears the row
        if (vk == VK_ESCAPE) g_cap = -1; else if (vk != VK_F2 && vk != VK_F1 && vk != VK_F11) capture_key((int)vk);
        return 1;
    }
    if (g_page == 1) {
        switch (vk) {
        case VK_UP: g_csel = (g_csel + C_ROWS - 1) % C_ROWS; break;
        case VK_DOWN: g_csel = (g_csel + 1) % C_ROWS; break;
        case VK_LEFT: case VK_RIGHT: if (g_csel == C_SCOPE) g_scope = !g_scope; break;
        case VK_RETURN: case VK_SPACE: controls_activate(g_csel); break;
        case VK_ESCAPE: case VK_BACK: g_page = 0; break;
        case VK_F2: ovl_menu(0); break;
        }
        return 1;
    }
    switch (vk) {
    case VK_UP: g_sel = (g_sel + M_ROWS - 1) % M_ROWS; break;
    case VK_DOWN: g_sel = (g_sel + 1) % M_ROWS; break;
    case VK_LEFT: menu_change(g_sel, -1); break;
    case VK_RIGHT: menu_change(g_sel, 1); break;
    case VK_RETURN: case VK_SPACE: menu_activate(g_sel); break;
    case VK_ESCAPE: case VK_F2: case VK_BACK: ovl_menu(0); break;
    default: return 1;   // every other key stays out of the game while the menu is open
    }
    return 1;
}
// The controller's inputs as pressed, not as mapped (a mapping that went wrong can still be fixed): D-pad moves, A chooses, B goes back, Menu closes.
// While a row waits, the first input pressed becomes that button
void ovl_menu_pad(uint32_t p) {
    if (!g_menu || !p) return;
    if (g_page == 1 && g_cap >= 0) {
        if (g_cap >= RM_NBTN) return;   // the keyboard's sticks: keys only
        for (int s = 0; s < RM_NSRC; s++) if (p >> s & 1) { rm_map was = g_map; rm_bind_pad(&g_map, g_cap, s); save_map_diff(&was); g_cap = -1; g_pad.dirty = 1; return; }
    }
    if (p & 1u << 12) ovl_menu_key(VK_UP);
    if (p & 1u << 13) ovl_menu_key(VK_DOWN);
    if (p & 1u << 14) ovl_menu_key(VK_LEFT);
    if (p & 1u << 15) ovl_menu_key(VK_RIGHT);
    if (p & 1u << 0) ovl_menu_key(VK_RETURN);
    if (p & 1u << 1) { if (g_page == 1) { g_page = 0; g_pad.dirty = 1; } else ovl_menu(0); }
    if (p & 1u << 10) ovl_menu(0);
}
void ovl_fps(const char *t) { if (!t) { if (g_fps[0]) { g_fps[0] = 0; g_pad.dirty = 1; } return; } if (strcmp(t, g_fps)) { snprintf(g_fps, sizeof g_fps, "%s", t); g_pad.dirty = 1; } }
// The panel's geometry: u (a unit), its rectangle and row height; rows start 1.6 rows down and a note line follows them
static int page_rows(void) { return g_page ? C_ROWS : M_ROWS; }
static void menu_geom(float *u, float *x0, float *y0, float *w, float *rh) {
    float s = (float)(g_bw < g_bh ? g_bw : g_bh); *u = s / 30; *rh = *u * (g_page ? 1.12f : 1.4f);
    *w = *u * (g_page ? 34 : 30); if (*w > (float)g_bw * 0.96f) *w = (float)g_bw * 0.96f;
    float h = *rh * (page_rows() + 2.7f); *x0 = ((float)g_bw - *w) / 2; *y0 = ((float)g_bh - h) / 2; if (*y0 < 0) *y0 = 0;
}
static int menu_row_at(float px, float py, int *side) {
    float u, x0, y0, w, rh; menu_geom(&u, &x0, &y0, &w, &rh); int n = page_rows();
    if (px < x0 || px > x0 + w) return -2;
    int r = (int)floorf((py - (y0 + rh * 1.6f)) / rh);
    if (py < y0 || r >= n) return py < y0 || py > y0 + rh * (n + 2.7f) ? -2 : -1;
    if (r < 0) return -1;
    *side = px < x0 + w * 0.62f ? 0 : px < x0 + w * 0.77f ? -1 : 1; return r;
}
static void controls_draw(float u, float x0, float y0, float w, float rh) {
    int ts = (int)(u * 0.6f);
    text_at("Controls", (int)(x0 + u), (int)(y0 + rh * 0.75f), (int)(u * 1.0f), 1, 1);
    text_at("Keyboard", (int)(x0 + w * 0.40f), (int)(y0 + rh * 1.2f), (int)(u * 0.5f), 0.6f, 1);
    text_at("Controller (Xbox)", (int)(x0 + w * 0.68f), (int)(y0 + rh * 1.2f), (int)(u * 0.5f), 0.6f, 1);
    for (int r = 0; r < C_ROWS; r++) {
        float ry = y0 + rh * (1.6f + r), cy = ry + rh / 2;
        if (r == g_csel || r == g_cap) tp_rect(g_px, g_bw, g_bh, x0 + u * 0.4f, ry + 1, x0 + w - u * 0.4f, ry + rh - 1, u * 0.35f, r == g_cap ? 0xA66E2E : 0x2E6EA6, 0.85f);
        char k[80], p[80];
        if (r < RM_NBTN) {
            text_at(rm_act_label[r], (int)(x0 + u), (int)cy, ts, 0.95f, 1);
            rm_key_str(&g_map, r, k, sizeof k); rm_pad_str(&g_map, r, p, sizeof p);
            text_at(k[0] ? k : "\xE2\x80\x94", (int)(x0 + w * 0.40f), (int)cy, ts, 0.95f, 1);
            text_at(p[0] ? p : "\xE2\x80\x94", (int)(x0 + w * 0.68f), (int)cy, ts, 0.95f, 1);
        } else if (r <= C_STICK_R) {
            int base = r == C_STICK_L ? RM_LUP : RM_RUP; char b[4][24]; size_t l = 0; k[0] = 0;
            for (int d = 0; d < 4; d++) { rm_key_str(&g_map, base + d, b[d], sizeof b[d]); char *sp = strchr(b[d], ' '); if (sp) *sp = 0; }
            l = (size_t)snprintf(k, sizeof k, "%s %s %s %s", b[0][0] ? b[0] : "-", b[2][0] ? b[2] : "-", b[1][0] ? b[1] : "-", b[3][0] ? b[3] : "-"); (void)l;
            text_at(r == C_STICK_L ? "Left stick (up left down right)" : "Right stick (up left down right)", (int)(x0 + u), (int)cy, ts, 0.95f, 1);
            text_at(k, (int)(x0 + w * 0.40f), (int)cy, ts, 0.95f, 1);
            text_at("the controller's sticks", (int)(x0 + w * 0.68f), (int)cy, ts, 0.5f, 1);
        } else if (r == C_SCOPE) {
            text_at("Changes are saved for", (int)(x0 + u), (int)cy, ts, 0.95f, 1);
            text_at(g_scope ? "\u2039  every game  \u203a" : "\u2039  this game  \u203a", (int)(x0 + w * 0.40f), (int)cy, ts, 0.95f, 1);
        } else if (r == C_RESET) text_at(g_reset_armed ? (g_scope ? "Again to reset every game's mappings" : "Again to reset this game's mappings") : "Reset to the defaults", (int)(x0 + u), (int)cy, ts, g_reset_armed ? 1.0f : 0.95f, 1);
        else text_at("Back", (int)(x0 + u), (int)cy, ts, 0.95f, 1);
    }
    const char *note = "Choose a row, then press the key or controller button for it";
    char nb[160];
    if (g_cap >= 0 && g_cap < RM_NBTN) { snprintf(nb, sizeof nb, "Press a key or controller button for %s   (Esc: keep it, Delete: none)", rm_act_label[g_cap]); note = nb; }
    else if (g_cap >= 0) { static const char *const dn[4] = {"up", "down", "left", "right"}; snprintf(nb, sizeof nb, "Press the key for %s %s   (Esc: stop, Delete: none)", g_cap == C_STICK_L ? "left stick" : "right stick", dn[g_cap_step]); note = nb; }
    text_at(note, (int)(x0 + u), (int)(y0 + rh * (C_ROWS + 2.05f)), (int)(u * 0.55f), g_cap >= 0 ? 1.0f : 0.65f, 1);
}
static void menu_draw(void) {
    float u, x0, y0, w, rh; menu_geom(&u, &x0, &y0, &w, &rh);
    tp_rect(g_px, g_bw, g_bh, 0, 0, (float)g_bw, (float)g_bh, 0, 0x000000, 0.45f);
    tp_rect(g_px, g_bw, g_bh, x0, y0, x0 + w, y0 + rh * (page_rows() + 2.7f), u * 0.8f, 0x14181E, 0.94f);
    if (g_page == 1) { controls_draw(u, x0, y0, w, rh); return; }
    text_at("Settings", (int)(x0 + u), (int)(y0 + rh * 0.8f), (int)(u * 1.05f), 1, 1);
    text_at("F2 / Esc / B closes", (int)(x0 + w - u * 9), (int)(y0 + rh * 0.8f), (int)(u * 0.55f), 0.55f, 1);
    for (int r = 0; r < M_ROWS; r++) {
        float ry = y0 + rh * (1.6f + r), cy = ry + rh / 2;
        if (r == g_sel) tp_rect(g_px, g_bw, g_bh, x0 + u * 0.4f, ry + 2, x0 + w - u * 0.4f, ry + rh - 2, u * 0.4f, 0x2E6EA6, 0.85f);
        if (r < NM) {
            int k = cur_val(r);
            text_at(k_menu[r].label, (int)(x0 + u), (int)cy, (int)(u * 0.68f), 0.95f, 1);
            char v[64]; snprintf(v, sizeof v, "\u2039  %s  \u203a%s", k_menu[r].names[k], k_menu[r].live ? "" : " *");
            text_at(v, (int)(x0 + w * 0.62f), (int)cy, (int)(u * 0.68f), 0.95f, 1);
        } else if (r == M_CONTROLS) {
            text_at("Controls (keyboard and controller buttons)  \u203a", (int)(x0 + u), (int)cy, (int)(u * 0.68f), 0.95f, 1);
        } else if (r == M_RESTART) {
            text_at(g_restart_due ? "Restart the game now to apply *" : "* applies at the next start", (int)(x0 + u), (int)cy, (int)(u * 0.68f), g_restart_due ? 1.0f : 0.5f, 1);
        } else text_at("Close", (int)(x0 + u), (int)cy, (int)(u * 0.68f), 0.95f, 1);
    }
    if (g_sel < NM && k_menu[g_sel].note) text_at(k_menu[g_sel].note, (int)(x0 + u), (int)(y0 + rh * (M_ROWS + 2.05f)), (int)(u * 0.55f), 0.65f, 1);
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
    {   // DM_OVL_DUMP=<folder>: the first picture of the controls and of the menu as BMP files (CI looks at them: its desktop is covered by the set-up screen)
        static int dumped[3]; const char *dd = getenv("DM_OVL_DUMP"); int k = g_menu ? 1 + g_page : 0;
        if (dd && *dd && !dumped[k] && (g_menu || g_visible)) {
            dumped[k] = 1; char fn[700]; snprintf(fn, sizeof fn, "%s/ovl_%s.bmp", dd, k == 2 ? "menu_controls" : k ? "menu" : "controls"); FILE *f = fopen(fn, "wb");
            if (f) {
                uint32_t isz = (uint32_t)g_bw * (uint32_t)g_bh * 4u, hdr[13] = {0};
                uint8_t fh[14] = {'B', 'M'}; uint32_t fsz = 14 + 40 + isz, off = 54; memcpy(fh + 2, &fsz, 4); memcpy(fh + 10, &off, 4);
                hdr[0] = 40; hdr[1] = (uint32_t)g_bw; hdr[2] = (uint32_t)-g_bh; hdr[3] = 1u | 32u << 16; hdr[5] = isz;
                fwrite(fh, 1, 14, f); fwrite(hdr, 1, 40, f); fwrite(g_px, 1, isz, f); fclose(f); LOG("touch controls: picture %s", fn);
            }
        }
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
    if (!g_ov) return;   // overlay_window=0
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
        if (g_page == 1) {   // Controls: a tap on a row asks for its key / button (again: stops asking); outside the panel goes back
            if (r == -2) { g_page = 0; g_cap = -1; }
            else if (r >= 0) { if (g_cap == r) g_cap = -1; else { g_cap = -1; g_csel = r; controls_activate(r); } }
        }
        else if (r == -2) ovl_menu(0);
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
