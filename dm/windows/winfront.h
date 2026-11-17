// The Windows front end's parts beside front_windows.c: the settings file (settings_windows.c) and the touch controls' window (overlay_windows.c)
#pragma once
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <stdint.h>
#include "remap.h"

// settings.ini in %LOCALAPPDATA%\nxnative\<title ID>\: KEY=VALUE lines (# comments), written back when a setting changes in the program (F1, the menu).
// A key unset there comes from %LOCALAPPDATA%\nxnative\settings.ini (every game's), then the environment's DM_<KEY in capitals> (env.txt)
void st_load(const char *dir);
const char *st_get(const char *key);               // the value, or NULL
const char *st_str(const char *key, const char *def);
double st_num(const char *key, double def);
void st_put(const char *key, const char *val);     // set and save
int st_each(int i, const char **key, const char **val);   // the i-th setting (every game's, then this game's) → 1, 0 past the end
void st_put_scope(const char *key, const char *val, int all_games);   // val NULL removes it; all_games: the shared file (and the game's own key goes)

// The touch controls over the game (layered window owned by the game's window)
void ovl_init(HWND owner);
void ovl_follow(void);                                   // each turn of the main loop: follow the window, redraw what changed
int ovl_wndproc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res);   // the game window's pointer / mouse messages → 1 when taken (publish the input then)
void ovl_input(uint64_t *buttons, int32_t ax[4]);         // the controls' buttons and sticks (0 when no finger is on them)
void ovl_other_input(void);                              // a key or the controller was used: the controls hide (auto mode)
void ovl_toggle(void);                                   // F1
void ovl_picture(const int pic[4]);                      // the game picture's rectangle in the window, for the game's touch screen
void ovl_badge(const char *text);                        // a small "still working" note at the top with a spinner (NULL: none)

// The settings menu (F2; overlay_windows.c): touch, mouse, keyboard (arrows, Enter, Esc) and the controller (D-pad, A, B) move in it; the game gets no input meanwhile
void ovl_menu(int open);                 // open / close
int ovl_menu_open(void);
int ovl_menu_key(WPARAM vk);             // a key while open → 1 when taken
void ovl_menu_pad(uint32_t pressed);     // controller inputs pressed since the last call (remap.h's sources, bit i = source i), while open
const rm_map *ovl_map(void);              // the button mapping (remap.h's rm_map), loaded from the settings and changed by the menu's Controls page
void ovl_fps(const char *text);          // the frame-rate note at the top left (NULL: none)
void ovl_reload(void);                   // the touch settings changed
// front_windows.c: a setting the menu changed (applied at once when it can be), and the restart the menu asks for
void front_setting_changed(const char *key);
void front_restart(void);
