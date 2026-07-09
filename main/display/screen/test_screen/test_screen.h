#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// TestScreen — 硬件测试入口（自动测试 / 压力测试）
//
// 自动测试项逻辑在独立文件中，由 auto_test_screen 组装：
//   vibrate_motor_test  震动马达开关，人工确认 pass/fail
//   audio_test          按住录音 / 松开播放，人工确认 pass/fail
//   camera_test         读取 OV2710 传感器 ID，匹配即 pass
//   gps_test            显示 HDOP，收到 NMEA 即 pass
//   sc7a20h_test        加速度计读数
//   qmc6309_test        磁力计读数
//   sd_card_test        SD 卡容量
// ---------------------------------------------------------------------------
class TestScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // 供 home_screen 调用的统一入口，避免在外部重复写 launch 样板代码。
    static void LaunchFromHome(screen_lifecycle_cb_t lifecycle_cb);
};
