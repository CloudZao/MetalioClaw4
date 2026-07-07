#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// TestScreen — 硬件外设功能测试页（给产测 / 调试人员用）
//
// 列表式展示各测试项，每项逻辑在独立文件中：
//   vibrate_motor_test  震动马达开关，人工确认 pass/fail
//   audio_test          按住录音 / 松开播放，人工确认 pass/fail
//   camera_test         读取 OV2710 传感器 ID，匹配即 pass
//   gps_test            显示 HDOP，收到 NMEA 即 pass
//   sc7a20h_test   加速度计读数
//   qmc6309_test   磁力计读数
//   sd_card_test   SD 卡容量
//
// 后续新增测试项：在 items 目录加模块，并在 test_screen.cc 注册即可。
// ---------------------------------------------------------------------------
class TestScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // 供 home_screen 调用的统一入口，避免在外部重复写 launch 样板代码。
    static void LaunchFromHome(screen_lifecycle_cb_t lifecycle_cb);
};
