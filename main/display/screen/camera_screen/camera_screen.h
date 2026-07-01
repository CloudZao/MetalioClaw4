#pragma once

#include <esp_err.h>

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// CameraScreen
//
// 720x720 全屏摄像头 App：
//   - 屏幕上方 720x600  : 实时预览 (LVGL canvas, RGB888)
//   - 屏幕下方 720x120  : 黑色按钮区，居中「拍照 / 实时预览」按钮（固定中画质预览）
//
// 数据流（与 esp32-p4-395-camera-lcd 例程一致）：
//   OV2710 (1920x1080) --[V4L2/MIPI-CSI/ISP]--> RGB565 / YUYV / UYVY / RGB24
//          --[软件中心裁剪 + 180° 旋转 + RGB888(BGR)]--> LVGL canvas buffer
//          --[LVGL 渲染 + esp_lv_adapter DMA2D 刷屏]--> NV3051F panel
//
// 生命周期：
//   - HomeScreen 在每次启动 CameraScreen 时通过 screen_attach_lifecycle 绑定
//     `camera_lifecycle_cb`：
//       LOAD   -> CAM_PWDN 拉低 + 启动 esp_video / 摄像头采集任务
//       UNLOAD -> 停止采集任务 + esp_video_deinit + CAM_PWDN 拉高
//   - 这样摄像头只在该 App 处于前台时通电、流式采集，退出即立即断电省功耗。
// ---------------------------------------------------------------------------
class CameraScreen {
public:
    // 创建摄像头预览屏幕（parent = NULL）。
    // 调用方需要：
    //   1) 通过 screen_attach_lifecycle(scr, &CameraScreen::LifecycleCallback)
    //      把 LOAD/UNLOAD 事件挂到这个屏幕上。
    //   2) lv_screen_load(scr) 让屏幕成为活动屏幕；LOAD 事件会由 LVGL 自动触发。
    static lv_obj_t* Create();

    // 给 home_screen 直接复用的 lifecycle 回调：包含 CAM_PWDN 控制和
    // 摄像头流水线的启动 / 停止。
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
