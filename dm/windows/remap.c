// Button mapping (remap.h)
#include "remap.h"
#include "dm_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *const rm_act_name[RM_NACT] = {"a", "b", "x", "y", "l", "r", "zl", "zr", "ls", "rs", "plus", "minus", "up", "down", "left", "right",
                                          "lstick_up", "lstick_down", "lstick_left", "lstick_right", "rstick_up", "rstick_down", "rstick_left", "rstick_right"};
const char *const rm_act_label[RM_NACT] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Left stick press", "Right stick press", "+", "\xE2\x88\x92",
                                           "D-pad up", "D-pad down", "D-pad left", "D-pad right",
                                           "Left stick up", "Left stick down", "Left stick left", "Left stick right", "Right stick up", "Right stick down", "Right stick left", "Right stick right"};
const char *const rm_src_name[RM_NSRC] = {"a", "b", "x", "y", "lb", "rb", "lt", "rt", "ls", "rs", "start", "back", "up", "down", "left", "right"};
const char *const rm_src_label[RM_NSRC] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "Menu", "View", "D\xE2\x86\x91", "D\xE2\x86\x93", "D\xE2\x86\x90", "D\xE2\x86\x92"};
static const uint64_t k_btn[RM_NBTN] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y, DM_BTN_L, DM_BTN_R, DM_BTN_ZL, DM_BTN_ZR, DM_BTN_LS, DM_BTN_RS,
                                        DM_BTN_PLUS, DM_BTN_MINUS, DM_BTN_UP, DM_BTN_DOWN, DM_BTN_LEFT, DM_BTN_RIGHT};
uint64_t rm_act_btn(int a) { return a >= 0 && a < RM_NBTN ? k_btn[a] : 0; }

// Windows virtual-key codes (winuser.h), so this file builds anywhere
enum { VK_BACK_ = 0x08, VK_TAB_ = 0x09, VK_RETURN_ = 0x0D, VK_SHIFT_ = 0x10, VK_CONTROL_ = 0x11, VK_MENU_ = 0x12, VK_ESCAPE_ = 0x1B, VK_SPACE_ = 0x20,
       VK_PRIOR_ = 0x21, VK_NEXT_ = 0x22, VK_END_ = 0x23, VK_HOME_ = 0x24, VK_LEFT_ = 0x25, VK_UP_ = 0x26, VK_RIGHT_ = 0x27, VK_DOWN_ = 0x28,
       VK_INSERT_ = 0x2D, VK_DELETE_ = 0x2E, VK_NUMPAD0_ = 0x60, VK_MULTIPLY_ = 0x6A, VK_ADD_ = 0x6B, VK_SUBTRACT_ = 0x6D, VK_DECIMAL_ = 0x6E, VK_DIVIDE_ = 0x6F,
       VK_F1_ = 0x70, VK_F2_ = 0x71, VK_F11_ = 0x7A, VK_F24_ = 0x87, VK_LSHIFT_ = 0xA0, VK_RSHIFT_ = 0xA1, VK_LCONTROL_ = 0xA2, VK_RCONTROL_ = 0xA3,
       VK_OEM_1_ = 0xBA, VK_OEM_PLUS_ = 0xBB, VK_OEM_COMMA_ = 0xBC, VK_OEM_MINUS_ = 0xBD, VK_OEM_PERIOD_ = 0xBE, VK_OEM_2_ = 0xBF };
static const struct { int vk; const char *name; } k_names[] = {
    {VK_BACK_, "Backspace"}, {VK_TAB_, "Tab"}, {VK_RETURN_, "Enter"}, {VK_SHIFT_, "Shift"}, {VK_CONTROL_, "Ctrl"}, {VK_MENU_, "Alt"}, {VK_ESCAPE_, "Esc"},
    {VK_SPACE_, "Space"}, {VK_PRIOR_, "PageUp"}, {VK_NEXT_, "PageDown"}, {VK_END_, "End"}, {VK_HOME_, "Home"}, {VK_LEFT_, "Left"}, {VK_UP_, "Up"},
    {VK_RIGHT_, "Right"}, {VK_DOWN_, "Down"}, {VK_INSERT_, "Insert"}, {VK_DELETE_, "Delete"}, {VK_MULTIPLY_, "Num*"}, {VK_ADD_, "Num+"},
    {VK_SUBTRACT_, "Num-"}, {VK_DECIMAL_, "Num."}, {VK_DIVIDE_, "Num/"}, {VK_LSHIFT_, "LShift"}, {VK_RSHIFT_, "RShift"}, {VK_LCONTROL_, "LCtrl"},
    {VK_RCONTROL_, "RCtrl"}, {VK_OEM_1_, ";"}, {VK_OEM_PLUS_, "+"}, {VK_OEM_COMMA_, ","}, {VK_OEM_MINUS_, "-"}, {VK_OEM_PERIOD_, "."}, {VK_OEM_2_, "/"},
};
const char *rm_vk_name(int vk, char *b, size_t n) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) snprintf(b, n, "%c", vk);
    else if (vk >= VK_NUMPAD0_ && vk <= VK_NUMPAD0_ + 9) snprintf(b, n, "Num%d", vk - VK_NUMPAD0_);
    else if (vk >= VK_F1_ && vk <= VK_F24_) snprintf(b, n, "F%d", vk - VK_F1_ + 1);
    else {
        for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) if (k_names[i].vk == vk) { snprintf(b, n, "%s", k_names[i].name); return b; }
        snprintf(b, n, "0x%02X", vk);
    }
    return b;
}
int rm_vk_parse(const char *s) {
    if (!s || !*s) return 0;
    if (!s[1] && ((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return *s >= 'a' ? *s - 32 : *s;
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) if (!strcasecmp(k_names[i].name, s)) return k_names[i].vk;
    if (!strncasecmp(s, "Num", 3) && s[3] >= '0' && s[3] <= '9' && !s[4]) return VK_NUMPAD0_ + s[3] - '0';
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') { int f = atoi(s + 1); if (f >= 1 && f <= 24) return VK_F1_ + f - 1; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { long v = strtol(s, NULL, 16); if (v > 0 && v < 256) return (int)v; }
    return 0;
}
int rm_bindable_vk(int vk) { return vk > 0 && vk < 256 && vk != VK_F1_ && vk != VK_F2_ && vk != VK_F11_ && vk != VK_ESCAPE_; }

void rm_defaults(rm_map *m, int lab) {
    memset(m, 0, sizeof *m);
    static const int k[RM_NACT][RM_KEYS] = {{'Z'}, {'X'}, {'C'}, {'V'}, {'E'}, {'U'}, {'Q'}, {'O'}, {'F'}, {'H'},
        {VK_RETURN_, VK_OEM_PLUS_, VK_ADD_}, {VK_BACK_, VK_OEM_MINUS_, VK_SUBTRACT_}, {VK_UP_}, {VK_DOWN_}, {VK_LEFT_}, {VK_RIGHT_},
        {'W'}, {'S'}, {'A'}, {'D'}, {'I'}, {'K'}, {'J'}, {'L'}};
    memcpy(m->key, k, sizeof k);
    // sources a b x y lb rb lt rt ls rs start back up down left right; actions a=0 b=1 x=2 y=3
    static const int pos[RM_NSRC] = {1, 0, 3, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, byl[RM_NSRC] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    memcpy(m->pad, lab ? byl : pos, sizeof pos);
}
static int act_of(const char *s) { for (int i = 0; i < RM_NACT; i++) if (!strcasecmp(rm_act_name[i], s)) return i; return -1; }
static int src_of(const char *s) { for (int i = 0; i < RM_NSRC; i++) if (!strcasecmp(rm_src_name[i], s)) return i; return -1; }
int rm_set(rm_map *m, const char *key, const char *val) {
    if (!strncasecmp(key, "key.", 4)) {
        int a = act_of(key + 4); if (a < 0) return 0;
        int ks[RM_KEYS] = {0}, n = 0; char buf[128]; snprintf(buf, sizeof buf, "%s", val ? val : "");
        for (char *t = strtok(buf, " ,\t"); t; t = strtok(NULL, " ,\t")) {
            if (!strcasecmp(t, "none")) continue;
            int vk = rm_vk_parse(t); if (!rm_bindable_vk(vk)) return 0;
            if (n < RM_KEYS) ks[n++] = vk;
        }
        memcpy(m->key[a], ks, sizeof ks); return 1;
    }
    if (!strncasecmp(key, "pad.", 4)) {
        int s = src_of(key + 4); if (s < 0) return 0;
        if (!val || !*val || !strcasecmp(val, "none")) { m->pad[s] = -1; return 1; }
        int a = act_of(val); if (a < 0 || a >= RM_NBTN) return 0;
        m->pad[s] = a; return 1;
    }
    return 0;
}
static int has_key(const rm_map *m, int a, int vk) { for (int k = 0; k < RM_KEYS; k++) if (vk && m->key[a][k] == vk) return 1; return 0; }
uint64_t rm_key_buttons(const rm_map *m, int vk) { uint64_t b = 0; for (int a = 0; a < RM_NBTN; a++) if (has_key(m, a, vk)) b |= k_btn[a]; return b; }
void rm_key_dirs(const rm_map *m, int vk, int d[8]) { for (int i = 0; i < 8; i++) d[i] = has_key(m, RM_LUP + i, vk); }
uint64_t rm_pad_buttons(const rm_map *m, uint32_t src) { uint64_t b = 0; for (int s = 0; s < RM_NSRC; s++) if ((src >> s & 1) && m->pad[s] >= 0) b |= rm_act_btn(m->pad[s]); return b; }
int rm_bind_key(rm_map *m, int act, int vk) {
    if (act < 0 || act >= RM_NACT || !rm_bindable_vk(vk)) return -1;
    int old = m->key[act][0], other = -1;
    if (has_key(m, act, vk)) old = 0;   // already one of act's keys: act keeps only it
    for (int a = 0; a < RM_NACT; a++) {
        if (a == act || !has_key(m, a, vk)) continue;
        for (int k = 0; k < RM_KEYS; k++) if (m->key[a][k] == vk) m->key[a][k] = old && !has_key(m, a, old) ? old : 0;
        int t[RM_KEYS] = {0}, n = 0; for (int k = 0; k < RM_KEYS; k++) if (m->key[a][k]) t[n++] = m->key[a][k];   // packed to the front
        memcpy(m->key[a], t, sizeof t); if (other < 0) other = a;
    }
    memset(m->key[act], 0, sizeof m->key[act]); m->key[act][0] = vk;
    return other;
}
int rm_bind_pad(rm_map *m, int act, int src) {
    if (act < 0 || act >= RM_NBTN || src < 0 || src >= RM_NSRC) return 0;
    int old = m->pad[src];
    for (int s = 0; s < RM_NSRC; s++) if (s != src && m->pad[s] == act) m->pad[s] = old;
    m->pad[src] = act; return 1;
}
void rm_clear(rm_map *m, int act) {
    if (act < 0 || act >= RM_NACT) return;
    memset(m->key[act], 0, sizeof m->key[act]);
    for (int s = 0; s < RM_NSRC; s++) if (m->pad[s] == act) m->pad[s] = -1;
}
void rm_key_str(const rm_map *m, int a, char *o, size_t n) {
    size_t l = 0; o[0] = 0;
    for (int k = 0; k < RM_KEYS; k++) if (m->key[a][k]) { char b[24]; l += (size_t)snprintf(o + l, l < n ? n - l : 0, "%s%s", l ? " " : "", rm_vk_name(m->key[a][k], b, sizeof b)); if (l >= n) { o[n - 1] = 0; return; } }
}
void rm_pad_str(const rm_map *m, int a, char *o, size_t n) {
    size_t l = 0; o[0] = 0;
    for (int s = 0; s < RM_NSRC; s++) if (m->pad[s] == a) { l += (size_t)snprintf(o + l, l < n ? n - l : 0, "%s%s", l ? ", " : "", rm_src_label[s]); if (l >= n) { o[n - 1] = 0; return; } }
}
