#include "magnet_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/i2c_master.h>

#include "home_screen/home_screen.h"
#include "i2c_device.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

extern "C" i2c_master_bus_handle_t xingzhi_395_get_i2c_bus();

namespace {

constexpr const char* TAG = "MagnetScreen";

// ---------------------------------------------------------------------------
// QMC6309 (I2C 0x7C，7-bit) 三轴磁力计 —— 本页面只做数据可视化，不读姿态、
// 不做倾角补偿、不做 hard-iron 校准。
//
// 关键寄存器（出自规格书 §7 Register Map）：
//   0x00  CHIP_ID         RO   读到 0x90 表示芯片在线
//   0x01..0x06  XL/XH/YL/YH/ZL/ZH  RO  16-bit signed little-endian raw 输出
//   0x09  STATUS          RO   bit0=DRDY, bit1=OVL
//   0x0A  CR1             RW   配置寄存器 1：OSR / RNG / ODR / MODE
//   0x0B  CR2             RW   bit7=SOFT_RST，低位=SET/RESET 自校准频率
//
// 模式机抖动：手册要求从 suspend 进 continuous，必须 suspend → normal →
// suspend → continuous 走一遍，且每步 ≥10ms 让内部 SET/RESET 线圈完成
// self-cal，否则 DRDY 大概率不出。
// ---------------------------------------------------------------------------
constexpr uint8_t kQmcAddr        = 0x7C;
constexpr uint8_t kQmcRegChipId   = 0x00;
constexpr uint8_t kQmcRegXOutL    = 0x01;
constexpr uint8_t kQmcRegStatus   = 0x09;
constexpr uint8_t kQmcRegCr1      = 0x0A;
constexpr uint8_t kQmcRegCr2      = 0x0B;
constexpr uint8_t kQmcChipId      = 0x90;
constexpr uint8_t kQmcStatusDrdy  = 0x01;
constexpr uint8_t kQmcStatusOvl   = 0x02;

constexpr uint8_t kQmcOsr11       = 0xC0;   // bits 7:6
constexpr uint8_t kQmcRng01       = 0x10;   // bits 5:4
constexpr uint8_t kQmcModeSuspend = 0x00;
constexpr uint8_t kQmcModeNormal  = 0x01;
constexpr uint8_t kQmcModeContin  = 0x03;
constexpr uint8_t kQmcCr1Base     = kQmcOsr11 | kQmcRng01;  // 0xD0
constexpr uint8_t kQmcCr2Val      = 0x03;
constexpr uint8_t kQmcCr2SoftRst  = 0x80;

// 单位换算：手册标称 1 LSB ≈ 1 mG（在我们用的 RNG=01 量程下），
// 1 mG = 100 nT = 0.1 µT。读数大时显示 Gauss = LSB / 1000。
constexpr float kLsbToMilliGauss = 1.0f;
constexpr float kLsbToMicroTesla = 0.1f;
constexpr float kLsbToGauss      = 0.001f;

class Qmc6309 : public I2cDevice {
public:
    Qmc6309(i2c_master_bus_handle_t bus, uint8_t addr) : I2cDevice(bus, addr) {}

    bool Probe() {
        uint8_t id = ReadReg(kQmcRegChipId);
        last_chip_id_ = id;
        if (id != kQmcChipId) {
            ESP_LOGW(TAG, "QMC6309 CHIP_ID=0x%02X (expect 0x90)，按 best-effort 继续", id);
            return false;
        }
        ESP_LOGI(TAG, "QMC6309 online, CHIP_ID=0x%02X", id);
        return true;
    }

    // 严格按 PCBCUPID 实测可用的时序复位 + 进入 continuous 模式。
    void Configure() {
        WriteReg(kQmcRegCr2, kQmcCr2SoftRst);
        vTaskDelay(pdMS_TO_TICKS(25));
        WriteReg(kQmcRegCr2, 0x00);
        vTaskDelay(pdMS_TO_TICKS(25));

        SetMode(kQmcModeSuspend);
        SetMode(kQmcModeNormal);
        SetMode(kQmcModeSuspend);
        SetMode(kQmcModeContin);
        vTaskDelay(pdMS_TO_TICKS(50));

        uint8_t cr1 = ReadReg(kQmcRegCr1);
        uint8_t cr2 = ReadReg(kQmcRegCr2);
        ESP_LOGI(TAG, "QMC6309 configured: CR1=0x%02X CR2=0x%02X", cr1, cr2);
    }

    // 读一组 (X,Y,Z) raw 值。DRDY 仅作 “是否新鲜” 的返回值，寄存器始终读，
    // 因为 continuous 模式下数据寄存器保留最近一次有效采样。
    bool ReadMagRaw(int16_t* mx, int16_t* my, int16_t* mz) {
        uint8_t st = ReadReg(kQmcRegStatus);
        last_status_ = st;
        uint8_t buf[6] = {0};
        ReadRegs(kQmcRegXOutL, buf, sizeof(buf));
        *mx = static_cast<int16_t>((buf[1] << 8) | buf[0]);
        *my = static_cast<int16_t>((buf[3] << 8) | buf[2]);
        *mz = static_cast<int16_t>((buf[5] << 8) | buf[4]);
        if (st & kQmcStatusOvl) {
            ESP_LOGD(TAG, "QMC6309 OVL: raw=%d,%d,%d", *mx, *my, *mz);
        }
        return (st & kQmcStatusDrdy) != 0;
    }

    uint8_t LastChipId() const { return last_chip_id_; }
    uint8_t LastStatus() const { return last_status_; }

private:
    void SetMode(uint8_t mode) {
        uint8_t cr1 = kQmcCr1Base | (mode & 0x03);
        WriteReg(kQmcRegCr1, cr1);
        WriteReg(kQmcRegCr2, kQmcCr2Val);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint8_t last_chip_id_ = 0;
    uint8_t last_status_  = 0;
};

// ---------------------------------------------------------------------------
// 几何 / 视觉常量 (720x720 面板)
// ---------------------------------------------------------------------------
constexpr int kPanelW          = 720;
constexpr int kPanelH          = 720;
constexpr int kHeaderH         = 90;

// 三轴卡片区
constexpr int kCardLeft        = 36;
constexpr int kCardW           = kPanelW - kCardLeft * 2;   // 648
constexpr int kCardH           = 150;
constexpr int kCardGap         = 14;
constexpr int kCardTopY        = kHeaderH + 12;
constexpr int kBarH            = 12;

// 量程参考：在 ±32G / 1mG/LSB 下 raw 范围 ±32768，但常用磁场都 < ±5G ≈ 5000 LSB。
// 进度条按 ±kBarRangeLsb 映射，超出截断。
constexpr int kBarRangeLsb     = 5000;

// 底部汇总区
constexpr int kSummaryTopY     = kCardTopY + kCardH * 3 + kCardGap * 3;
constexpr int kSummaryH        = kPanelH - kSummaryTopY;

// 采样间隔：50ms (20Hz)
constexpr uint32_t kSamplePeriodMs = 50;

// ---------------------------------------------------------------------------
// 模块全局：传感器实例 + 帧计数 + UI 句柄
// ---------------------------------------------------------------------------
Qmc6309*   s_mag       = nullptr;
bool       s_mag_init  = false;
uint32_t   s_frame_cnt = 0;

// 三轴对应一组 UI 控件，每组包括标题、读数、条形指示
struct AxisRow {
    lv_obj_t* card        = nullptr;
    lv_obj_t* title_lbl   = nullptr;   // "X 轴"
    lv_obj_t* raw_lbl     = nullptr;   // "+1234 LSB"
    lv_obj_t* phys_lbl    = nullptr;   // "+1.234 G   +123.4 µT"
    lv_obj_t* bar         = nullptr;   // 条形指示（lv_bar）
};

struct UiState {
    lv_obj_t* screen        = nullptr;
    lv_obj_t* sensor_lbl    = nullptr;  // 顶栏右侧芯片状态
    AxisRow   axes[3];                  // 0:X  1:Y  2:Z
    lv_obj_t* total_lbl     = nullptr;  // "|B| = 1500 LSB / 1.5 G / 150 µT"
    lv_obj_t* status_lbl    = nullptr;  // "STATUS=0x01  FRAME=42"
    lv_timer_t* sample_timer = nullptr;
};

UiState s_ui;

// ---------------------------------------------------------------------------
// 懒初始化：第一次进入屏幕 / lifecycle LOAD 时调用
// ---------------------------------------------------------------------------
void EnsureSensorInited() {
    if (s_mag != nullptr && s_mag_init) return;
    i2c_master_bus_handle_t bus = xingzhi_395_get_i2c_bus();
    if (bus == nullptr) {
        ESP_LOGE(TAG, "I2C bus 未就绪，跳过 QMC6309 初始化");
        return;
    }
    if (s_mag == nullptr) {
        if (i2c_master_probe(bus, kQmcAddr, 100) != ESP_OK) {
            ESP_LOGW(TAG, "I2C probe 0x%02X 失败：未检测到 QMC6309", kQmcAddr);
            return;
        }
        s_mag = new Qmc6309(bus, kQmcAddr);
    }
    s_mag->Probe();             // chip id 只打日志，不阻塞
    s_mag->Configure();
    s_mag_init = true;
}

// ---------------------------------------------------------------------------
// 把一帧 (mx, my, mz) 反映到 UI
// ---------------------------------------------------------------------------
const char* AxisTitle(int idx) {
    static const char* kNames[] = {"X 轴", "Y 轴", "Z 轴"};
    return kNames[idx];
}

uint32_t AxisColor(int idx) {
    // X 红，Y 绿，Z 蓝 —— 跟普通示波器 / 3D 软件配色对齐。
    static const uint32_t kColors[] = {0xF87171, 0x34D399, 0x60A5FA};
    return kColors[idx];
}

void UpdateAxisRow(int idx, int16_t raw) {
    AxisRow& row = s_ui.axes[idx];
    if (row.raw_lbl != nullptr) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%+d  LSB", raw);
        lv_label_set_text(row.raw_lbl, buf);
    }
    if (row.phys_lbl != nullptr) {
        float gauss = raw * kLsbToGauss;
        float ut    = raw * kLsbToMicroTesla;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%+.3f G    %+.1f µT", gauss, ut);
        lv_label_set_text(row.phys_lbl, buf);
    }
    if (row.bar != nullptr) {
        int v = std::clamp(static_cast<int>(raw), -kBarRangeLsb, kBarRangeLsb);
        lv_bar_set_value(row.bar, v, LV_ANIM_OFF);
    }
}

void UpdateUi(int16_t mx, int16_t my, int16_t mz, bool sensor_ok) {
    if (sensor_ok) {
        UpdateAxisRow(0, mx);
        UpdateAxisRow(1, my);
        UpdateAxisRow(2, mz);
    } else {
        for (int i = 0; i < 3; ++i) {
            if (s_ui.axes[i].raw_lbl  != nullptr) lv_label_set_text(s_ui.axes[i].raw_lbl,  "-- LSB");
            if (s_ui.axes[i].phys_lbl != nullptr) lv_label_set_text(s_ui.axes[i].phys_lbl, "-- G   -- µT");
            if (s_ui.axes[i].bar      != nullptr) lv_bar_set_value(s_ui.axes[i].bar, 0, LV_ANIM_OFF);
        }
    }

    if (s_ui.total_lbl != nullptr) {
        if (sensor_ok) {
            float mag_lsb = std::sqrt(static_cast<float>(mx) * mx +
                                      static_cast<float>(my) * my +
                                      static_cast<float>(mz) * mz);
            float gauss = mag_lsb * kLsbToGauss;
            float ut    = mag_lsb * kLsbToMicroTesla;
            char buf[80];
            std::snprintf(buf, sizeof(buf), "|B| = %.0f LSB   %.3f G   %.1f µT",
                          mag_lsb, gauss, ut);
            lv_label_set_text(s_ui.total_lbl, buf);
        } else {
            lv_label_set_text(s_ui.total_lbl, "|B| = --");
        }
    }

    if (s_ui.status_lbl != nullptr) {
        if (sensor_ok && s_mag != nullptr) {
            uint8_t st = s_mag->LastStatus();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "STATUS=0x%02X  DRDY=%d  OVL=%d  FRAME=%lu",
                          st, (st & kQmcStatusDrdy) ? 1 : 0,
                          (st & kQmcStatusOvl) ? 1 : 0,
                          static_cast<unsigned long>(s_frame_cnt));
            lv_label_set_text(s_ui.status_lbl, buf);
        } else {
            lv_label_set_text(s_ui.status_lbl, "STATUS=--  未检测到 QMC6309");
        }
    }
}

// ---------------------------------------------------------------------------
// 主采样回调：每 50ms 跑一次
// ---------------------------------------------------------------------------
void OnSampleTick(lv_timer_t* /*t*/) {
    int16_t mx = 0, my = 0, mz = 0;
    bool fresh = false;
    if (s_mag_init && s_mag != nullptr) {
        fresh = s_mag->ReadMagRaw(&mx, &my, &mz);
    }
    s_frame_cnt++;

    // 启动前 10 帧打印 raw 值方便定位芯片初始化是否成功。
    static int s_dbg_ticks = 0;
    if (s_dbg_ticks < 10 && s_mag_init) {
        ESP_LOGI(TAG, "frame %d: drdy=%d status=0x%02X raw mx=%d my=%d mz=%d",
                 s_dbg_ticks, fresh ? 1 : 0,
                 s_mag ? s_mag->LastStatus() : 0,
                 mx, my, mz);
        s_dbg_ticks++;
    }

    UpdateUi(mx, my, mz, s_mag_init);
}

// ---------------------------------------------------------------------------
// 屏幕导航 / 生命周期
// ---------------------------------------------------------------------------
void OnSwipeBack() {
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

void OnBackBtnClicked(lv_event_t* /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_ui.sample_timer != nullptr) {
        lv_timer_delete(s_ui.sample_timer);
        s_ui.sample_timer = nullptr;
    }
    s_ui = UiState{};
    s_frame_cnt = 0;
}

// ---------------------------------------------------------------------------
// UI 构建
// ---------------------------------------------------------------------------
void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int kBackBtnSize = 72;
    lv_obj_t* back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackBtnClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "磁场");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    lv_obj_t* sensor_lbl = lv_label_create(header);
    s_ui.sensor_lbl = sensor_lbl;
    char buf[48];
    if (s_mag_init && s_mag != nullptr) {
        std::snprintf(buf, sizeof(buf), "QMC6309  ID 0x%02X", s_mag->LastChipId());
    } else {
        std::snprintf(buf, sizeof(buf), "QMC6309  --");
    }
    lv_label_set_text(sensor_lbl, buf);
    lv_obj_set_style_text_color(sensor_lbl,
                                s_mag_init ? lv_color_hex(0x9AA3B2) : lv_color_hex(0xF87171),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(sensor_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(sensor_lbl, LV_ALIGN_RIGHT_MID, -16, 0);
}

void BuildAxisCard(lv_obj_t* parent, int idx) {
    int y = kCardTopY + idx * (kCardH + kCardGap);
    uint32_t color = AxisColor(idx);

    lv_obj_t* card = lv_obj_create(parent);
    s_ui.axes[idx].card = card;
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_set_pos(card, kCardLeft, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_60, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

    // 左上：轴标题（"X 轴"）
    lv_obj_t* title_lbl = lv_label_create(card);
    s_ui.axes[idx].title_lbl = title_lbl;
    lv_label_set_text(title_lbl, AxisTitle(idx));
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 24, 12);

    // 右上：raw LSB 大字
    lv_obj_t* raw_lbl = lv_label_create(card);
    s_ui.axes[idx].raw_lbl = raw_lbl;
    lv_label_set_text(raw_lbl, "-- LSB");
    lv_obj_set_style_text_color(raw_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(raw_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(raw_lbl, LV_ALIGN_TOP_RIGHT, -24, 12);

    // 物理单位（G / µT）—— 中等字号
    lv_obj_t* phys_lbl = lv_label_create(card);
    s_ui.axes[idx].phys_lbl = phys_lbl;
    lv_label_set_text(phys_lbl, "-- G   -- µT");
    lv_obj_set_style_text_color(phys_lbl, lv_color_hex(0xC7CDD9), LV_PART_MAIN);
    lv_obj_set_style_text_font(phys_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(phys_lbl, LV_ALIGN_LEFT_MID, 24, 10);

    // 进度条：双向（-kBarRangeLsb..+kBarRangeLsb），中点对齐
    lv_obj_t* bar = lv_bar_create(card);
    s_ui.axes[idx].bar = bar;
    lv_obj_set_size(bar, kCardW - 48, kBarH);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_bar_set_mode(bar, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_range(bar, -kBarRangeLsb, kBarRangeLsb);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0B0F18), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, kBarH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, kBarH / 2, LV_PART_INDICATOR);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
}

void BuildSummary(lv_obj_t* parent) {
    // |B| 总磁场强度
    lv_obj_t* total = lv_label_create(parent);
    s_ui.total_lbl = total;
    lv_label_set_text(total, "|B| = --");
    lv_obj_set_style_text_color(total, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(total, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_pos(total, 0, kSummaryTopY + 4);
    lv_obj_set_width(total, kPanelW);
    lv_obj_set_style_text_align(total, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // STATUS / FRAME 调试信息
    lv_obj_t* status = lv_label_create(parent);
    s_ui.status_lbl = status;
    lv_label_set_text(status, "STATUS=--  FRAME=0");
    lv_obj_set_style_text_color(status, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(status, 0, kSummaryTopY + 50);
    lv_obj_set_width(status, kPanelW);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

}  // namespace

// ===========================================================================
// 公共接口
// ===========================================================================
lv_obj_t* MagnetScreen::Create() {
    EnsureSensorInited();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui = UiState{};
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildAxisCard(scr, 0);
    BuildAxisCard(scr, 1);
    BuildAxisCard(scr, 2);
    BuildSummary(scr);

    s_ui.sample_timer = lv_timer_create(OnSampleTick, kSamplePeriodMs, nullptr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    ESP_LOGI(TAG, "magnet screen ready (mag=%s)", s_mag_init ? "ok" : "absent");
    return scr;
}

void MagnetScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: magnet_screen");
        EnsureSensorInited();
    } else {
        ESP_LOGI(TAG, "unload: magnet_screen");
        // sample timer 已在 OnScreenUnloaded 中关闭；I2C 设备保留下次复用。
    }
}
