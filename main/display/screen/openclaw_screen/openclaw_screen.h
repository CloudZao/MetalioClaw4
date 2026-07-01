#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// OpenClawScreen — 暗黑主题。进入页面先检查设备在线状态，再拉会话与消息；
// 后台每 3s 自动刷新，数据未变化时不重绘 UI。
//
// 录音 / 上传都在独立 FreeRTOS task 里跑，不阻塞 LVGL。UI 上有：
//   - 上方消息列表（参考 chat_screen 的右侧绿气泡风格）
//   - 中部状态文字（"按住说话" / "已录 1.2s" / "上传中…" / "失败"…）
//   - 底部主按钮（按住录音，松开停止，按钮文字实时显示秒数）
//
// 进入页面前检查设备是否已激活；未激活时弹出不可关闭的拦截弹窗（含返回
// 按钮），背后页面内容保持可见但不可操作，并打印日志。
//
// 风险点：
//   - mic 与「唤醒词」共用 I2S 通道，录音前后必须临时关掉 wake word
//     检测，否则会和 AudioInputTask 抢 ReadAudioData。LifecycleCallback
//     里同样兜底一次，保证退出屏幕时把 wake word 还原回去。
//   - 当前设备状态非 Idle（已经在和 AI 对话）时，按钮置灰拒绝触发，避
//     免破坏正在进行的会话。
// ---------------------------------------------------------------------------
class OpenClawScreen {
 public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
