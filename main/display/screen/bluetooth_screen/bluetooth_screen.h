#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// BluetoothScreen
//
// 蓝牙音频模块 AT 命令控制界面，通过 SimpleUart 与 BT 芯片通信。
// 封装协议中的三种模式：
//   模式1: AT+RX=2 -> AT+MODE=1  (接收模式)
//   模式2: AT+TX=1 -> AT+MODE=2  (发射/配对模式，含扫描与连接)
//   模式3: AT+RX=1 -> AT+MODE=3  (音乐接收模式)
// ---------------------------------------------------------------------------
class BluetoothScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // 设备开机时调用：向 BT 模块发送 `AT+RX=2 / AT+MODE=1` 把它切到接收
    // 模式作为默认配置。可以在 UART 已经初始化但 LVGL / UI 尚未启动时
    // 调用（内部启动独立 FreeRTOS task 发送命令，所有 UI 写操作都有
    // s_screen_active 守卫，会自动 no-op）。同时把模块默认模式记录到
    // 内部状态，用户后续进入蓝牙设置页时 UI 会直接显示 "模式1 已激活"。
    static void ApplyDefaultMode();
};
