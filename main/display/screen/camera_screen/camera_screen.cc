#include "camera_screen.h"

#include <fcntl.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_lv_adapter.h"
#include "lvgl.h"

#include "linux/videodev2.h"
#include "driver/i2c_master.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"

#include "esp_lcd_panel_io.h"

#include "IOExpander.hpp"
#include "home_screen/home_screen.h"
#include "screen_util.h"

// 由 boards/xingzhi-395/esp_lcd_nv3051f.c 提供：重发 NV3051F 厂商初始化序列。
// 这里前向声明，避免把 board 私有头加进 INCLUDE_DIRS。
extern "C" esp_err_t esp_lcd_nv3051f_replay_vendor_init(esp_lcd_panel_io_handle_t io);

// 由 boards/xingzhi-395/xingzhi-395.cc 提供：返回 NV3051F 面板 IO 句柄。
extern "C" esp_lcd_panel_io_handle_t xingzhi_395_get_panel_io();

// 由 boards/xingzhi-395/xingzhi-395.cc 提供：返回板上 I2C_NUM_1 主控总线句柄。
// 摄像头 SCCB 复用这个 handle，避免在同一对 GPIO (7/8) 上同时挂两个 I2C 控制器。
extern "C" i2c_master_bus_handle_t xingzhi_395_get_i2c_bus();

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "CameraScreen";

// ---------- 屏幕分区 ----------
// 720x720 屏幕被纵向切成两条：上 720x600 摄像头预览，下 720x120 按钮区。
constexpr int kPanelW       = 720;
constexpr int kPanelH       = 720;
constexpr int kCameraAreaW  = 720;
constexpr int kCameraAreaH  = 600;
constexpr int kButtonStripH = kPanelH - kCameraAreaH;     // 120
constexpr int kButtonStripY = kCameraAreaH;               // 600

// V4L2 缓冲组数（与原例程一致）
constexpr int kCameraBufferNum = 2;

// 摄像头供电稳定时间 / LCD 恢复等待。
// OV2710 PWDN 拉低（通电）到 SCCB 可读，硬件需要 ~5ms；XCLK 启动后还需要
// 等内部 PLL 锁定。原 example 是开机就唤醒摄像头，到 esp_video_init 之间已经
// 经过了几百毫秒的 LCD/LVGL 初始化，所以从未有时序问题；本工程是“点击图标
// 才唤醒”，必须显式给足等待时间。
constexpr int kCamPowerOnSettleMs   = 200;   // CAM_PWDN low -> XCLK start 之前
constexpr int kCamXclkSettleMs      = 50;    // XCLK start -> esp_video_init 之间
constexpr int kCamResetRecoverMs    = 120;   // GPIO3 复位脉冲 -> LCD vendor init 重发
constexpr int kCamWorkerStopMs      = 3000;

// MIPI-CSI / OV2710 引脚（板上硬件接线，与例程 example_video_common.h 完全一致）
// 注意：摄像头 SCCB 不能新建 I2C 控制器，必须复用 xingzhi_395_get_i2c_bus()
// 返回的 I2C_NUM_1 主控（GPIO 7/8 上还挂着 GT911 触摸和 TCA9555 IO 扩展器）。
constexpr int kSccbI2cFreq   = 100000;
constexpr int kCamXclkPin    = 32;          // ESP_CLOCK_ROUTER -> GPIO32 输出 24MHz XCLK
constexpr int kCamXclkFreq   = 24000000;

// ---------- 设备状态 ----------
struct CameraDev {
    int       fd                              = -1;
    uint8_t*  buffer[kCameraBufferNum]        = {};
    uint32_t  buffer_size                     = 0;
    uint32_t  width                           = 0;
    uint32_t  height                          = 0;
    uint32_t  pixel_format                    = 0;
    uint32_t  bytes_per_pixel                 = 0;       // RGB565=2, YUYV=2, RGB888=3
    uint32_t  crop_offset_x                   = 0;
    uint32_t  crop_offset_y                   = 0;
};

// ---------------------------------------------------------------------------
// 全局状态（CameraScreen 是单例，一次只能存在一个实例）
// ---------------------------------------------------------------------------
struct UiState {
    lv_obj_t* screen      = nullptr;
    lv_obj_t* canvas      = nullptr;
    lv_obj_t* btn_capture = nullptr;
    lv_obj_t* btn_label   = nullptr;
    lv_obj_t* status_lbl  = nullptr;   // “正在启动摄像头…” / 错误提示
};

UiState s_ui;

// camera_out_buf 与 LVGL canvas 绑定 —— 只分配一次、永不释放，避免重复进入摄像头屏幕时
// 反复申请 1.3 MB 的 PSRAM。摄像头采集任务直接覆写这个缓冲。
uint8_t* s_canvas_buf = nullptr;

// 拍照冻结标志：true 时摄像头任务暂停刷新画面（屏幕显示最后一帧 = 照片）
volatile bool s_photo_frozen = false;

// 画质：worker 每 s_frame_interval 帧做一次 RGB 转换 + canvas invalidate。
// 1 = 高画质（每帧渲染）  2 = 中画质（隔一帧，默认）  4 = 低画质
volatile int s_frame_interval = 2;

// Worker 控制
//
// 摄像头 worker 是一次性任务，做完“通电 -> esp_video_init -> 取流 -> 拉黑屏”
// 全套流程后自我销毁。屏幕 LOAD/UNLOAD 由 LVGL 主线程触发，**绝不能**在主线程
// 里同步等待 worker 完成（否则 LVGL adapter 后台任务拿不到 LVGL 锁就会
// 报 “Failed to acquire LVGL lock”，并且整个 UI 卡 1 秒以上）。
//
// 设计：
//   * s_worker_slot 是一个 mutex，worker 任务在自己的 task 里 take 一次、
//     退出前 give 一次，保证同一时刻最多只有一个 worker 真正在做硬件初始化
//     / V4L2 / 断电。
//   * start_camera_worker() 只创建任务并立即返回。如果上一次的 worker 还
//     在清理中，新 worker 会在 take(s_worker_slot) 时排队等它退出，整个
//     等待发生在 worker 自己的 task 里，LVGL 主线程不阻塞。
//   * stop_camera_worker() 只把 s_camera_running 置 0，立即返回。worker
//     主循环每帧（≤40ms）会检查这个标志并自行 cleanup。
volatile bool       s_camera_running    = false;
TaskHandle_t        s_camera_task_handle = nullptr;
SemaphoreHandle_t   s_worker_slot        = nullptr;

// 视频流水线 / 设备
CameraDev                       s_cam{};
esp_cam_sensor_xclk_handle_t    s_xclk_handle  = nullptr;
bool                            s_video_init   = false;

// ---------------------------------------------------------------------------
// 像素格式转换：与 esp32-p4-395-camera-lcd 例程一致。把摄像头中心
// CAMERA_AREA_W x CAMERA_AREA_H 区域裁剪出来，180° 旋转后写成
// RGB888 (B-G-R 字节序) 到 dst。这就是 NV3051F 在
// LCD_COLOR_PIXEL_FORMAT_RGB888 + LVGL LV_COLOR_DEPTH_24 下的内存布局。
// ---------------------------------------------------------------------------
inline void rgb565_to_rgb888(const uint8_t* src, const CameraDev* cam, uint8_t* dst) {
    const uint16_t* src_base = reinterpret_cast<const uint16_t*>(src) +
                               cam->crop_offset_y * cam->width +
                               cam->crop_offset_x;
    const uint16_t* src_row = src_base +
                              (kCameraAreaH - 1) * cam->width +
                              (kCameraAreaW - 1);
    for (uint32_t y = 0; y < kCameraAreaH; y++) {
        const uint16_t* sp = src_row;
        for (uint32_t x = 0; x < kCameraAreaW; x++) {
            uint16_t px = *sp--;
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5)  & 0x3F;
            uint8_t b5 = (px)       & 0x1F;
            *dst++ = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
            *dst++ = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
            *dst++ = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
        }
        src_row -= cam->width;
    }
}

// YUV422 -> RGB888（BT.601 全范围公式），u_first 控制 UYVY/YUYV 顺序。
inline void yuv422_to_rgb888(const uint8_t* src, const CameraDev* cam,
                             uint8_t* dst, bool u_first) {
    const uint8_t* src_base = src +
                              cam->crop_offset_y * cam->width * 2 +
                              cam->crop_offset_x * 2;
    const int row_bytes = cam->width * 2;
    // 旋转 180°：从最后一行的最右侧像素对开始反向遍历；YUV422 必须以
    // “像素对”为单位回退，否则 U/V 会错位。kCameraAreaW=720 是偶数，安全。
    const uint8_t* src_row = src_base + (kCameraAreaH - 1) * row_bytes +
                             (kCameraAreaW - 2) * 2;
    for (uint32_t y = 0; y < kCameraAreaH; y++) {
        const uint8_t* sp = src_row;
        for (uint32_t x = 0; x < kCameraAreaW; x += 2) {
            uint8_t y0, y1, u, v;
            if (u_first) {            // UYVY: U Y0 V Y1
                u  = sp[0]; y0 = sp[1]; v  = sp[2]; y1 = sp[3];
            } else {                  // YUYV: Y0 U Y1 V
                y0 = sp[0]; u  = sp[1]; y1 = sp[2]; v  = sp[3];
            }
            sp -= 4;

            int c0 = static_cast<int>(y0) - 16;
            int c1 = static_cast<int>(y1) - 16;
            int d  = static_cast<int>(u) - 128;
            int e  = static_cast<int>(v) - 128;
            int r0 = (298 * c0           + 409 * e + 128) >> 8;
            int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
            int b0 = (298 * c0 + 516 * d           + 128) >> 8;
            int r1 = (298 * c1           + 409 * e + 128) >> 8;
            int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
            int b1 = (298 * c1 + 516 * d           + 128) >> 8;

#define CLAMP_U8(v_) (static_cast<uint8_t>((v_) < 0 ? 0 : ((v_) > 255 ? 255 : (v_))))
            // 旋转 180°：先输出 pixel1，再输出 pixel0
            *dst++ = CLAMP_U8(b1);
            *dst++ = CLAMP_U8(g1);
            *dst++ = CLAMP_U8(r1);
            *dst++ = CLAMP_U8(b0);
            *dst++ = CLAMP_U8(g0);
            *dst++ = CLAMP_U8(r0);
#undef CLAMP_U8
        }
        src_row -= row_bytes;
    }
}

inline void rgb888_to_rgb888(const uint8_t* src, const CameraDev* cam, uint8_t* dst) {
    const uint8_t* src_base = src +
                              (cam->crop_offset_y * cam->width + cam->crop_offset_x) * 3;
    const uint8_t* src_row = src_base +
                             (kCameraAreaH - 1) * cam->width * 3 +
                             (kCameraAreaW - 1) * 3;
    for (uint32_t y = 0; y < kCameraAreaH; y++) {
        const uint8_t* sp = src_row;
        for (uint32_t x = 0; x < kCameraAreaW; x++) {
            uint8_t r = sp[0], g = sp[1], b = sp[2];
            sp -= 3;
            *dst++ = b;
            *dst++ = g;
            *dst++ = r;
        }
        src_row -= cam->width * 3;
    }
}

// ---------------------------------------------------------------------------
// 视频流水线 (XCLK + esp_video_init)
// ---------------------------------------------------------------------------
esp_err_t init_video_pipeline() {
    if (s_video_init) {
        return ESP_OK;
    }

    esp_cam_sensor_xclk_config_t xclk_cfg = {};
    xclk_cfg.esp_clock_router_cfg.xclk_pin     = static_cast<gpio_num_t>(kCamXclkPin);
    xclk_cfg.esp_clock_router_cfg.xclk_freq_hz = kCamXclkFreq;

    ESP_LOGI(TAG, "MIPI-CSI xclk pin=%d freq=%d", kCamXclkPin, kCamXclkFreq);
    ESP_RETURN_ON_ERROR(
        esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &s_xclk_handle),
        TAG, "xclk allocate failed");
    ESP_RETURN_ON_ERROR(esp_cam_sensor_xclk_start(s_xclk_handle, &xclk_cfg),
                        TAG, "xclk start failed");

    // 等 XCLK 稳定 + sensor 内部 PLL 锁定，再做 SCCB 探测，否则可能读到陈旧的
    // 寄存器内容（例如 OV2710 0x300A 应得 0x27，结果读到了 0x300B 的 0x10）。
    vTaskDelay(pdMS_TO_TICKS(kCamXclkSettleMs));

    esp_video_init_csi_config_t csi_config = {};
    // OV2710 在本板上没有独立的 RST/PWDN 脚（CAM_PWDN 由 TCA9555 IO2 控制），
    // 让驱动跳过 host 端的复位脚操作。
    csi_config.reset_pin = static_cast<gpio_num_t>(-1);
    csi_config.pwdn_pin  = static_cast<gpio_num_t>(-1);

    // 摄像头 SCCB 复用板上 I2C_NUM_1 主控（GPIO 7/8）。GT911 触摸和 TCA9555
    // IO 扩展器都挂在这条总线上；OV2710 (默认 SCCB 地址 0x36) 不会和它们冲突。
    i2c_master_bus_handle_t bus = xingzhi_395_get_i2c_bus();
    if (bus == nullptr) {
        ESP_LOGE(TAG, "xingzhi_395_get_i2c_bus() returned NULL");
        esp_cam_sensor_xclk_stop(s_xclk_handle);
        esp_cam_sensor_xclk_free(s_xclk_handle);
        s_xclk_handle = nullptr;
        return ESP_ERR_INVALID_STATE;
    }
    csi_config.sccb_config.init_sccb = false;
    csi_config.sccb_config.i2c_handle = bus;
    csi_config.sccb_config.freq      = kSccbI2cFreq;

    // 探测一下 SCCB：可见 OV2710 7-bit 地址 0x36 是否真的在线，避免后面 esp_video_init
    // 内部失败时只看到 “PID=0xXX” 的迷糊日志。
    {
        esp_err_t pr = i2c_master_probe(bus, 0x36, 200);
        if (pr == ESP_OK) {
            ESP_LOGI(TAG, "SCCB probe @0x36 OK (OV2710 awake)");
        } else {
            ESP_LOGW(TAG, "SCCB probe @0x36 failed (%s) — sensor 可能还没醒",
                     esp_err_to_name(pr));
        }
    }

    const esp_video_init_config_t cam_config = {
        .csi = &csi_config,
    };

    ESP_LOGI(TAG, "esp_video_init: SCCB reuse i2c_bus=%p freq=%d", bus, kSccbI2cFreq);
    esp_err_t err = esp_video_init(&cam_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
        esp_cam_sensor_xclk_stop(s_xclk_handle);
        esp_cam_sensor_xclk_free(s_xclk_handle);
        s_xclk_handle = nullptr;
        return err;
    }

    s_video_init = true;
    return ESP_OK;
}

void deinit_video_pipeline() {
    if (s_video_init) {
        esp_video_deinit();
        s_video_init = false;
    }
    if (s_xclk_handle) {
        esp_cam_sensor_xclk_stop(s_xclk_handle);
        esp_cam_sensor_xclk_free(s_xclk_handle);
        s_xclk_handle = nullptr;
    }
}

// ---------------------------------------------------------------------------
// V4L2 设备
// ---------------------------------------------------------------------------
esp_err_t configure_camera_format(int fd, CameraDev* cam) {
    struct v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Camera default: %" PRIu32 "x%" PRIu32 " format=" V4L2_FMT_STR,
             format.fmt.pix.width, format.fmt.pix.height,
             V4L2_FMT_STR_ARG(format.fmt.pix.pixelformat));

    // 优先尝试 RGB565 —— ISP 流水线下 OV2710 通常支持。
    struct v4l2_format try_fmt = format;
    try_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(fd, VIDIOC_S_FMT, &try_fmt) == 0 &&
        try_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565) {
        format = try_fmt;
        ESP_LOGI(TAG, "VIDIOC_S_FMT to RGB565 accepted: %" PRIu32 "x%" PRIu32,
                 format.fmt.pix.width, format.fmt.pix.height);
    } else {
        ESP_LOGW(TAG, "VIDIOC_S_FMT to RGB565 not accepted, keep default format=" V4L2_FMT_STR,
                 V4L2_FMT_STR_ARG(format.fmt.pix.pixelformat));
    }

    cam->width        = format.fmt.pix.width;
    cam->height       = format.fmt.pix.height;
    cam->pixel_format = format.fmt.pix.pixelformat;

    switch (cam->pixel_format) {
    case V4L2_PIX_FMT_RGB565:
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
        cam->bytes_per_pixel = 2;
        break;
    case V4L2_PIX_FMT_RGB24:
        cam->bytes_per_pixel = 3;
        break;
    default:
        ESP_LOGE(TAG, "Unsupported camera pixel format %.4s",
                 reinterpret_cast<const char*>(&cam->pixel_format));
        return ESP_FAIL;
    }

    cam->crop_offset_x = (cam->width  > kCameraAreaW) ? (cam->width  - kCameraAreaW) / 2 : 0;
    cam->crop_offset_y = (cam->height > kCameraAreaH) ? (cam->height - kCameraAreaH) / 2 : 0;

    ESP_LOGI(TAG,
             "SW pipeline: crop %lux%lu @(%lu,%lu) src_fmt=" V4L2_FMT_STR
             " bpp=%lu -> dst RGB888 %dx%d",
             static_cast<unsigned long>(cam->width),
             static_cast<unsigned long>(cam->height),
             static_cast<unsigned long>(cam->crop_offset_x),
             static_cast<unsigned long>(cam->crop_offset_y),
             V4L2_FMT_STR_ARG(cam->pixel_format),
             static_cast<unsigned long>(cam->bytes_per_pixel),
             kCameraAreaW, kCameraAreaH);
    return ESP_OK;
}

esp_err_t open_camera_device(CameraDev* cam) {
    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return ESP_FAIL;
    }

    if (configure_camera_format(fd, cam) != ESP_OK) {
        close(fd);
        return ESP_FAIL;
    }

    struct v4l2_requestbuffers req = {};
    req.count  = kCameraBufferNum;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        close(fd);
        return ESP_FAIL;
    }

    for (int i = 0; i < kCameraBufferNum; i++) {
        struct v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%d] failed", i);
            close(fd);
            return ESP_FAIL;
        }
        cam->buffer[i] = static_cast<uint8_t*>(
            mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset));
        if (cam->buffer[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap buffer[%d] failed", i);
            cam->buffer[i] = nullptr;
            close(fd);
            return ESP_FAIL;
        }
        cam->buffer_size = buf.length;
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed", i);
            close(fd);
            return ESP_FAIL;
        }
    }
    cam->fd = fd;

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam->fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void close_camera_device(CameraDev* cam) {
    if (cam->fd < 0) {
        return;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < kCameraBufferNum; i++) {
        if (cam->buffer[i]) {
            munmap(cam->buffer[i], cam->buffer_size);
            cam->buffer[i] = nullptr;
        }
    }
    close(cam->fd);
    cam->fd = -1;
    cam->buffer_size = 0;
}

// ---------------------------------------------------------------------------
// LVGL UI 辅助：仅在 LVGL 线程持锁状态下调用。
// ---------------------------------------------------------------------------
void update_status_label(const char* text, lv_color_t color) {
    if (s_ui.status_lbl == nullptr) return;
    if (text == nullptr) {
        lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ui.status_lbl, text);
        lv_obj_set_style_text_color(s_ui.status_lbl, color, LV_PART_MAIN);
    }
}

// 摄像头任务可在任意线程调用 —— 内部加 LVGL 锁。
void post_status_text(const char* text, uint32_t color_hex) {
    if (esp_lv_adapter_lock(200) == ESP_OK) {
        update_status_label(text, lv_color_hex(color_hex));
        esp_lv_adapter_unlock();
    }
}

void post_status_clear() {
    if (esp_lv_adapter_lock(200) == ESP_OK) {
        update_status_label(nullptr, lv_color_white());
        esp_lv_adapter_unlock();
    }
}

// ---------------------------------------------------------------------------
// 摄像头 worker 任务
//
//   0) take(s_worker_slot)  —— 等任何上一轮 worker 把 V4L2/电源清理完
//   1) 检查 s_camera_running——可能用户已经在 worker 还没起来时就切走了
//   2) 设置 CAM_PWDN = LOW（通电）
//   3) init_video_pipeline()  —— 内部会拉一次 GPIO 3，等 120ms 后重发
//      NV3051F vendor init 把 LCD 救活
//   4) 打开 V4L2 设备 + STREAMON
//   5) 主循环：DQBUF -> 转换/旋转 -> 写 canvas buffer -> invalidate canvas -> QBUF
//   6) 收到 stop 信号后 STREAMOFF / munmap / close / esp_video_deinit
//   7) CAM_PWDN = HIGH（断电）
//   8) give(s_worker_slot) 然后 vTaskDelete(自己)
// ---------------------------------------------------------------------------
void camera_worker_task(void* /*arg*/) {
    auto& io = IOExpander::getInstance();

    // ----- 0) 排队：等上一轮 worker 把清理做完 -----
    xSemaphoreTake(s_worker_slot, portMAX_DELAY);

    // ----- 1) 期间用户可能已经又切走相机屏，停止信号已经下来了 -----
    if (!s_camera_running) {
        ESP_LOGI(TAG, "worker started but already cancelled, exit");
        xSemaphoreGive(s_worker_slot);
        s_camera_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // ----- 2) 通电 -----
    io.setLevel(IOExpander::Pin::CAM_PWDN, false);
    vTaskDelay(pdMS_TO_TICKS(kCamPowerOnSettleMs));

    // ----- 3) esp_video / XCLK -----
    esp_err_t err = init_video_pipeline();
    if (err != ESP_OK) {
        post_status_text("摄像头初始化失败", 0xFF5555);
        goto cleanup_power;
    }

    // 摄像头驱动会拉低 GPIO 3（与 LCD RST 共用），导致 LCD 控制器寄存器丢失。
    // 这里等待复位结束并重发 NV3051F vendor 初始化序列。
    vTaskDelay(pdMS_TO_TICKS(kCamResetRecoverMs));
    {
        esp_lcd_panel_io_handle_t panel_io = xingzhi_395_get_panel_io();
        if (panel_io != nullptr) {
            esp_err_t r = esp_lcd_nv3051f_replay_vendor_init(panel_io);
            if (r != ESP_OK) {
                ESP_LOGW(TAG, "replay LCD vendor init failed: %s", esp_err_to_name(r));
            } else {
                ESP_LOGI(TAG, "LCD recovered after camera reset pulse");
            }
        } else {
            ESP_LOGW(TAG, "panel_io handle is NULL, skip LCD replay");
        }
    }

    // ----- 4) 打开 V4L2 设备 -----
    if (open_camera_device(&s_cam) != ESP_OK) {
        post_status_text("打开摄像头设备失败", 0xFF5555);
        goto cleanup_video;
    }

    post_status_clear();

    // ----- 4) 主循环 -----
    {
        struct v4l2_buffer buf;
        uint32_t frame_count   = 0;   // 实际渲染帧（用于 FPS 日志）
        uint32_t capture_count = 0;   // 取流帧（用于画质跳帧判断）
        int64_t  fps_start     = esp_timer_get_time();

        while (s_camera_running) {
            std::memset(&buf, 0, sizeof(buf));
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
                ESP_LOGE(TAG, "VIDIOC_DQBUF failed");
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // 画质 -> 跳帧：interval 帧中只渲染 1 帧；其余只 QBUF 回去，
            // 这样 V4L2 不堆积，但跳过的帧免去 ~1.3MB 像素转换 + LVGL 刷屏。
            uint32_t interval = static_cast<uint32_t>(s_frame_interval);
            if (interval < 1) interval = 1;
            const bool should_render = ((capture_count++ % interval) == 0);

            if ((buf.flags & V4L2_BUF_FLAG_DONE) && !s_photo_frozen && should_render) {
                const uint8_t* src = s_cam.buffer[buf.index];
                switch (s_cam.pixel_format) {
                case V4L2_PIX_FMT_RGB565:
                    rgb565_to_rgb888(src, &s_cam, s_canvas_buf);
                    break;
                case V4L2_PIX_FMT_YUYV:
                    yuv422_to_rgb888(src, &s_cam, s_canvas_buf, /*u_first=*/false);
                    break;
                case V4L2_PIX_FMT_UYVY:
                    yuv422_to_rgb888(src, &s_cam, s_canvas_buf, /*u_first=*/true);
                    break;
                case V4L2_PIX_FMT_RGB24:
                    rgb888_to_rgb888(src, &s_cam, s_canvas_buf);
                    break;
                default:
                    break;
                }

                // 让 LVGL 在下次刷新时重绘 canvas。
                if (esp_lv_adapter_lock(20) == ESP_OK) {
                    if (s_ui.canvas != nullptr) {
                        lv_obj_invalidate(s_ui.canvas);
                    }
                    esp_lv_adapter_unlock();
                }

                frame_count++;
                int64_t now = esp_timer_get_time();
                if (now - fps_start >= 5LL * 1000 * 1000) {
                    ESP_LOGI(TAG, "Camera FPS: %.1f",
                             static_cast<double>(frame_count) * 1e6 /
                                 static_cast<double>(now - fps_start));
                    frame_count = 0;
                    fps_start = now;
                }
            }

            if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
                ESP_LOGE(TAG, "VIDIOC_QBUF failed");
            }

            if (s_photo_frozen) {
                vTaskDelay(pdMS_TO_TICKS(30));
            }
        }
    }

    // ----- 6) 关流 / 释放 -----
    close_camera_device(&s_cam);

cleanup_video:
    deinit_video_pipeline();

cleanup_power:
    // ----- 7) 断电 -----
    io.setLevel(IOExpander::Pin::CAM_PWDN, true);

    // ----- 8) 让出 worker slot 给下一个 worker，并自我销毁 -----
    s_camera_task_handle = nullptr;
    xSemaphoreGive(s_worker_slot);
    vTaskDelete(nullptr);
}

esp_err_t start_camera_worker() {
    if (s_worker_slot == nullptr) {
        s_worker_slot = xSemaphoreCreateMutex();
        if (s_worker_slot == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_camera_task_handle != nullptr) {
        // 上一个 worker 还在跑（用户来回快速切换屏幕）。把 running 重新置 1，
        // 让它继续保持 stream，不再额外起 task。
        s_camera_running = true;
        s_photo_frozen   = false;
        ESP_LOGW(TAG, "camera worker already exists, reuse");
        return ESP_OK;
    }
    s_camera_running = true;
    s_photo_frozen   = false;

    BaseType_t ok = xTaskCreatePinnedToCore(camera_worker_task, "cam_screen",
                                            8 * 1024, nullptr,
                                            tskIDLE_PRIORITY + 4,
                                            &s_camera_task_handle, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create camera worker task failed");
        s_camera_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void stop_camera_worker() {
    // 只发信号，不等待。worker 自己会在主循环里看到 s_camera_running == false
    // 并退出，cleanup 在 worker 自己的 task 上下文中完成；下次 LOAD 时新 worker
    // 会在 take(s_worker_slot) 处自动排队，等待这次的 cleanup 结束。
    s_camera_running = false;
}

// ---------------------------------------------------------------------------
// LVGL 事件
// ---------------------------------------------------------------------------

void on_capture_btn_clicked(lv_event_t* /*e*/) {
    s_photo_frozen = !s_photo_frozen;
    if (s_ui.btn_label != nullptr) {
        lv_label_set_text(s_ui.btn_label, s_photo_frozen ? "实时预览" : "拍照");
    }
    if (s_ui.btn_capture != nullptr) {
        lv_obj_set_style_bg_color(
            s_ui.btn_capture,
            s_photo_frozen ? lv_color_hex(0xC62828) : lv_color_hex(0x222222),
            LV_PART_MAIN);
    }
    ESP_LOGI(TAG, "capture button: %s", s_photo_frozen ? "FROZEN (photo)" : "LIVE preview");
}

void OnSwipeBack() {
    // 让 LVGL 在加载新屏前等当前手指 release，否则按 “返回” 按钮的 PRESSED
    // 事件会被新加载的主屏当成一次新 click —— 落在主屏的 “相机” 图标上时
    // 就会立刻又 LaunchCamera 一次。
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home    = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void on_screen_unloaded(lv_event_t* /*e*/) {
    // 屏幕被切走 -> 抹掉对 LVGL 对象的引用。worker 任务停止由
    // CameraScreen::LifecycleCallback 在同一时机负责，等 stop 完成后
    // 屏幕对象才会被 lv_obj_delete_async 真正释放，所以这里的清理仅是“标记”。
    s_ui.screen      = nullptr;
    s_ui.canvas      = nullptr;
    s_ui.btn_capture = nullptr;
    s_ui.btn_label   = nullptr;
    s_ui.status_lbl  = nullptr;
    s_photo_frozen   = false;
}

}  // namespace

// ---------------------------------------------------------------------------
// CameraScreen 公共接口
// ---------------------------------------------------------------------------
lv_obj_t* CameraScreen::Create() {
    // 固定中画质：隔一帧渲染一次，平衡流畅度与画面质量。
    s_frame_interval = 2;

    // canvas 缓冲：720x600 RGB888，1.296 MB。一次申请、永不释放（重复进入摄像头屏幕
    // 时复用），避免反复 1.3MB 大块 PSRAM 分配抖动。
    const size_t out_size = static_cast<size_t>(kCameraAreaW) * kCameraAreaH * 3;
    if (s_canvas_buf == nullptr) {
        s_canvas_buf = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(64, out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_canvas_buf == nullptr) {
            ESP_LOGE(TAG, "alloc canvas buf %u failed", static_cast<unsigned>(out_size));
            return nullptr;
        }
    }
    // 每次进入相机屏先把画布抹黑：上一次退出时 canvas 里残留的最后一帧
    // 在 worker 出第一帧前会一直可见，看上去像 “上次的画面又出现了”。
    std::memset(s_canvas_buf, 0, out_size);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen   = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ----- 摄像头预览 canvas（720x600） -----
    lv_obj_t* canvas = lv_canvas_create(scr);
    s_ui.canvas      = canvas;
    lv_canvas_set_buffer(canvas, s_canvas_buf, kCameraAreaW, kCameraAreaH,
                         LV_COLOR_FORMAT_RGB888);
    lv_obj_set_pos(canvas, 0, 0);
    // canvas 不接收触摸事件 —— 让 PRESSED/RELEASED 透传到屏幕，方便右滑返回手势。
    screen_make_input_passive(canvas);

    // “正在启动摄像头…” 提示标签覆盖在 canvas 中央。
    lv_obj_t* status = lv_label_create(scr);
    s_ui.status_lbl  = status;
    lv_label_set_text(status, "正在启动摄像头…");
    lv_obj_set_style_text_color(status, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, kCameraAreaH / 2 - 30);
    screen_make_input_passive(status);

    // ----- 底部按钮区（720x120 黑底） -----
    lv_obj_t* strip = lv_obj_create(scr);
    screen_strip_obj_chrome(strip);
    lv_obj_set_size(strip, kPanelW, kButtonStripH);
    lv_obj_set_pos(strip, 0, kButtonStripY);
    lv_obj_set_style_bg_color(strip, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* btn = lv_button_create(strip);
    s_ui.btn_capture = btn;
    lv_obj_set_size(btn, 200, 80);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(btn, 40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 3, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, on_capture_btn_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    s_ui.btn_label = lbl;
    lv_label_set_text(lbl, "拍照");
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(lbl);

    // 返回交给右滑手势，不再额外画返回按钮。worker 任务的停止由
    // LifecycleCallback(UNLOAD) 处理，顺序是：
    //   lv_screen_load(home) -> SCREEN_UNLOADED on cam scr ->
    //   lifecycle UNLOAD -> stop_camera_worker() -> 关闭摄像头 / 断电。
    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

void CameraScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: camera_screen -> start worker");
        // CAM_PWDN 由 worker 任务在拿到 s_worker_slot 后串行处理。这里如果
        // 直接动 IOExpander，会和上一个 worker 还没完成的 cleanup（也在改
        // CAM_PWDN）撞车。
        if (start_camera_worker() != ESP_OK) {
            ESP_LOGE(TAG, "start_camera_worker failed");
        }
    } else {
        ESP_LOGI(TAG, "unload: camera_screen -> signal worker stop (non-blocking)");
        // 只发停止信号；worker 在自己的 task 里 cleanup（关流、esp_video_deinit、
        // 断电）。LVGL 主线程不阻塞，避免 adapter 后台任务长时间拿不到锁。
        stop_camera_worker();
    }
}
