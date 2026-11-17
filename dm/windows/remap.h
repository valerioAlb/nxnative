// Button mapping for the keyboard and Xbox (XInput) controllers (pure C: tests/t_remap.c runs it on any host).
// Actions: the Switch's 16 buttons, then the two sticks' four directions (keyboard only). Sources: the Xbox controller's 16 inputs (triggers count as buttons).
// settings.ini keys: key.<action>=<key names, space separated> (Z, Enter, Num+, F5, 0xBA ...), pad.<source>=<action or none>
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { RM_NBTN = 16, RM_NACT = 24, RM_NSRC = 16, RM_KEYS = 3 };
enum { RM_LUP = 16, RM_LDOWN, RM_LLEFT, RM_LRIGHT, RM_RUP, RM_RDOWN, RM_RLEFT, RM_RRIGHT };
typedef struct { int key[RM_NACT][RM_KEYS]; int pad[RM_NSRC]; } rm_map;   // key: virtual-key codes (0 none); pad: the action of each source, -1 none

extern const char *const rm_act_name[RM_NACT], *const rm_act_label[RM_NACT];
extern const char *const rm_src_name[RM_NSRC], *const rm_src_label[RM_NSRC];
uint64_t rm_act_btn(int act);                          // DM_BTN_* of a button action (0 for a stick direction)
void rm_defaults(rm_map *m, int abxy_by_label);        // Ryujinx's keys; Xbox buttons by position (Xbox A = the Switch's B) unless by label
int rm_set(rm_map *m, const char *key, const char *val);   // one settings.ini line → 1 when it was a mapping (key.* / pad.*) and valid
uint64_t rm_key_buttons(const rm_map *m, int vk);
void rm_key_dirs(const rm_map *m, int vk, int dirs[8]);    // dirs[d] = 1 when vk is bound to stick direction RM_LUP + d
uint64_t rm_pad_buttons(const rm_map *m, uint32_t src);    // src: bit i = source i held
int rm_bind_key(rm_map *m, int act, int vk);   // act gets vk alone; an action that had vk gets act's old key (a swap) → that action, or -1
int rm_bind_pad(rm_map *m, int act, int src);  // source src becomes act; the sources that were act take src's old action (a swap) → 1 when done
void rm_clear(rm_map *m, int act);             // no key and no controller input for act
void rm_key_str(const rm_map *m, int act, char *out, size_t n);   // "Z Enter" (the settings value); "" for none
void rm_pad_str(const rm_map *m, int act, char *out, size_t n);   // "Xbox B, Xbox Y" (for the screen)
const char *rm_vk_name(int vk, char *buf, size_t n);
int rm_vk_parse(const char *s);                // → the virtual-key code, 0 when unknown
int rm_bindable_vk(int vk);                    // 0 for the program's own keys (F1, F2, F11, Esc)
