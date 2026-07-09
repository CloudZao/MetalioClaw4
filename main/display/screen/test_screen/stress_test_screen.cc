#include "stress_test_screen.h"

#include "audio_codec.h"
#include "board.h"
#include "camera_screen/camera_screen.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stress_demo.h"
#include "screen_util.h"
#include "vibrate_motor_test.h"

#include <vector>

#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"

#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
#include "esp_gmf_pipeline.h"
#include "esp_gmf_rate_cvt.h"
#endif

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

volatile bool s_bg_music_stop = false;
TaskHandle_t s_bg_music_task = nullptr;
esp_asp_handle_t s_bg_player = nullptr;
bool s_factory_test_mounted = false;
AudioCodec* s_audio_codec = nullptr;

StressPhase s_phase = StressPhase::LvglAndMusic;
lv_timer_t* s_cycle_timer = nullptr;
bool s_cycle_running = false;
lv_obj_t* s_cam_overlay = nullptr;
lv_obj_t* s_cam_canvas = nullptr;
bool s_cam_preview_active = false;

extern "C" int BgMusicOutCallback(uint8_t* data, int data_size, void* ctx) {
    auto* codec = static_cast<AudioCodec*>(ctx);
    if (codec == nullptr || data == nullptr || data_size <= 0) {
        return 0;
    }

    const int samples = data_size / static_cast<int>(sizeof(int16_t));
    if (samples <= 0) {
        return 0;
    }

    const auto* pcm_data = reinterpret_cast<int16_t*>(data);
    std::vector<int16_t> pcm(pcm_data, pcm_data + samples);
    codec->OutputData(pcm);
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

    if (s_audio_codec != nullptr) {
        s_audio_codec->EnableOutput(true);
    }

    while (!s_bg_music_stop) {
        const esp_gmf_err_t err =
            esp_audio_simple_player_run_to_end(s_bg_player, kBgMusicUri, nullptr);
        if (err != ESP_GMF_ERR_OK) {
            ESP_LOGW(TAG, "bg music play failed: 0x%x", err);
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

void StopBgMusic();

void StartBgMusic() {
    StopBgMusic();

    s_audio_codec = Board::GetInstance().GetAudioCodec();
    if (s_audio_codec == nullptr) {
        ESP_LOGW(TAG, "no audio codec, skip bg music");
        return;
    }

    if (!MountFactoryTestPartition()) {
        return;
    }

    s_bg_music_stop = false;

    const BaseType_t ok = xTaskCreate(BgMusicTask, "stress_bgm", 8192, nullptr,
                                      5, &s_bg_music_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create bg music task failed");
        UnmountFactoryTestPartition();
        s_audio_codec = nullptr;
        s_bg_music_task = nullptr;
    }
}

void StopBgMusic() {
    s_bg_music_stop = true;

    if (s_bg_player != nullptr) {
        esp_audio_simple_player_stop(s_bg_player);
    }

    WaitBgMusicTaskStopped();
    UnmountFactoryTestPartition();
    s_audio_codec = nullptr;
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

void StopStressCycle();

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
    StopBgMusic();
    VibrateMotorTest::StopMotor();
    StopCameraPreview();

    s_phase = phase;

    uint32_t duration_ms = 0;
    switch (phase) {
    case StressPhase::LvglAndMusic:
        ESP_LOGI(TAG, "phase: LVGL stress + bg music (%lus)",
                 static_cast<unsigned long>(kLvglMusicDurationMs / 1000));
        stress_demo_start();
        StartBgMusic();
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
}

void StartStressCycle() {
    StopStressCycle();

    VibrateMotorTest::OnLoad();

    s_cycle_running = true;
    EnterPhase(StressPhase::LvglAndMusic);
}

void StopStressCycle() {
    s_cycle_running = false;
    StopCycleTimer();
    stress_demo_stop();
    StopBgMusic();
    VibrateMotorTest::StopMotor();
    VibrateMotorTest::OnUnload();
    StopCameraPreview();
}

void OnScreenLoaded(lv_event_t* /*e*/) {
    StartStressCycle();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    StopStressCycle();
    s_screen = nullptr;
}

}  // namespace

lv_obj_t* StressTestScreen::Create() {
    ESP_LOGI(TAG, "create stress test screen");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(scr, OnScreenLoaded, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}
