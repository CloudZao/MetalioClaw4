#pragma once

#include "lvgl.h"
#include "screen_util.h"

class StandbyScreen {
public:
    // 全屏待机页：显示大号时分秒、日期星期、电量等；点击或侧面键短按返回首页。
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // 切到待机页（须在 LVGL 线程调用）。
    static void Show();
    // 从待机回到首页（须在 LVGL 线程调用）。
    static void ReturnHome();
};
