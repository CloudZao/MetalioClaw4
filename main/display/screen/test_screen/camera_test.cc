#include "camera_test.h"

#include <cstdio>

#include "IOExpander.hpp"
#include "camera_screen/camera_screen.h"
#include "driver/i2c_master.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "screen_util.h"
#include "test_ui_common.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

extern "C" i2c_master_bus_handle_t metalio_claw_4_get_i2c_bus();

namespace {

constexpr const char* TAG = "CameraTest";

constexpr int      kCamPowerOnSettleMs = 200;
constexpr int      kCamXclkSettleMs    = 50;
constexpr int      kSccbI2cFreq        = 100000;
constexpr int      kCamXclkPin         = 32;
constexpr int      kCamXclkFreq        = 24000000;
constexpr uint8_t  kOv2710Addr         = 0x36;
constexpr uint16_t kOv2710RegPidH      = 0x300A;
constexpr uint16_t kOv2710RegPidL      = 0x300B;
constexpr uint8_t  kOv2710PidH         = 0x27;
constexpr uint8_t  kOv2710PidL         = 0x10;

constexpr uint32_t kColorBtnIdle = 0x2563EB;
constexpr int      kPreviewAreaW = 720;
constexpr int      kPreviewAreaH = 600;
constexpr int      kPreviewStripH = 120;

lv_obj_t* s_status_icon  = nullptr;
lv_obj_t* s_value_lbl    = nullptr;
lv_obj_t* s_preview_btn  = nullptr;
lv_obj_t* s_preview_mask = nullptr;
lv_obj_t* s_preview_canvas = nullptr;

bool              s_cam_powered     = false;
bool              s_preview_started = false;
esp_cam_sensor_xclk_handle_t s_xclk_handle = nullptr;
i2c_master_dev_handle_t      s_sccb_dev    = nullptr;
int64_t           s_power_on_us = 0;
bool              s_detect_done = false;

void SetErrorText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorError),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, false);
}

void SetPassText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, true);
}

bool EnsureSccbDevice() {
    if (s_sccb_dev != nullptr) {
        return true;
    }

    i2c_master_bus_handle_t bus = metalio_claw_4_get_i2c_bus();
    if (bus == nullptr) {
        ESP_LOGE(TAG, "I2C bus not ready");
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = kOv2710Addr;
    dev_cfg.scl_speed_hz    = kSccbI2cFreq;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_sccb_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add SCCB device failed");
        s_sccb_dev = nullptr;
        return false;
    }
    return true;
}

bool ReadSccbReg16(uint16_t reg, uint8_t* out) {
    if (s_sccb_dev == nullptr || out == nullptr) {
        return false;
    }
    const uint8_t reg_buf[2] = {
        static_cast<uint8_t>(reg >> 8),
        static_cast<uint8_t>(reg & 0xFF),
    };
    return i2c_master_transmit_receive(s_sccb_dev, reg_buf, sizeof(reg_buf),
                                       out, 1, 200) == ESP_OK;
}

bool StartCameraPower() {
    if (s_cam_powered) {
        return true;
    }

    IOExpander::getInstance().setLevel(IOExpander::Pin::CAM_PWDN, false);
    s_cam_powered = true;
    vTaskDelay(pdMS_TO_TICKS(kCamPowerOnSettleMs));

    esp_cam_sensor_xclk_config_t xclk_cfg = {};
    xclk_cfg.esp_clock_router_cfg.xclk_pin =
        static_cast<gpio_num_t>(kCamXclkPin);
    xclk_cfg.esp_clock_router_cfg.xclk_freq_hz = kCamXclkFreq;

    if (esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER,
                                     &s_xclk_handle) != ESP_OK) {
        ESP_LOGE(TAG, "xclk allocate failed");
        return false;
    }
    if (esp_cam_sensor_xclk_start(s_xclk_handle, &xclk_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "xclk start failed");
        esp_cam_sensor_xclk_free(s_xclk_handle);
        s_xclk_handle = nullptr;
        return false;
    }

    s_power_on_us = esp_timer_get_time();
    ESP_LOGI(TAG, "camera powered, XCLK started on GPIO%d", kCamXclkPin);
    return true;
}

void StopCameraPower() {
    if (s_xclk_handle != nullptr) {
        esp_cam_sensor_xclk_stop(s_xclk_handle);
        esp_cam_sensor_xclk_free(s_xclk_handle);
        s_xclk_handle = nullptr;
    }
    if (s_sccb_dev != nullptr) {
        i2c_master_bus_rm_device(s_sccb_dev);
        s_sccb_dev = nullptr;
    }
    if (s_cam_powered) {
        IOExpander::getInstance().setLevel(IOExpander::Pin::CAM_PWDN, true);
        s_cam_powered = false;
    }
}

void ClosePreviewOverlay() {
    if (s_preview_started) {
        CameraScreen::StopExternalPreview();
        s_preview_started = false;
    }
    if (s_preview_mask != nullptr) {
        lv_obj_delete(s_preview_mask);
        s_preview_mask = nullptr;
        s_preview_canvas = nullptr;
    }
}

void OnClosePreviewClicked(lv_event_t* /*e*/) {
    ClosePreviewOverlay();
}

void OpenPreviewOverlay() {
    if (s_preview_mask != nullptr) {
        return;
    }

    lv_obj_t* parent = TestUiGetScreen();
    if (parent == nullptr) {
        return;
    }

    // 预览走完整视频管线，先关掉 SCCB 探测用的 XCLK/上电，避免资源冲突。
    s_detect_done = true;
    StopCameraPower();

    CameraScreen::PreviewBuffer preview_buf = {};
    if (!CameraScreen::PreparePreviewBuffer(&preview_buf)) {
        ESP_LOGE(TAG, "prepare preview buffer failed");
        SetErrorText("预览缓冲失败");
        return;
    }

    lv_obj_t* mask = lv_obj_create(parent);
    s_preview_mask = mask;
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kTestPanelW, kTestPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);

    lv_obj_t* canvas_host = lv_obj_create(mask);
    screen_strip_obj_chrome(canvas_host);
    lv_obj_set_size(canvas_host, kPreviewAreaW, kPreviewAreaH);
    lv_obj_set_pos(canvas_host, 0, 0);
    lv_obj_set_style_bg_color(canvas_host, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(canvas_host, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(canvas_host, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(canvas_host);

    s_preview_canvas = lv_canvas_create(canvas_host);
    lv_canvas_set_buffer(s_preview_canvas, preview_buf.data, preview_buf.width,
                         preview_buf.height, LV_COLOR_FORMAT_RGB888);
    lv_obj_set_size(s_preview_canvas, preview_buf.width, preview_buf.height);
    lv_obj_set_pos(s_preview_canvas, 0, 0);
    screen_make_input_passive(s_preview_canvas);

    lv_obj_t* strip = lv_obj_create(mask);
    screen_strip_obj_chrome(strip);
    lv_obj_set_size(strip, kTestPanelW, kPreviewStripH);
    lv_obj_set_pos(strip, 0, kPreviewAreaH);
    lv_obj_set_style_bg_color(strip, lv_color_hex(kTestColorCardBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(strip, true);

    lv_obj_t* title = lv_label_create(strip);
    lv_label_set_text(title, "摄像头预览");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 28, 0);
    screen_make_input_passive(title);

    lv_obj_t* close_btn = lv_button_create(strip);
    lv_obj_remove_style_all(close_btn);
    lv_obj_set_size(close_btn, 140, 56);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -28, 0);
    lv_obj_set_style_radius(close_btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(kColorBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x1D4ED8),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    screen_swipe_back_ignore(close_btn, true);

    lv_obj_t* close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "关闭");
    lv_obj_set_style_text_color(close_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(close_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(close_lbl);
    lv_obj_remove_flag(close_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(close_btn, OnClosePreviewClicked, LV_EVENT_CLICKED,
                        nullptr);

    if (CameraScreen::StartExternalPreview(s_preview_canvas) != ESP_OK) {
        ESP_LOGE(TAG, "start preview failed");
        ClosePreviewOverlay();
        SetErrorText("预览启动失败");
        return;
    }

    s_preview_started = true;
    ESP_LOGI(TAG, "preview overlay started (%dx%d)", preview_buf.width,
             preview_buf.height);
}

void OnPreviewBtnClicked(lv_event_t* /*e*/) {
    OpenPreviewOverlay();
}

bool TryDetectSensorId() {
    if (!EnsureSccbDevice()) {
        SetErrorText("I2C未就绪");
        return true;
    }

    if (i2c_master_probe(metalio_claw_4_get_i2c_bus(), kOv2710Addr, 200) !=
        ESP_OK) {
        SetErrorText("SCCB无应答");
        ESP_LOGW(TAG, "SCCB probe @0x36 failed");
        return true;
    }

    uint8_t pid_h = 0;
    uint8_t pid_l = 0;
    if (!ReadSccbReg16(kOv2710RegPidH, &pid_h) ||
        !ReadSccbReg16(kOv2710RegPidL, &pid_l)) {
        SetErrorText("读取ID失败");
        ESP_LOGW(TAG, "read OV2710 PID failed");
        return true;
    }

    ESP_LOGI(TAG, "OV2710 PID=0x%02X%02X", pid_h, pid_l);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "ID 0x%02X%02X", pid_h, pid_l);
    if (pid_h == kOv2710PidH && pid_l == kOv2710PidL) {
        SetPassText(buf);
    } else {
        lv_label_set_text(s_value_lbl, buf);
        lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorError),
                                    LV_PART_MAIN);
        TestUiUpdateStatus(s_status_icon, false);
    }
    // 探测完成后关掉 XCLK/上电，预览时再由视频管线接管。
    StopCameraPower();
    return true;
}

}  // namespace

namespace CameraTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "摄像头", &s_status_icon, &ctrl);

    s_value_lbl = lv_label_create(ctrl);
    lv_label_set_text(s_value_lbl, "检测中...");
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_value_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_label_set_long_mode(s_value_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_value_lbl, 1);
    lv_obj_set_style_text_align(s_value_lbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    s_preview_btn = lv_button_create(ctrl);
    lv_obj_remove_style_all(s_preview_btn);
    lv_obj_set_size(s_preview_btn, 120, 52);
    lv_obj_set_style_radius(s_preview_btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_preview_btn, lv_color_hex(kColorBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_preview_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_preview_btn, lv_color_hex(0x1D4ED8),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_preview_btn, OnPreviewBtnClicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(s_preview_btn, true);

    lv_obj_t* preview_lbl = lv_label_create(s_preview_btn);
    lv_label_set_text(preview_lbl, "预览");
    lv_obj_set_style_text_color(preview_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(preview_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(preview_lbl);
    lv_obj_remove_flag(preview_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void OnLoad() {
    s_detect_done = false;
    s_preview_started = false;
    s_power_on_us = 0;
    if (!StartCameraPower()) {
        SetErrorText("上电失败");
        s_detect_done = true;
        return;
    }
    if (s_value_lbl != nullptr) {
        lv_label_set_text(s_value_lbl, "初始化中...");
        lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                    LV_PART_MAIN);
    }
}

void OnUnload() {
    ClosePreviewOverlay();
    StopCameraPower();
    s_value_lbl = nullptr;
    s_status_icon = nullptr;
    s_preview_btn = nullptr;
    s_detect_done = false;
    s_power_on_us = 0;
}

void Poll() {
    if (s_detect_done || s_value_lbl == nullptr || !s_cam_powered ||
        s_preview_mask != nullptr) {
        return;
    }

    const int64_t elapsed_ms =
        (esp_timer_get_time() - s_power_on_us) / 1000;
    if (elapsed_ms < kCamXclkSettleMs) {
        return;
    }

    s_detect_done = TryDetectSensorId();
}

}  // namespace CameraTest
