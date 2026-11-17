// remap.c's mapping: the defaults, settings lines, swaps
#include "../remap.h"
#include "dm_input.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int main(void) {
    rm_map m; rm_defaults(&m, 0); char s[64];
    CHECK(rm_key_buttons(&m, 'Z') == DM_BTN_A); CHECK(rm_key_buttons(&m, 0x0D) == DM_BTN_PLUS); CHECK(rm_key_buttons(&m, 'W') == 0);
    int d[8]; rm_key_dirs(&m, 'W', d); CHECK(d[0] == 1 && d[1] == 0); rm_key_dirs(&m, 'J', d); CHECK(d[6] == 1);
    CHECK(rm_pad_buttons(&m, 1u << 0) == DM_BTN_B);   // Xbox A by position = the Switch's B
    rm_defaults(&m, 1); CHECK(rm_pad_buttons(&m, 1u << 0) == DM_BTN_A);
    rm_defaults(&m, 0);
    CHECK(rm_set(&m, "key.a", "Space K")); CHECK(rm_key_buttons(&m, 0x20) == DM_BTN_A && rm_key_buttons(&m, 'K') == DM_BTN_A && rm_key_buttons(&m, 'Z') == 0);
    CHECK(!rm_set(&m, "key.a", "F1")); CHECK(!rm_set(&m, "key.nope", "Z")); CHECK(!rm_set(&m, "touch.a", "1,2"));
    CHECK(rm_set(&m, "pad.lt", "zr") && rm_pad_buttons(&m, 1u << 6) == DM_BTN_ZR); CHECK(rm_set(&m, "pad.back", "none") && rm_pad_buttons(&m, 1u << 11) == 0);
    CHECK(!rm_set(&m, "pad.a", "lstick_up"));
    rm_defaults(&m, 0);
    int o = rm_bind_key(&m, 0, 'X');   // A gets X: B (which had X) gets Z
    CHECK(o == 1 && rm_key_buttons(&m, 'X') == DM_BTN_A && rm_key_buttons(&m, 'Z') == DM_BTN_B);
    rm_key_str(&m, 11, s, sizeof s); CHECK(!strcmp(s, "Backspace - Num-"));
    o = rm_bind_key(&m, 10, 0xBD);     // + gets '-': minus keeps Backspace and gets Enter
    CHECK(o == 11 && rm_key_buttons(&m, 0xBD) == DM_BTN_PLUS && rm_key_buttons(&m, 0x08) == DM_BTN_MINUS && rm_key_buttons(&m, 0x0D) == DM_BTN_MINUS);
    CHECK(rm_bind_key(&m, 0, 0x7A) == -1);   // F11 is the program's
    rm_defaults(&m, 0);
    CHECK(rm_bind_pad(&m, 0, 0));      // Xbox A → Switch A: Xbox B (which was A) gets B
    CHECK(rm_pad_buttons(&m, 1u << 0) == DM_BTN_A && rm_pad_buttons(&m, 1u << 1) == DM_BTN_B);
    rm_pad_str(&m, 0, s, sizeof s); CHECK(!strcmp(s, "A"));
    rm_clear(&m, 0); CHECK(rm_pad_buttons(&m, 1u << 0) == 0 && rm_key_buttons(&m, 'Z') == 0);
    CHECK(rm_vk_parse("num+") == 0x6B && rm_vk_parse("f5") == 0x74 && rm_vk_parse("0xBA") == 0xBA && rm_vk_parse("q") == 'Q' && rm_vk_parse("Num7") == 0x67);
    char b[16]; CHECK(!strcmp(rm_vk_name(0x67, b, sizeof b), "Num7") && !strcmp(rm_vk_name(0xDE, b, sizeof b), "0xDE"));
    printf(fails ? "t_remap: %d FAILED\n" : "t_remap: PASS\n", fails); return fails != 0;
}
