#include "vibrate_screen.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "home_screen/home_screen.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_50_4);

namespace {

constexpr const char* TAG = "VibrateScreen";

// ---------------------------------------------------------------------------
// 硬件 / PWM
// ---------------------------------------------------------------------------
// !! 不能用 LEDC_TIMER_0 / LEDC_CHANNEL_0 —— 那是 PwmBacklight 给屏幕背光用的
// （见 main/boards/common/backlight.cc）。如果共用 channel 0，写 duty 实际会
// 改背光亮度，且会把 channel 0 的 GPIO 输出重新映射，导致 “拉滑条 -> 背光
// 变暗 / 马达不动” 的现象。
constexpr gpio_num_t            kVibrateGpio    = GPIO_NUM_22;
constexpr ledc_mode_t           kLedcMode       = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t          kLedcTimer      = LEDC_TIMER_1;
constexpr ledc_channel_t        kLedcChannel    = LEDC_CHANNEL_1;
constexpr ledc_timer_bit_t      kLedcDutyRes    = LEDC_TIMER_10_BIT;   // 0..1023
constexpr uint32_t              kLedcFreqHz     = 5000;                 // 5kHz, 高于人耳可听 + 马达响应充分
constexpr uint32_t              kLedcDutyMax    = (1U << kLedcDutyRes) - 1U;
constexpr uint32_t              kPatternTickMs  = 30;                   // pattern timer 步进

bool s_ledc_inited = false;

// ---------------------------------------------------------------------------
// 模式定义
// ---------------------------------------------------------------------------
enum class Mode : uint8_t {
    kManual = 0,    // duty 直接 = slider 当前值
    kPulse,         // 500ms 周期方波（半亮半暗）
    kHeartbeat,     // 1s 周期：100ms on / 100ms off / 100ms on / 700ms off
};

// 预设按钮，点哪个按钮 -> 把对应配置应用到 (slider, mode)：
//
//   - target_pct == -1 表示 “不修改 slider，仅切模式”，给“脉冲 / 心跳”用：
//     用户先调好强度，再点脉冲 / 心跳，让该强度作为脉冲幅值。
//   - 否则按下后会把 slider 设到 target_pct，并切换到 target_mode。
struct PresetEntry {
    const char* label;
    int8_t      target_pct;     // -1 表示沿用当前 slider 值
    Mode        target_mode;
};

constexpr PresetEntry kPresets[] = {
    { "关闭", 0,   Mode::kManual    },
    { "弱",   30,  Mode::kManual    },
    { "中",   60,  Mode::kManual    },
    { "强",   100, Mode::kManual    },
    { "脉冲", -1,  Mode::kPulse     },
    { "心跳", -1,  Mode::kHeartbeat },
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

// ---------------------------------------------------------------------------
// UI / 运行状态
// ---------------------------------------------------------------------------
struct UiState {
    lv_obj_t*  screen           = nullptr;
    lv_obj_t*  slider           = nullptr;
    lv_obj_t*  pct_label        = nullptr;     // 大字百分比 “60%”
    lv_obj_t*  mode_label       = nullptr;     // 当前模式名
    lv_obj_t*  preset_btns[kPresetCount] = {}; // 预设按钮指针，方便高亮当前选中
};

UiState     s_ui;
Mode        s_mode          = Mode::kManual;
int         s_slider_pct    = 0;        // 0..100
int         s_active_preset = 0;        // 当前高亮的按钮（默认“关闭”）

lv_timer_t* s_pattern_timer = nullptr;
int64_t     s_pattern_t0_us = 0;        // 模式启动时间，pattern 周期内递增

// ---------------------------------------------------------------------------
// LEDC 工具
// ---------------------------------------------------------------------------
void ledc_init_once() {
    if (s_ledc_inited) {
        return;
    }
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode      = kLedcMode;
    timer_cfg.timer_num       = kLedcTimer;
    timer_cfg.duty_resolution = kLedcDutyRes;
    timer_cfg.freq_hz         = kLedcFreqHz;
    timer_cfg.clk_cfg         = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config: %s", esp_err_to_name(err));
        return;
    }

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num   = kVibrateGpio;
    ch_cfg.speed_mode = kLedcMode;
    ch_cfg.channel    = kLedcChannel;
    ch_cfg.intr_type  = LEDC_INTR_DISABLE;
    ch_cfg.timer_sel  = kLedcTimer;
    ch_cfg.duty       = 0;
    ch_cfg.hpoint     = 0;
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config: %s", esp_err_to_name(err));
        return;
    }
    s_ledc_inited = true;
    ESP_LOGI(TAG, "LEDC ready: gpio=%d freq=%lu res=%d-bit", kVibrateGpio,
             static_cast<unsigned long>(kLedcFreqHz), kLedcDutyRes);
}

void apply_duty_pct(int pct) {
    if (!s_ledc_inited) {
        return;
    }
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    uint32_t duty = (static_cast<uint32_t>(pct) * kLedcDutyMax) / 100U;
    ledc_set_duty(kLedcMode, kLedcChannel, duty);
    ledc_update_duty(kLedcMode, kLedcChannel);
}

// ---------------------------------------------------------------------------
// 模式 / 模式 timer
// ---------------------------------------------------------------------------
void on_pattern_tick(lv_timer_t* /*t*/);

void stop_pattern_timer() {
    if (s_pattern_timer != nullptr) {
        lv_timer_delete(s_pattern_timer);
        s_pattern_timer = nullptr;
    }
}

void start_pattern_timer() {
    if (s_pattern_timer != nullptr) {
        return;
    }
    s_pattern_t0_us = esp_timer_get_time();
    s_pattern_timer = lv_timer_create(on_pattern_tick, kPatternTickMs, nullptr);
}

// 把 (mode, pct) 应用到 PWM。负责开 / 关 pattern timer，恒定模式直接拉
// duty，脉冲 / 心跳模式由 timer 周期性切换。
void apply_state() {
    if (s_mode == Mode::kManual) {
        stop_pattern_timer();
        apply_duty_pct(s_slider_pct);
    } else {
        // 脉冲 / 心跳：先把当前周期重置，再启动 timer，立即敲一次让用户能听到响应。
        if (s_pattern_timer == nullptr) {
            start_pattern_timer();
        } else {
            s_pattern_t0_us = esp_timer_get_time();
        }
        on_pattern_tick(nullptr);
    }
}

void on_pattern_tick(lv_timer_t* /*t*/) {
    int64_t  now    = esp_timer_get_time();
    uint32_t elapsed_ms =
        static_cast<uint32_t>((now - s_pattern_t0_us) / 1000);

    bool on = false;
    if (s_mode == Mode::kPulse) {
        // 500ms 周期：前半亮、后半暗
        on = (elapsed_ms % 500U) < 250U;
    } else if (s_mode == Mode::kHeartbeat) {
        // 1000ms 周期：0..100 亮，100..200 暗，200..300 亮，剩下暗
        uint32_t p = elapsed_ms % 1000U;
        on = (p < 100U) || (p >= 200U && p < 300U);
    }
    apply_duty_pct(on ? s_slider_pct : 0);
}

// ---------------------------------------------------------------------------
// 进入 / 离开屏幕时统一停摆
// ---------------------------------------------------------------------------
void shutdown_pwm() {
    stop_pattern_timer();
    apply_duty_pct(0);
}

// ---------------------------------------------------------------------------
// UI 同步
// ---------------------------------------------------------------------------
const char* mode_name(Mode m) {
    switch (m) {
    case Mode::kManual:    return s_slider_pct == 0 ? "关闭" : "恒定";
    case Mode::kPulse:     return "脉冲";
    case Mode::kHeartbeat: return "心跳";
    }
    return "?";
}

void refresh_pct_label() {
    if (s_ui.pct_label == nullptr) return;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d%%", s_slider_pct);
    lv_label_set_text(s_ui.pct_label, buf);
}

void refresh_mode_label() {
    if (s_ui.mode_label == nullptr) return;
    lv_label_set_text(s_ui.mode_label, mode_name(s_mode));
}

void refresh_preset_buttons() {
    for (int i = 0; i < kPresetCount; i++) {
        lv_obj_t* btn = s_ui.preset_btns[i];
        if (btn == nullptr) continue;
        bool active = (i == s_active_preset);
        // 高亮用深蓝 (0x3B82F6) + 白字，对比度 ~5:1，比之前的黄绿底白字清晰得多。
        lv_obj_set_style_bg_color(
            btn, lv_color_hex(active ? 0x3B82F6 : 0x2A2F3A), LV_PART_MAIN);
        lv_obj_set_style_border_color(
            btn, lv_color_hex(active ? 0x60A5FA : 0x3A4050), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, active ? 2 : 0, LV_PART_MAIN);
    }
}

// ---------------------------------------------------------------------------
// 事件回调
// ---------------------------------------------------------------------------
void on_slider_value_changed(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    s_slider_pct = static_cast<int>(lv_slider_get_value(slider));

    // 用户拖滑条 -> 自动回到 “手动恒定” 模式。
    s_mode = Mode::kManual;
    // 同步选中态：slider 拖到的值如果正好命中某个预设挡位（0/30/60/100）
    // 就高亮对应按钮，否则不高亮任何挡。
    s_active_preset = -1;
    for (int i = 0; i < kPresetCount; i++) {
        if (kPresets[i].target_mode == Mode::kManual &&
            kPresets[i].target_pct  == s_slider_pct) {
            s_active_preset = i;
            break;
        }
    }

    refresh_pct_label();
    refresh_mode_label();
    refresh_preset_buttons();
    apply_state();
}

void on_preset_clicked(lv_event_t* e) {
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= kPresetCount) return;
    const PresetEntry& p = kPresets[idx];

    if (p.target_pct >= 0) {
        s_slider_pct = p.target_pct;
        if (s_ui.slider != nullptr) {
            lv_slider_set_value(s_ui.slider, s_slider_pct, LV_ANIM_ON);
        }
    }
    s_mode          = p.target_mode;
    s_active_preset = idx;

    refresh_pct_label();
    refresh_mode_label();
    refresh_preset_buttons();
    apply_state();
}

// ---------------------------------------------------------------------------
// 屏幕导航
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

void on_back_btn_clicked(lv_event_t* /*e*/) {
    OnSwipeBack();
}

void on_screen_unloaded(lv_event_t* /*e*/) {
    // 清掉对 LVGL 对象的引用——屏幕会被 lv_obj_delete_async 释放。
    s_ui.screen     = nullptr;
    s_ui.slider     = nullptr;
    s_ui.pct_label  = nullptr;
    s_ui.mode_label = nullptr;
    for (int i = 0; i < kPresetCount; i++) {
        s_ui.preset_btns[i] = nullptr;
    }
    // pattern timer 是建在 LVGL 主线程里的 lv_timer，屏幕没了也得关掉。
    stop_pattern_timer();
    // 离开屏幕一律静默：避免人离开页面马达还在响。
    apply_duty_pct(0);
}

}  // namespace

// ===========================================================================
// 公共接口
// ===========================================================================
lv_obj_t* VibrateScreen::Create() {
    ledc_init_once();

    constexpr int kPanelW = 720;
    constexpr int kPanelH = 720;

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen   = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---------------- 顶栏 ----------------
    lv_obj_t* header = lv_obj_create(scr);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, 90);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // 左上角返回按钮：透明圆形按钮 + "←" 图标，按下时白色半透明叠加
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
    lv_obj_add_event_cb(back, on_back_btn_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "震动");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    // ---------------- 大数显（百分比） ----------------
    lv_obj_t* card = lv_obj_create(scr);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 660, 220);
    lv_obj_set_pos(card, 30, 100);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(card);

    lv_obj_t* pct = lv_label_create(card);
    s_ui.pct_label = pct;
    lv_obj_set_width(pct, LV_PCT(100));
    lv_label_set_long_mode(pct, LV_LABEL_LONG_CLIP);
    lv_label_set_text(pct, "0%");
    lv_obj_set_style_text_color(pct, lv_color_hex(0x60A5FA), LV_PART_MAIN);
    lv_obj_set_style_text_font(pct, &font_puhui_number_50_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t* mode_lbl = lv_label_create(card);
    s_ui.mode_label = mode_lbl;
    lv_label_set_text(mode_lbl, "关闭");
    lv_obj_set_style_text_color(mode_lbl, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(mode_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(mode_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);

    // ---------------- 滑条 ----------------
    constexpr int kSliderY = 350;
    lv_obj_t* slider = lv_slider_create(scr);
    s_ui.slider = slider;
    lv_obj_set_size(slider, 600, 36);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, kSliderY);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3B82F6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 14, LV_PART_KNOB);    // 加大旋钮，方便手指拖
    lv_obj_set_style_radius(slider, 18, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 18, LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, on_slider_value_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, "拖动滑条调整振幅");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, kSliderY + 50);

    // ---------------- 预设模式按钮（2 行 x 3 列） ----------------
    constexpr int kButtonsRowH = 80;
    constexpr int kButtonsGap  = 16;
    constexpr int kButtonsCols = 3;
    constexpr int kButtonW = (kPanelW - 60 - (kButtonsCols - 1) * kButtonsGap) / kButtonsCols;
    constexpr int kButtonsTop = 460;

    for (int i = 0; i < kPresetCount; i++) {
        const int row = i / kButtonsCols;
        const int col = i % kButtonsCols;
        int x = 30 + col * (kButtonW + kButtonsGap);
        int y = kButtonsTop + row * (kButtonsRowH + kButtonsGap);

        lv_obj_t* btn = lv_button_create(scr);
        s_ui.preset_btns[i] = btn;
        lv_obj_set_size(btn, kButtonW, kButtonsRowH);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, on_preset_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, kPresets[i].label);
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_center(lbl);
    }

    // ---------------- 默认状态：关闭 ----------------
    s_slider_pct    = 0;
    s_mode          = Mode::kManual;
    s_active_preset = 0;   // “关闭”
    refresh_pct_label();
    refresh_mode_label();
    refresh_preset_buttons();
    apply_duty_pct(0);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}

void VibrateScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: vibrate_screen");
        // 首次或冷启动后，确保 LEDC 已初始化但 duty=0。
        ledc_init_once();
        apply_duty_pct(0);
    } else {
        ESP_LOGI(TAG, "unload: vibrate_screen -> stop pattern + duty=0");
        shutdown_pwm();
    }
}
