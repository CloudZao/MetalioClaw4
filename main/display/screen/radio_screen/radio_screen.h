#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// RadioScreen
//
// 720x720 网络电台界面：HLS（m3u8）直播 + gmf_fft 频谱律动可视化。
//   - LOAD：暂停系统语音链路，启动 HLS 播放与频谱任务
//   - UNLOAD：停止播放并恢复唤醒词等系统音频
// ---------------------------------------------------------------------------
class RadioScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
