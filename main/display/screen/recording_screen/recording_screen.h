#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// RecordingScreen
//
// 录音 App（依赖 SD 卡）：
//   - 进入前检查 SdCardManager 是否已挂载；无卡则仅显示提示，无法录音/列表
//   - Tab「录音」：开始/结束录音，显示计时，保存 Ogg Opus 到 /sdcard/recordings/
//   - Tab「列表」：列出录音；点击进入详情（播放 / 转写），可删除
//   - 详情页：播放、调用 /api/v1/asr/transcribe 转写并展示全文/对话/摘要
//
// 生命周期：UNLOAD 时停止录音/播放/转写任务并恢复唤醒词。
// ---------------------------------------------------------------------------
class RecordingScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
