// The touch screen's Switch controls (touchpad.c): their layout, the fingers on them and the picture of them, with no Windows call, so the logic and the
// drawing are tested on any host (tests/t_touchpad.c). front_windows.c feeds it the window's pointers (WM_POINTER) and shows the picture in a layered
// window over the game's.
//   Coordinates: a control's centre is a fraction of the window's width (x) and height (y); its radius a fraction of the shorter side, times the scale.
#pragma once
#include <stdint.h>

enum { TP_BUTTON, TP_STICK_L, TP_STICK_R, TP_DPAD };
enum { TP_MAXCTL = 32, TP_MAXFINGER = 16 };
typedef struct { char name[12]; char label[8]; int kind; uint64_t btn; float x, y, r; } tp_ctl;
typedef struct { int used; uint32_t id; int ctl; float sx, sy; } tp_finger;   // ctl: the control the finger holds (-1: none, the game's touch screen)
typedef struct {
    tp_ctl c[TP_MAXCTL]; int n;
    float opacity, scale;
    tp_finger f[TP_MAXFINGER];
    uint64_t btn; int32_t ax[4];   // the controls' output: buttons, left x y, right x y (±32767, y up)
    int dirty;                     // the picture changed since tp_render
} tp_pad;

void tp_init(tp_pad *p);   // the default layout (Switch-like: left stick and D-pad left, the face buttons and the right stick right, shoulders at the top)
// One setting line's key and value: "opacity" 0..1, "scale" 0.5..2, "<name>" "x,y[,r]" (a control moved / resized; r 0 hides it). → 1 when the key was one of these
int tp_set(tp_pad *p, const char *key, const char *val);
int tp_find(const tp_pad *p, const char *name);
// A finger (any id) down / moved / up at (px, py) in a w x h window. down → 1 when it landed on a control (the caller keeps it from the game's touch screen)
int tp_down(tp_pad *p, uint32_t id, float px, float py, int w, int h);
int tp_move(tp_pad *p, uint32_t id, float px, float py, int w, int h);   // → 1 when the finger holds a control
int tp_up(tp_pad *p, uint32_t id);                                        // → 1 when the finger held a control
void tp_release_all(tp_pad *p);
// The picture: premultiplied BGRA (0xAARRGGBB words), w x h, drawn over what px holds (cleared by the caller). pressed controls brighter
void tp_render(tp_pad *p, uint32_t *px, int w, int h);
// The label's place for the caller's text: centre and size in pixels of control i
void tp_label_box(const tp_pad *p, int i, int w, int h, int *cx, int *cy, int *size);
// Drawing primitives (also the loader's spinner): a filled anti-aliased disc / ring, colour 0xRRGGBB at alpha a (0..1), composited over px
void tp_disc(uint32_t *px, int w, int h, float cx, float cy, float r, uint32_t rgb, float a);
void tp_ring(uint32_t *px, int w, int h, float cx, float cy, float r, float thick, uint32_t rgb, float a);
void tp_rect(uint32_t *px, int w, int h, float x0, float y0, float x1, float y1, float rad, uint32_t rgb, float a);   // rounded rectangle
