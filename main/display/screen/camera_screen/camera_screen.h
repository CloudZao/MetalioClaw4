#pragma once

#include <esp_err.h>

#include "lvgl.h"
#include "screen_util.h"

// JPEG 保存质量（1–100）。数值越大画质越好、文件越大；越小压缩越强、体积更小。
#ifndef CAMERA_JPEG_QUALITY
#define CAMERA_JPEG_QUALITY 55
#endif

// ---------------------------------------------------------------------------
// CameraScreen
//
// 720x720 全屏摄像头 App：
//   - 屏幕上方 720x600  : 实时预览 / 相册网格 / 大图查看
//   - 屏幕下方 720x120  : 「拍照」+ 相册图标；相册页全屏显示顶部 header（返回 + 标题）
//
// 拍照：冻结当前画面；有 SD 卡时保存 JPG 到 /sdcard 根目录（质量见 CAMERA_JPEG_QUALITY）。
//       无 SD 卡时仍显示照片，但提示「无 SD 卡，未保存」。
// 相册：仅 SD 卡已挂载时可进入，展示根目录 .jpg 缩略图网格。
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

    // 供硬件测试页复用：720x600 RGB888 预览缓冲与外部 canvas。
    struct PreviewBuffer {
        uint8_t* data;
        int      width;
        int      height;
    };

    static bool PreparePreviewBuffer(PreviewBuffer* out);
    static esp_err_t StartExternalPreview(lv_obj_t* canvas);
    static void StopExternalPreview();
};
