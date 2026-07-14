#include "stress_test_screen.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "camera_screen/camera_screen.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stress_demo.h"
#include "screen_util.h"
#include "test_screen.h"
#include "test_ui_common.h"
#include "vibrate_motor_test.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"

#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
#include "esp_gmf_pipeline.h"
#include "esp_gmf_rate_cvt.h"
#endif

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_50_4);

namespace {

constexpr const char* TAG = "StressTestScreen";
constexpr const char* kFactoryTestMount = "/factory_test";
constexpr const char* kBgMusicUri =
    "file://factory_test/factory_test_audio.mp3";

constexpr uint32_t kLvglMusicDurationMs = 5 * 60 * 1000;
constexpr uint32_t kMotorDurationMs = 30 * 1000;
constexpr uint32_t kCameraDurationMs = 30 * 1000;
static_assert(kLvglMusicDurationMs + kMotorDurationMs + kCameraDurationMs ==
                  6 * 60 * 1000,
              "stress cycle must be 6 minutes");

enum class StressPhase {
    LvglAndMusic,
    MotorVibrate,
    CameraPreview,
};

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_setup_panel = nullptr;
lv_obj_t* s_volume_pct_lbl = nullptr;

volatile bool s_bg_music_shutdown = false;
volatile bool s_bg_music_playing = false;
TaskHandle_t s_bg_music_task = nullptr;
esp_asp_handle_t s_bg_player = nullptr;
bool s_factory_test_mounted = false;
AudioCodec* s_audio_codec = nullptr;
std::vector<int16_t> s_bgm_pcm_buf;

StressPhase s_phase = StressPhase::LvglAndMusic;
lv_timer_t* s_cycle_timer = nullptr;
bool s_cycle_running = false;
lv_obj_t* s_cam_overlay = nullptr;
lv_obj_t* s_cam_canvas = nullptr;
bool s_cam_preview_active = false;

void StartStressCycle();
void StopStressCycle();

int ReadStressVolume() {
    int volume = 70;
    if (AudioCodec* codec = Board::GetInstance().GetAudioCodec()) {
        volume = codec->output_volume();
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    return volume;
}

void ApplyStressVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    if (s_volume_pct_lbl != nullptr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", volume);
        lv_label_set_text(s_volume_pct_lbl, buf);
    }

    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr || codec->output_volume() == volume) {
        return;
    }
    codec->SetOutputVolume(volume);
}

void OnVolumeSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    ApplyStressVolume(static_cast<int>(lv_slider_get_value(slider)));
}

void OnSwipeBackToMenu() {
    if (s_cycle_running) {
        return;
    }
    TestUiNavigateTo(TestScreen::Create);
}

void OnBackBtnClicked(lv_event_t* /*e*/) {
    OnSwipeBackToMenu();
}

void OnStartStressClicked(lv_event_t* /*e*/) {
    if (s_cycle_running || s_setup_panel == nullptr) {
        return;
    }

    lv_obj_add_flag(s_setup_panel, LV_OBJ_FLAG_HIDDEN);
    StartStressCycle();
}

void BuildSetupPanel(lv_obj_t* scr) {
    s_setup_panel = lv_obj_create(scr);
    screen_strip_obj_chrome(s_setup_panel);
    lv_obj_set_size(s_setup_panel, kTestPanelW, kTestPanelH);
    lv_obj_set_pos(s_setup_panel, 0, 0);
    lv_obj_set_style_bg_color(s_setup_panel, lv_color_hex(kTestColorBg),
                             LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_setup_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_setup_panel, LV_OBJ_FLAG_SCROLLABLE);

    TestUiCreateHeader(s_setup_panel, "压力测试", OnBackBtnClicked);

    const int initial_volume = ReadStressVolume();

    lv_obj_t* card = lv_obj_create(s_setup_panel);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kTestPanelW - 2 * kTestSideMargin, 220);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, kTestHeaderH + 24);
    lv_obj_set_style_bg_color(card, lv_color_hex(kTestColorCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* pct = lv_label_create(card);
    s_volume_pct_lbl = pct;
    lv_obj_set_width(pct, LV_PCT(100));
    lv_label_set_long_mode(pct, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(pct, lv_color_hex(0x60A5FA), LV_PART_MAIN);
    lv_obj_set_style_text_font(pct, &font_puhui_number_50_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, -20);
    ApplyStressVolume(initial_volume);

    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint, "背景音乐音量");
    lv_obj_set_style_text_color(hint, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_t* slider_row = lv_obj_create(s_setup_panel);
    lv_obj_remove_style_all(slider_row);
    lv_obj_set_size(slider_row, kTestPanelW - 2 * kTestSideMargin, 52);
    lv_obj_align(slider_row, LV_ALIGN_TOP_MID, 0, kTestHeaderH + 24 + 220 + 20);
    lv_obj_set_style_bg_opa(slider_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(slider_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider = lv_slider_create(slider_row);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 28);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, initial_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kTestColorMuted), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3B82F6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 10, LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, OnVolumeSliderChanged, LV_EVENT_VALUE_CHANGED,
                        nullptr);
    screen_swipe_back_ignore(slider, true);

    lv_obj_t* range = lv_label_create(s_setup_panel);
    lv_label_set_text(range, "0% ~ 100%");
    lv_obj_set_style_text_color(range, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(range, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(range, LV_ALIGN_TOP_MID, 0, kTestHeaderH + 24 + 220 + 20 + 56);

    lv_obj_t* start = lv_button_create(s_setup_panel);
    lv_obj_set_size(start, 320, 72);
    lv_obj_align(start, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_radius(start, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(start, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_add_event_cb(start, OnStartStressClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(start, true);

    lv_obj_t* start_lbl = lv_label_create(start);
    lv_label_set_text(start_lbl, "开始压力测试");
    lv_obj_set_style_text_color(start_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(start_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(start_lbl);

    lv_obj_t* foot = lv_label_create(s_setup_panel);
    lv_label_set_text(foot, "6 分钟循环：LVGL 压测 + 背景音乐 → 马达 → 摄像头");
    lv_obj_set_width(foot, kTestPanelW - 2 * kTestSideMargin);
    lv_label_set_long_mode(foot, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(foot, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -132);
}

extern "C" int BgMusicEventCallback(esp_asp_event_pkt_t* /*event*/, void* /*ctx*/) {
    // Required so run_to_end receives STOPPED/FINISHED via wait_event.
    return 0;
}

extern "C" int BgMusicOutCallback(uint8_t* data, int data_size, void* ctx) {
    auto* codec = static_cast<AudioCodec*>(ctx);
    if (codec == nullptr || data == nullptr || data_size <= 0) {
        return 0;
    }

    const int samples = data_size / static_cast<int>(sizeof(int16_t));
    if (samples <= 0) {
        return 0;
    }

    const auto* pcm_data = reinterpret_cast<const int16_t*>(data);
    s_bgm_pcm_buf.resize(static_cast<size_t>(samples));
    std::memcpy(s_bgm_pcm_buf.data(), pcm_data, static_cast<size_t>(data_size));
    codec->OutputData(s_bgm_pcm_buf);
    return 0;
}

extern "C" int BgMusicPrevCallback(esp_asp_handle_t* handle, void* ctx) {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    const esp_asp_handle_t player =
        reinterpret_cast<esp_asp_handle_t>(handle);
    if (player == nullptr) {
        return 0;
    }

    auto* codec = static_cast<AudioCodec*>(ctx);
    if (codec == nullptr) {
        return 0;
    }

    esp_gmf_pipeline_handle_t pipe = nullptr;
    esp_gmf_element_handle_t rate_el = nullptr;
    if (esp_audio_simple_player_get_pipeline(player, &pipe) != ESP_GMF_ERR_OK ||
        pipe == nullptr) {
        return 0;
    }
    if (esp_gmf_pipeline_get_el_by_name(pipe, "aud_rate_cvt", &rate_el) !=
            ESP_GMF_ERR_OK ||
        rate_el == nullptr) {
        return 0;
    }

    esp_gmf_rate_cvt_set_dest_rate(rate_el, codec->output_sample_rate());
#endif
    return 0;
}

bool MountFactoryTestPartition() {
    if (s_factory_test_mounted) {
        return true;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = kFactoryTestMount,
        .partition_label = "factory_test",
        .max_files = 2,
        .format_if_mount_failed = false,
    };

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount factory_test spiffs failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    s_factory_test_mounted = true;
    ESP_LOGI(TAG, "factory_test spiffs mounted at %s", kFactoryTestMount);
    return true;
}

void UnmountFactoryTestPartition() {
    if (!s_factory_test_mounted) {
        return;
    }

    esp_vfs_spiffs_unregister("factory_test");
    s_factory_test_mounted = false;
}

void BgMusicTask(void* /*arg*/) {
    esp_asp_cfg_t cfg = {
        .in = {},
        .out =
            {
                .cb = BgMusicOutCallback,
                .user_ctx = s_audio_codec,
            },
        .task_prio = 5,
        .task_stack = 8 * 1024,
        .prev = BgMusicPrevCallback,
        .prev_ctx = s_audio_codec,
    };

    if (esp_audio_simple_player_new(&cfg, &s_bg_player) != ESP_GMF_ERR_OK ||
        s_bg_player == nullptr) {
        ESP_LOGE(TAG, "create bg music player failed");
        s_bg_music_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    esp_audio_simple_player_set_event(s_bg_player, BgMusicEventCallback, nullptr);

    if (s_audio_codec != nullptr) {
        s_audio_codec->EnableOutput(true);
    }

    while (!s_bg_music_shutdown) {
        if (!s_bg_music_playing) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const esp_gmf_err_t err =
            esp_audio_simple_player_run_to_end(s_bg_player, kBgMusicUri, nullptr);
        if (err != ESP_GMF_ERR_OK) {
            ESP_LOGW(TAG, "bg music play ended/failed: 0x%x", err);
            if (!s_bg_music_playing || s_bg_music_shutdown) {
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    esp_audio_simple_player_stop(s_bg_player);
    esp_audio_simple_player_destroy(s_bg_player);
    s_bg_player = nullptr;
    s_bg_music_task = nullptr;
    vTaskDelete(nullptr);
}

void WaitBgMusicTaskStopped() {
    for (int i = 0; i < 100 && s_bg_music_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ShutdownBgMusicSession();

void InitBgMusicSession() {
    ShutdownBgMusicSession();

    s_audio_codec = Board::GetInstance().GetAudioCodec();
    if (s_audio_codec == nullptr) {
        ESP_LOGW(TAG, "no audio codec, skip bg music");
        return;
    }

    if (!MountFactoryTestPartition()) {
        s_audio_codec = nullptr;
        return;
    }

    s_bg_music_shutdown = false;
    s_bg_music_playing = false;

    const BaseType_t ok = xTaskCreate(BgMusicTask, "stress_bgm", 8192, nullptr,
                                      5, &s_bg_music_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create bg music task failed");
        UnmountFactoryTestPartition();
        s_audio_codec = nullptr;
        s_bg_music_task = nullptr;
    }
}

void ShutdownBgMusicSession() {
    s_bg_music_playing = false;
    s_bg_music_shutdown = true;

    if (s_bg_player != nullptr) {
        esp_audio_simple_player_stop(s_bg_player);
    }

    WaitBgMusicTaskStopped();
    UnmountFactoryTestPartition();
    s_audio_codec = nullptr;
}

void StartBgMusicForPhase() {
    if (s_bg_music_task == nullptr) {
        return;
    }
    s_bg_music_playing = true;
}

void PauseBgMusicForPhase() {
    s_bg_music_playing = false;
    if (s_bg_player != nullptr) {
        esp_audio_simple_player_stop(s_bg_player);
    }
}

void CleanupStressDemoWidgets() {
    if (s_screen == nullptr) {
        return;
    }

    uint32_t i = 0;
    while (i < lv_obj_get_child_count(s_screen)) {
        lv_obj_t* child = lv_obj_get_child(s_screen, i);
        if (child == s_setup_panel) {
            ++i;
            continue;
        }
        lv_obj_delete(child);
    }
}

void LogHeapFree(const char* where) {
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "%s: free internal=%lu spiram=%lu", where,
             static_cast<unsigned long>(internal),
             static_cast<unsigned long>(spiram));
}

void StopCameraPreview() {
    if (s_cam_preview_active) {
        CameraScreen::StopExternalPreview();
        s_cam_preview_active = false;
    }

    if (s_cam_canvas != nullptr) {
        lv_obj_delete(s_cam_canvas);
        s_cam_canvas = nullptr;
    }
    if (s_cam_overlay != nullptr) {
        lv_obj_delete(s_cam_overlay);
        s_cam_overlay = nullptr;
    }
}

void StartCameraPreview() {
    StopCameraPreview();

    CameraScreen::PreviewBuffer preview_buf = {};
    if (!CameraScreen::PreparePreviewBuffer(&preview_buf)) {
        ESP_LOGE(TAG, "prepare preview buffer failed");
        return;
    }

    s_cam_overlay = lv_obj_create(lv_layer_top());
    screen_strip_obj_chrome(s_cam_overlay);
    lv_obj_set_size(s_cam_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_cam_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cam_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cam_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_cam_overlay, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_cam_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_cam_canvas = lv_canvas_create(s_cam_overlay);
    lv_canvas_set_buffer(s_cam_canvas, preview_buf.data, preview_buf.width,
                         preview_buf.height, LV_COLOR_FORMAT_RGB888);
    lv_obj_set_size(s_cam_canvas, preview_buf.width, preview_buf.height);
    lv_obj_center(s_cam_canvas);
    screen_make_input_passive(s_cam_canvas);

    if (CameraScreen::StartExternalPreview(s_cam_canvas) != ESP_OK) {
        ESP_LOGE(TAG, "start fullscreen preview failed");
        StopCameraPreview();
        return;
    }

    s_cam_preview_active = true;
    ESP_LOGI(TAG, "camera preview started (%dx%d)", preview_buf.width,
             preview_buf.height);
}

void StopCycleTimer() {
    if (s_cycle_timer != nullptr) {
        lv_timer_delete(s_cycle_timer);
        s_cycle_timer = nullptr;
    }
}

void SchedulePhaseTimer(uint32_t duration_ms);

void EnterPhase(StressPhase phase);

void OnCycleTimer(lv_timer_t* /*timer*/) {
    s_cycle_timer = nullptr;
    if (!s_cycle_running) {
        return;
    }

    switch (s_phase) {
    case StressPhase::LvglAndMusic:
        EnterPhase(StressPhase::MotorVibrate);
        break;
    case StressPhase::MotorVibrate:
        EnterPhase(StressPhase::CameraPreview);
        break;
    case StressPhase::CameraPreview:
        EnterPhase(StressPhase::LvglAndMusic);
        break;
    }
}

void SchedulePhaseTimer(uint32_t duration_ms) {
    StopCycleTimer();
    s_cycle_timer = lv_timer_create(OnCycleTimer, duration_ms, nullptr);
    lv_timer_set_repeat_count(s_cycle_timer, 1);
}

void EnterPhase(StressPhase phase) {
    stress_demo_stop();
    CleanupStressDemoWidgets();
    PauseBgMusicForPhase();
    VibrateMotorTest::StopMotor();
    StopCameraPreview();

    s_phase = phase;

    uint32_t duration_ms = 0;
    switch (phase) {
    case StressPhase::LvglAndMusic:
        ESP_LOGI(TAG, "phase: LVGL stress + bg music (%lus)",
                 static_cast<unsigned long>(kLvglMusicDurationMs / 1000));
        stress_demo_start();
        StartBgMusicForPhase();
        duration_ms = kLvglMusicDurationMs;
        break;
    case StressPhase::MotorVibrate:
        ESP_LOGI(TAG, "phase: motor vibrate (%lus)",
                 static_cast<unsigned long>(kMotorDurationMs / 1000));
        VibrateMotorTest::StartMotor();
        duration_ms = kMotorDurationMs;
        break;
    case StressPhase::CameraPreview:
        ESP_LOGI(TAG, "phase: camera preview (%lus)",
                 static_cast<unsigned long>(kCameraDurationMs / 1000));
        StartCameraPreview();
        duration_ms = kCameraDurationMs;
        break;
    }

    if (s_cycle_running) {
        SchedulePhaseTimer(duration_ms);
    }

    LogHeapFree("phase entered");
}

void StartStressCycle() {
    StopStressCycle();

    auto& app = Application::GetInstance();
    app.SetActivationSuspended(true);
    app.StopSystemAudioForStressTest();

    VibrateMotorTest::OnLoad();
    InitBgMusicSession();

    s_cycle_running = true;
    EnterPhase(StressPhase::LvglAndMusic);
}

void StopStressCycle() {
    s_cycle_running = false;
    StopCycleTimer();
    stress_demo_stop();
    CleanupStressDemoWidgets();
    ShutdownBgMusicSession();
    VibrateMotorTest::StopMotor();
    VibrateMotorTest::OnUnload();
    StopCameraPreview();
    auto& app = Application::GetInstance();
    app.SetActivationSuspended(false);
    app.RestoreSystemAudioAfterStressTest();
    LogHeapFree("stress cycle stopped");
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    StopStressCycle();
    s_screen = nullptr;
    s_setup_panel = nullptr;
    s_volume_pct_lbl = nullptr;
}

}  // namespace

lv_obj_t* StressTestScreen::Create() {
    ESP_LOGI(TAG, "create stress test screen");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kTestColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildSetupPanel(scr);

    screen_attach_swipe_back(scr, OnSwipeBackToMenu);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}
