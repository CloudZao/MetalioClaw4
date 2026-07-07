#include "vibrate_motor_test.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "test_ui_common.h"

namespace {

constexpr const char* TAG = "VibrateMotorTest";
constexpr uint32_t kConfirmDelayMs = 1000;

constexpr gpio_num_t       kVibrateGpio = GPIO_NUM_22;
constexpr ledc_mode_t      kLedcMode    = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t     kLedcTimer   = LEDC_TIMER_1;
constexpr ledc_channel_t   kLedcChannel = LEDC_CHANNEL_1;
constexpr ledc_timer_bit_t kLedcDutyRes = LEDC_TIMER_10_BIT;
constexpr uint32_t         kLedcFreqHz  = 5000;
constexpr uint32_t         kLedcDutyMax = (1U << kLedcDutyRes) - 1U;
constexpr int              kOnDutyPct   = 60;

bool        s_ledc_inited    = false;
bool        s_motor_on       = false;
lv_obj_t*   s_status_icon    = nullptr;
lv_timer_t* s_confirm_timer  = nullptr;

void StopConfirmTimer() {
    if (s_confirm_timer != nullptr) {
        lv_timer_delete(s_confirm_timer);
        s_confirm_timer = nullptr;
    }
}

void OnConfirmResult(bool pass, void* user_data) {
    TestUiUpdateStatus(static_cast<lv_obj_t*>(user_data), pass);
    ESP_LOGI(TAG, "user confirm vibrate motor: %s", pass ? "pass" : "fail");
}

void OnConfirmTimer(lv_timer_t* /*t*/) {
    s_confirm_timer = nullptr;
    TestUiShowConfirmDialog("是否正常震动？", OnConfirmResult, s_status_icon);
}

void ScheduleConfirmDialog() {
    StopConfirmTimer();
    s_confirm_timer = lv_timer_create(OnConfirmTimer, kConfirmDelayMs, nullptr);
    lv_timer_set_repeat_count(s_confirm_timer, 1);
}

void LedcInitOnce() {
    if (s_ledc_inited) {
        return;
    }

    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode      = kLedcMode;
    timer_cfg.timer_num       = kLedcTimer;
    timer_cfg.duty_resolution = kLedcDutyRes;
    timer_cfg.freq_hz         = kLedcFreqHz;
    timer_cfg.clk_cfg         = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed");
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
    if (ledc_channel_config(&ch_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed");
        return;
    }

    s_ledc_inited = true;
    ESP_LOGI(TAG, "vibrate motor PWM ready on GPIO%d",
             static_cast<int>(kVibrateGpio));
}

void ApplyDutyPct(int pct) {
    if (!s_ledc_inited) {
        return;
    }
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    const uint32_t duty =
        (static_cast<uint32_t>(pct) * kLedcDutyMax) / 100U;
    ledc_set_duty(kLedcMode, kLedcChannel, duty);
    ledc_update_duty(kLedcMode, kLedcChannel);
}

void OnSwitchChanged(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target_obj(e);
    s_motor_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ApplyDutyPct(s_motor_on ? kOnDutyPct : 0);
    ESP_LOGI(TAG, "vibrate motor %s", s_motor_on ? "ON" : "OFF");

    if (s_motor_on) {
        ScheduleConfirmDialog();
    } else {
        StopConfirmTimer();
    }
}

}  // namespace

namespace VibrateMotorTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "震动马达", &s_status_icon, &ctrl);
    TestUiCreateSwitch(ctrl, OnSwitchChanged, nullptr);
}

void OnLoad() {
    LedcInitOnce();
    ApplyDutyPct(0);
    s_motor_on = false;
}

void OnUnload() {
    StopConfirmTimer();
    s_motor_on = false;
    ApplyDutyPct(0);
    s_status_icon = nullptr;
}

}  // namespace VibrateMotorTest
