#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// LevelScreen
//
// 水平仪 App。720x720 面板上展示一个同心圆水平仪：
//   - 外大圆（刻度参考圈）+ 内小圆（中心目标）+ 十字基准线
//   - 中心“气泡”根据 SC7A20H（I2C 0x19）实时姿态平滑移动
//   - 底部数显面板：ax/ay/az(mg)、Pitch/Roll（°）、水平判定状态
//   - “校准” 按钮 -> 弹窗提示用户水平放置 -> 取样平均 -> 保存到 NVS
//   - “重置校准” 把已保存的偏移清零，回到出厂默认
//
// 生命周期：
//   - LOAD 时确保 SC7A20H 已上电 / 配置完毕；启动 50ms 采样 timer
//   - UNLOAD 时停掉 timer，避免离开页面后还在跑 I2C
// ---------------------------------------------------------------------------
class LevelScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
