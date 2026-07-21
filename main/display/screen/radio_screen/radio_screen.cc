#include "radio_screen.h"
#include "i18n.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"

#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_crt_bundle.h"
#include "esp_fourcc.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_fft.h"
#include "esp_gmf_fft_heap.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_hls_io.h"
#include "media_lib_adapter.h"

#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
#include "esp_gmf_rate_cvt.h"
#endif

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "RadioScreen";
constexpr const char* kRadioStreamUrl =
    "http://satellitepull.cnr.cn/live/wxszfy971/playlist.m3u8";
constexpr const char* kStationNameMsgId = "深圳飞扬971";
constexpr int kRadioSampleRate = 16000;

constexpr int32_t kPanelSize = 720;
constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorBgGrad = 0x161A22;
constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
constexpr uint32_t kColorSubtle = 0x9AA3B2;
constexpr uint32_t kColorAccent = 0xE0FB3C;
constexpr uint32_t kColorCtrlBtnBg = 0x232732;
constexpr uint32_t kColorCtrlBtnBgPressed = 0x303644;
constexpr uint32_t kColorPlayBtnBg = 0x3A4150;
constexpr uint32_t kColorPlayBtnBgPressed = 0x4A5260;
constexpr uint32_t kColorVizFloor = 0x1C2230;

constexpr int32_t kTitleY = 48;
constexpr int32_t kStatusY = 100;
constexpr int32_t kVizY = 150;
constexpr int32_t kVizW = 520;
constexpr int32_t kVizH = 280;
constexpr int32_t kCtrlRowY = 560;
constexpr int32_t kCtrlRowWidth = 520;
constexpr int32_t kCtrlRowHeight = 120;
constexpr int32_t kCtrlSideBtnSize = 80;
constexpr int32_t kCtrlPlayBtnSize = 112;
constexpr int kVolStep = 5;

constexpr int kFftN = 256;
constexpr int kBarCount = 12;
constexpr int kBarGap = 18;
constexpr int kBarMinH = 12;
constexpr int kBarMaxH = kVizH - 28;
constexpr size_t kPcmRingSize = 2048;  // 2^n
constexpr float kAttack = 0.55f;
constexpr float kRelease = 0.18f;

// 彩虹基色：红→橙→黄→绿→青→蓝→紫（按柱位取色，再按幅度提亮）
constexpr uint32_t kRainbowBase[kBarCount] = {
    0xFF3B5C, 0xFF6B2D, 0xFFB020, 0xFFE84A, 0x7CFF4A, 0x2DFFB0,
    0x2DE8FF, 0x3D8BFF, 0x6B5CFF, 0xB24DFF, 0xFF4DC8, 0xFF4D7A,
};

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

enum class StatusKind { Connecting, Playing, Paused, Failed };

struct RadioUi {
    lv_obj_t* lbl_title = nullptr;
    lv_obj_t* lbl_status = nullptr;
    lv_obj_t* img_play_icon = nullptr;
    lv_obj_t* viz_host = nullptr;
    lv_obj_t* bars[kBarCount] = {};
    lv_obj_t* lbl_volume = nullptr;
    lv_timer_t* viz_timer = nullptr;
};

RadioUi s_ui;
lv_obj_t* s_bound_scr = nullptr;  // 仅当前前台页可改 UI / 响应 unload

enum class LifeState : uint8_t {
    Idle = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
};

std::atomic<LifeState> s_life{LifeState::Idle};
std::atomic<bool> s_restart_pending{false};
std::atomic<bool> s_stop_worker_busy{false};
std::atomic<bool> s_screen_active{false};
std::atomic<bool> s_want_play{false};
std::atomic<bool> s_shutdown{false};
std::atomic<bool> s_stop_pending{false};
std::atomic<int> s_pcm_channels{2};
std::atomic<bool> s_spectrum_run{false};
std::atomic<uint32_t> s_session_gen{0};
std::atomic<int> s_last_status{static_cast<int>(StatusKind::Connecting)};
std::atomic<bool> s_last_play_icon{true};
std::atomic<bool> s_reported_playing{false};

SemaphoreHandle_t s_life_mu = nullptr;
TaskHandle_t s_play_task = nullptr;
TaskHandle_t s_spectrum_task = nullptr;
esp_asp_handle_t s_player = nullptr;
AudioCodec* s_codec = nullptr;
std::vector<int16_t> s_pcm_buf;
bool s_media_lib_ready = false;
uint32_t s_hls_last_format = 0;

alignas(16) int16_t s_pcm_ring[kPcmRingSize];
std::atomic<uint32_t> s_pcm_w{0};
std::atomic<uint32_t> s_pcm_r{0};

portMUX_TYPE s_bar_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_bar_levels[kBarCount] = {};

void SetStatus(StatusKind kind);
void SetPlayIcon(bool playing);
void SetStatusDirect(StatusKind kind);
void SetPlayIconDirect(bool playing);
void SyncStatusToUi();
void UpdateVolumeLabel();
void RequestPlayerStop();
void RequestSessionStart();
void RequestSessionStop(bool allow_restart);
void SessionStartWorker(void* arg);
void SessionStopWorker(void* arg);
void KickSessionStopWorker();
void StartSpectrumAnalyzer();
void StopSpectrumAnalyzer();
void EnsureLifeMutex();
void WaitTaskGone(TaskHandle_t* slot, int max_ms);

uint32_t LerpColor(uint32_t a, uint32_t b, float t) {
    if (t <= 0.f) {
        return a;
    }
    if (t >= 1.f) {
        return b;
    }
    const auto ch = [&](int shift) {
        const int ca = static_cast<int>((a >> shift) & 0xFF);
        const int cb = static_cast<int>((b >> shift) & 0xFF);
        return static_cast<uint32_t>(ca + (cb - ca) * t);
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// 柱位彩虹色，幅度越高越亮（向白提亮）
uint32_t BarColor(int index, float level) {
    const uint32_t base = kRainbowBase[index % kBarCount];
    const float boost = 0.25f + 0.75f * std::clamp(level, 0.f, 1.f);
    return LerpColor(0x2A2F3A, LerpColor(base, 0xFFFFFF, boost * 0.35f),
                     boost);
}

uint32_t Isqrt32(uint32_t n) {
    if (n == 0) {
        return 0;
    }
    uint32_t x = n;
    uint32_t y = (n + 1u) >> 1;
    while (y < x) {
        x = y;
        y = (x + n / x) >> 1;
    }
    return x;
}

uint32_t BinMagnitude(const int16_t* data, unsigned k) {
    int32_t re = 0;
    int32_t im = 0;
    if (k == 0) {
        re = data[0];
    } else if (k == static_cast<unsigned>(kFftN / 2)) {
        re = data[1];
    } else {
        re = data[2u * k];
        im = data[2u * k + 1u];
    }
    return Isqrt32(static_cast<uint32_t>(re * re) +
                   static_cast<uint32_t>(im * im));
}

void PushPcmMono(const int16_t* samples, int n) {
    if (samples == nullptr || n <= 0) {
        return;
    }
    uint32_t w = s_pcm_w.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        s_pcm_ring[w & (kPcmRingSize - 1)] = samples[i];
        ++w;
    }
    s_pcm_w.store(w, std::memory_order_release);
    // 避免写指针甩开读指针超过一圈
    uint32_t r = s_pcm_r.load(std::memory_order_relaxed);
    if (static_cast<uint32_t>(w - r) > kPcmRingSize) {
        s_pcm_r.store(w - kPcmRingSize, std::memory_order_relaxed);
    }
}

bool PopPcmMono(int16_t* out, int n) {
    if (out == nullptr || n <= 0) {
        return false;
    }
    const uint32_t w = s_pcm_w.load(std::memory_order_acquire);
    uint32_t r = s_pcm_r.load(std::memory_order_relaxed);
    if (static_cast<uint32_t>(w - r) < static_cast<uint32_t>(n)) {
        return false;
    }
    for (int i = 0; i < n; ++i) {
        out[i] = s_pcm_ring[r & (kPcmRingSize - 1)];
        ++r;
    }
    s_pcm_r.store(r, std::memory_order_release);
    return true;
}

void PublishBarLevels(const float* levels) {
    portENTER_CRITICAL(&s_bar_mux);
    for (int i = 0; i < kBarCount; ++i) {
        const float v = std::clamp(levels[i], 0.f, 1.f);
        s_bar_levels[i] = static_cast<uint8_t>(v * 255.f + 0.5f);
    }
    portEXIT_CRITICAL(&s_bar_mux);
}

void ClearBarLevels() {
    portENTER_CRITICAL(&s_bar_mux);
    std::memset(s_bar_levels, 0, sizeof(s_bar_levels));
    portEXIT_CRITICAL(&s_bar_mux);
}

void ApplyVizFrame() {
    if (!s_screen_active.load(std::memory_order_relaxed) ||
        s_ui.viz_host == nullptr) {
        return;
    }

    uint8_t levels[kBarCount];
    portENTER_CRITICAL(&s_bar_mux);
    std::memcpy(levels, s_bar_levels, sizeof(levels));
    portEXIT_CRITICAL(&s_bar_mux);

    const int32_t bar_w = (kVizW - 48 - (kBarCount - 1) * kBarGap) / kBarCount;
    const int32_t usable_w = kBarCount * bar_w + (kBarCount - 1) * kBarGap;
    const int32_t x0 = (kVizW - usable_w) / 2;

    for (int i = 0; i < kBarCount; ++i) {
        lv_obj_t* bar = s_ui.bars[i];
        if (bar == nullptr) {
            continue;
        }
        const float t = levels[i] / 255.f;
        const int32_t h =
            kBarMinH + static_cast<int32_t>((kBarMaxH - kBarMinH) * t);
        lv_obj_set_height(bar, h);
        lv_obj_set_style_bg_color(bar, lv_color_hex(BarColor(i, t)),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(
            bar, static_cast<lv_opa_t>(140 + static_cast<int>(115 * t)),
            LV_PART_MAIN);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, x0 + i * (bar_w + kBarGap), -10);
    }
}

void VizTimerCb(lv_timer_t* /*t*/) {
    ApplyVizFrame();
}

void SpectrumTask(void* /*arg*/) {
    esp_gmf_fft_handle_t fft = nullptr;
    const esp_gmf_fft_cfg_t cfg = {
        .n_fft = static_cast<int16_t>(kFftN),
        .fft_type = ESP_GMF_FFT_TYPE_REAL_Q15,
    };
    if (esp_gmf_fft_init(&cfg, &fft) != ESP_GMF_FFT_OK || fft == nullptr) {
        ESP_LOGE(TAG, "gmf_fft init failed");
        s_spectrum_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    auto* fft_buf = static_cast<int16_t*>(esp_gmf_fft_calloc_aligned(
        ESP_GMF_FFT_BUFFER_SIZE(kFftN), sizeof(int16_t), 16));
    if (fft_buf == nullptr) {
        esp_gmf_fft_deinit(&fft);
        s_spectrum_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // 预计算 Hann 窗（Q15）
    int16_t hann[kFftN];
    for (int i = 0; i < kFftN; ++i) {
        const float w =
            0.5f * (1.f - std::cos(2.f * static_cast<float>(M_PI) * i /
                                   static_cast<float>(kFftN - 1)));
        hann[i] = static_cast<int16_t>(w * 32767.f);
    }

    // 对数频带：覆盖约 60Hz ~ 6kHz（16kHz Nyquist）
    int band_lo[kBarCount];
    int band_hi[kBarCount];
    const float f_min = 60.f;
    const float f_max = 6000.f;
    const float bin_hz = static_cast<float>(kRadioSampleRate) / kFftN;
    for (int b = 0; b < kBarCount; ++b) {
        const float t0 = static_cast<float>(b) / kBarCount;
        const float t1 = static_cast<float>(b + 1) / kBarCount;
        const float f0 = f_min * std::pow(f_max / f_min, t0);
        const float f1 = f_min * std::pow(f_max / f_min, t1);
        band_lo[b] = std::max(1, static_cast<int>(f0 / bin_hz));
        band_hi[b] = std::min(kFftN / 2 - 1, static_cast<int>(f1 / bin_hz));
        if (band_hi[b] < band_lo[b]) {
            band_hi[b] = band_lo[b];
        }
    }

    float smooth[kBarCount] = {};
    int16_t pcm[kFftN];

    while (s_spectrum_run.load(std::memory_order_relaxed)) {
        if (!s_want_play.load(std::memory_order_relaxed) ||
            s_shutdown.load(std::memory_order_relaxed)) {
            for (int i = 0; i < kBarCount; ++i) {
                smooth[i] *= (1.f - kRelease);
            }
            PublishBarLevels(smooth);
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        if (!PopPcmMono(pcm, kFftN)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        for (int i = 0; i < kFftN; ++i) {
            const int32_t v =
                (static_cast<int32_t>(pcm[i]) * hann[i]) >> 15;
            fft_buf[i] = static_cast<int16_t>(v);
        }
        // 半谱缓冲尾部清零
        for (size_t i = kFftN; i < ESP_GMF_FFT_BUFFER_SIZE(kFftN); ++i) {
            fft_buf[i] = 0;
        }

        if (esp_gmf_fft_forward(fft, fft_buf) != ESP_GMF_FFT_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        float raw[kBarCount];
        float peak = 1.f;
        for (int b = 0; b < kBarCount; ++b) {
            uint32_t sum = 0;
            int count = 0;
            for (int k = band_lo[b]; k <= band_hi[b]; ++k) {
                sum += BinMagnitude(fft_buf, static_cast<unsigned>(k));
                ++count;
            }
            const float mag =
                (count > 0) ? (static_cast<float>(sum) / count) : 0.f;
            raw[b] = mag;
            if (mag > peak) {
                peak = mag;
            }
        }

        // 相对峰值 + 轻度压缩，避免静音闪烁、大声爆顶
        for (int b = 0; b < kBarCount; ++b) {
            float n = raw[b] / peak;
            n = std::sqrt(std::clamp(n, 0.f, 1.f));
            if (n > smooth[b]) {
                smooth[b] += (n - smooth[b]) * kAttack;
            } else {
                smooth[b] += (n - smooth[b]) * kRelease;
            }
        }
        PublishBarLevels(smooth);

        // ~30 FPS 上限，错开音频回调
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ClearBarLevels();
    esp_gmf_fft_free_aligned(fft_buf);
    esp_gmf_fft_deinit(&fft);
    s_spectrum_task = nullptr;
    vTaskDelete(nullptr);
}

void EnsureLifeMutex() {
    if (s_life_mu != nullptr) {
        return;
    }
    s_life_mu = xSemaphoreCreateMutex();
}

void WaitTaskGone(TaskHandle_t* slot, int max_ms) {
    if (slot == nullptr) {
        return;
    }
    const int steps = std::max(1, max_ms / 20);
    for (int i = 0; i < steps && *slot != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void StartSpectrumAnalyzer() {
    if (s_spectrum_task != nullptr) {
        return;
    }
    s_pcm_w.store(0, std::memory_order_relaxed);
    s_pcm_r.store(0, std::memory_order_relaxed);
    ClearBarLevels();
    s_spectrum_run.store(true, std::memory_order_release);
    if (xTaskCreate(SpectrumTask, "radio_fft", 6144, nullptr, 4,
                    &s_spectrum_task) != pdPASS) {
        s_spectrum_run.store(false, std::memory_order_relaxed);
        s_spectrum_task = nullptr;
        ESP_LOGW(TAG, "spectrum task create failed");
    }
}

void StopSpectrumAnalyzer() {
    s_spectrum_run.store(false, std::memory_order_release);
    WaitTaskGone(&s_spectrum_task, 1500);
    if (s_spectrum_task != nullptr) {
        ESP_LOGW(TAG, "spectrum task still alive, force-clear handle");
        s_spectrum_task = nullptr;
    }
    ClearBarLevels();
}

void EnsureMediaLibAdapter() {
    if (!s_media_lib_ready) {
        media_lib_add_default_adapter();
        s_media_lib_ready = true;
    }
}

esp_gmf_err_t HttpTlsGetScore(esp_gmf_io_handle_t /*handle*/, const char* url,
                              int* score) {
    *score = ESP_GMF_IO_SCORE_NONE;
    if (url == nullptr) {
        return ESP_GMF_ERR_OK;
    }
    const char* file_name = strrchr(url, '/');
    file_name = (file_name != nullptr) ? (file_name + 1) : url;
    if (strstr(file_name, ".m3u8") != nullptr ||
        strstr(file_name, ".M3U8") != nullptr) {
        return ESP_GMF_ERR_OK;
    }
    if (strncasecmp(url, "http://", 7) == 0 ||
        strncasecmp(url, "https://", 8) == 0) {
        *score = ESP_GMF_IO_SCORE_STANDARD + 10;
    }
    return ESP_GMF_ERR_OK;
}

bool RegisterHttpWithCertBundle(esp_asp_handle_t player) {
    http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.dir = ESP_GMF_IO_DIR_READER;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_gmf_io_handle_t http_io = nullptr;
    if (esp_gmf_io_http_init(&http_cfg, &http_io) != ESP_GMF_ERR_OK ||
        http_io == nullptr) {
        return false;
    }
    reinterpret_cast<esp_gmf_io_t*>(http_io)->get_score = HttpTlsGetScore;
    if (esp_audio_simple_player_register_io(player, http_io) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(http_io);
        return false;
    }
    return true;
}

void ForcePoolRateCvt16k(esp_gmf_pool_handle_t pool) {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    if (pool == nullptr) {
        return;
    }
    const void* it = nullptr;
    esp_gmf_element_handle_t el = nullptr;
    while (esp_gmf_pool_iterate_element(pool, &it, &el) == ESP_GMF_ERR_OK) {
        if (el == nullptr) {
            continue;
        }
        const char* tag = OBJ_GET_TAG(el);
        if (tag == nullptr || strcmp(tag, "aud_rate_cvt") != 0) {
            continue;
        }
        esp_gmf_rate_cvt_set_dest_rate(el, static_cast<uint32_t>(kRadioSampleRate));
        auto* cfg = static_cast<esp_ae_rate_cvt_cfg_t*>(OBJ_GET_CFG(el));
        if (cfg != nullptr) {
            cfg->dest_rate = static_cast<uint32_t>(kRadioSampleRate);
        }
    }
#else
    (void)pool;
#endif
}

void ApplyRateCvt16k(esp_asp_handle_t player) {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    esp_gmf_pipeline_handle_t pipe = nullptr;
    esp_gmf_element_handle_t rate_el = nullptr;
    if (esp_audio_simple_player_get_pipeline(player, &pipe) != ESP_GMF_ERR_OK ||
        pipe == nullptr) {
        return;
    }
    if (esp_gmf_pipeline_get_el_by_name(pipe, "aud_rate_cvt", &rate_el) ==
            ESP_GMF_ERR_OK &&
        rate_el != nullptr) {
        esp_gmf_rate_cvt_set_dest_rate(rate_el,
                                       static_cast<uint32_t>(kRadioSampleRate));
    }
#else
    (void)player;
#endif
}

int RadioOutCallback(uint8_t* data, int data_size, void* ctx) {
    if (!s_want_play.load(std::memory_order_relaxed) ||
        s_shutdown.load(std::memory_order_relaxed)) {
        return 0;
    }
    auto* codec = static_cast<AudioCodec*>(ctx);
    if (codec == nullptr || data == nullptr || data_size <= 0) {
        return 0;
    }

    const int frames = data_size / static_cast<int>(sizeof(int16_t));
    if (frames <= 0) {
        return 0;
    }
    const auto* pcm = reinterpret_cast<const int16_t*>(data);
    const int ch = s_pcm_channels.load(std::memory_order_relaxed);

    if (ch >= 2) {
        const int mono = frames / ch;
        if (mono <= 0) {
            return 0;
        }
        s_pcm_buf.resize(static_cast<size_t>(mono));
        for (int i = 0; i < mono; ++i) {
            int sum = 0;
            for (int c = 0; c < ch; ++c) {
                sum += pcm[i * ch + c];
            }
            s_pcm_buf[static_cast<size_t>(i)] =
                static_cast<int16_t>(sum / ch);
        }
    } else {
        s_pcm_buf.assign(pcm, pcm + frames);
    }
    PushPcmMono(s_pcm_buf.data(), static_cast<int>(s_pcm_buf.size()));
    // 首包 PCM：RUNNING 事件可能早于 UI 就绪被丢掉，这里兜底切「正在播放」
    if (!s_reported_playing.exchange(true, std::memory_order_acq_rel)) {
        SetStatus(StatusKind::Playing);
        SetPlayIcon(true);
    }
    codec->OutputData(s_pcm_buf);
    return 0;
}

extern "C" int RadioPrevCallback(esp_asp_handle_t* handle, void* ctx) {
    (void)ctx;
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    ApplyRateCvt16k(reinterpret_cast<esp_asp_handle_t>(handle));
#else
    (void)handle;
#endif
    return 0;
}

struct UiStatusMsg {
    StatusKind kind;
};
struct UiPlayIconMsg {
    bool playing;
};

void AsyncApplyStatus(void* user_data) {
    auto* msg = static_cast<UiStatusMsg*>(user_data);
    if (s_screen_active.load(std::memory_order_relaxed) &&
        s_ui.lbl_status != nullptr) {
        SetStatusDirect(msg->kind);
    }
    delete msg;
}

void AsyncApplyPlayIcon(void* user_data) {
    auto* msg = static_cast<UiPlayIconMsg*>(user_data);
    if (s_screen_active.load(std::memory_order_relaxed) &&
        s_ui.img_play_icon != nullptr) {
        SetPlayIconDirect(msg->playing);
    }
    delete msg;
}

void SetStatusDirect(StatusKind kind) {
    s_last_status.store(static_cast<int>(kind), std::memory_order_relaxed);
    if (s_ui.lbl_status == nullptr) {
        return;
    }
    const char* text = I18n::T("连接中");
    switch (kind) {
        case StatusKind::Connecting:
            text = I18n::T("连接中");
            break;
        case StatusKind::Playing:
            text = I18n::T("正在播放");
            break;
        case StatusKind::Paused:
            text = I18n::T("已暂停");
            break;
        case StatusKind::Failed:
            text = I18n::T("播放失败");
            break;
    }
    lv_label_set_text(s_ui.lbl_status, text);
}

void SetPlayIconDirect(bool playing) {
    s_last_play_icon.store(playing, std::memory_order_relaxed);
    if (s_ui.img_play_icon == nullptr) {
        return;
    }
    lv_image_set_src(s_ui.img_play_icon, playing ? "A:ic_s_player_pause.spng"
                                                 : "A:ic_s_player_play.spng");
}

void SyncStatusToUi() {
    if (!s_screen_active.load(std::memory_order_relaxed)) {
        return;
    }
    SetStatusDirect(static_cast<StatusKind>(
        s_last_status.load(std::memory_order_relaxed)));
    SetPlayIconDirect(s_last_play_icon.load(std::memory_order_relaxed));
}

void SetStatus(StatusKind kind) {
    s_last_status.store(static_cast<int>(kind), std::memory_order_relaxed);
    if (!s_screen_active.load(std::memory_order_relaxed)) {
        return;
    }
    auto* msg = new UiStatusMsg{kind};
    if (lv_async_call(AsyncApplyStatus, msg) != LV_RESULT_OK) {
        delete msg;
    }
}

void SetPlayIcon(bool playing) {
    s_last_play_icon.store(playing, std::memory_order_relaxed);
    if (!s_screen_active.load(std::memory_order_relaxed)) {
        return;
    }
    auto* msg = new UiPlayIconMsg{playing};
    if (lv_async_call(AsyncApplyPlayIcon, msg) != LV_RESULT_OK) {
        delete msg;
    }
}

int RadioEventCallback(esp_asp_event_pkt_t* event, void* /*ctx*/) {
    if (event == nullptr) {
        return 0;
    }
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
        event->payload_size >= static_cast<int>(sizeof(esp_asp_music_info_t))) {
        esp_asp_music_info_t info = {};
        std::memcpy(&info, event->payload, sizeof(info));
        if (info.channels > 0) {
            s_pcm_channels.store(info.channels, std::memory_order_relaxed);
        }
        ESP_LOGI(TAG, "music info: rate=%d ch=%d bits=%d", info.sample_rate,
                 info.channels, info.bits);
    } else if (event->type == ESP_ASP_EVENT_TYPE_STATE &&
               event->payload_size == sizeof(esp_asp_state_t)) {
        esp_asp_state_t st = ESP_ASP_STATE_NONE;
        std::memcpy(&st, event->payload, event->payload_size);
        if (st == ESP_ASP_STATE_RUNNING &&
            s_want_play.load(std::memory_order_relaxed)) {
            SetStatus(StatusKind::Playing);
            SetPlayIcon(true);
        } else if (st == ESP_ASP_STATE_ERROR) {
            SetStatus(StatusKind::Failed);
            SetPlayIcon(false);
        }
    }
    return 0;
}

int HlsMediaTypeCallback(esp_hls_file_seg_info_t* info, void* ctx) {
    if (info == nullptr || ctx == nullptr ||
        s_hls_last_format == info->format) {
        return 0;
    }
    s_hls_last_format = info->format;

    auto player = static_cast<esp_asp_handle_t>(ctx);
    esp_gmf_pipeline_handle_t pipeline = nullptr;
    esp_gmf_element_handle_t dec_el = nullptr;
    if (esp_audio_simple_player_get_pipeline(player, &pipeline) !=
            ESP_GMF_ERR_OK ||
        pipeline == nullptr ||
        esp_gmf_pipeline_get_el_by_name(pipeline, "aud_dec", &dec_el) !=
            ESP_GMF_ERR_OK ||
        dec_el == nullptr) {
        return 0;
    }

    esp_gmf_info_sound_t music_info = {};
    music_info.format_id = info->format;
    music_info.sample_rates = 44100;
    music_info.channels = 2;
    music_info.bits = 16;
    esp_gmf_audio_dec_reconfig_by_sound_info(dec_el, &music_info);
    ApplyRateCvt16k(player);
    ESP_LOGI(TAG, "HLS format %s -> out %d Hz", ESP_FOURCC_TO_STR(info->format),
             kRadioSampleRate);
    return 0;
}

void DestroyPlayer() {
    if (s_player == nullptr) {
        return;
    }
    esp_asp_handle_t player = s_player;
    s_player = nullptr;
    esp_audio_simple_player_stop(player);
    esp_audio_simple_player_destroy(player);
}

bool CreatePlayerWithHls() {
    DestroyPlayer();
    if (s_codec == nullptr) {
        return false;
    }
    s_hls_last_format = 0;
    s_pcm_channels.store(2, std::memory_order_relaxed);

    esp_asp_cfg_t cfg = {
        .in = {},
        .out = {.cb = RadioOutCallback, .user_ctx = s_codec},
        .task_prio = 5,
        .task_stack = 8 * 1024,
        .prev = RadioPrevCallback,
        .prev_ctx = s_codec,
    };
    if (esp_audio_simple_player_new(&cfg, &s_player) != ESP_GMF_ERR_OK ||
        s_player == nullptr) {
        return false;
    }

    esp_gmf_pool_handle_t pool = nullptr;
    if (esp_audio_simple_player_get_pool(s_player, &pool) != ESP_GMF_ERR_OK ||
        pool == nullptr) {
        DestroyPlayer();
        return false;
    }
    RegisterHttpWithCertBundle(s_player);
    ForcePoolRateCvt16k(pool);

    esp_gmf_io_handle_t hls_io = nullptr;
    esp_hls_io_cfg_t hls_cfg = {};
    hls_cfg.name = "io_hls";
    hls_cfg.file_seg_cb = HlsMediaTypeCallback;
    hls_cfg.ctx = s_player;
    hls_cfg.pool = pool;
    hls_cfg.io_cfg = DEFAULT_HLS_IO_CFG();
    if (esp_gmf_io_hls_init(&hls_cfg, &hls_io) != ESP_GMF_ERR_OK ||
        hls_io == nullptr) {
        DestroyPlayer();
        return false;
    }
    if (esp_audio_simple_player_register_io(s_player, hls_io) !=
        ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(hls_io);
        DestroyPlayer();
        return false;
    }

    esp_audio_simple_player_set_event(s_player, RadioEventCallback, nullptr);
    s_codec->EnableOutput(true);
    return true;
}

void RadioStopWorker(void* /*arg*/) {
    esp_asp_handle_t player = s_player;
    if (player != nullptr) {
        esp_audio_simple_player_stop(player);
    }
    s_stop_pending.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void RequestPlayerStop() {
    bool expected = false;
    if (!s_stop_pending.compare_exchange_strong(expected, true)) {
        return;
    }
    if (xTaskCreate(RadioStopWorker, "radio_stop", 4096, nullptr, 6, nullptr) !=
        pdPASS) {
        s_stop_pending.store(false, std::memory_order_release);
        esp_asp_handle_t player = s_player;
        if (player != nullptr) {
            esp_audio_simple_player_stop(player);
        }
    }
}

void RadioPlayTask(void* arg) {
    const uint32_t gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    EnsureMediaLibAdapter();
    if (!CreatePlayerWithHls()) {
        SetStatus(StatusKind::Failed);
        if (s_play_task == self) {
            s_play_task = nullptr;
        }
        vTaskDelete(nullptr);
        return;
    }

    while (!s_shutdown.load(std::memory_order_relaxed) &&
           s_session_gen.load(std::memory_order_relaxed) == gen) {
        if (!s_want_play.load(std::memory_order_relaxed)) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        s_reported_playing.store(false, std::memory_order_relaxed);
        SetStatus(StatusKind::Connecting);
        ESP_LOGI(TAG, "start HLS gen=%u: %s", gen, kRadioStreamUrl);
        const esp_gmf_err_t err = esp_audio_simple_player_run_to_end(
            s_player, kRadioStreamUrl, nullptr);

        if (s_shutdown.load(std::memory_order_relaxed) ||
            s_session_gen.load(std::memory_order_relaxed) != gen) {
            break;
        }
        if (!s_want_play.load(std::memory_order_relaxed)) {
            continue;
        }
        if (err != ESP_GMF_ERR_OK) {
            ESP_LOGW(TAG, "HLS play ended/failed: 0x%x", err);
            SetStatus(StatusKind::Failed);
            SetPlayIcon(false);
            vTaskDelay(pdMS_TO_TICKS(1200));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // 仅清自己的句柄，避免把后建的 play task 指针抹掉
    if (s_play_task == self) {
        s_play_task = nullptr;
    }
    vTaskDelete(nullptr);
}

void KickSessionStopWorker() {
    bool expected = false;
    if (!s_stop_worker_busy.compare_exchange_strong(expected, true)) {
        return;
    }
    if (xTaskCreate(SessionStopWorker, "radio_off", 6144, nullptr, 6,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG, "session stop worker create failed, sync fallback");
        SessionStopWorker(nullptr);
    }
}

void SessionStopWorker(void* /*arg*/) {
    ESP_LOGI(TAG, "session stop begin");
    s_want_play.store(false, std::memory_order_relaxed);
    s_shutdown.store(true, std::memory_order_relaxed);
    s_session_gen.fetch_add(1, std::memory_order_acq_rel);

    StopSpectrumAnalyzer();

    esp_asp_handle_t player = s_player;
    if (player != nullptr) {
        esp_audio_simple_player_stop(player);
    }
    for (int i = 0; i < 50 && s_stop_pending.load(std::memory_order_relaxed);
         ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_stop_pending.store(false, std::memory_order_release);

    WaitTaskGone(&s_play_task, 4000);
    if (s_play_task != nullptr) {
        ESP_LOGW(TAG, "play task stuck after stop; clearing handle");
        s_play_task = nullptr;
    }

    DestroyPlayer();
    s_pcm_buf.clear();
    s_pcm_buf.shrink_to_fit();
    s_pcm_w.store(0, std::memory_order_relaxed);
    s_pcm_r.store(0, std::memory_order_relaxed);
    s_codec = nullptr;

    Application::GetInstance().RestoreSystemAudioAfterStressTest();

    EnsureLifeMutex();
    bool restart = false;
    if (s_life_mu != nullptr &&
        xSemaphoreTake(s_life_mu, pdMS_TO_TICKS(2000)) == pdTRUE) {
        restart = s_restart_pending.exchange(false, std::memory_order_acq_rel);
        if (restart) {
            s_shutdown.store(false, std::memory_order_relaxed);
            s_want_play.store(true, std::memory_order_relaxed);
            s_life.store(LifeState::Starting, std::memory_order_release);
        } else {
            s_life.store(LifeState::Idle, std::memory_order_release);
            s_shutdown.store(false, std::memory_order_relaxed);
        }
        xSemaphoreGive(s_life_mu);
    } else {
        restart = s_restart_pending.exchange(false, std::memory_order_acq_rel);
        s_shutdown.store(false, std::memory_order_relaxed);
        if (restart) {
            s_want_play.store(true, std::memory_order_relaxed);
            s_life.store(LifeState::Starting, std::memory_order_release);
        } else {
            s_life.store(LifeState::Idle, std::memory_order_release);
        }
    }

    s_stop_worker_busy.store(false, std::memory_order_release);

    ESP_LOGI(TAG, "session stop done restart=%d", restart ? 1 : 0);
    if (restart) {
        if (xTaskCreate(SessionStartWorker, "radio_on", 6144, nullptr, 5,
                        nullptr) != pdPASS) {
            s_life.store(LifeState::Idle, std::memory_order_release);
            ESP_LOGE(TAG, "restart start worker failed");
        }
    }
    vTaskDelete(nullptr);
}

void SessionStartWorker(void* /*arg*/) {
    ESP_LOGI(TAG, "session start begin");

    // 已被 Stop 抢占：直接退出，由 StopWorker 负责收尾
    if (s_life.load(std::memory_order_relaxed) == LifeState::Stopping ||
        (s_shutdown.load(std::memory_order_relaxed) &&
         s_life.load(std::memory_order_relaxed) != LifeState::Starting)) {
        ESP_LOGW(TAG, "session start aborted (stopping)");
        vTaskDelete(nullptr);
        return;
    }

    s_codec = Board::GetInstance().GetAudioCodec();
    if (s_codec == nullptr) {
        SetStatus(StatusKind::Failed);
        EnsureLifeMutex();
        if (s_life_mu != nullptr &&
            xSemaphoreTake(s_life_mu, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_life.load(std::memory_order_relaxed) == LifeState::Starting) {
                s_life.store(LifeState::Idle, std::memory_order_release);
            }
            xSemaphoreGive(s_life_mu);
        } else {
            s_life.store(LifeState::Idle, std::memory_order_release);
        }
        vTaskDelete(nullptr);
        return;
    }

    Application::GetInstance().StopSystemAudioForStressTest();

    if (s_shutdown.load(std::memory_order_relaxed) ||
        s_life.load(std::memory_order_relaxed) == LifeState::Stopping) {
        Application::GetInstance().RestoreSystemAudioAfterStressTest();
        ESP_LOGW(TAG, "session start canceled after audio grab");
        // StopWorker 若已在跑则由它置 Idle；否则我们置 Idle
        EnsureLifeMutex();
        if (s_life_mu != nullptr &&
            xSemaphoreTake(s_life_mu, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_life.load(std::memory_order_relaxed) == LifeState::Starting) {
                s_life.store(LifeState::Idle, std::memory_order_release);
            }
            xSemaphoreGive(s_life_mu);
        }
        vTaskDelete(nullptr);
        return;
    }

    const uint32_t gen =
        s_session_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    s_shutdown.store(false, std::memory_order_relaxed);
    s_reported_playing.store(false, std::memory_order_relaxed);
    s_want_play.store(true, std::memory_order_relaxed);
    s_stop_pending.store(false, std::memory_order_relaxed);

    SetStatus(StatusKind::Connecting);
    SetPlayIcon(true);
    if (s_screen_active.load(std::memory_order_relaxed)) {
        lv_async_call(
            [](void*) {
                if (s_screen_active.load(std::memory_order_relaxed)) {
                    UpdateVolumeLabel();
                }
            },
            nullptr);
    }

    StartSpectrumAnalyzer();

    // 旧 play 可能因 gen 变更正在退出：必须等掉再创建，否则会误跳过创建
    if (s_play_task != nullptr) {
        if (s_player != nullptr) {
            esp_audio_simple_player_stop(s_player);
        }
        WaitTaskGone(&s_play_task, 3000);
        if (s_play_task != nullptr) {
            ESP_LOGW(TAG, "old play task still alive, abandoning handle");
            s_play_task = nullptr;
        }
    }

    if (xTaskCreate(RadioPlayTask, "radio_hls", 8192,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(gen)), 5,
                    &s_play_task) != pdPASS) {
        s_play_task = nullptr;
        s_want_play.store(false, std::memory_order_relaxed);
        StopSpectrumAnalyzer();
        Application::GetInstance().RestoreSystemAudioAfterStressTest();
        SetStatus(StatusKind::Failed);
        SetPlayIcon(false);
        EnsureLifeMutex();
        if (s_life_mu != nullptr &&
            xSemaphoreTake(s_life_mu, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_life.load(std::memory_order_relaxed) == LifeState::Starting) {
                s_life.store(LifeState::Idle, std::memory_order_release);
            }
            xSemaphoreGive(s_life_mu);
        } else {
            s_life.store(LifeState::Idle, std::memory_order_release);
        }
        vTaskDelete(nullptr);
        return;
    }

    EnsureLifeMutex();
    if (s_life_mu != nullptr &&
        xSemaphoreTake(s_life_mu, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (s_shutdown.load(std::memory_order_relaxed) ||
            s_life.load(std::memory_order_relaxed) == LifeState::Stopping) {
            // Stop 已在进行或已请求：不要标 Running
            if (s_life.load(std::memory_order_relaxed) == LifeState::Starting) {
                s_life.store(LifeState::Stopping, std::memory_order_release);
                xSemaphoreGive(s_life_mu);
                KickSessionStopWorker();
                vTaskDelete(nullptr);
                return;
            }
            xSemaphoreGive(s_life_mu);
            vTaskDelete(nullptr);
            return;
        }
        s_life.store(LifeState::Running, std::memory_order_release);
        xSemaphoreGive(s_life_mu);
    } else if (!s_shutdown.load(std::memory_order_relaxed)) {
        s_life.store(LifeState::Running, std::memory_order_release);
    }

    ESP_LOGI(TAG, "session start done gen=%u", gen);
    vTaskDelete(nullptr);
}

void RequestSessionStart() {
    EnsureLifeMutex();
    if (s_life_mu == nullptr) {
        return;
    }
    if (xSemaphoreTake(s_life_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "start: life mutex timeout");
        return;
    }

    const LifeState st = s_life.load(std::memory_order_relaxed);
    if (st == LifeState::Running || st == LifeState::Starting) {
        // 僵尸 Running：状态还在但任务/播放器已没 → 强制重建
        if (st == LifeState::Running &&
            (s_play_task == nullptr || s_player == nullptr)) {
            ESP_LOGW(TAG, "start: zombie Running, force restart");
            s_restart_pending.store(true, std::memory_order_release);
            s_want_play.store(true, std::memory_order_relaxed);
            s_shutdown.store(true, std::memory_order_relaxed);
            s_life.store(LifeState::Stopping, std::memory_order_release);
            xSemaphoreGive(s_life_mu);
            KickSessionStopWorker();
            return;
        }
        s_want_play.store(true, std::memory_order_relaxed);
        s_shutdown.store(false, std::memory_order_relaxed);
        xSemaphoreGive(s_life_mu);
        // UI 可能错过 RUNNING 事件，把已缓存状态刷回去
        if (s_screen_active.load(std::memory_order_relaxed)) {
            lv_async_call(
                [](void*) {
                    SyncStatusToUi();
                    if (s_want_play.load(std::memory_order_relaxed) &&
                        s_reported_playing.load(std::memory_order_relaxed)) {
                        SetStatusDirect(StatusKind::Playing);
                        SetPlayIconDirect(true);
                    }
                },
                nullptr);
        }
        return;
    }
    if (st == LifeState::Stopping) {
        // 停播尚未完成：标记停完后立刻再开，禁止 Start 抢清 flags
        s_restart_pending.store(true, std::memory_order_release);
        xSemaphoreGive(s_life_mu);
        ESP_LOGI(TAG, "start deferred until stop finishes");
        return;
    }

    s_restart_pending.store(false, std::memory_order_relaxed);
    s_life.store(LifeState::Starting, std::memory_order_release);
    xSemaphoreGive(s_life_mu);

    if (xTaskCreate(SessionStartWorker, "radio_on", 6144, nullptr, 5,
                    nullptr) != pdPASS) {
        s_life.store(LifeState::Idle, std::memory_order_release);
        SetStatusDirect(StatusKind::Failed);
        ESP_LOGE(TAG, "session start worker create failed");
    }
}

void RequestSessionStop(bool allow_restart) {
    EnsureLifeMutex();
    if (s_life_mu == nullptr) {
        return;
    }
    if (xSemaphoreTake(s_life_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "stop: life mutex timeout");
        return;
    }

    if (!allow_restart) {
        s_restart_pending.store(false, std::memory_order_relaxed);
    }

    const LifeState st = s_life.load(std::memory_order_relaxed);
    if (st == LifeState::Idle) {
        xSemaphoreGive(s_life_mu);
        return;
    }
    if (st == LifeState::Stopping) {
        xSemaphoreGive(s_life_mu);
        return;
    }

    s_want_play.store(false, std::memory_order_relaxed);
    s_shutdown.store(true, std::memory_order_relaxed);
    s_life.store(LifeState::Stopping, std::memory_order_release);
    xSemaphoreGive(s_life_mu);

    KickSessionStopWorker();
}

void UpdateVolumeLabel() {
    if (s_ui.lbl_volume == nullptr || s_codec == nullptr) {
        return;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%s %d%%", I18n::T("音量"),
                  s_codec->output_volume());
    lv_label_set_text(s_ui.lbl_volume, buf);
}

void OnSwipeBack() {
    RequestSessionStop(false);
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnScreenUnloaded(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    // 快速进出时旧页 UNLOAD 晚到，禁止清掉新页的 UI / timer
    if (target == nullptr || target != s_bound_scr) {
        return;
    }
    s_bound_scr = nullptr;
    s_screen_active.store(false, std::memory_order_relaxed);
    if (s_ui.viz_timer != nullptr) {
        lv_timer_delete(s_ui.viz_timer);
        s_ui.viz_timer = nullptr;
    }
    s_ui = RadioUi{};
}

void OnPlayClicked(lv_event_t* /*e*/) {
    if (s_life.load(std::memory_order_relaxed) != LifeState::Running) {
        return;
    }
    if (s_want_play.load(std::memory_order_relaxed)) {
        s_want_play.store(false, std::memory_order_relaxed);
        s_reported_playing.store(false, std::memory_order_relaxed);
        SetStatusDirect(StatusKind::Paused);
        SetPlayIconDirect(false);
        RequestPlayerStop();
    } else if (!s_shutdown.load(std::memory_order_relaxed)) {
        s_want_play.store(true, std::memory_order_relaxed);
        s_reported_playing.store(false, std::memory_order_relaxed);
        SetStatusDirect(StatusKind::Connecting);
        SetPlayIconDirect(true);
    }
}

void OnVolDownClicked(lv_event_t* /*e*/) {
    if (s_codec == nullptr) {
        return;
    }
    int vol = s_codec->output_volume() - kVolStep;
    s_codec->SetOutputVolume(vol < 0 ? 0 : vol);
    UpdateVolumeLabel();
}

void OnVolUpClicked(lv_event_t* /*e*/) {
    if (s_codec == nullptr) {
        return;
    }
    int vol = s_codec->output_volume() + kVolStep;
    s_codec->SetOutputVolume(vol > 100 ? 100 : vol);
    UpdateVolumeLabel();
}

lv_obj_t* CreateRoundButton(lv_obj_t* parent, int32_t size, uint32_t bg_color,
                            uint32_t bg_pressed, const char* icon_path,
                            lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_pressed),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(btn, 12);
    screen_swipe_back_ignore(btn, true);

    lv_obj_t* img = lv_image_create(btn);
    lv_image_set_src(img, icon_path);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return img;
}

void BuildBackButton(lv_obj_t* scr) {
    lv_obj_t* back_btn = lv_button_create(scr);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 72, 72);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 32, 36);
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t* back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);
    lv_obj_add_event_cb(
        back_btn, [](lv_event_t* /*e*/) { OnSwipeBack(); }, LV_EVENT_CLICKED,
        nullptr);
}

void BuildTitle(lv_obj_t* scr) {
    s_ui.lbl_title = lv_label_create(scr);
    lv_label_set_text(s_ui.lbl_title, I18n::T(kStationNameMsgId));
    lv_obj_set_style_text_font(s_ui.lbl_title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_title, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_title, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.lbl_title, kPanelSize - 80);
    lv_obj_align(s_ui.lbl_title, LV_ALIGN_TOP_MID, 0, kTitleY);
    screen_make_input_passive(s_ui.lbl_title);

    s_ui.lbl_status = lv_label_create(scr);
    lv_label_set_text(s_ui.lbl_status, I18n::T("连接中"));
    lv_obj_set_style_text_font(s_ui.lbl_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_status, lv_color_hex(kColorAccent),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_status, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_width(s_ui.lbl_status, kPanelSize - 80);
    lv_obj_align(s_ui.lbl_status, LV_ALIGN_TOP_MID, 0, kStatusY);
    screen_make_input_passive(s_ui.lbl_status);
}

void BuildVisualizer(lv_obj_t* scr) {
    s_ui.viz_host = lv_obj_create(scr);
    lv_obj_set_size(s_ui.viz_host, kVizW, kVizH);
    lv_obj_align(s_ui.viz_host, LV_ALIGN_TOP_MID, 0, kVizY);
    screen_strip_obj_chrome(s_ui.viz_host);
    lv_obj_remove_flag(s_ui.viz_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.viz_host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_ui.viz_host, lv_color_hex(kColorVizFloor),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.viz_host, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.viz_host, 28, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.viz_host, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.viz_host, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_ui.viz_host, true, LV_PART_MAIN);
    screen_make_input_passive(s_ui.viz_host);

    const int32_t bar_w = (kVizW - 48 - (kBarCount - 1) * kBarGap) / kBarCount;
    const int32_t usable_w = kBarCount * bar_w + (kBarCount - 1) * kBarGap;
    const int32_t x0 = (kVizW - usable_w) / 2;

    for (int i = 0; i < kBarCount; ++i) {
        lv_obj_t* bar = lv_obj_create(s_ui.viz_host);
        lv_obj_set_size(bar, bar_w, kBarMinH);
        screen_strip_obj_chrome(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(bar, bar_w / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(kRainbowBase[i]),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, x0 + i * (bar_w + kBarGap), -10);
        s_ui.bars[i] = bar;
    }

    s_ui.viz_timer = lv_timer_create(VizTimerCb, 33, nullptr);
}

void BuildVolumeLabel(lv_obj_t* scr) {
    s_ui.lbl_volume = lv_label_create(scr);
    lv_label_set_text(s_ui.lbl_volume, I18n::T("音量"));
    lv_obj_set_style_text_font(s_ui.lbl_volume, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_volume, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_volume, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_width(s_ui.lbl_volume, kPanelSize - 80);
    lv_obj_align(s_ui.lbl_volume, LV_ALIGN_TOP_MID, 0, kVizY + kVizH + 24);
    screen_make_input_passive(s_ui.lbl_volume);
}

void BuildControls(lv_obj_t* scr) {
    lv_obj_t* row = lv_obj_create(scr);
    lv_obj_set_size(row, kCtrlRowWidth, kCtrlRowHeight);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, kCtrlRowY);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed, "A:ic_s_music_volume_down.spng",
                      OnVolDownClicked);
    s_ui.img_play_icon =
        CreateRoundButton(row, kCtrlPlayBtnSize, kColorPlayBtnBg,
                          kColorPlayBtnBgPressed, "A:ic_s_player_play.spng",
                          OnPlayClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed, "A:ic_s_music_volume_up.spng",
                      OnVolUpClicked);
}

}  // namespace

lv_obj_t* RadioScreen::Create() {
    // 旧页可能尚未 UNLOAD：先删 timer，避免野指针回调 + 句柄泄漏
    if (s_ui.viz_timer != nullptr) {
        lv_timer_delete(s_ui.viz_timer);
        s_ui.viz_timer = nullptr;
    }
    s_ui = RadioUi{};

    lv_obj_t* scr = lv_obj_create(nullptr);
    screen_strip_obj_chrome(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    BuildTitle(scr);
    BuildVisualizer(scr);
    BuildVolumeLabel(scr);
    BuildControls(scr);
    BuildBackButton(scr);

    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);

    s_bound_scr = scr;
    s_screen_active.store(true, std::memory_order_relaxed);
    SyncStatusToUi();
    return scr;
}

void RadioScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: radio_screen");
        SetStatusDirect(StatusKind::Connecting);
        SetPlayIconDirect(true);
        RequestSessionStart();
        // 若会话其实已在播，把缓存状态刷到 UI（避免一直「连接中」）
        lv_async_call(
            [](void*) {
                if (!s_screen_active.load(std::memory_order_relaxed)) {
                    return;
                }
                if (s_reported_playing.load(std::memory_order_relaxed) &&
                    s_want_play.load(std::memory_order_relaxed)) {
                    SetStatusDirect(StatusKind::Playing);
                    SetPlayIconDirect(true);
                } else {
                    SyncStatusToUi();
                }
            },
            nullptr);
    } else {
        ESP_LOGI(TAG, "unload: radio_screen");
        RequestSessionStop(false);
    }
}
