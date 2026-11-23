// reader_settings.h — value ladders shared by the settings UI and the keys.
//
// Pure tables + rounding, no LVGL and no NVS: the settings pages draw the dots,
// the keys step them, and the host tests pin the rounding. Keeping the ladders
// here means a value written by an older firmware (the old brightness steps were
// 25/50/75/100) snaps onto the current one instead of being rejected.
#pragma once

// Brightness: a 5 % floor the panel is still readable at in a dark room, then
// 10 % steps.
#define READER_BACKLIGHT_STEPS 11
int reader_backlight_step_value(int index);      // clamped to the ladder
int reader_backlight_step_index(int percent);    // nearest ladder position

// Auto page-turn intervals in seconds. The setting picks the INTERVAL; whether
// pages actually turn is the reading session's own on/off state, toggled from
// the reading screen.
#define READER_AUTOPAGE_OPTIONS 8
int reader_autopage_option(int index);           // clamped to the table
int reader_autopage_index(int seconds);          // nearest table position
