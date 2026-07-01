#pragma once

#include "lvgl.h"
#include "screen_util.h"

class InfoScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
