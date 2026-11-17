// touchpad.c on any host: cc -I dm/host -I dm/windows dm/windows/tests/t_touchpad.c dm/windows/touchpad.c -lm && ./a.out
#include "touchpad.h"
#include "dm_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)
static void at(tp_pad *p, const char *n, int w, int h, float *x, float *y) { int cx, cy, s; tp_label_box(p, tp_find(p, n), w, h, &cx, &cy, &s); *x = (float)cx; *y = (float)cy; }
int main(void) {
    tp_pad p; tp_init(&p); int W = 1920, H = 1080; float x, y;
    // a button: press, slide to its neighbour, release
    at(&p, "a", W, H, &x, &y); CHECK(tp_down(&p, 7, x, y, W, H)); CHECK(p.btn == DM_BTN_A);
    at(&p, "b", W, H, &x, &y); CHECK(tp_move(&p, 7, x, y, W, H)); CHECK(p.btn == DM_BTN_B);
    CHECK(tp_up(&p, 7)); CHECK(p.btn == 0);
    // multitouch: left stick pushed right, ZR held, a touch elsewhere goes to the game
    at(&p, "lstick", W, H, &x, &y); CHECK(tp_down(&p, 1, x, y, W, H)); CHECK(p.ax[0] == 0 && p.ax[1] == 0);
    CHECK(tp_move(&p, 1, x + 500, y, W, H)); CHECK(p.ax[0] > 32700 && abs(p.ax[1]) < 200);
    CHECK(tp_move(&p, 1, x, y - 30, W, H)); CHECK(p.ax[1] > 1000 && p.ax[1] < DM_STICK_MAX && abs(p.ax[0]) < 200);
    at(&p, "zr", W, H, &x, &y); CHECK(tp_down(&p, 2, x, y, W, H)); CHECK(p.btn == DM_BTN_ZR);
    CHECK(!tp_down(&p, 3, W / 2.0f, H / 2.0f, W, H));
    CHECK(tp_up(&p, 1)); CHECK(p.ax[0] == 0 && p.ax[1] == 0); CHECK(p.btn == DM_BTN_ZR);
    CHECK(!tp_up(&p, 3)); CHECK(tp_up(&p, 2)); CHECK(p.btn == 0);
    // the D-pad: up-right diagonal, then down
    at(&p, "dpad", W, H, &x, &y); CHECK(tp_down(&p, 4, x + 40, y - 40, W, H)); CHECK(p.btn == (DM_BTN_UP | DM_BTN_RIGHT));
    tp_move(&p, 4, x + 2, y + 60, W, H); CHECK(p.btn == DM_BTN_DOWN); tp_up(&p, 4);
    // settings: move A to the top-left, hide B, opacity
    CHECK(tp_set(&p, "a", "0.5,0.5,0.1")); CHECK(tp_set(&p, "b", "0,0,0")); CHECK(tp_set(&p, "opacity", "0.3")); CHECK(!tp_set(&p, "nope", "1"));
    CHECK(tp_down(&p, 5, 540, 540, W, H) && p.btn == DM_BTN_A); tp_up(&p, 5);
    at(&p, "b", W, H, &x, &y); CHECK(!tp_down(&p, 6, x, y, W, H) || p.btn != DM_BTN_B); tp_release_all(&p);
    // the picture: transparent where no control is, drawn on the controls, a portrait window too
    tp_init(&p); uint32_t *px = calloc((size_t)W * H, 4); tp_render(&p, px, W, H);
    CHECK(px[(size_t)(H / 2) * W + W / 2] == 0);
    at(&p, "a", W, H, &x, &y); uint32_t v = px[(size_t)y * W + (int)x]; CHECK((v >> 24) > 20);
    int Hp = 1920, Wp = 1080; uint32_t *pp = calloc((size_t)Wp * Hp, 4); tp_render(&p, pp, Wp, Hp); at(&p, "x", Wp, Hp, &x, &y); CHECK(x > 0 && x < Wp && y > 0 && y < Hp);
    free(px); free(pp);
    printf("t_touchpad: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
