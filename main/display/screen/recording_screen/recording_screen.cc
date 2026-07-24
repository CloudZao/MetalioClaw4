#include "recording_screen.h"
#include "i18n.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "application.h"
#include "api_endpoints.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "protocol.h"
#include "SdCardManager.hpp"
#include "screen_util.h"
#include "system_info.h"

#include <cJSON.h>
#include <opus_encoder.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
// 注意：font_puhui_number_50_4 只有 % 和 0-9，没有冒号，计时不能用它。

namespace {

constexpr const char* TAG = "RecordingScreen";

constexpr int32_t kPanelSize = 720;
constexpr int32_t kHeaderH = 90;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kTabBarH = 64;
constexpr int32_t kBodyH = kPanelSize - kHeaderH;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorTabBar = 0x12151C;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorSubtle = 0x9AA3B2;
constexpr uint32_t kColorAccent = 0x3B82F6;
constexpr uint32_t kColorRecord = 0xDC2626;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorDanger = 0xEF4444;

constexpr const char* kPosixDir = "/sdcard/recordings";
constexpr int kSampleRate = 16000;
constexpr int kFrameDurationMs = OPUS_FRAME_DURATION_MS;  // 60
constexpr int kSamplesPerFrame = kSampleRate * kFrameDurationMs / 1000;  // 960
// Opus granule 固定按 48 kHz 计：60ms → 2880
constexpr int64_t kGranulePerFrame = 48000 * kFrameDurationMs / 1000;
constexpr int kMaxRecordSeconds = 30 * 60;
constexpr int kMinRecordMs = 300;
constexpr int kPlayChunkSamples = kSamplesPerFrame;
constexpr int kMaxListItems = 80;
constexpr uint32_t kOggSerial = 0x4F505553;  // 'OPUS'

enum class RecFormat : uint8_t {
    Opus = 0,
    Wav,
};

enum class RecState : uint8_t {
    Idle = 0,
    Recording,
    Saving,
    Playing,
};

struct UiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* tabview = nullptr;
    lv_obj_t* timer_lbl = nullptr;
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* record_btn = nullptr;
    lv_obj_t* record_btn_lbl = nullptr;
    lv_obj_t* list_scroll = nullptr;
    lv_obj_t* list_empty = nullptr;
    lv_obj_t* list_status = nullptr;

    // 录音详情覆盖层
    lv_obj_t* detail_panel = nullptr;
    lv_obj_t* detail_title = nullptr;
    lv_obj_t* detail_meta = nullptr;
    lv_obj_t* detail_play_btn = nullptr;
    lv_obj_t* detail_play_lbl = nullptr;
    lv_obj_t* detail_asr_btn = nullptr;
    lv_obj_t* detail_asr_lbl = nullptr;
    lv_obj_t* detail_status = nullptr;
    lv_obj_t* detail_result = nullptr;
};

UiState s_ui;
bool s_screen_alive = false;
bool s_sd_ready = false;
bool s_detail_open = false;
int s_detail_idx = -1;

std::atomic<RecState> s_state{RecState::Idle};
std::atomic<bool> s_stop_record{false};
std::atomic<bool> s_stop_play{false};
std::atomic<bool> s_stop_asr{false};
std::atomic<int64_t> s_record_start_us{0};
std::atomic<uint32_t> s_recorded_frames{0};

TaskHandle_t s_record_task = nullptr;
TaskHandle_t s_play_task = nullptr;
TaskHandle_t s_asr_task = nullptr;        // 上传转写
TaskHandle_t s_asr_query_task = nullptr;  // 查询转写结果
TaskHandle_t s_meta_task = nullptr;
lv_timer_t* s_tick_timer = nullptr;
bool s_wake_disabled_by_us = false;
std::atomic<bool> s_stop_meta{false};

char s_playing_path[192] = {};
char s_detail_path[192] = {};
char s_detail_name[96] = {};

struct FileEntry {
    char name[96];
    char path[160];
    int duration_sec = 0;
    size_t size_bytes = 0;
    RecFormat format = RecFormat::Opus;
    bool duration_ready = false;  // sidecar / WAV 尺寸已就绪；否则后台补算
};

std::vector<FileEntry> s_files;

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

void OnSwipeBack();
void OnBackClicked(lv_event_t* e);
void UpdateRecordButtonUi();
void UpdateTimerLabel();
void RebuildFileList();
void RebuildFileList(bool schedule_fill);
void StopPlayback();
void ForceStopAll();
void ShowDetail(int idx);
void HideDetail();
void UpdateDetailPlayButton();
void SetDetailStatusText(const char* text);
void SetDetailResultText(const char* text);
void BuildDetailPanel(lv_obj_t* parent);
void ScheduleDurationFill();
void ScheduleAsrQuery();
bool WriteDurationSidecar(const char* opus_path, int duration_sec);
int ReadDurationSidecar(const char* opus_path);
void UnlinkDurationSidecar(const char* opus_path);
int DurationSecFromOpusFileTail(const char* path);
int EnsureOpusDuration(FileEntry& entry);

bool EnsureRecordingsDir() {
    struct stat st;
    if (stat(kPosixDir, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(kPosixDir, 0755) == 0) {
        return true;
    }
    ESP_LOGE(TAG, "mkdir(%s) failed", kPosixDir);
    return false;
}

void PutLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void PutLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void PutLe64(uint8_t* p, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
    }
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int64_t ReadLe64(const uint8_t* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) {
        u |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return static_cast<int64_t>(u);
}

// Ogg CRC（多项式 0x04C11DB7，与 RFC 3533 一致）
uint32_t* OggCrcTable() {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int j = 0; j < 8; ++j) {
                r = (r & 0x80000000u) ? ((r << 1) ^ 0x04c11db7u) : (r << 1);
            }
            table[i] = r;
        }
        ready = true;
    }
    return table;
}

uint32_t OggCrc(const uint8_t* data, size_t len) {
    const uint32_t* table = OggCrcTable();
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc << 8) ^ table[((crc >> 24) & 0xFFu) ^ data[i]];
    }
    return crc;
}

bool WriteOggPage(FILE* f, uint8_t header_type, int64_t granule, uint32_t serial,
                  uint32_t page_seq, const uint8_t* body, size_t body_len) {
    if (f == nullptr) {
        return false;
    }

    std::vector<uint8_t> segs;
    if (body_len == 0) {
        segs.push_back(0);
    } else {
        size_t rem = body_len;
        while (rem >= 255) {
            segs.push_back(255);
            rem -= 255;
        }
        segs.push_back(static_cast<uint8_t>(rem));
    }
    if (segs.size() > 255) {
        ESP_LOGE(TAG, "ogg packet too large: %u", static_cast<unsigned>(body_len));
        return false;
    }

    const size_t header_len = 27 + segs.size();
    std::vector<uint8_t> page(header_len + body_len);
    std::memcpy(page.data(), "OggS", 4);
    page[4] = 0;
    page[5] = header_type;
    PutLe64(page.data() + 6, granule);
    PutLe32(page.data() + 14, serial);
    PutLe32(page.data() + 18, page_seq);
    PutLe32(page.data() + 22, 0);  // checksum placeholder
    page[26] = static_cast<uint8_t>(segs.size());
    std::memcpy(page.data() + 27, segs.data(), segs.size());
    if (body_len > 0) {
        std::memcpy(page.data() + header_len, body, body_len);
    }

    const uint32_t crc = OggCrc(page.data(), page.size());
    PutLe32(page.data() + 22, crc);

    return fwrite(page.data(), 1, page.size(), f) == page.size();
}

bool WriteOpusHeaders(FILE* f, uint32_t serial, uint32_t& page_seq) {
    uint8_t head[19] = {};
    std::memcpy(head, "OpusHead", 8);
    head[8] = 1;   // version
    head[9] = 1;   // mono
    PutLe16(head + 10, 0);  // pre_skip
    PutLe32(head + 12, static_cast<uint32_t>(kSampleRate));
    PutLe16(head + 16, 0);  // output gain
    head[18] = 0;  // mapping family
    if (!WriteOggPage(f, 0x02 /* BOS */, 0, serial, page_seq++, head, sizeof(head))) {
        return false;
    }

    const char* vendor = "xingzhi-ai";
    const uint32_t vendor_len = static_cast<uint32_t>(std::strlen(vendor));
    std::vector<uint8_t> tags(8 + 4 + vendor_len + 4);
    std::memcpy(tags.data(), "OpusTags", 8);
    PutLe32(tags.data() + 8, vendor_len);
    std::memcpy(tags.data() + 12, vendor, vendor_len);
    PutLe32(tags.data() + 12 + vendor_len, 0);  // user comment list count
    return WriteOggPage(f, 0x00, 0, serial, page_seq++, tags.data(), tags.size());
}

void KeepLeftChannelOnly(std::vector<int16_t>& samples) {
    if (samples.size() < 2) {
        return;
    }
    const size_t mono_count = samples.size() / 2;
    for (size_t i = 0, j = 0; i < mono_count; ++i, j += 2) {
        samples[i] = samples[j];
    }
    samples.resize(mono_count);
}

void DisableWakeWordIfNeeded() {
    auto& as = Application::GetInstance().GetAudioService();
    if (!as.IsWakeWordRunning()) {
        return;
    }
    as.EnableWakeWordDetection(false);
    s_wake_disabled_by_us = true;
    vTaskDelay(pdMS_TO_TICKS(100));
}

void RestoreWakeWordIfNeeded() {
    if (!s_wake_disabled_by_us) {
        return;
    }
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
    s_wake_disabled_by_us = false;
}

void FormatTimer(char* buf, size_t buf_size, int total_sec) {
    if (total_sec < 0) {
        total_sec = 0;
    }
    const int mm = total_sec / 60;
    const int ss = total_sec % 60;
    std::snprintf(buf, buf_size, "%02d:%02d", mm, ss);
}

void FormatFileSize(char* buf, size_t buf_size, size_t bytes) {
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, buf_size, I18n::T("%.2f MB"), bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(buf, buf_size, I18n::T("%.1f KB"), bytes / 1024.0);
    } else {
        std::snprintf(buf, buf_size, I18n::T("%u B"), static_cast<unsigned>(bytes));
    }
}

void SetRecordStatusText(const char* text) {
    if (!s_screen_alive || text == nullptr || s_ui.status_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.status_lbl, text);
}

void SetIdleRecordHint() {
    char buf[96];
    std::snprintf(buf, sizeof(buf), I18n::T("点击下方按钮开始录音（最长 %d 分钟）"),
                  kMaxRecordSeconds / 60);
    SetRecordStatusText(buf);
}

void SetRecordingStatusHint() {
    char buf[96];
    std::snprintf(buf, sizeof(buf), I18n::T("正在录音…（最长 %d 分钟）"),
                  kMaxRecordSeconds / 60);
    SetRecordStatusText(buf);
}

void SetListStatusText(const char* text) {
    if (!s_screen_alive || text == nullptr || s_ui.list_status == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.list_status, text);
}

void SetDetailStatusText(const char* text) {
    if (!s_screen_alive || text == nullptr || s_ui.detail_status == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.detail_status, text);
}

void SetDetailResultText(const char* text) {
    if (!s_screen_alive || text == nullptr || s_ui.detail_result == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.detail_result, text);
}

void PostPlayStatusFromWorker(const char* text) {
    if (text == nullptr) {
        return;
    }
    auto* copy = static_cast<char*>(heap_caps_malloc(std::strlen(text) + 1, MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        return;
    }
    std::strcpy(copy, text);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(copy);
        return;
    }
    if (s_screen_alive) {
        if (s_detail_open) {
            SetDetailStatusText(copy);
            UpdateDetailPlayButton();
        } else {
            SetListStatusText(copy);
        }
        UpdateRecordButtonUi();
    }
    esp_lv_adapter_unlock();
    heap_caps_free(copy);
}

void PostDetailStatusFromWorker(const char* text) {
    if (text == nullptr) {
        return;
    }
    auto* copy = static_cast<char*>(heap_caps_malloc(std::strlen(text) + 1, MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        return;
    }
    std::strcpy(copy, text);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(copy);
        return;
    }
    if (s_screen_alive && s_detail_open) {
        SetDetailStatusText(copy);
    }
    esp_lv_adapter_unlock();
    heap_caps_free(copy);
}

void PostDetailResultFromWorker(const char* text) {
    if (text == nullptr) {
        return;
    }
    auto* copy = static_cast<char*>(heap_caps_malloc(std::strlen(text) + 1, MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        return;
    }
    std::strcpy(copy, text);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(copy);
        return;
    }
    if (s_screen_alive && s_detail_open) {
        SetDetailResultText(copy);
    }
    esp_lv_adapter_unlock();
    heap_caps_free(copy);
}

void PostStatusFromWorker(bool to_list_tab, const char* text) {
    if (text == nullptr) {
        return;
    }
    auto* copy = static_cast<char*>(heap_caps_malloc(std::strlen(text) + 1, MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        return;
    }
    std::strcpy(copy, text);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(copy);
        return;
    }
    if (s_screen_alive) {
        if (to_list_tab) {
            SetListStatusText(copy);
        } else {
            SetRecordStatusText(copy);
        }
    }
    esp_lv_adapter_unlock();
    heap_caps_free(copy);
}

void UpdateTimerLabel() {
    if (!s_screen_alive || s_ui.timer_lbl == nullptr) {
        return;
    }
    int sec = 0;
    if (s_state.load() == RecState::Recording) {
        const int64_t start = s_record_start_us.load();
        if (start > 0) {
            sec = static_cast<int>((esp_timer_get_time() - start) / 1000000LL);
        }
    } else if (s_recorded_frames.load() > 0 && s_state.load() == RecState::Idle) {
        // 保留结束时写好的最终时长
        return;
    }
    char buf[16];
    FormatTimer(buf, sizeof(buf), sec);
    lv_label_set_text(s_ui.timer_lbl, buf);
}

void OnTickTimer(lv_timer_t* /*t*/) {
    if (s_state.load() == RecState::Recording) {
        UpdateTimerLabel();
    }
}

void StartTickTimer() {
    if (s_tick_timer != nullptr) {
        return;
    }
    s_tick_timer = lv_timer_create(OnTickTimer, 200, nullptr);
}

void StopTickTimer() {
    if (s_tick_timer != nullptr) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = nullptr;
    }
}

void UpdateRecordButtonUi() {
    if (!s_screen_alive || s_ui.record_btn == nullptr || s_ui.record_btn_lbl == nullptr) {
        return;
    }
    const RecState st = s_state.load();
    switch (st) {
    case RecState::Idle:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(kColorRecord), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("开始录音"));
        lv_obj_add_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case RecState::Recording:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(0x4B5563), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("结束录音"));
        lv_obj_add_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case RecState::Saving:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(0x4B5563), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("保存中…"));
        lv_obj_remove_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case RecState::Playing:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(kColorRecord), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("开始录音"));
        lv_obj_remove_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    }
}

void MakeNextRecordPath(char* path, size_t path_size) {
    // 设备序列：与配网 AP 相同取 MAC 末两字节 %02X%02X；MAC 来源统一 SystemInfo::GetMacAddress()
    char serial[8] = "0000";
    {
        const std::string mac = SystemInfo::GetMacAddress();
        unsigned b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
        if (std::sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b0, &b1, &b2, &b3, &b4,
                        &b5) == 6) {
            std::snprintf(serial, sizeof(serial), "%02X%02X", b4 & 0xff, b5 & 0xff);
        }
    }

    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year > 70) {
        std::snprintf(path, path_size, "%s/REC_%s_%04d%02d%02d_%02d%02d%02d.opus",
                      kPosixDir, serial, tm_info.tm_year + 1900, tm_info.tm_mon + 1,
                      tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        return;
    }
    const unsigned long ts_ms =
        static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
    std::snprintf(path, path_size, "%s/REC_%s_%lu.opus", kPosixDir, serial, ts_ms);
}

void RecordTask(void* /*arg*/) {
    auto& as = Application::GetInstance().GetAudioService();
    char path[192] = {};
    FILE* file = nullptr;
    uint32_t frames = 0;
    uint32_t page_seq = 0;
    bool ok = false;
    bool hit_max = false;
    std::unique_ptr<OpusEncoderWrapper> encoder;

    DisableWakeWordIfNeeded();
    as.ResetDecoder();

    if (!EnsureRecordingsDir()) {
        PostStatusFromWorker(false, I18n::T("无法创建录音目录"));
        goto done;
    }

    MakeNextRecordPath(path, sizeof(path));
    file = fopen(path, "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "fopen failed: %s", path);
        PostStatusFromWorker(false, I18n::T("写入失败，未保存"));
        goto done;
    }

    encoder = std::make_unique<OpusEncoderWrapper>(kSampleRate, 1, kFrameDurationMs);
    encoder->SetComplexity(0);

    if (!WriteOpusHeaders(file, kOggSerial, page_seq)) {
        PostStatusFromWorker(false, I18n::T("写入失败，未保存"));
        goto done;
    }

    s_record_start_us.store(esp_timer_get_time());
    s_recorded_frames.store(0);

    {
        std::vector<int16_t> frame;
        frame.reserve(static_cast<size_t>(kSamplesPerFrame) * 2);
        std::vector<uint8_t> opus_pkt;
        opus_pkt.reserve(256);

        while (!s_stop_record.load()) {
            if (!as.ReadAudioData(frame, kSampleRate, kSamplesPerFrame)) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            AudioCodec* codec = Board::GetInstance().GetAudioCodec();
            if (codec != nullptr && codec->input_channels() == 2) {
                KeepLeftChannelOnly(frame);
            }
            if (static_cast<int>(frame.size()) != kSamplesPerFrame) {
                continue;
            }
            if (!encoder->Encode(std::move(frame), opus_pkt)) {
                ESP_LOGW(TAG, "opus encode failed");
                continue;
            }

            ++frames;
            const int64_t granule = static_cast<int64_t>(frames) * kGranulePerFrame;
            const bool force_eos =
                ((esp_timer_get_time() - s_record_start_us.load()) / 1000) >=
                kMaxRecordSeconds * 1000;
            if (!WriteOggPage(file, 0x00, granule, kOggSerial, page_seq++,
                              opus_pkt.data(), opus_pkt.size())) {
                PostStatusFromWorker(false, I18n::T("写入失败，未保存"));
                goto done;
            }
            s_recorded_frames.store(frames);

            if (force_eos) {
                ESP_LOGI(TAG, "max record duration reached");
                hit_max = true;
                break;
            }
        }

        // 若最后一页未标 EOS，补一帧空 EOS 页（或把最后一页重写太麻烦，写空 EOS）
        if (frames > 0) {
            const int64_t granule = static_cast<int64_t>(frames) * kGranulePerFrame;
            if (!WriteOggPage(file, 0x04 /* EOS */, granule, kOggSerial, page_seq++,
                              nullptr, 0)) {
                PostStatusFromWorker(false, I18n::T("写入失败，未保存"));
                goto done;
            }
        }
    }

    {
        const int duration_ms = static_cast<int>(
            (esp_timer_get_time() - s_record_start_us.load()) / 1000);
        if (duration_ms < kMinRecordMs || frames < 5) {
            ESP_LOGI(TAG, "discard short recording: %d ms / %u frames", duration_ms,
                     static_cast<unsigned>(frames));
            fclose(file);
            file = nullptr;
            unlink(path);
            UnlinkDurationSidecar(path);
            PostStatusFromWorker(false, I18n::T("录音太短，再试一次"));
            goto done;
        }

        fflush(file);
        fclose(file);
        file = nullptr;
        ok = true;
        {
            const int duration_sec =
                static_cast<int>(frames * kFrameDurationMs / 1000);
            WriteDurationSidecar(path, duration_sec);
        }
        ESP_LOGI(TAG, "saved %s (%u frames, %d ms)", path,
                 static_cast<unsigned>(frames), duration_ms);
        if (hit_max) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), I18n::T("已达最长 %d 分钟，已自动保存"),
                          kMaxRecordSeconds / 60);
            PostStatusFromWorker(false, msg);
        } else {
            PostStatusFromWorker(false, I18n::T("已保存到 SD 卡"));
        }
    }

done:
    if (file != nullptr) {
        fclose(file);
        if (!ok && path[0] != '\0') {
            unlink(path);
            UnlinkDurationSidecar(path);
        }
    }
    encoder.reset();
    RestoreWakeWordIfNeeded();

    s_state.store(RecState::Idle);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (s_screen_alive) {
            UpdateRecordButtonUi();
            if (ok) {
                char buf[16];
                FormatTimer(buf, sizeof(buf),
                            static_cast<int>(frames * kFrameDurationMs / 1000));
                if (s_ui.timer_lbl != nullptr) {
                    lv_label_set_text(s_ui.timer_lbl, buf);
                }
                RebuildFileList();
            }
        }
        esp_lv_adapter_unlock();
    }

    s_record_task = nullptr;
    vTaskDelete(nullptr);
}

void StartRecording() {
    if (!s_sd_ready) {
        SetRecordStatusText(I18n::T("请插入 SD 卡"));
        return;
    }
    if (s_state.load() != RecState::Idle || s_record_task != nullptr) {
        return;
    }

    StopPlayback();

    auto& as = Application::GetInstance().GetAudioService();
    if (!as.IsStarted()) {
        SetRecordStatusText(I18n::T("音频未就绪"));
        return;
    }
    if (as.IsAudioProcessorRunning()) {
        SetRecordStatusText(I18n::T("音频忙，稍后再试"));
        return;
    }

    s_stop_record.store(false);
    s_state.store(RecState::Recording);
    if (s_ui.timer_lbl != nullptr) {
        lv_label_set_text(s_ui.timer_lbl, "00:00");
    }
    SetRecordingStatusHint();
    UpdateRecordButtonUi();
    StartTickTimer();

    // Opus/SILK 编码栈很深：AudioService opus_codec 用 2048*13，
    // wake_word encode 用 4096*7；16KB 会 Stack protection fault。
    constexpr uint32_t kRecordTaskStack = 2048 * 14;
    if (xTaskCreatePinnedToCore(RecordTask, "rec_save", kRecordTaskStack, nullptr,
                                tskIDLE_PRIORITY + 3, &s_record_task, 0) != pdPASS) {
        s_record_task = nullptr;
        s_state.store(RecState::Idle);
        StopTickTimer();
        UpdateRecordButtonUi();
        SetRecordStatusText(I18n::T("录音任务启动失败"));
    }
}

void StopRecording() {
    if (s_state.load() != RecState::Recording) {
        return;
    }
    s_stop_record.store(true);
    s_state.store(RecState::Saving);
    StopTickTimer();
    UpdateRecordButtonUi();
    SetRecordStatusText(I18n::T("保存中…"));
}

void OnRecordBtnClicked(lv_event_t* /*e*/) {
    const RecState st = s_state.load();
    if (st == RecState::Idle) {
        StartRecording();
    } else if (st == RecState::Recording) {
        StopRecording();
    }
}

bool HasExt(const char* name, const char* ext4) {
    // ext4 like ".wav" / ".opus" (lowercase), case-insensitive match on last 4 or 5 chars
    if (name == nullptr || ext4 == nullptr) {
        return false;
    }
    const size_t len = std::strlen(name);
    const size_t elen = std::strlen(ext4);
    if (len < elen) {
        return false;
    }
    for (size_t i = 0; i < elen; ++i) {
        char a = name[len - elen + i];
        char b = ext4[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool IsRecordingFilename(const char* name) {
    return HasExt(name, ".opus") || HasExt(name, ".wav");
}

void MakeDurationSidecarPath(char* out, size_t out_size, const char* opus_path) {
    std::snprintf(out, out_size, "%s.dur", opus_path);
}

bool WriteDurationSidecar(const char* opus_path, int duration_sec) {
    if (opus_path == nullptr || duration_sec < 0) {
        return false;
    }
    char dur_path[192];
    MakeDurationSidecarPath(dur_path, sizeof(dur_path), opus_path);
    FILE* f = fopen(dur_path, "wb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "write dur sidecar failed: %s", dur_path);
        return false;
    }
    const int n = std::fprintf(f, "%d\n", duration_sec);
    fclose(f);
    return n > 0;
}

int ReadDurationSidecar(const char* opus_path) {
    if (opus_path == nullptr) {
        return -1;
    }
    char dur_path[192];
    MakeDurationSidecarPath(dur_path, sizeof(dur_path), opus_path);
    FILE* f = fopen(dur_path, "rb");
    if (f == nullptr) {
        return -1;
    }
    int sec = -1;
    if (std::fscanf(f, "%d", &sec) != 1 || sec < 0) {
        sec = -1;
    }
    fclose(f);
    return sec;
}

void UnlinkDurationSidecar(const char* opus_path) {
    if (opus_path == nullptr) {
        return;
    }
    char dur_path[192];
    MakeDurationSidecarPath(dur_path, sizeof(dur_path), opus_path);
    unlink(dur_path);
}

// 从文件尾读最后若干 KB，解析 Ogg 页 granule（避免整文件逐页扫描）。
int DurationSecFromOpusFileTail(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    const long file_size = ftell(f);
    if (file_size <= 27) {
        fclose(f);
        return 0;
    }

    constexpr long kTailMax = 64 * 1024;
    const long start = (file_size > kTailMax) ? (file_size - kTailMax) : 0;
    const size_t len = static_cast<size_t>(file_size - start);
    auto* buf = static_cast<uint8_t*>(
        heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        fclose(f);
        return 0;
    }
    if (fseek(f, start, SEEK_SET) != 0 || fread(buf, 1, len, f) != len) {
        heap_caps_free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);

    int64_t last_granule = 0;
    for (size_t i = 0; i + 27 <= len; ++i) {
        if (std::memcmp(buf + i, "OggS", 4) != 0 || buf[i + 4] != 0) {
            continue;
        }
        const uint8_t nseg = buf[i + 26];
        if (nseg == 0 || i + 27 + nseg > len) {
            continue;
        }
        size_t body = 0;
        for (uint8_t s = 0; s < nseg; ++s) {
            body += buf[i + 27 + s];
        }
        if (i + 27 + nseg + body > len) {
            continue;  // 不完整页（缓冲区开头可能截断）
        }
        const int64_t granule = ReadLe64(buf + i + 6);
        if (granule > 0) {
            last_granule = granule;
        }
    }
    heap_caps_free(buf);
    if (last_granule <= 0) {
        return 0;
    }
    return static_cast<int>(last_granule / 48000);
}

int EnsureOpusDuration(FileEntry& entry) {
    if (entry.format != RecFormat::Opus) {
        return entry.duration_sec;
    }
    if (entry.duration_ready && entry.duration_sec >= 0) {
        return entry.duration_sec;
    }
    const int from_side = ReadDurationSidecar(entry.path);
    if (from_side >= 0) {
        entry.duration_sec = from_side;
        entry.duration_ready = true;
        return entry.duration_sec;
    }
    const int from_tail = DurationSecFromOpusFileTail(entry.path);
    entry.duration_sec = from_tail;
    entry.duration_ready = true;
    if (from_tail > 0) {
        WriteDurationSidecar(entry.path, from_tail);
    }
    return entry.duration_sec;
}

void DurationFillTask(void* /*arg*/) {
    std::vector<std::string> pending;
    pending.reserve(16);
    for (const auto& e : s_files) {
        if (e.format == RecFormat::Opus && !e.duration_ready) {
            pending.emplace_back(e.path);
        }
    }

    for (const auto& path : pending) {
        if (s_stop_meta.load() || !s_screen_alive) {
            break;
        }
        const int from_side = ReadDurationSidecar(path.c_str());
        int sec = from_side;
        if (sec < 0) {
            sec = DurationSecFromOpusFileTail(path.c_str());
            if (sec > 0) {
                WriteDurationSidecar(path.c_str(), sec);
            } else if (sec == 0) {
                // 仍写入 0，避免反复扫损坏文件
                WriteDurationSidecar(path.c_str(), 0);
            }
        }
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (s_screen_alive) {
                for (auto& e : s_files) {
                    if (e.path == path) {
                        e.duration_sec = (sec >= 0) ? sec : 0;
                        e.duration_ready = true;
                        break;
                    }
                }
            }
            esp_lv_adapter_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!s_stop_meta.load() && s_screen_alive) {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (s_screen_alive && s_ui.list_scroll != nullptr && !s_detail_open) {
                // 列表可见时刷新时长文案（sidecar 已齐，Collect 很快）
                RebuildFileList(false);
            } else if (s_screen_alive && s_detail_open && s_detail_idx >= 0 &&
                       s_detail_idx < static_cast<int>(s_files.size())) {
                FileEntry& e = s_files[static_cast<size_t>(s_detail_idx)];
                EnsureOpusDuration(e);
                if (s_ui.detail_meta != nullptr) {
                    char dur[16];
                    char size_buf[24];
                    FormatTimer(dur, sizeof(dur), e.duration_sec);
                    FormatFileSize(size_buf, sizeof(size_buf), e.size_bytes);
                    char meta[80];
                    std::snprintf(meta, sizeof(meta), I18n::T("时长 %s · %s"), dur,
                                  size_buf);
                    lv_label_set_text(s_ui.detail_meta, meta);
                }
            }
            esp_lv_adapter_unlock();
        }
    }

    s_meta_task = nullptr;
    if (!s_stop_meta.load() && s_screen_alive) {
        ScheduleDurationFill();  // sidecar 写入失败时再补一轮
    }
    vTaskDelete(nullptr);
}

void ScheduleDurationFill() {
    if (!s_screen_alive || s_meta_task != nullptr) {
        return;
    }
    bool need = false;
    for (const auto& e : s_files) {
        if (e.format == RecFormat::Opus && !e.duration_ready) {
            need = true;
            break;
        }
    }
    if (!need) {
        return;
    }
    s_stop_meta.store(false);
    if (xTaskCreatePinnedToCore(DurationFillTask, "rec_meta", 8 * 1024, nullptr,
                                tskIDLE_PRIORITY + 1, &s_meta_task, 0) != pdPASS) {
        s_meta_task = nullptr;
        ESP_LOGW(TAG, "duration fill task start failed");
    }
}

void CollectRecordings() {
    s_files.clear();
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    DIR* dir = opendir(kPosixDir);
    if (dir == nullptr) {
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (ent->d_type == DT_DIR) {
            continue;
        }
        if (!IsRecordingFilename(ent->d_name)) {
            continue;
        }
        // d_name 最长 255，直接 snprintf 会触发 -Werror=format-truncation
        if (std::strlen(ent->d_name) >= sizeof(FileEntry{}.name)) {
            continue;
        }
        FileEntry entry = {};
        strlcpy(entry.name, ent->d_name, sizeof(entry.name));
        const int n = std::snprintf(entry.path, sizeof(entry.path), "%s/%s",
                                    kPosixDir, entry.name);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(entry.path)) {
            continue;
        }
        entry.format = HasExt(entry.name, ".opus") ? RecFormat::Opus : RecFormat::Wav;
        struct stat st;
        if (stat(entry.path, &st) == 0 && st.st_size > 0) {
            entry.size_bytes = static_cast<size_t>(st.st_size);
            if (entry.format == RecFormat::Opus) {
                // 进页只读 sidecar；缺失则留给后台 / 详情懒加载
                const int side = ReadDurationSidecar(entry.path);
                if (side >= 0) {
                    entry.duration_sec = side;
                    entry.duration_ready = true;
                } else {
                    entry.duration_sec = 0;
                    entry.duration_ready = false;
                }
            } else if (st.st_size > 44) {
                entry.duration_sec =
                    static_cast<int>((st.st_size - 44) / (kSampleRate * 2));
                if (entry.duration_sec < 0) {
                    entry.duration_sec = 0;
                }
                entry.duration_ready = true;
            }
        }
        s_files.push_back(entry);
        if (s_files.size() >= static_cast<size_t>(kMaxListItems)) {
            break;
        }
    }
    closedir(dir);

    std::sort(s_files.begin(), s_files.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  return std::strcmp(a.name, b.name) > 0;
              });
}

bool PlayOpusFile(FILE* file, AudioService& as) {
    as.ResetDecoder();
    PostPlayStatusFromWorker(I18n::T("播放中…"));

    bool seen_head = false;
    bool seen_tags = false;
    int sample_rate = kSampleRate;
    uint8_t hdr[27];

    while (!s_stop_play.load()) {
        if (fread(hdr, 1, 27, file) != 27) {
            break;
        }
        if (std::memcmp(hdr, "OggS", 4) != 0) {
            PostPlayStatusFromWorker(I18n::T("录音文件损坏"));
            return false;
        }
        const uint8_t nseg = hdr[26];
        uint8_t segs[255];
        if (nseg == 0 || fread(segs, 1, nseg, file) != nseg) {
            break;
        }

        size_t body_off = 0;
        size_t body_size = 0;
        for (uint8_t i = 0; i < nseg; ++i) {
            body_size += segs[i];
        }
        std::vector<uint8_t> body(body_size);
        if (body_size > 0 && fread(body.data(), 1, body_size, file) != body_size) {
            break;
        }

        size_t seg_idx = 0;
        while (seg_idx < nseg && !s_stop_play.load()) {
            size_t pkt_len = 0;
            const size_t pkt_start = body_off;
            bool continued = false;
            do {
                const uint8_t l = segs[seg_idx++];
                pkt_len += l;
                body_off += l;
                continued = (l == 255);
            } while (continued && seg_idx < nseg);

            if (pkt_len == 0) {
                continue;
            }
            const uint8_t* pkt = body.data() + pkt_start;

            if (!seen_head) {
                if (pkt_len >= 19 && std::memcmp(pkt, "OpusHead", 8) == 0) {
                    seen_head = true;
                    sample_rate = static_cast<int>(ReadLe32(pkt + 12));
                    if (sample_rate <= 0) {
                        sample_rate = kSampleRate;
                    }
                }
                continue;
            }
            if (!seen_tags) {
                if (pkt_len >= 8 && std::memcmp(pkt, "OpusTags", 8) == 0) {
                    seen_tags = true;
                }
                continue;
            }

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = kFrameDurationMs;
            packet->payload.assign(pkt, pkt + pkt_len);
            if (!as.PushPacketToDecodeQueue(std::move(packet), true)) {
                break;
            }
        }

        if (hdr[5] & 0x04) {
            break;
        }
    }

    // 等解码播放队列排空
    while (!s_stop_play.load() && !as.IsIdle()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

bool PlayWavFile(FILE* file, AudioCodec* codec) {
    uint8_t hdr[44] = {};
    if (fread(hdr, 1, sizeof(hdr), file) != sizeof(hdr) ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        PostPlayStatusFromWorker(I18n::T("录音文件损坏"));
        return false;
    }
    if (codec == nullptr) {
        PostPlayStatusFromWorker(I18n::T("音频未就绪"));
        return false;
    }
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }
    PostPlayStatusFromWorker(I18n::T("播放中…"));

    std::vector<int16_t> pcm(static_cast<size_t>(kPlayChunkSamples));
    while (!s_stop_play.load()) {
        const size_t want = pcm.size() * sizeof(int16_t);
        const size_t got = fread(pcm.data(), 1, want, file);
        if (got < sizeof(int16_t)) {
            break;
        }
        const size_t samples = got / sizeof(int16_t);
        pcm.resize(samples);
        codec->OutputData(pcm);
        pcm.resize(static_cast<size_t>(kPlayChunkSamples));
    }
    return true;
}

void PlayTask(void* arg) {
    auto* path_copy = static_cast<char*>(arg);
    const bool is_opus = HasExt(path_copy, ".opus");
    FILE* file = fopen(path_copy, "rb");
    heap_caps_free(path_copy);
    path_copy = nullptr;

    if (file == nullptr) {
        PostPlayStatusFromWorker(I18n::T("无法打开录音文件"));
        s_state.store(RecState::Idle);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (s_screen_alive) {
                UpdateRecordButtonUi();
            }
            esp_lv_adapter_unlock();
        }
        s_play_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    auto& as = Application::GetInstance().GetAudioService();
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    bool played_ok = false;
    if (is_opus) {
        played_ok = PlayOpusFile(file, as);
    } else {
        as.ResetDecoder();
        played_ok = PlayWavFile(file, codec);
    }
    fclose(file);

    s_playing_path[0] = '\0';
    s_state.store(RecState::Idle);
    if (played_ok && !s_stop_play.load()) {
        PostPlayStatusFromWorker(I18n::T("播放结束"));
    } else if (s_stop_play.load()) {
        PostPlayStatusFromWorker(I18n::T("已停止播放"));
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (s_screen_alive) {
            UpdateRecordButtonUi();
        }
        esp_lv_adapter_unlock();
    }
    s_play_task = nullptr;
    vTaskDelete(nullptr);
}

void StopPlayback() {
    if (s_play_task == nullptr && s_state.load() != RecState::Playing) {
        return;
    }
    s_stop_play.store(true);
    // 最多等约 300ms，避免长时间卡住 LVGL 线程。
    for (int i = 0; i < 15 && s_play_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_stop_play.store(false);
    s_playing_path[0] = '\0';
    if (s_state.load() == RecState::Playing && s_play_task == nullptr) {
        s_state.store(RecState::Idle);
    }
    Application::GetInstance().GetAudioService().ResetDecoder();
    UpdateDetailPlayButton();
}

void StartPlayback(const char* path) {
    if (!s_sd_ready || path == nullptr || path[0] == '\0') {
        return;
    }
    auto set_status = [](const char* text) {
        if (s_detail_open) {
            SetDetailStatusText(text);
        } else {
            SetListStatusText(text);
        }
    };
    if (s_state.load() == RecState::Recording || s_state.load() == RecState::Saving) {
        set_status(I18n::T("请先结束录音"));
        return;
    }

    StopPlayback();
    if (s_play_task != nullptr) {
        set_status(I18n::T("请稍候"));
        return;
    }

    auto* path_copy = static_cast<char*>(heap_caps_malloc(std::strlen(path) + 1, MALLOC_CAP_8BIT));
    if (path_copy == nullptr) {
        set_status(I18n::T("内存不足"));
        return;
    }
    std::strcpy(path_copy, path);
    std::snprintf(s_playing_path, sizeof(s_playing_path), "%s", path);

    s_stop_play.store(false);
    s_state.store(RecState::Playing);
    UpdateRecordButtonUi();
    UpdateDetailPlayButton();

    if (xTaskCreatePinnedToCore(PlayTask, "rec_play", 8 * 1024, path_copy,
                                tskIDLE_PRIORITY + 2, &s_play_task, 0) != pdPASS) {
        heap_caps_free(path_copy);
        s_play_task = nullptr;
        s_playing_path[0] = '\0';
        s_state.store(RecState::Idle);
        UpdateRecordButtonUi();
        UpdateDetailPlayButton();
        set_status(I18n::T("播放任务启动失败"));
    }
}

int EventIndex(lv_event_t* e) {
    return static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
}

void UpdateDetailPlayButton() {
    if (!s_screen_alive || s_ui.detail_play_lbl == nullptr) {
        return;
    }
    const bool playing_this =
        s_state.load() == RecState::Playing && s_detail_path[0] != '\0' &&
        std::strcmp(s_playing_path, s_detail_path) == 0;
    lv_label_set_text(s_ui.detail_play_lbl,
                      playing_this ? I18n::T("停止播放") : I18n::T("播放"));
}

void OnDetailPlayClicked(lv_event_t* /*e*/) {
    if (s_detail_path[0] == '\0') {
        return;
    }
    if (s_state.load() == RecState::Playing &&
        std::strcmp(s_playing_path, s_detail_path) == 0) {
        StopPlayback();
        SetDetailStatusText(I18n::T("已停止播放"));
        UpdateDetailPlayButton();
        UpdateRecordButtonUi();
        return;
    }
    StartPlayback(s_detail_path);
    UpdateDetailPlayButton();
}

void AppendJsonStringArray(std::string& out, cJSON* arr, const char* title) {
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) <= 0) {
        return;
    }
    out += title;
    out += "\n";
    const int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; ++i) {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsString(item) && item->valuestring != nullptr &&
            item->valuestring[0] != '\0') {
            out += item->valuestring;
            out += "\n";
        }
    }
    out += "\n";
}

std::string FormatCompletedAsrText(cJSON* data) {
    std::string out;
    cJSON* text = cJSON_GetObjectItemCaseSensitive(data, "asrText");
    if (cJSON_IsString(text) && text->valuestring != nullptr &&
        text->valuestring[0] != '\0') {
        out = text->valuestring;
    }

    cJSON* duration = cJSON_GetObjectItemCaseSensitive(data, "durationMs");
    if (cJSON_IsNumber(duration) && duration->valuedouble > 0) {
        char line[64];
        std::snprintf(line, sizeof(line), I18n::T("音频时长：%.1f 秒"),
                      duration->valuedouble / 1000.0);
        if (!out.empty()) {
            out += "\n\n";
        }
        out += line;
    }

    AppendJsonStringArray(out, cJSON_GetObjectItemCaseSensitive(data, "summary"),
                          I18n::T("摘要"));

    if (out.empty()) {
        out = I18n::T("转写成功，但没有可显示的内容");
    }
    return out;
}

void ApplyAsrRecordData(cJSON* data) {
    if (!cJSON_IsObject(data)) {
        return;
    }
    cJSON* status = cJSON_GetObjectItemCaseSensitive(data, "status");
    if (!cJSON_IsString(status) || status->valuestring == nullptr) {
        return;
    }
    const char* st = status->valuestring;
    if (std::strcmp(st, "transcribing") == 0) {
        PostDetailResultFromWorker(I18n::T("转写处理中，请稍后"));
        PostDetailStatusFromWorker(I18n::T("转写处理中，请稍后"));
        return;
    }
    if (std::strcmp(st, "completed") == 0) {
        const std::string formatted = FormatCompletedAsrText(data);
        PostDetailResultFromWorker(formatted.c_str());
        PostDetailStatusFromWorker(I18n::T("转写完成"));
        return;
    }
    if (std::strcmp(st, "failed") == 0) {
        cJSON* err = cJSON_GetObjectItemCaseSensitive(data, "errorMessage");
        if (cJSON_IsString(err) && err->valuestring != nullptr &&
            err->valuestring[0] != '\0') {
            PostDetailResultFromWorker(err->valuestring);
            PostDetailStatusFromWorker(I18n::T("转写失败"));
        } else {
            PostDetailStatusFromWorker(I18n::T("转写失败"));
        }
    }
    // 其它状态：静默忽略
}

void AsrQueryTask(void* /*arg*/) {
    char name_snapshot[96] = {};
    strlcpy(name_snapshot, s_detail_name, sizeof(name_snapshot));
    if (name_snapshot[0] == '\0') {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const std::string url = api::AsrAudioRecordsUrl(name_snapshot);
    const std::string device_id = SystemInfo::GetMacAddress();
    api::LogHttpRequest(TAG, "GET", url, std::string(), "ASR audio-records");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
    http->SetHeader("X-Device-Id", device_id.c_str());

    if (!http->Open("GET", url)) {
        http->Close();
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    const int status = http->GetStatusCode();
    const std::string resp = http->ReadAll();
    http->Close();
    api::LogHttpResponse(TAG, status, api::RedactClawUrlsForLog(resp));

    if (s_stop_asr.load() || !s_detail_open ||
        std::strcmp(name_snapshot, s_detail_name) != 0) {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // 无记录 / 请求失败：不改动 UI（「没有就不管」）
    if (status != 200 || resp.empty()) {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        s_asr_query_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsNumber(code) && code->valueint == 200 && cJSON_IsObject(data)) {
        ApplyAsrRecordData(data);
    }
    cJSON_Delete(root);

    const bool need_again =
        !s_stop_asr.load() && s_detail_open && s_detail_name[0] != '\0' &&
        std::strcmp(name_snapshot, s_detail_name) != 0;
    s_asr_query_task = nullptr;
    if (need_again) {
        ScheduleAsrQuery();
    }
    vTaskDelete(nullptr);
}

void ScheduleAsrQuery() {
    if (!s_screen_alive || !s_detail_open || s_detail_name[0] == '\0') {
        return;
    }
    if (s_asr_query_task != nullptr) {
        return;
    }
    s_stop_asr.store(false);
    if (xTaskCreatePinnedToCore(AsrQueryTask, "rec_asr_q", 8 * 1024, nullptr,
                                tskIDLE_PRIORITY + 1, &s_asr_query_task,
                                0) != pdPASS) {
        s_asr_query_task = nullptr;
        ESP_LOGW(TAG, "asr query task start failed");
    }
}

void AsrUploadTask(void* /*arg*/) {
    FILE* file = fopen(s_detail_path, "rb");
    if (file == nullptr) {
        PostDetailStatusFromWorker(I18n::T("无法打开录音文件"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        PostDetailStatusFromWorker(I18n::T("读取录音失败"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    const long file_size = ftell(file);
    if (file_size <= 0 || file_size > 8 * 1024 * 1024) {
        fclose(file);
        PostDetailStatusFromWorker(I18n::T("录音文件过大或无效"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    rewind(file);

    auto* file_buf = static_cast<uint8_t*>(
        heap_caps_malloc(static_cast<size_t>(file_size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (file_buf == nullptr) {
        file_buf = static_cast<uint8_t*>(
            heap_caps_malloc(static_cast<size_t>(file_size), MALLOC_CAP_8BIT));
    }
    if (file_buf == nullptr) {
        fclose(file);
        PostDetailStatusFromWorker(I18n::T("内存不足"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    const size_t got = fread(file_buf, 1, static_cast<size_t>(file_size), file);
    fclose(file);
    if (got != static_cast<size_t>(file_size)) {
        heap_caps_free(file_buf);
        PostDetailStatusFromWorker(I18n::T("读取录音失败"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (s_stop_asr.load()) {
        heap_caps_free(file_buf);
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        heap_caps_free(file_buf);
        PostDetailStatusFromWorker(I18n::T("网络不可用"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        heap_caps_free(file_buf);
        PostDetailStatusFromWorker(I18n::T("网络不可用"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const std::string boundary = "----XingzhiAsrBoundary";
    const char* fname = s_detail_name[0] != '\0' ? s_detail_name : "recording.opus";
    std::string body;
    body.reserve(static_cast<size_t>(file_size) + 256);
    body += "--";
    body += boundary;
    body += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
    body += fname;
    body += "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    body.append(reinterpret_cast<const char*>(file_buf), static_cast<size_t>(file_size));
    heap_caps_free(file_buf);
    file_buf = nullptr;
    body += "\r\n--";
    body += boundary;
    body += "--\r\n";

    const std::string url = api::Url(api::kAsrTranscribe);
    const std::string device_id = SystemInfo::GetMacAddress();
    api::LogHttpBinaryRequest(TAG, "POST", url, body.size(), "ASR transcribe");

    http->SetContent(std::move(body));
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
    http->SetHeader("X-Device-Id", device_id.c_str());

    PostDetailStatusFromWorker(I18n::T("正在上传…"));
    if (!http->Open("POST", url)) {
        PostDetailStatusFromWorker(I18n::T("转写请求失败"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const int status = http->GetStatusCode();
    const std::string resp = http->ReadAll();
    http->Close();
    api::LogHttpResponse(TAG, status, api::RedactClawUrlsForLog(resp));

    if (s_stop_asr.load() || !s_detail_open) {
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (status != 200) {
        char err[64];
        std::snprintf(err, sizeof(err), I18n::T("转写失败（HTTP %d）"), status);
        PostDetailStatusFromWorker(err);
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        PostDetailStatusFromWorker(I18n::T("转写结果解析失败"));
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsNumber(code) || code->valueint != 200 || !cJSON_IsObject(data)) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(msg) && msg->valuestring != nullptr) {
            PostDetailStatusFromWorker(msg->valuestring);
        } else {
            PostDetailStatusFromWorker(I18n::T("转写失败"));
        }
        cJSON_Delete(root);
        s_asr_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    cJSON_Delete(root);
    PostDetailResultFromWorker(I18n::T("转写处理中，请稍后"));
    PostDetailStatusFromWorker(
        I18n::T("上传成功，请稍后来查看转写结果，勿反复上传"));
    s_asr_task = nullptr;
    vTaskDelete(nullptr);
}

void OnDetailAsrClicked(lv_event_t* /*e*/) {
    if (s_detail_path[0] == '\0') {
        return;
    }
    if (s_asr_task != nullptr) {
        SetDetailStatusText(I18n::T("正在上传…"));
        return;
    }
    if (s_state.load() == RecState::Recording || s_state.load() == RecState::Saving) {
        SetDetailStatusText(I18n::T("请先结束录音"));
        return;
    }

    s_stop_asr.store(false);
    SetDetailStatusText(I18n::T("正在上传…"));
    if (xTaskCreatePinnedToCore(AsrUploadTask, "rec_asr", 16 * 1024, nullptr,
                                tskIDLE_PRIORITY + 2, &s_asr_task, 0) != pdPASS) {
        s_asr_task = nullptr;
        SetDetailStatusText(I18n::T("转写任务启动失败"));
    }
}

void OnDetailBackClicked(lv_event_t* /*e*/) { HideDetail(); }

void HideDetail() {
    if (!s_detail_open) {
        return;
    }
    if (s_state.load() == RecState::Playing &&
        std::strcmp(s_playing_path, s_detail_path) == 0) {
        StopPlayback();
    }
    s_stop_asr.store(true);
    s_detail_open = false;
    s_detail_idx = -1;
    s_detail_path[0] = '\0';
    s_detail_name[0] = '\0';
    if (s_ui.detail_panel != nullptr) {
        lv_obj_add_flag(s_ui.detail_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void ShowDetail(int idx) {
    if (idx < 0 || idx >= static_cast<int>(s_files.size()) ||
        s_ui.detail_panel == nullptr) {
        return;
    }
    FileEntry& entry = s_files[static_cast<size_t>(idx)];
    // 详情打开时同步补算一次（尾部扫描，通常很快）
    EnsureOpusDuration(entry);
    s_detail_idx = idx;
    s_detail_open = true;
    strlcpy(s_detail_path, entry.path, sizeof(s_detail_path));
    strlcpy(s_detail_name, entry.name, sizeof(s_detail_name));

    if (s_ui.detail_title != nullptr) {
        lv_label_set_text(s_ui.detail_title, entry.name);
    }
    if (s_ui.detail_meta != nullptr) {
        char dur[16];
        char size_buf[24];
        FormatTimer(dur, sizeof(dur), entry.duration_sec);
        FormatFileSize(size_buf, sizeof(size_buf), entry.size_bytes);
        char meta[80];
        std::snprintf(meta, sizeof(meta), I18n::T("时长 %s · %s"), dur, size_buf);
        lv_label_set_text(s_ui.detail_meta, meta);
    }
    SetDetailStatusText("");
    SetDetailResultText(I18n::T("点击「录音转写」上传，稍后再进详情查看结果"));
    UpdateDetailPlayButton();
    lv_obj_remove_flag(s_ui.detail_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.detail_panel);
    ScheduleAsrQuery();
}

void OnRowClicked(lv_event_t* e) {
    const int idx = EventIndex(e);
    if (idx < 0 || idx >= static_cast<int>(s_files.size())) {
        return;
    }
    ShowDetail(idx);
}

void OnDeleteClicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int idx = EventIndex(e);
    if (idx < 0 || idx >= static_cast<int>(s_files.size())) {
        return;
    }
    const FileEntry& entry = s_files[static_cast<size_t>(idx)];
    const char* path = entry.path;
    if (s_state.load() == RecState::Playing &&
        std::strcmp(s_playing_path, path) == 0) {
        StopPlayback();
    }
    if (unlink(path) != 0) {
        ESP_LOGE(TAG, "delete failed: %s", path);
        SetListStatusText(I18n::T("删除失败"));
        return;
    }
    if (entry.format == RecFormat::Opus) {
        UnlinkDurationSidecar(path);
    }
    ESP_LOGI(TAG, "deleted: %s", path);
    SetListStatusText(I18n::T("已删除"));
    RebuildFileList();
}

void ClearListChildren() {
    if (s_ui.list_scroll == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.list_scroll);
}

void RebuildFileList() { RebuildFileList(true); }

void RebuildFileList(bool schedule_fill) {
    if (!s_screen_alive || s_ui.list_scroll == nullptr) {
        return;
    }
    CollectRecordings();
    ClearListChildren();

    if (s_files.empty()) {
        if (s_ui.list_empty != nullptr) {
            lv_obj_remove_flag(s_ui.list_empty, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (s_ui.list_empty != nullptr) {
        lv_obj_add_flag(s_ui.list_empty, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < s_files.size(); ++i) {
        void* idx_ud = reinterpret_cast<void*>(static_cast<intptr_t>(i));

        lv_obj_t* row = lv_obj_create(s_ui.list_scroll);
        screen_strip_obj_chrome(row);
        lv_obj_set_size(row, lv_pct(100), 88);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, 8, LV_PART_MAIN);
        lv_obj_set_style_margin_bottom(row, 12, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, OnRowClicked, LV_EVENT_CLICKED, idx_ud);
        screen_swipe_back_ignore(row, true);

        lv_obj_t* name = lv_label_create(row);
        lv_label_set_text(name, s_files[i].name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 420);
        lv_obj_set_style_text_color(name, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, -12);

        lv_obj_t* hint = lv_label_create(row);
        char dur[16];
        char size_buf[24];
        if (s_files[i].duration_ready) {
            FormatTimer(dur, sizeof(dur), s_files[i].duration_sec);
        } else {
            std::snprintf(dur, sizeof(dur), "--:--");
        }
        FormatFileSize(size_buf, sizeof(size_buf), s_files[i].size_bytes);
        char hint_buf[72];
        std::snprintf(hint_buf, sizeof(hint_buf), I18n::T("时长 %s · %s · 点击查看"),
                      dur, size_buf);
        lv_label_set_text(hint, hint_buf);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
        lv_obj_set_width(hint, 420);
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_LEFT_MID, 0, 16);

        lv_obj_t* del = lv_button_create(row);
        lv_obj_remove_style_all(del);
        lv_obj_set_size(del, 96, 56);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(del, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del, lv_color_hex(kColorDanger), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(del, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del, lv_color_hex(0xB91C1C),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_add_event_cb(del, OnDeleteClicked, LV_EVENT_CLICKED, idx_ud);
        screen_swipe_back_ignore(del, true);

        lv_obj_t* del_lbl = lv_label_create(del);
        lv_label_set_text(del_lbl, I18n::T("删除"));
        lv_obj_set_style_text_color(del_lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(del_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(del_lbl);
    }

    if (schedule_fill) {
        ScheduleDurationFill();
    }
}

void OnTabChanged(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    if (s_ui.tabview == nullptr) {
        return;
    }
    const uint32_t idx = lv_tabview_get_tab_active(s_ui.tabview);
    if (idx == 1) {
        RebuildFileList();
    }
}

void ForceStopAll() {
    s_stop_record.store(true);
    s_stop_play.store(true);
    s_stop_asr.store(true);
    s_stop_meta.store(true);
    StopTickTimer();
    for (int i = 0; i < 80 &&
         (s_record_task != nullptr || s_play_task != nullptr || s_asr_task != nullptr ||
          s_asr_query_task != nullptr || s_meta_task != nullptr);
         ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_stop_record.store(false);
    s_stop_play.store(false);
    s_stop_asr.store(false);
    s_stop_meta.store(false);
    s_playing_path[0] = '\0';
    s_detail_open = false;
    s_detail_idx = -1;
    s_detail_path[0] = '\0';
    s_detail_name[0] = '\0';
    s_state.store(RecState::Idle);
    RestoreWakeWordIfNeeded();
    Application::GetInstance().GetAudioService().ResetDecoder();
}

void BuildDetailPanel(lv_obj_t* parent) {
    lv_obj_t* panel = lv_obj_create(parent);
    s_ui.detail_panel = panel;
    screen_strip_obj_chrome(panel);
    lv_obj_set_size(panel, kPanelSize, kPanelSize);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    screen_swipe_back_ignore(panel, true);

    lv_obj_t* header = lv_obj_create(panel);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnDetailBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    s_ui.detail_title = lv_label_create(header);
    lv_label_set_text(s_ui.detail_title, "");
    lv_label_set_long_mode(s_ui.detail_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.detail_title, kPanelSize - 16 - kBackBtnSize - 40);
    lv_obj_set_style_text_color(s_ui.detail_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(s_ui.detail_title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 8, 0);

    s_ui.detail_meta = lv_label_create(panel);
    lv_label_set_text(s_ui.detail_meta, "");
    lv_obj_set_style_text_color(s_ui.detail_meta, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_meta, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.detail_meta, LV_ALIGN_TOP_MID, 0, kHeaderH + 8);

    // 操作按钮行
    lv_obj_t* play = lv_button_create(panel);
    s_ui.detail_play_btn = play;
    lv_obj_remove_style_all(play);
    lv_obj_set_size(play, 300, 72);
    lv_obj_align(play, LV_ALIGN_TOP_LEFT, 40, kHeaderH + 48);
    lv_obj_set_style_radius(play, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(play, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(play, lv_color_hex(0x2563EB),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_event_cb(play, OnDetailPlayClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(play, true);

    s_ui.detail_play_lbl = lv_label_create(play);
    lv_label_set_text(s_ui.detail_play_lbl, I18n::T("播放"));
    lv_obj_set_style_text_color(s_ui.detail_play_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_play_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(s_ui.detail_play_lbl);

    lv_obj_t* asr = lv_button_create(panel);
    s_ui.detail_asr_btn = asr;
    lv_obj_remove_style_all(asr);
    lv_obj_set_size(asr, 300, 72);
    lv_obj_align(asr, LV_ALIGN_TOP_RIGHT, -40, kHeaderH + 48);
    lv_obj_set_style_radius(asr, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(asr, lv_color_hex(0x059669), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(asr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(asr, lv_color_hex(0x047857),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_event_cb(asr, OnDetailAsrClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(asr, true);

    s_ui.detail_asr_lbl = lv_label_create(asr);
    lv_label_set_text(s_ui.detail_asr_lbl, I18n::T("录音转写"));
    lv_obj_set_style_text_color(s_ui.detail_asr_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_asr_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(s_ui.detail_asr_lbl);

    s_ui.detail_status = lv_label_create(panel);
    lv_label_set_text(s_ui.detail_status, "");
    lv_obj_set_width(s_ui.detail_status, kPanelSize - 80);
    lv_label_set_long_mode(s_ui.detail_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_ui.detail_status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.detail_status, LV_ALIGN_TOP_MID, 0, kHeaderH + 136);
    screen_make_input_passive(s_ui.detail_status);

    lv_obj_t* result_box = lv_obj_create(panel);
    screen_strip_obj_chrome(result_box);
    lv_obj_set_size(result_box, kPanelSize - 48, kPanelSize - (kHeaderH + 180));
    lv_obj_align(result_box, LV_ALIGN_TOP_MID, 0, kHeaderH + 168);
    lv_obj_set_style_bg_color(result_box, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(result_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(result_box, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(result_box, 20, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(result_box, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(result_box, true);

    s_ui.detail_result = lv_label_create(result_box);
    lv_label_set_text(s_ui.detail_result, "");
    lv_obj_set_width(s_ui.detail_result, kPanelSize - 48 - 40);
    lv_label_set_long_mode(s_ui.detail_result, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_ui.detail_result, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_result, &font_puhui_20_4, LV_PART_MAIN);
}

void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("录音"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);
}

void BuildNoSdHint(lv_obj_t* parent) {
    lv_obj_t* hint = lv_label_create(parent);
    lv_label_set_text(hint, I18n::T("未检测到 SD 卡\n\n请插入 SD 卡后再使用录音功能"));
    lv_obj_set_width(hint, kPanelSize - 80);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 20);
    screen_make_input_passive(hint);
}

void BuildRecordTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(tab);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 640, 220);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(card);

    s_ui.timer_lbl = lv_label_create(card);
    lv_label_set_text(s_ui.timer_lbl, "00:00");
    lv_obj_set_style_text_color(s_ui.timer_lbl, lv_color_hex(0xF87171), LV_PART_MAIN);
    // 用含冒号字形的字体；纯数字字库会把 "00:07" 渲成 "0007"
    lv_obj_set_style_text_font(s_ui.timer_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(s_ui.timer_lbl, LV_ALIGN_CENTER, 0, -16);

    s_ui.status_lbl = lv_label_create(card);
    SetIdleRecordHint();
    lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ui.status_lbl, 560);
    lv_obj_set_style_text_align(s_ui.status_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_ui.status_lbl, LV_ALIGN_BOTTOM_MID, 0, -24);

    s_ui.record_btn = lv_button_create(tab);
    lv_obj_remove_style_all(s_ui.record_btn);
    lv_obj_set_size(s_ui.record_btn, 360, 88);
    lv_obj_align(s_ui.record_btn, LV_ALIGN_TOP_MID, 0, 320);
    lv_obj_set_style_radius(s_ui.record_btn, 44, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(kColorRecord), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.record_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(0xB91C1C),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_event_cb(s_ui.record_btn, OnRecordBtnClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(s_ui.record_btn, true);

    s_ui.record_btn_lbl = lv_label_create(s_ui.record_btn);
    lv_label_set_text(s_ui.record_btn_lbl, I18n::T("开始录音"));
    lv_obj_set_style_text_color(s_ui.record_btn_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.record_btn_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(s_ui.record_btn_lbl);

    lv_obj_t* tip = lv_label_create(tab);
    {
        char tip_buf[96];
        std::snprintf(tip_buf, sizeof(tip_buf),
                      I18n::T("单次最长 %d 分钟 · Opus：/sdcard/recordings"),
                      kMaxRecordSeconds / 60);
        lv_label_set_text(tip, tip_buf);
    }
    lv_obj_set_style_text_color(tip, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(tip, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 440);
    screen_make_input_passive(tip);
}

void BuildListTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.list_status = lv_label_create(tab);
    lv_label_set_text(s_ui.list_status, "");
    lv_obj_set_width(s_ui.list_status, kPanelSize - 48);
    lv_label_set_long_mode(s_ui.list_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.list_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.list_status, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.list_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.list_status, LV_ALIGN_BOTTOM_MID, 0, -12);
    screen_make_input_passive(s_ui.list_status);

    s_ui.list_empty = lv_label_create(tab);
    lv_label_set_text(s_ui.list_empty, I18n::T("暂无录音"));
    lv_obj_set_style_text_color(s_ui.list_empty, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.list_empty, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.list_empty, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(s_ui.list_empty);

    s_ui.list_scroll = lv_obj_create(tab);
    screen_strip_obj_chrome(s_ui.list_scroll);
    lv_obj_set_size(s_ui.list_scroll, kPanelSize - 40, kBodyH - kTabBarH - 56);
    lv_obj_align(s_ui.list_scroll, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_opa(s_ui.list_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.list_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.list_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_ui.list_scroll, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_ui.list_scroll, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(s_ui.list_scroll, true);
}

void BuildTabView(lv_obj_t* parent) {
    lv_obj_t* tv = lv_tabview_create(parent);
    s_ui.tabview = tv;
    lv_obj_set_size(tv, kPanelSize, kBodyH);
    lv_obj_set_pos(tv, 0, kHeaderH);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);

    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTabBar), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorAccent),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t* content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);
    lv_obj_add_event_cb(tv, OnTabChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* tab_rec = lv_tabview_add_tab(tv, I18n::T("录音"));
    BuildRecordTab(tab_rec);

    lv_obj_t* tab_list = lv_tabview_add_tab(tv, I18n::T("列表"));
    BuildListTab(tab_list);
}

void OnSwipeBack() {
    if (s_detail_open) {
        HideDetail();
        return;
    }
    if (s_state.load() == RecState::Recording) {
        StopRecording();
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnBackClicked(lv_event_t* /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t* /*e*/) {
    ForceStopAll();
    s_screen_alive = false;
    s_detail_open = false;
    s_ui = {};
    s_files.clear();
}

}  // namespace

lv_obj_t* RecordingScreen::Create() {
    s_ui = {};
    s_screen_alive = true;
    s_state.store(RecState::Idle);
    s_sd_ready = SdCardManager::GetInstance().IsMounted();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);

    if (!s_sd_ready) {
        BuildNoSdHint(scr);
    } else {
        EnsureRecordingsDir();
        BuildTabView(scr);
        BuildDetailPanel(scr);
        RebuildFileList();
    }

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    return scr;
}

void RecordingScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: recording_screen sd=%d", s_sd_ready ? 1 : 0);
    } else {
        ESP_LOGI(TAG, "unload: recording_screen");
        ForceStopAll();
    }
}
