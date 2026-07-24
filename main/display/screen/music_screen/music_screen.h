#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// MusicScreen
//
// 720x720 蓝牙音乐播放器界面：
//   - 进入页面（LIFECYCLE_LOAD）时通过 SimpleUart 把蓝牙模块切到模式三
//     （AT+RX=1 / AT+MODE=3），切换后注册自己的 UART RX 回调用于解析手机
//     回传的 JSON 数据流：
//       {"type":"song",  "data":"人间共鸣-李健"}   -> 显示成歌名标题
//       {"type":"lyrics","data":"人间共鸣 - 李健"}  -> 显示成当前歌词
//   - 按钮通过 AT 命令控制播放：
//       上一曲 AT+PREV / 下一曲 AT+NEXT / 播放暂停 AT+PP
//       音量加 AT+VOLUP / 音量减 AT+VOLDOWN
//   - 离开页面（LIFECYCLE_UNLOAD）时摘掉 UART 回调，避免在屏幕销毁后
//     还往野指针上写 LVGL 操作。
// ---------------------------------------------------------------------------
class MusicScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
