#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// NetworkScreen
//
// 720x720 网络配置界面：
//   - 进入页面时停掉 WifiStation 单例（esp_wifi_deinit），自己初始化 STA
//     栈，避免 WifiStation 的 SCAN_DONE / STA_DISCONNECTED 自动重连逻辑
//     与用户手动选 / 连过程互相打架。
//   - 自动扫描附近 AP，按信号强度排序列出，带 RSSI（dBm）和加密图标。
//   - 点击列表项弹出密码键盘（开放网络无需密码直接连接）。
//   - 连接结果通过事件通知反馈到 UI；成功时调用
//     SsidManager::AddSsid 保存到 NVS。
//   - 已保存 WiFi 列表（来自 SsidManager::GetSsidList）支持「置为默认」、
//     「删除」、「清空」操作。
//   - 「网络切换」Tab：开关在 WiFi / 4G 间切换，提示重启后调用
//     DualNetworkBoard::SwitchNetworkType()。
//   - 当设备处于 4G 模式时，「附近 WiFi」「已保存 WiFi」两个 Tab 会被隐藏，
//     只保留「网络切换」Tab；切换回 WiFi 模式后两个 Tab 重新出现。
//   - 滑动右滑返回时（或离开屏幕时）销毁本地 STA 栈，并重启 WifiStation
//     让设备恢复原来的网络管理逻辑。
// ---------------------------------------------------------------------------
class NetworkScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
