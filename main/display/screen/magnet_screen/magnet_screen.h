#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// MagnetScreen
//
// 磁场 App。基于 QMC6309（I2C 0x7C，7-bit）三轴磁力计的纯数据显示页：
//   - 顶栏：返回按钮 + 标题 “磁场” + 芯片在线状态
//   - 主区：X / Y / Z 三轴大字读数（LSB / mG / µT）+ 条形可视化
//   - 底部：总磁场强度 + 状态寄存器 / 帧计数 调试信息
//
// 不做指南针 / 倾角补偿 / hard-iron 校准 —— 只把传感器读到的原始数据
// 直接呈现给用户。需要罗盘功能可以另起 App。
//
// 生命周期：
//   - LOAD 时确保 QMC6309 已经 probe + configure；启动 50ms 采样 timer
//   - UNLOAD 时停掉 timer；I2C 设备保留下次再用，不重新初始化
// ---------------------------------------------------------------------------
class MagnetScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
