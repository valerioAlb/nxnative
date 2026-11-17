// The touch screen's Switch controls: layout, fingers and picture (touchpad.h). No Windows call: tests/t_touchpad.c runs it on any host.
#include "touchpad.h"
#include "dm_input.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Positions in units of the shorter side times the scale, from the left / top edge (>= 0) or the right / bottom one (< 0), so a layout keeps its shape on 16:9, 3:2 or 16:10
static const tp_ctl k_def[] = {
    {"lstick", "", TP_STICK_L, 0, 0.22f, -0.62f, 0.14f},
    {"dpad", "", TP_DPAD, 0, 0.44f, -0.22f, 0.12f},
    {"rstick", "", TP_STICK_R, 0, -0.44f, -0.22f, 0.12f},
    {"a", "A", TP_BUTTON, DM_BTN_A, -0.10f, -0.62f, 0.058f},
    {"b", "B", TP_BUTTON, DM_BTN_B, -0.22f, -0.50f, 0.058f},
    {"x", "X", TP_BUTTON, DM_BTN_X, -0.22f, -0.74f, 0.058f},
    {"y", "Y", TP_BUTTON, DM_BTN_Y, -0.34f, -0.62f, 0.058f},
    {"zl", "ZL", TP_BUTTON, DM_BTN_ZL, 0.08f, 0.09f, 0.055f},
    {"l", "L", TP_BUTTON, DM_BTN_L, 0.22f, 0.09f, 0.055f},
    {"zr", "ZR", TP_BUTTON, DM_BTN_ZR, -0.08f, 0.09f, 0.055f},
    {"r", "R", TP_BUTTON, DM_BTN_R, -0.22f, 0.09f, 0.055f},
    {"minus", "-", TP_BUTTON, DM_BTN_MINUS, 0.40f, 0.09f, 0.04f},
    {"plus", "+", TP_BUTTON, DM_BTN_PLUS, -0.40f, 0.09f, 0.04f},
    {"ls", "LS", TP_BUTTON, DM_BTN_LS, 0.07f, -0.30f, 0.045f},
    {"rs", "RS", TP_BUTTON, DM_BTN_RS, -0.07f, -0.30f, 0.045f},
};

void tp_init(tp_pad *p) {
    memset(p, 0, sizeof *p);
    p->n = (int)(sizeof k_def / sizeof k_def[0]); memcpy(p->c, k_def, sizeof k_def);
    p->opacity = 0.6f; p->scale = 1.0f; p->dirty = 1;
    for (int i = 0; i < TP_MAXFINGER; i++) p->f[i].ctl = -1;
}
int tp_find(const tp_pad *p, const char *name) { for (int i = 0; i < p->n; i++) if (!strcmp(p->c[i].name, name)) return i; return -1; }
int tp_set(tp_pad *p, const char *key, const char *val) {
    if (!strcmp(key, "opacity")) { float v = strtof(val, NULL); p->opacity = v < 0 ? 0 : v > 1 ? 1 : v; p->dirty = 1; return 1; }
    if (!strcmp(key, "scale")) { float v = strtof(val, NULL); p->scale = v < 0.5f ? 0.5f : v > 2 ? 2 : v; p->dirty = 1; return 1; }
    int i = tp_find(p, key); if (i < 0) return 0;
    float x, y, r; int k = sscanf(val, "%f , %f , %f", &x, &y, &r);
    if (k >= 2) { p->c[i].x = x; p->c[i].y = y; }
    if (k == 3) p->c[i].r = r < 0 ? 0 : r;
    p->dirty = 1; return k >= 2;
}
static float unit(const tp_pad *p, int w, int h) { return (float)(w < h ? w : h) * p->scale; }
static void centre(const tp_pad *p, int i, int w, int h, float *cx, float *cy, float *r) {
    float s = unit(p, w, h); const tp_ctl *c = &p->c[i];
    *cx = c->x >= 0 ? c->x * s : (float)w + c->x * s;
    *cy = c->y >= 0 ? c->y * s : (float)h + c->y * s;
    *r = c->r * s;
}
void tp_label_box(const tp_pad *p, int i, int w, int h, int *cx, int *cy, int *size) {
    float x, y, r; centre(p, i, w, h, &x, &y, &r); *cx = (int)lroundf(x); *cy = (int)lroundf(y); *size = (int)lroundf(r * 0.9f);
}
static int reach(int kind) { return kind == TP_BUTTON ? 125 : kind == TP_DPAD ? 125 : 150; }   // the touch area, % of the radius
static int hit(const tp_pad *p, float px, float py, int w, int h, int buttons_only) {
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < p->n; i++) {
        if (p->c[i].r <= 0 || (buttons_only && p->c[i].kind != TP_BUTTON)) continue;
        float cx, cy, r; centre(p, i, w, h, &cx, &cy, &r);
        float d = hypotf(px - cx, py - cy), lim = r * (float)reach(p->c[i].kind) / 100.0f;
        if (d <= lim && d / lim < bd) { bd = d / lim; best = i; }
    }
    return best;
}
static void output(tp_pad *p, int w, int h, float pos[][2]) {
    uint64_t b = 0; int32_t ax[4] = {0, 0, 0, 0};
    for (int k = 0; k < TP_MAXFINGER; k++) {
        tp_finger *f = &p->f[k]; if (!f->used || f->ctl < 0) continue;
        const tp_ctl *c = &p->c[f->ctl];
        if (c->kind == TP_BUTTON) { b |= c->btn; continue; }
        float cx, cy, r; centre(p, f->ctl, w, h, &cx, &cy, &r);
        float dx = pos[k][0] - cx, dy = pos[k][1] - cy, len = hypotf(dx, dy);
        if (c->kind == TP_DPAD) {
            if (len < r * 0.25f) continue;
            if (dx > 0.38f * len) b |= DM_BTN_RIGHT;
            if (dx < -0.38f * len) b |= DM_BTN_LEFT;
            if (dy > 0.38f * len) b |= DM_BTN_DOWN;
            if (dy < -0.38f * len) b |= DM_BTN_UP;
            continue;
        }
        float full = r * 0.8f, dz = full * 0.08f;   // a dead zone for the finger's jitter: the stick rests centred
        if (len < dz) continue;
        float mag = (len - dz) / (full - dz); if (mag > 1) mag = 1;
        int32_t x = (int32_t)lroundf(dx / len * mag * DM_STICK_MAX), y = (int32_t)lroundf(-dy / len * mag * DM_STICK_MAX);
        int o = c->kind == TP_STICK_L ? 0 : 2; ax[o] = x; ax[o + 1] = y;
    }
    if (b != p->btn || memcmp(ax, p->ax, sizeof ax)) p->dirty = 1;
    p->btn = b; memcpy(p->ax, ax, sizeof ax);
}
// Each finger's last position (for the sticks and the D-pad), kept beside the pad
static float g_pos[TP_MAXFINGER][2];
static int slot_of(tp_pad *p, uint32_t id) { for (int k = 0; k < TP_MAXFINGER; k++) if (p->f[k].used && p->f[k].id == id) return k; return -1; }
int tp_down(tp_pad *p, uint32_t id, float px, float py, int w, int h) {
    int k = slot_of(p, id);
    if (k < 0) { for (k = 0; k < TP_MAXFINGER && p->f[k].used; k++) {} if (k == TP_MAXFINGER) return 0; }
    int c = hit(p, px, py, w, h, 0);
    p->f[k] = (tp_finger){1, id, c, px, py}; g_pos[k][0] = px; g_pos[k][1] = py;
    output(p, w, h, g_pos);
    return c >= 0;
}
int tp_move(tp_pad *p, uint32_t id, float px, float py, int w, int h) {
    int k = slot_of(p, id); if (k < 0) return 0;
    tp_finger *f = &p->f[k]; g_pos[k][0] = px; g_pos[k][1] = py;
    if (f->ctl >= 0 && p->c[f->ctl].kind == TP_BUTTON) { int c = hit(p, px, py, w, h, 1); if (c >= 0) f->ctl = c; }   // a finger slides from button to button (A to B)
    output(p, w, h, g_pos);
    return f->ctl >= 0;
}
int tp_up(tp_pad *p, uint32_t id) {
    int k = slot_of(p, id); if (k < 0) return 0;
    int held = p->f[k].ctl >= 0; p->f[k].used = 0; p->f[k].ctl = -1;
    output(p, 1, 1, g_pos);   // the remaining fingers' sticks keep their values: recomputed at their next move with the real size; buttons are exact
    return held;
}
void tp_release_all(tp_pad *p) { for (int k = 0; k < TP_MAXFINGER; k++) { p->f[k].used = 0; p->f[k].ctl = -1; } p->btn = 0; memset(p->ax, 0, sizeof p->ax); p->dirty = 1; }

// ---- drawing ----
static inline void blend(uint32_t *d, uint32_t rgb, float a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    uint32_t v = *d; float ia = 1 - a;
    unsigned db = v & 255, dg = v >> 8 & 255, dr = v >> 16 & 255, da = v >> 24;
    unsigned sr = rgb >> 16 & 255, sg = rgb >> 8 & 255, sb = rgb & 255;
    unsigned r = (unsigned)(sr * a + dr * ia + 0.5f), g = (unsigned)(sg * a + dg * ia + 0.5f), b = (unsigned)(sb * a + db * ia + 0.5f), al = (unsigned)(255 * a + da * ia + 0.5f);
    *d = al << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}
static void bbox(int w, int h, float x0, float y0, float x1, float y1, int *a, int *b, int *c, int *d) {
    *a = (int)floorf(x0); *b = (int)floorf(y0); *c = (int)ceilf(x1); *d = (int)ceilf(y1);
    if (*a < 0) *a = 0;
    if (*b < 0) *b = 0;
    if (*c > w) *c = w;
    if (*d > h) *d = h;
}
void tp_disc(uint32_t *px, int w, int h, float cx, float cy, float r, uint32_t rgb, float a) {
    int x0, y0, x1, y1; bbox(w, h, cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        float d = hypotf(x + 0.5f - cx, y + 0.5f - cy) - r, cov = 0.5f - d; if (cov <= 0) continue; if (cov > 1) cov = 1;
        blend(&px[(size_t)y * w + x], rgb, a * cov);
    }
}
void tp_ring(uint32_t *px, int w, int h, float cx, float cy, float r, float t, uint32_t rgb, float a) {
    int x0, y0, x1, y1; bbox(w, h, cx - r - t - 1, cy - r - t - 1, cx + r + t + 1, cy + r + t + 1, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        float d = fabsf(hypotf(x + 0.5f - cx, y + 0.5f - cy) - r) - t / 2, cov = 0.5f - d; if (cov <= 0) continue; if (cov > 1) cov = 1;
        blend(&px[(size_t)y * w + x], rgb, a * cov);
    }
}
void tp_rect(uint32_t *px, int w, int h, float rx0, float ry0, float rx1, float ry1, float rad, uint32_t rgb, float a) {
    int x0, y0, x1, y1; bbox(w, h, rx0 - 1, ry0 - 1, rx1 + 1, ry1 + 1, &x0, &y0, &x1, &y1);
    float hx = (rx1 - rx0) / 2, hy = (ry1 - ry0) / 2, mx = rx0 + hx, my = ry0 + hy;
    if (rad > hx) rad = hx;
    if (rad > hy) rad = hy;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        float qx = fabsf(x + 0.5f - mx) - (hx - rad), qy = fabsf(y + 0.5f - my) - (hy - rad);
        float d = hypotf(qx > 0 ? qx : 0, qy > 0 ? qy : 0) + fminf(fmaxf(qx, qy), 0) - rad, cov = 0.5f - d; if (cov <= 0) continue; if (cov > 1) cov = 1;
        blend(&px[(size_t)y * w + x], rgb, a * cov);
    }
}
void tp_render(tp_pad *p, uint32_t *px, int w, int h) {
    float o = p->opacity; const uint32_t W = 0xFFFFFF, K = 0x000000, HI = 0x8FD8FF;
    for (int i = 0; i < p->n; i++) {
        const tp_ctl *c = &p->c[i]; if (c->r <= 0) continue;
        float cx, cy, r; centre(p, i, w, h, &cx, &cy, &r);
        float lw = r * 0.06f < 1.5f ? 1.5f : r * 0.06f;
        if (c->kind == TP_BUTTON) {
            int on = (p->btn & c->btn) != 0;
            tp_disc(px, w, h, cx, cy, r, on ? HI : K, (on ? 0.55f : 0.30f) * o);
            tp_ring(px, w, h, cx, cy, r - lw / 2, lw, W, 0.75f * o);
        } else if (c->kind == TP_DPAD) {
            tp_disc(px, w, h, cx, cy, r, K, 0.22f * o);
            float a = r * 0.34f, L = r * 0.92f;
            struct { uint64_t b; float x0, y0, x1, y1; } arm[4] = {
                {DM_BTN_UP, cx - a, cy - L, cx + a, cy - a * 0.6f}, {DM_BTN_DOWN, cx - a, cy + a * 0.6f, cx + a, cy + L},
                {DM_BTN_LEFT, cx - L, cy - a, cx - a * 0.6f, cy + a}, {DM_BTN_RIGHT, cx + a * 0.6f, cy - a, cx + L, cy + a}};
            for (int k = 0; k < 4; k++) {
                int on = (p->btn & arm[k].b) != 0;
                tp_rect(px, w, h, arm[k].x0, arm[k].y0, arm[k].x1, arm[k].y1, a * 0.35f, on ? HI : W, (on ? 0.85f : 0.45f) * o);
            }
        } else {
            int s = c->kind == TP_STICK_L ? 0 : 2;
            float kx = cx + (float)p->ax[s] / DM_STICK_MAX * r * 0.8f, ky = cy - (float)p->ax[s + 1] / DM_STICK_MAX * r * 0.8f;
            int on = p->ax[s] || p->ax[s + 1];
            for (int k = 0; k < TP_MAXFINGER; k++) if (p->f[k].used && p->f[k].ctl == i) on = 1;
            tp_disc(px, w, h, cx, cy, r, K, 0.22f * o);
            tp_ring(px, w, h, cx, cy, r - lw / 2, lw, W, 0.55f * o);
            tp_disc(px, w, h, kx, ky, r * 0.45f, on ? HI : W, (on ? 0.75f : 0.45f) * o);
        }
    }
    p->dirty = 0;
}
