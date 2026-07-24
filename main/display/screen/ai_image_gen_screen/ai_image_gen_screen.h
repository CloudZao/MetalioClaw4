#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// AiImageGenScreen — AI 语音生图
//
// 流程：按住说话 → POST /xiaozhi/api/asr?format=wav 取 prompt →
//       POST /xiaozhi/api/dashscope/text2image（n=1~4）→
//       轮询 GET .../text2image/tasks/{taskId}?maxSide=500 →
//       下载 imageUrls 并在页面展示。
//
// 录音 / HTTP 均在独立 FreeRTOS 任务中执行，不阻塞 LVGL。
// ---------------------------------------------------------------------------
class AiImageGenScreen {
 public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
