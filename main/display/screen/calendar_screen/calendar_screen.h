#pragma once

#include "lvgl.h"

class CalendarScreen {
public:
    // Create a 720x720 fullscreen monthly calendar page.
    // - Reads current system time (year/month/day) and renders the
    //   appropriate month with weekend coloring and "today" highlight.
    // - Right-swipe gesture navigates back to HomeScreen.
    // Returns a new LVGL screen object (parent = NULL); the caller is
    // responsible for loading it via lv_screen_load().
    static lv_obj_t* Create();
};
