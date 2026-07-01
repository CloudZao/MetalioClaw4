#pragma once

#include "lvgl.h"
#include "screen_util.h"

// 改此项切换表情资源格式：".eaf" / ".jpg" / ".png" / ".sjpg"
#ifndef DIGITAL_PEOPLE_EMOTION_EXT
#define DIGITAL_PEOPLE_EMOTION_EXT ".sjpg"
#endif

// ---------------------------------------------------------------------------
// DigitalPeopleScreen
//
// 720x720 数字人界面：纯黑背景 + 居中显示 SD 卡 system/emotion/ 下的
// 表情资源（.eaf / .jpg / .png / .sjpg，由 DIGITAL_PEOPLE_EMOTION_EXT 切换）。
// 进入时会检查 6 个大类资源是否齐全；缺失时在屏幕中央提示用户将资源
// 包放入 SD 卡。
//
// 进入页面前检查设备是否已激活；未激活时弹出不可关闭的拦截弹窗（含返回
// 按钮），背后页面内容保持可见但不可操作，并打印日志。
//
// 对话气泡：
//   - ShowUserMessage(): 用户说的话 -> 屏幕底部气泡（gif 下方）。
//   - ShowSystemMessage(): 系统 / assistant 反馈 -> gif 左上方气泡。
//   两个气泡都是带白色边框的半透明深色聊天气泡，初始隐藏，调用时刷新内
//   容并显示，会自动覆盖上一条同类型消息。屏幕未在前台时全部 no-op。
// ---------------------------------------------------------------------------
class DigitalPeopleScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    static bool IsActive();
    static void ShowUserMessage(const char* text);
    static void ShowSystemMessage(const char* text);
    static void ClearMessages();

    // 切换数字人屏正在播放的表情动画 / 静态图。
    // category 必须是 6 个大类之一：crying / happy / loving / neutral /
    // surprised / thinking。文件路径会被拼成
    //   S:/sdcard/system/emotion/{category}{ext}
    // ext 由 DIGITAL_PEOPLE_EMOTION_EXT 决定（.eaf / .jpg / .png / .sjpg）。
    // 屏幕不在前台时也可以调用：本类会记住当前大类，下次 Create()
    // 时直接加载请求过的那一张，不会丢失。
    static void SetEmotion(const char* category);
};
