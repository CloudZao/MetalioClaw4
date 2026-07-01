#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// GpsScreen
//
// 卫星定位 App。展示 GpsService 解析出来的 GNSS 数据。
//
// 生命周期：GPS 模块靠 TCA9555 的 GPS_POWER 线供电；只在用户进入本屏幕
// 期间通电，离开就断电（节流 + 避免常驻冷启动）。GpsService 自身从不
// 触碰这条电源线 —— 这是 GpsScreen 的职责。
// ---------------------------------------------------------------------------
class GpsScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
