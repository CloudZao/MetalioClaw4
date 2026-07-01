#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// ChatScreen
//
// 暗黑主题聊天会话页面：左侧深灰气泡=对方（assistant / system），右侧
// 墨绿气泡=自己（user）。消息通过 LVAdapterDisplay::SetChatMessage 注入，
// 由 Application 的对话流程驱动；屏幕本身不持有任何长连接 / 网络逻辑。
//
// 进入页面前检查设备是否已激活；未激活时弹出不可关闭的拦截弹窗（含返回
// 按钮），背后页面内容保持可见但不可操作，并打印日志。
//
// 设计要点（720x720 屏）：
//   - 顶部 88px header（标题 "聊天" + 旁侧状态 + 右侧 "清空"），与
//     vibrate / bluetooth 页保持一致的视觉规范；状态随设备态切换
//     （待唤醒 / 聆听中 / 讲话中）。
//   - 右下角 80×80 白色圆形按钮（内嵌 64×64 图标 + 阴影），点击切换对话状态。
//   - 中部消息列表占满剩余空间，垂直滚动，自动滚到最新。
//   - 气泡最大宽度 = 屏宽 * 72%（≈ 518px），文字超长换行。
//   - 最多保留 kMaxMessages 条历史，超出滚出窗口的会被释放，避免长会话
//     堆 PSRAM。
//   - 屏幕未在前台时（s_screen == nullptr）AddMessage 直接丢弃，不在后台
//     缓存消息，符合参考实现的语义。
// ---------------------------------------------------------------------------

enum class ChatMsgDir : uint8_t {
    Left,   // assistant / system -> 左侧深灰气泡
    Right,  // user                -> 右侧墨绿气泡
};

class ChatScreen {
 public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // 由 LVAdapterDisplay::SetChatMessage 在持有 esp_lv_adapter 锁后调用。
    // 屏幕未加载时 no-op。
    static void AddMessage(const char* text, ChatMsgDir dir);

    // 清空消息列表（"清空" 按钮 / 外部调用）。
    static void ClearMessages();

    static bool IsActive();

    // 刷新 header 标题旁的设备聊天状态；屏幕未加载时 no-op。
    static void RefreshDeviceState();
};
