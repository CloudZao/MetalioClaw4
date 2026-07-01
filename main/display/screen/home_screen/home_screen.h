#pragma once

#include "lvgl.h"

class HomeScreen {
public:
    // Create a fullscreen "home" page on a 720x720 panel.
    // Lays out app icons in a 3x3 grid (phone-style) with the icon name
    // shown directly below each icon. Returns the created LVGL screen
    // object (parent = NULL).
    static lv_obj_t* Create();
    static void RefreshStatusBar();
};
