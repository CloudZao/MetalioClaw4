#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// VibrateScreen
//
// 震动马达控制 App。马达接在 GPIO 22 上，通过 LEDC PWM 控制振幅：
//   - 大幅滑块（0..100%）实时改变 PWM duty
//   - 模式按钮：关闭 / 弱 / 中 / 强 / 脉冲 / 心跳
//     · 关闭/弱/中/强 把 slider 切换到对应 duty 并保持恒定
//     · 脉冲   = 500ms 周期方波（半亮半暗），强度由 slider 决定
//     · 心跳   = 1s 周期内两次短脉冲，模拟心跳
//
// 生命周期：
//   - 首次进入屏幕时懒初始化 LEDC（之后保留）
//   - LOAD / UNLOAD 都会先把 PWM duty 拉到 0、停掉 pattern timer，
//     避免人离开屏幕马达还在响。
// ---------------------------------------------------------------------------
class VibrateScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
