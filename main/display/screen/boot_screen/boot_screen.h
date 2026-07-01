#pragma once

#include "lvgl.h"

class BootScreen {
public:
    // Create a fullscreen "boot" page (black background with text).
    // Returns the created LVGL screen object (parent = NULL).
    static lv_obj_t* Create();
};
