// reader_settings.c — see reader_settings.h. Pure arithmetic.
#include "reader_settings.h"

static const unsigned char k_backlight_steps[READER_BACKLIGHT_STEPS] = {
    5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100,
};
static const unsigned char k_autopage_seconds[READER_AUTOPAGE_OPTIONS] = {
    5, 10, 15, 20, 25, 30, 45, 60,
};

int reader_backlight_step_value(int index)
{
    if (index < 0) index = 0;
    if (index >= READER_BACKLIGHT_STEPS) index = READER_BACKLIGHT_STEPS - 1;
    return (int)k_backlight_steps[index];
}

int reader_backlight_step_index(int percent)
{
    int best = 0;
    int best_gap = 0x7FFFFFFF;
    for (int i = 0; i < READER_BACKLIGHT_STEPS; ++i) {
        int step = (int)k_backlight_steps[i];
        int gap = percent > step ? percent - step : step - percent;
        if (gap < best_gap) {
            best_gap = gap;
            best = i;
        }
    }
    return best;
}

int reader_autopage_option(int index)
{
    if (index < 0) index = 0;
    if (index >= READER_AUTOPAGE_OPTIONS) index = READER_AUTOPAGE_OPTIONS - 1;
    return (int)k_autopage_seconds[index];
}

int reader_autopage_index(int seconds)
{
    int best = 0;
    int best_gap = 0x7FFFFFFF;
    for (int i = 0; i < READER_AUTOPAGE_OPTIONS; ++i) {
        int step = (int)k_autopage_seconds[i];
        int gap = seconds > step ? seconds - step : step - seconds;
        if (gap < best_gap) {
            best_gap = gap;
            best = i;
        }
    }
    return best;
}
