#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// ChatScreen
//
// 暗黑主题聊天页，两种视图：
//   - 聊天：左右文字气泡会话（assistant/system 左，user 右）
//   - 表情：EAF 偏上播放 system/chat/{emotion}.eaf，底部共用白色字幕（新消息覆盖）
//
// 消息 / 表情由 LVAdapterDisplay 注入；未激活时全屏拦截弹窗。
// Header：返回 + 标题/状态 +「表情/聊天」切换 + 清空；右下角切换对话状态。
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

    // 按服务器完整情绪名切换 .eaf（非法名回退 neutral）。
    // 路径：S:/sdcard/system/chat/{emotion}.eaf
    static void SetEmotion(const char* emotion);
};
