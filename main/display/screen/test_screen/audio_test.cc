#include "audio_test.h"

#include "application.h"
#include "audio_service.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "screen_util.h"
#include "test_ui_common.h"

LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char* TAG = "AudioTest";
constexpr uint32_t kMaxRecordMs = 5000;

constexpr uint32_t kColorBtnIdle      = 0x2563EB;
constexpr uint32_t kColorBtnRecording = 0xDC2626;
constexpr uint32_t kColorBtnPlaying    = 0x4B5563;

enum class State {
    Idle,
    Recording,
    Playing,
};

constexpr uint32_t kPlaybackMarginMs = 500;
constexpr uint32_t kConfirmDelayMs    = 1000;

State     s_state             = State::Idle;
bool      s_wake_disabled     = false;
int64_t   s_record_start_us   = 0;
lv_obj_t* s_status_icon       = nullptr;
lv_obj_t* s_record_btn        = nullptr;
lv_obj_t* s_record_lbl        = nullptr;
lv_timer_t* s_max_record_timer = nullptr;
lv_timer_t* s_playback_timer   = nullptr;
lv_timer_t* s_confirm_timer    = nullptr;

void UpdateButtonUi();

void StopConfirmTimer() {
    if (s_confirm_timer != nullptr) {
        lv_timer_delete(s_confirm_timer);
        s_confirm_timer = nullptr;
    }
}

void OnAudioConfirmResult(bool pass, void* user_data) {
    TestUiUpdateStatus(static_cast<lv_obj_t*>(user_data), pass);
    ESP_LOGI(TAG, "user confirm audio: %s", pass ? "pass" : "fail");
}

void OnConfirmTimer(lv_timer_t* /*t*/) {
    s_confirm_timer = nullptr;
    TestUiShowConfirmDialog("录音、喇叭是否正常？", OnAudioConfirmResult,
                            s_status_icon);
}

void ScheduleConfirmDialog() {
    StopConfirmTimer();
    s_confirm_timer = lv_timer_create(OnConfirmTimer, kConfirmDelayMs, nullptr);
    lv_timer_set_repeat_count(s_confirm_timer, 1);
}

void StopPlaybackTimer() {
    if (s_playback_timer != nullptr) {
        lv_timer_delete(s_playback_timer);
        s_playback_timer = nullptr;
    }
}

void FinishPlaybackUi() {
    if (s_state != State::Playing) {
        return;
    }
    StopPlaybackTimer();
    s_state = State::Idle;
    UpdateButtonUi();
    ScheduleConfirmDialog();
    ESP_LOGI(TAG, "playback finished");
}

void OnPlaybackTimer(lv_timer_t* /*t*/) {
    s_playback_timer = nullptr;
    FinishPlaybackUi();
}

void SchedulePlaybackUiReset() {
    const int64_t now = esp_timer_get_time();
    int duration_ms = static_cast<int>((now - s_record_start_us) / 1000);
    if (duration_ms < 1) {
        duration_ms = 1;
    }
    const uint32_t wait_ms =
        static_cast<uint32_t>(duration_ms) + kPlaybackMarginMs;

    StopPlaybackTimer();
    s_playback_timer = lv_timer_create(OnPlaybackTimer, wait_ms, nullptr);
    lv_timer_set_repeat_count(s_playback_timer, 1);
}

void RestoreWakeWord() {
    if (!s_wake_disabled) {
        return;
    }
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
    s_wake_disabled = false;
}

void DisableWakeWordIfNeeded() {
    auto& as = Application::GetInstance().GetAudioService();
    if (!as.IsWakeWordRunning()) {
        return;
    }
    as.EnableWakeWordDetection(false);
    s_wake_disabled = true;
}

void UpdateButtonUi() {
    if (s_record_btn == nullptr || s_record_lbl == nullptr) {
        return;
    }

    switch (s_state) {
    case State::Idle:
        lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(kColorBtnIdle),
                                  LV_PART_MAIN);
        lv_label_set_text(s_record_lbl, "按住录音");
        lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case State::Recording:
        lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(kColorBtnRecording),
                                  LV_PART_MAIN);
        lv_label_set_text(s_record_lbl, "录音中…");
        lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case State::Playing:
        lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(kColorBtnPlaying),
                                  LV_PART_MAIN);
        lv_label_set_text(s_record_lbl, "播放中…");
        lv_obj_remove_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    }
}

void StopMaxRecordTimer() {
    if (s_max_record_timer != nullptr) {
        lv_timer_delete(s_max_record_timer);
        s_max_record_timer = nullptr;
    }
}

void StopRecordingAndPlay() {
    if (s_state != State::Recording) {
        return;
    }

    StopMaxRecordTimer();
    auto& as = Application::GetInstance().GetAudioService();
    as.EnableAudioTesting(false);
    RestoreWakeWord();
    s_state = State::Playing;
    UpdateButtonUi();
    SchedulePlaybackUiReset();
    ESP_LOGI(TAG, "record stopped, playback started");
}

void OnMaxRecordTimer(lv_timer_t* /*t*/) {
    s_max_record_timer = nullptr;
    ESP_LOGI(TAG, "max record duration reached (%lums)", kMaxRecordMs);
    StopRecordingAndPlay();
}

void StartRecording() {
    if (s_state != State::Idle) {
        return;
    }

    StopConfirmTimer();
    TestUiDismissConfirmDialog();

    auto& as = Application::GetInstance().GetAudioService();
    if (!as.IsStarted()) {
        ESP_LOGW(TAG, "audio service not started");
        return;
    }
    if (as.IsAudioProcessorRunning()) {
        ESP_LOGW(TAG, "audio processor busy, skip record");
        return;
    }

    DisableWakeWordIfNeeded();
    as.ResetDecoder();
    as.EnableAudioTesting(true);

    s_record_start_us = esp_timer_get_time();
    s_state = State::Recording;
    UpdateButtonUi();
    StopMaxRecordTimer();
    s_max_record_timer = lv_timer_create(OnMaxRecordTimer, kMaxRecordMs, nullptr);
    lv_timer_set_repeat_count(s_max_record_timer, 1);
    ESP_LOGI(TAG, "recording started");
}

void OnRecordPressed(lv_event_t* /*e*/) {
    StartRecording();
}

void OnRecordReleased(lv_event_t* /*e*/) {
    if (s_state == State::Recording) {
        StopRecordingAndPlay();
    }
}

void ForceStopWithoutPlayback() {
    StopMaxRecordTimer();
    StopPlaybackTimer();
    StopConfirmTimer();
    auto& as = Application::GetInstance().GetAudioService();
    if (s_state == State::Recording) {
        as.ResetDecoder();
        as.EnableAudioTesting(false);
    } else if (s_state == State::Playing) {
        as.ResetDecoder();
    }
    RestoreWakeWord();
    s_state = State::Idle;
}

}  // namespace

namespace AudioTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "音频", &s_status_icon, &ctrl);

    s_record_btn = lv_button_create(ctrl);
    lv_obj_remove_style_all(s_record_btn);
    lv_obj_set_size(s_record_btn, 180, 52);
    lv_obj_set_style_radius(s_record_btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(kColorBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_record_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(0x1D4ED8),
                              LV_PART_MAIN | LV_STATE_PRESSED);

    s_record_lbl = lv_label_create(s_record_btn);
    lv_label_set_text(s_record_lbl, "按住录音");
    lv_obj_set_style_text_color(s_record_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_record_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(s_record_lbl);

    lv_obj_add_event_cb(s_record_btn, OnRecordPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_record_btn, OnRecordReleased, LV_EVENT_RELEASED,
                        nullptr);
    screen_swipe_back_ignore(s_record_btn, true);
}

void OnLoad() {
    s_state = State::Idle;
    UpdateButtonUi();
}

void OnUnload() {
    ForceStopWithoutPlayback();
    s_record_btn = nullptr;
    s_record_lbl = nullptr;
    s_status_icon = nullptr;
}

void Poll() {
    // 播放结束由 SchedulePlaybackUiReset() 定时器驱动；Poll 仅作兜底。
    if (s_state != State::Playing || s_playback_timer != nullptr) {
        return;
    }
    FinishPlaybackUi();
}

}  // namespace AudioTest
