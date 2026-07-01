#pragma once

#include "lvgl.h"

class Game2048 {
public:
    // Create a 720x720 fullscreen 2048 game page.
    // Returns a new LVGL screen object (parent = NULL); the caller is
    // responsible for loading it via lv_screen_load().
    // The page contains a "返回" button that navigates back to HomeScreen.
    static lv_obj_t* Create();
};
