#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// TranslateScreen — Sonicloud 实时同声传译
//
// 流程：选择源/目标语言 → 点击「实时翻译」→
//       POST /xiaozhi/api/sinicloud/token 取 wsUrl →
//       WebSocket 推送 16kHz PCM，展示识别原文与译文。
//
// 协议见 open.sinicloud.com 实时语音流识别文档；实现参考 translate-test Web 演示。
// ---------------------------------------------------------------------------
class TranslateScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
