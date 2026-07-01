#include "openclaw_screen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "api_endpoints.h"
#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "device_state.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"
#include "system_info.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "OpenClawScreen";

// 录音参数与缓冲。一秒 16000 * 2 = 32KB，最大 60s = ~1.9MB，从 PSRAM 取。
constexpr int  kSampleRate        = 16000;
constexpr int  kSamplesPerFrame   = kSampleRate * 60 / 1000;  // 60ms 一帧
constexpr int  kBytesPerSample    = 2;
constexpr int  kMaxRecordSeconds  = 60;
constexpr size_t kMaxRecordBytes  =
    static_cast<size_t>(kSampleRate) * kBytesPerSample * kMaxRecordSeconds;
constexpr int  kMinRecordMs       = 300;  // 低于此时长视为误触

// ---------------------------------------------------------------------------
// 720x720 暗黑主题布局（配色对齐 network_screen）
//
//   ┌───────────────────────────────────────────┐ 0
//   │ Header   "OpenClaw"  [清空]                  │ 88
//   ├───────────────────────────────────────────┤
//   │  消息区（右侧绿气泡，自动滚到最新）        │
//   │                                           │
//   ├───────────────────────────────────────────┤ 580
//   │ 状态文字                                   │
//   │        ┌───────────────┐                  │
//   │        │   按住说话     │                  │
//   │        └───────────────┘                  │
//   └───────────────────────────────────────────┘ 720
// ---------------------------------------------------------------------------
constexpr int32_t kPanelW       = 720;
constexpr int32_t kPanelH       = 720;
constexpr int32_t kHeaderH      = 88;
constexpr int32_t kBackBtnSize  = 72;
constexpr int32_t kFooterH      = 140;
constexpr int32_t kListH        = kPanelH - kHeaderH - kFooterH;
constexpr int32_t kRefreshIntervalMs = 3000;
constexpr int32_t kListPadH     = 18;
constexpr int32_t kBubblePadX   = 18;
constexpr int32_t kBubblePadY   = 14;
constexpr int32_t kBubbleRadius = 18;
constexpr int32_t kSideMargin   = 8;
constexpr int32_t kRowGap       = 12;
constexpr int32_t kMaxMessages  = 50;

constexpr uint32_t kColorBg          = 0x0E1116;
constexpr uint32_t kColorHeaderBg    = 0x12151C;
constexpr uint32_t kColorDivider     = 0x2A2F3A;
constexpr uint32_t kColorHeaderText  = 0xFFFFFF;
constexpr uint32_t kColorHeaderBtn   = 0x2A2F3A;
constexpr uint32_t kColorHeaderBtnBorder = 0x3B4556;
constexpr uint32_t kColorHeaderBtnText   = 0xE5E7EB;
constexpr uint32_t kColorRightBubble = 0x1E3A2F;
constexpr uint32_t kColorLeftBubble  = 0x202736;
constexpr uint32_t kColorBubbleText  = 0xE8F5E9;
constexpr uint32_t kColorLeftBubbleText = 0xE5E7EB;
constexpr uint32_t kColorHintText    = 0x9AA3B2;
constexpr uint32_t kColorErrorText   = 0xF87171;

constexpr uint32_t kColorRecordBtnIdle   = 0x2563EB;
constexpr uint32_t kColorRecordBtnActive = 0xDC2626;
constexpr uint32_t kColorRecordBtnBusy   = 0x4B5563;

constexpr const char kEmptyHint[] = "开始与龙虾对话吧！";

// ---------------------------------------------------------------------------
// 屏幕级状态机
//
// 由按钮事件、录音 task、上传 task 在不同线程之间共享。
//
//   Idle      -> 默认；按下 -> Recording
//   Recording -> 录音 task 在跑；松手 -> Uploading
//   Uploading -> HTTP 进行中；完成 -> Idle（追加气泡 / 错误提示）
//   Closing   -> 屏幕正在卸载，录音 / 上传应尽快收尾
// ---------------------------------------------------------------------------
enum class State : uint8_t {
    Idle,
    Recording,
    Uploading,
    Closing,
};

std::atomic<State>    s_state{State::Idle};
std::atomic<int64_t>  s_record_start_us{0};
std::atomic<bool>     s_stop_requested{false};

// 屏幕静态引用。所有 UI 节点都挂在 s_screen 下，OnScreenUnloaded 里整个
// 屏幕被 LVGL 销毁，对应静态指针也要清掉，防止后台 task 继续往野指针上
// 调 lv_label_set_text。
lv_obj_t* s_screen      = nullptr;
lv_obj_t* s_msg_list    = nullptr;
lv_obj_t* s_empty_hint  = nullptr;
lv_obj_t* s_record_btn  = nullptr;
lv_obj_t* s_record_lbl  = nullptr;
lv_obj_t* s_status_lbl  = nullptr;
lv_timer_t* s_tick_timer = nullptr;
lv_timer_t* s_activation_guard_timer = nullptr;
lv_timer_t* s_auto_refresh_timer = nullptr;

// 录音 task 句柄；UNLOAD 时用来等它退出。
TaskHandle_t s_record_task = nullptr;

// 常驻后台 worker：避免每 3s 自动刷新都 xTaskCreate（8KB 栈）导致堆持续下降。
// 每次 HTTP 还会在 EspTcp 内再起 4KB 的 tcp_receive 任务，频繁建/删 task 易泄漏。
enum class WorkerJob : uint8_t {
    None = 0,
    FetchHistory,
    ClearAll,
};
TaskHandle_t          s_worker_task = nullptr;
std::atomic<bool>     s_worker_shutdown{false};
std::atomic<WorkerJob> s_worker_job{WorkerJob::None};
std::atomic<bool>     s_fetch_update_status{false};
std::atomic<uint32_t> s_worker_fetch_session{0};

// 历史会话拉取：session 递增使旧 task 的结果失效。
std::atomic<uint32_t> s_history_session{0};
std::atomic<bool>     s_history_loading{false};
std::atomic<bool>     s_service_available{false};
std::string           s_conversation_id;

struct RefreshSnapshot {
    bool valid = false;
    bool service_ok = false;
    bool bridge_online = false;
    bool gateway_online = false;
    std::string conversation_id;
    std::string messages_json;
};
RefreshSnapshot s_last_refresh_snapshot;

struct HistoryMessage {
    std::string text;
    bool is_user = true;
};

// 清空会话确认对话框
struct ClearDialogUi {
    lv_obj_t* mask = nullptr;
};
ClearDialogUi s_clear_dlg;

// 未激活拦截：全屏模态弹窗，不可关闭，仅能通过返回键离开。
struct ActivationBlockedDialogUi {
    lv_obj_t* mask = nullptr;
};
ActivationBlockedDialogUi s_activation_dlg;
bool s_activation_blocked = false;

const lv_font_t* chat_font() { return &font_puhui_30_4; }

std::string get_upload_url() {
    return api::Url(api::kOpenClawUpload);
}

std::string get_device_status_url() {
    return api::Url(api::kOpenClawDeviceStatus);
}

std::string get_conversation_list_url() {
    return api::Url(api::kOpenClawConversationList);
}

std::string get_messages_url(const std::string& conversation_id) {
    return api::OpenClawMessagesUrl(conversation_id);
}

std::string get_remove_all_url() {
    return api::Url(api::kOpenClawRemoveAll);
}

void close_clear_dialog();
void open_clear_confirm_dialog();
void on_swipe_back();
void trigger_fetch_history(bool update_status = true);
void ensure_openclaw_worker();
void stop_openclaw_worker();
bool submit_worker_job(WorkerJob job);
void execute_fetch_history(uint32_t session, bool update_status);
void execute_clear_all();

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
bool is_device_activated() {
    auto& app = Application::GetInstance();
    if (app.HasPendingActivation()) {
        return false;
    }
    if (app.GetDeviceState() == kDeviceStateActivating) {
        return false;
    }
    return true;
}

void log_activation_blocked() {
    auto& app = Application::GetInstance();
    ESP_LOGW(TAG, "OpenClaw blocked: device not activated");
    if (app.HasPendingActivation()) {
        ESP_LOGW(TAG, "pending activation code: %s",
                 app.GetPendingActivationCode().c_str());
    }
    if (app.GetDeviceState() == kDeviceStateActivating) {
        ESP_LOGW(TAG, "device state: activating");
    }
}

void open_activation_blocked_dialog() {
    if (s_screen == nullptr || s_activation_dlg.mask != nullptr) {
        return;
    }

    auto& app = Application::GetInstance();
    const bool has_code = app.HasPendingActivation();

    constexpr int32_t kCardW = 520;
    const int32_t kCardH = has_code ? 420 : 340;
    constexpr int32_t kBackBtnW = 200;
    constexpr int32_t kBackBtnH = 72;

    lv_obj_t* mask = lv_obj_create(s_screen);
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);
    s_activation_dlg.mask = mask;

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "设备未激活");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* desc = lv_label_create(card);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, kCardW - 56);
    lv_label_set_text(desc, "请先完成设备激活后再使用 OpenClaw。");
    lv_obj_set_style_text_color(desc, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, has_code ? -30 : -10);
    lv_obj_remove_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    if (has_code) {
        char code_buf[64];
        std::snprintf(code_buf, sizeof(code_buf), "验证码: %s",
                      app.GetPendingActivationCode().c_str());
        lv_obj_t* code_lbl = lv_label_create(card);
        lv_label_set_text(code_lbl, code_buf);
        lv_obj_set_style_text_color(code_lbl, lv_color_hex(0xFBBF24),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(code_lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_align(code_lbl, LV_ALIGN_BOTTOM_MID, 0, -(kBackBtnH + 24));
        lv_obj_remove_flag(code_lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* back = lv_button_create(card);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnW, kBackBtnH);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 16, LV_PART_MAIN);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(back,
                        [](lv_event_t* /*e*/) { on_swipe_back(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(back_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(back_lbl);
    lv_obj_remove_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void ensure_activation_blocked_dialog() {
    if (!s_activation_blocked) {
        return;
    }
    if (s_activation_dlg.mask == nullptr) {
        open_activation_blocked_dialog();
    }
}

void on_activation_guard_timer(lv_timer_t* /*timer*/) {
    ensure_activation_blocked_dialog();
}

// ---------------------------------------------------------------------------
// 工具（续）
// ---------------------------------------------------------------------------
bool is_screen_alive() { return s_screen != nullptr; }

const std::string& device_id_header() {
    static const std::string kId = SystemInfo::GetMacAddress();
    return kId;
}

// 给录音缓冲贴一个标准 RIFF/WAV 头。data_bytes 是纯 PCM 字节长度。
void fill_wav_header(uint8_t* hdr, uint32_t data_bytes) {
    const uint32_t sample_rate = kSampleRate;
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t byte_rate = sample_rate * channels * bits / 8;
    const uint16_t block_align = channels * bits / 8;
    const uint32_t riff_size = 36 + data_bytes;
    auto put32 = [](uint8_t* p, uint32_t v) {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
        p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
    };
    auto put16 = [](uint8_t* p, uint16_t v) {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    };
    std::memcpy(hdr + 0, "RIFF", 4);
    put32(hdr + 4, riff_size);
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    put32(hdr + 16, 16);                // fmt chunk size
    put16(hdr + 20, 1);                 // PCM
    put16(hdr + 22, channels);
    put32(hdr + 24, sample_rate);
    put32(hdr + 28, byte_rate);
    put16(hdr + 32, block_align);
    put16(hdr + 34, bits);
    std::memcpy(hdr + 36, "data", 4);
    put32(hdr + 40, data_bytes);
}

// 双声道输入（mic + 参考）时 ReadAudioData 返回 L/R 交错数据，只保留左声道
void keep_left_channel_only(std::vector<int16_t>& samples) {
    if (samples.size() < 2) {
        return;
    }
    const size_t mono_count = samples.size() / 2;
    for (size_t i = 0, j = 0; i < mono_count; ++i, j += 2) {
        samples[i] = samples[j];
    }
    samples.resize(mono_count);
}

struct UploadResult {
    bool ok = false;
    int  status = 0;
    std::string text;
    std::string conversation_id;
    std::string err;
};

// 解析 /upload 响应：200 + JSON code==0，或纯文本 "OK ..."
bool parse_upload_response(int status, const std::string& body,
                           UploadResult& out) {
    out.status = status;
    if (status != 200) {
        out.err = body.empty() ? ("HTTP " + std::to_string(status)) : body;
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root != nullptr) {
        cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (cJSON_IsNumber(code) && code->valueint == 0) {
            cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsObject(data)) {
                cJSON* text =
                    cJSON_GetObjectItemCaseSensitive(data, "text");
                if (cJSON_IsString(text) && text->valuestring != nullptr &&
                    text->valuestring[0] != '\0') {
                    out.text = text->valuestring;
                }
                cJSON* conv_id =
                    cJSON_GetObjectItemCaseSensitive(data, "conversationId");
                if (cJSON_IsString(conv_id) && conv_id->valuestring != nullptr &&
                    conv_id->valuestring[0] != '\0') {
                    out.conversation_id = conv_id->valuestring;
                }
            }
            out.ok = true;
            cJSON_Delete(root);
            return true;
        }
        cJSON_Delete(root);
    }

    size_t b = 0, e = body.size();
    while (b < e && (body[b] == ' ' || body[b] == '\r' || body[b] == '\n' ||
                     body[b] == '\t')) {
        ++b;
    }
    while (e > b && (body[e - 1] == ' ' || body[e - 1] == '\r' ||
                     body[e - 1] == '\n' || body[e - 1] == '\t')) {
        --e;
    }
    const std::string trimmed = body.substr(b, e - b);
    if (trimmed.size() >= 3 && trimmed.compare(0, 3, "OK ") == 0) {
        out.ok = true;
        return true;
    }

    out.err = trimmed.empty() ? "empty response" : trimmed;
    return false;
}

// ---------------------------------------------------------------------------
// 消息气泡（user=右，assistant/其它=左）
// ---------------------------------------------------------------------------
void clear_messages();
void update_empty_hint() {
    if (s_empty_hint == nullptr || s_msg_list == nullptr) return;
    if (lv_obj_get_child_count(s_msg_list) == 0) {
        lv_obj_remove_flag(s_empty_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void scroll_to_latest() {
    if (s_msg_list == nullptr) return;
    const uint32_t count = lv_obj_get_child_count(s_msg_list);
    if (count == 0) return;
    lv_obj_t* latest = lv_obj_get_child(s_msg_list, count - 1);
    if (latest != nullptr) {
        lv_obj_scroll_to_view_recursive(latest, LV_ANIM_ON);
    }
}

void trim_old_msgs() {
    if (s_msg_list == nullptr) return;
    while (static_cast<int32_t>(lv_obj_get_child_count(s_msg_list)) >
           kMaxMessages) {
        lv_obj_t* oldest = lv_obj_get_child(s_msg_list, 0);
        if (oldest == nullptr) break;
        lv_obj_delete(oldest);
    }
}

void add_bubble(const char* text, bool is_user) {
    if (s_msg_list == nullptr || text == nullptr || text[0] == '\0') return;

    const lv_font_t* font = chat_font();
    const uint32_t bubble_bg =
        is_user ? kColorRightBubble : kColorLeftBubble;
    const uint32_t text_color =
        is_user ? kColorBubbleText : kColorLeftBubbleText;

    lv_obj_t* row = lv_obj_create(s_msg_list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, kRowGap, LV_PART_MAIN);

    lv_obj_t* bubble = lv_obj_create(row);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, kBubbleRadius, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bubble, kBubblePadX, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, kBubblePadY, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t max_bubble_w = kPanelW * 72 / 100;
    int32_t text_w = lv_txt_get_width(text, std::strlen(text), font, 0);
    if (text_w < 24) text_w = 24;
    int32_t bubble_w = text_w + kBubblePadX * 2;
    if (bubble_w > max_bubble_w) bubble_w = max_bubble_w;
    lv_obj_set_width(bubble, bubble_w);

    lv_obj_set_style_bg_color(bubble, lv_color_hex(bubble_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    if (is_user) {
        lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, -kSideMargin, 0);
    } else {
        lv_obj_align(bubble, LV_ALIGN_TOP_LEFT, kSideMargin, 0);
    }

    lv_obj_t* label = lv_label_create(bubble);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, bubble_w - kBubblePadX * 2);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    lv_obj_update_layout(label);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    screen_make_input_passive(row);
}

void finalize_message_list_update() {
    trim_old_msgs();
    update_empty_hint();
    if (s_msg_list != nullptr) {
        lv_obj_update_layout(s_msg_list);
    }
    scroll_to_latest();
}

void add_right_bubble(const char* text) {
    add_bubble(text, true);
    finalize_message_list_update();
}

// 在「非 LVGL 线程」里安全地往屏幕上加气泡 / 改文字。esp_lv_adapter_lock
// 是项目里统一的 LVGL 互斥锁包装。
void post_bubble_from_worker(const std::string& text) {
    if (text.empty()) return;
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;
    if (is_screen_alive()) {
        add_right_bubble(text.c_str());
    }
    esp_lv_adapter_unlock();
}

void post_status_from_worker(const char* text, uint32_t color) {
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;
    if (is_screen_alive() && s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, text);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(color),
                                    LV_PART_MAIN);
    }
    esp_lv_adapter_unlock();
}

// ---------------------------------------------------------------------------
// HTTP 与会话 API
// ---------------------------------------------------------------------------
struct HttpGetResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string err;
};

HttpGetResult http_get_json(const std::string& url) {
    HttpGetResult r;
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        r.err = "no network";
        return r;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        r.err = "create http failed";
        return r;
    }
    http->SetHeader("Connection", "close");
    http->SetHeader("Accept", "application/json");
    http->SetHeader("X-Device-Id", device_id_header());
    if (!http->Open("GET", url)) {
        r.err = "open failed";
        http->Close();
        return r;
    }
    r.status = http->GetStatusCode();
    if (r.status != 200) {
        r.err = "status " + std::to_string(r.status);
        (void)http->ReadAll();
        http->Close();
        return r;
    }
    r.body = http->ReadAll();
    http->Close();
    r.ok = !r.body.empty();
    if (!r.ok) {
        r.err = "empty body";
    }
    return r;
}

bool parse_api_code_200(const cJSON* root) {
    if (root == nullptr) {
        return false;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    return cJSON_IsNumber(code) && code->valueint == 200;
}

struct DeviceStatusResult {
    bool ok = false;
    bool bridge_online = false;
    bool gateway_online = false;
    std::string err;
};

struct ConversationListResult {
    bool ok = false;
    int total = 0;
    std::string conversation_id;
    std::string err;
};

DeviceStatusResult fetch_device_status() {
    DeviceStatusResult r;
    const std::string url = get_device_status_url();
    ESP_LOGI(TAG, "fetch device status");

    HttpGetResult http_res = http_get_json(url);
    if (!http_res.ok) {
        r.err = http_res.err;
        return r;
    }

    cJSON* root = cJSON_Parse(http_res.body.c_str());
    if (root == nullptr || !parse_api_code_200(root)) {
        r.err = "invalid status response";
        cJSON_Delete(root);
        return r;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        cJSON* bridge =
            cJSON_GetObjectItemCaseSensitive(data, "bridgeOnline");
        cJSON* gateway =
            cJSON_GetObjectItemCaseSensitive(data, "gatewayOnline");
        r.bridge_online = cJSON_IsTrue(bridge);
        r.gateway_online = cJSON_IsTrue(gateway);
        r.ok = true;
    } else {
        r.err = "missing data";
    }
    cJSON_Delete(root);
    return r;
}

ConversationListResult fetch_conversation_list() {
    ConversationListResult r;
    const std::string url = get_conversation_list_url();
    ESP_LOGI(TAG, "fetch conversation list");

    HttpGetResult http_res = http_get_json(url);
    if (!http_res.ok) {
        r.err = http_res.err;
        return r;
    }

    cJSON* root = cJSON_Parse(http_res.body.c_str());
    if (root == nullptr || !parse_api_code_200(root)) {
        r.err = "invalid conversation list response";
        cJSON_Delete(root);
        return r;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* total_item = data ? cJSON_GetObjectItemCaseSensitive(data, "total")
                             : nullptr;
    cJSON* records = data ? cJSON_GetObjectItemCaseSensitive(data, "records")
                          : nullptr;
    if (!cJSON_IsObject(data) || !cJSON_IsNumber(total_item) ||
        !cJSON_IsArray(records)) {
        r.err = "missing conversation list data";
        cJSON_Delete(root);
        return r;
    }

    r.total = total_item->valueint;
    if (r.total >= 1 && cJSON_GetArraySize(records) > 0) {
        const int record_count = cJSON_GetArraySize(records);
        const int pick_idx = (r.total > 1) ? (record_count - 1) : 0;
        cJSON* picked = cJSON_GetArrayItem(records, pick_idx);
        cJSON* conv_id =
            picked ? cJSON_GetObjectItemCaseSensitive(picked, "conversationId")
                   : nullptr;
        if (cJSON_IsString(conv_id) && conv_id->valuestring != nullptr &&
            conv_id->valuestring[0] != '\0') {
            r.conversation_id = conv_id->valuestring;
        }
    }
    r.ok = true;
    cJSON_Delete(root);
    return r;
}

bool parse_messages_json(const std::string& body,
                         std::vector<HistoryMessage>& out) {
    out.clear();
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr || !parse_api_code_200(root)) {
        cJSON_Delete(root);
        return false;
    }

    bool ok = false;
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* records = data ? cJSON_GetObjectItemCaseSensitive(data, "records")
                          : nullptr;
    if (cJSON_IsArray(records)) {
        const int count = cJSON_GetArraySize(records);
        for (int i = 0; i < count; ++i) {
            cJSON* item = cJSON_GetArrayItem(records, i);
            if (item == nullptr) {
                continue;
            }
            cJSON* content_item =
                cJSON_GetObjectItemCaseSensitive(item, "content");
            cJSON* role_item =
                cJSON_GetObjectItemCaseSensitive(item, "role");
            if (!cJSON_IsString(content_item) ||
                content_item->valuestring == nullptr ||
                content_item->valuestring[0] == '\0') {
                continue;
            }
            HistoryMessage msg;
            msg.text = content_item->valuestring;
            msg.is_user =
                cJSON_IsString(role_item) &&
                role_item->valuestring != nullptr &&
                std::strcmp(role_item->valuestring, "user") == 0;
            out.push_back(std::move(msg));
        }
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

bool fetch_messages_body(const std::string& conversation_id,
                         std::string& raw_body,
                         std::string& err) {
    raw_body.clear();
    if (conversation_id.empty()) {
        return true;
    }
    const std::string url = get_messages_url(conversation_id);
    ESP_LOGI(TAG, "fetch messages");

    HttpGetResult http_res = http_get_json(url);
    if (!http_res.ok) {
        err = http_res.err;
        return false;
    }
    raw_body = std::move(http_res.body);
    return true;
}

bool fetch_messages(const std::string& conversation_id,
                    std::vector<HistoryMessage>& out,
                    std::string& raw_body,
                    std::string& err) {
    out.clear();
    if (!fetch_messages_body(conversation_id, raw_body, err)) {
        return false;
    }
    if (raw_body.empty()) {
        return true;
    }
    if (!parse_messages_json(raw_body, out)) {
        err = "parse failed";
        return false;
    }
    return true;
}

bool refresh_snapshot_unchanged(bool service_ok, bool bridge_online,
                                bool gateway_online,
                                const std::string& conversation_id,
                                const std::string& messages_json) {
    if (!s_last_refresh_snapshot.valid) {
        return false;
    }
    return s_last_refresh_snapshot.service_ok == service_ok &&
           s_last_refresh_snapshot.bridge_online == bridge_online &&
           s_last_refresh_snapshot.gateway_online == gateway_online &&
           s_last_refresh_snapshot.conversation_id == conversation_id &&
           s_last_refresh_snapshot.messages_json == messages_json;
}

void save_refresh_snapshot(bool service_ok, bool bridge_online,
                           bool gateway_online,
                           const std::string& conversation_id,
                           const std::string& messages_json) {
    s_last_refresh_snapshot.valid = true;
    s_last_refresh_snapshot.service_ok = service_ok;
    s_last_refresh_snapshot.bridge_online = bridge_online;
    s_last_refresh_snapshot.gateway_online = gateway_online;
    s_last_refresh_snapshot.conversation_id = conversation_id;
    s_last_refresh_snapshot.messages_json = messages_json;
}

void invalidate_refresh_snapshot() {
    s_last_refresh_snapshot = RefreshSnapshot{};
}

void apply_history_locked(const std::vector<HistoryMessage>& messages) {
    clear_messages();
    for (const auto& msg : messages) {
        add_bubble(msg.text.c_str(), msg.is_user);
    }
    finalize_message_list_update();
}

std::string build_service_unavailable_message(bool bridge_online,
                                              bool gateway_online) {
    if (!bridge_online && !gateway_online) {
        return "龙虾插件不在线，龙虾不在线";
    }
    if (!bridge_online) {
        return "龙虾插件不在线";
    }
    if (!gateway_online) {
        return "龙虾不在线";
    }
    return "";
}

void execute_fetch_history(uint32_t session, bool /*update_status*/) {
    DeviceStatusResult status_res = fetch_device_status();
    ConversationListResult list_res;
    std::vector<HistoryMessage> messages;
    bool messages_ok = false;
    std::string messages_err;
    std::string messages_json;
    std::string conversation_id;
    bool service_ok = false;
    std::string status_msg;
    const bool bridge_online =
        status_res.ok && status_res.bridge_online;
    const bool gateway_online =
        status_res.ok && status_res.gateway_online;

    if (status_res.ok && status_res.bridge_online && status_res.gateway_online) {
        service_ok = true;
        list_res = fetch_conversation_list();
        if (list_res.ok) {
            conversation_id = list_res.conversation_id;
            messages_ok = fetch_messages_body(conversation_id, messages_json,
                                              messages_err);
        }
    } else if (status_res.ok) {
        status_msg = build_service_unavailable_message(
            status_res.bridge_online, status_res.gateway_online);
    } else {
        status_msg = "检查在线状态失败: " + status_res.err;
    }

    s_history_loading.store(false);

    if (session != s_history_session.load(std::memory_order_relaxed)) {
        return;
    }

    s_service_available.store(service_ok);
    if (service_ok) {
        s_conversation_id = conversation_id;
    }

    const bool data_ready =
        service_ok && list_res.ok && messages_ok;
    if (data_ready &&
        refresh_snapshot_unchanged(service_ok, bridge_online, gateway_online,
                                 conversation_id, messages_json)) {
        ESP_LOGI(TAG, "refresh skipped: data unchanged");
        return;
    }

    if (data_ready && !messages_json.empty() &&
        !parse_messages_json(messages_json, messages)) {
        messages_ok = false;
        messages_err = "parse failed";
    }

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (is_screen_alive()) {
        if (service_ok) {
            if (list_res.ok) {
                if (messages_ok) {
                    apply_history_locked(messages);
                    save_refresh_snapshot(service_ok, bridge_online,
                                          gateway_online, conversation_id,
                                          messages_json);
                    if (s_status_lbl != nullptr) {
                        if (messages.empty()) {
                            lv_label_set_text(s_status_lbl, "按住说话");
                        } else {
                            char buf[64];
                            std::snprintf(buf, sizeof(buf), "已加载 %u 条消息",
                                          static_cast<unsigned>(messages.size()));
                            lv_label_set_text(s_status_lbl, buf);
                        }
                        lv_obj_set_style_text_color(
                            s_status_lbl, lv_color_hex(kColorHintText),
                            LV_PART_MAIN);
                    }
                    ESP_LOGI(TAG, "messages loaded: %u",
                             static_cast<unsigned>(messages.size()));
                } else {
                    if (s_status_lbl != nullptr) {
                        char buf[80];
                        std::snprintf(buf, sizeof(buf), "加载消息失败: %s",
                                      messages_err.c_str());
                        lv_label_set_text(s_status_lbl, buf);
                        lv_obj_set_style_text_color(
                            s_status_lbl, lv_color_hex(kColorErrorText),
                            LV_PART_MAIN);
                    }
                    ESP_LOGW(TAG, "fetch messages failed: %s",
                             messages_err.c_str());
                }
            } else {
                if (s_status_lbl != nullptr) {
                    char buf[80];
                    std::snprintf(buf, sizeof(buf), "加载会话失败: %s",
                                  list_res.err.c_str());
                    lv_label_set_text(s_status_lbl, buf);
                    lv_obj_set_style_text_color(
                        s_status_lbl, lv_color_hex(kColorErrorText),
                        LV_PART_MAIN);
                }
            }
            if (s_record_btn != nullptr) {
                lv_obj_set_style_bg_color(s_record_btn,
                                          lv_color_hex(kColorRecordBtnIdle),
                                          LV_PART_MAIN);
                lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            }
        } else {
            if (s_status_lbl != nullptr) {
                lv_label_set_text(s_status_lbl, status_msg.c_str());
                lv_obj_set_style_text_color(s_status_lbl,
                                            lv_color_hex(kColorErrorText),
                                            LV_PART_MAIN);
            }
            save_refresh_snapshot(false, bridge_online, gateway_online, "",
                                  "");
            if (s_record_btn != nullptr) {
                lv_obj_remove_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_bg_color(s_record_btn,
                                          lv_color_hex(kColorRecordBtnBusy),
                                          LV_PART_MAIN);
            }
            ESP_LOGW(TAG, "service unavailable: %s", status_msg.c_str());
        }
    }
    esp_lv_adapter_unlock();
}

void openclaw_worker_task(void* /*arg*/) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_worker_shutdown.load(std::memory_order_acquire)) {
            break;
        }

        const WorkerJob job = s_worker_job.exchange(
            WorkerJob::None, std::memory_order_acq_rel);
        if (job == WorkerJob::FetchHistory) {
            const uint32_t session = s_worker_fetch_session.load(
                std::memory_order_relaxed);
            const bool update_status = s_fetch_update_status.load(
                std::memory_order_relaxed);
            (void)update_status;
            execute_fetch_history(session, update_status);
        } else if (job == WorkerJob::ClearAll) {
            execute_clear_all();
        }

        if (s_worker_shutdown.load(std::memory_order_acquire)) {
            break;
        }
    }
    s_worker_task = nullptr;
    vTaskDelete(nullptr);
}

void ensure_openclaw_worker() {
    if (s_worker_shutdown.load(std::memory_order_acquire) ||
        s_worker_task != nullptr) {
        return;
    }
    BaseType_t ok = xTaskCreate(openclaw_worker_task, "openclaw_work", 8192,
                                nullptr, 4, &s_worker_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create openclaw worker failed");
        s_worker_task = nullptr;
    }
}

void stop_openclaw_worker() {
    s_worker_shutdown.store(true, std::memory_order_release);
    TaskHandle_t worker = s_worker_task;
    if (worker != nullptr) {
        xTaskNotifyGive(worker);
        for (int i = 0; i < 50 && s_worker_task != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

bool submit_worker_job(WorkerJob job) {
    ensure_openclaw_worker();
    if (s_worker_task == nullptr) {
        return false;
    }
    s_worker_job.store(job, std::memory_order_release);
    xTaskNotifyGive(s_worker_task);
    return true;
}

void trigger_fetch_history(bool update_status) {
    if (!is_screen_alive() || s_state.load() == State::Closing) {
        return;
    }
    if (s_activation_blocked) {
        ESP_LOGW(TAG, "fetch history skipped: device not activated");
        return;
    }
    if (s_history_loading.exchange(true)) {
        return;
    }

    const uint32_t session =
        s_history_session.fetch_add(1, std::memory_order_relaxed) + 1;
    s_worker_fetch_session.store(session, std::memory_order_relaxed);
    s_fetch_update_status.store(update_status, std::memory_order_relaxed);

    if (update_status && s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, "正在检查龙虾状态…");
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
    }

    if (!submit_worker_job(WorkerJob::FetchHistory)) {
        s_history_loading.store(false);
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, "无法启动加载任务");
            lv_obj_set_style_text_color(s_status_lbl,
                                        lv_color_hex(kColorErrorText),
                                        LV_PART_MAIN);
        }
    }
}

void on_auto_refresh_timer(lv_timer_t* /*t*/) {
    if (!is_screen_alive() || s_activation_blocked) {
        return;
    }
    if (s_state.load() != State::Idle) {
        return;
    }
    trigger_fetch_history(false);
}

void start_auto_refresh_timer() {
    if (s_auto_refresh_timer != nullptr) {
        return;
    }
    s_auto_refresh_timer =
        lv_timer_create(on_auto_refresh_timer, kRefreshIntervalMs, nullptr);
}

void stop_auto_refresh_timer() {
    if (s_auto_refresh_timer != nullptr) {
        lv_timer_delete(s_auto_refresh_timer);
        s_auto_refresh_timer = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 清空全部会话 GET /api/v1/conversation/removeAll
// ---------------------------------------------------------------------------
struct HttpDeleteResult {
    bool ok = false;
    int status = 0;
    std::string err;
    std::string body;
};

HttpDeleteResult http_remove_all_conversations() {
    HttpDeleteResult r;
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        r.err = "no network";
        return r;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        r.err = "create http failed";
        return r;
    }

    const std::string url = get_remove_all_url();
    http->SetHeader("Accept", "application/json");
    http->SetHeader("X-Device-Id", device_id_header());
    if (!http->Open("GET", url)) {
        r.err = "open failed";
        http->Close();
        return r;
    }

    r.status = http->GetStatusCode();
    r.body = http->ReadAll();
    http->Close();
    if (r.status != 200) {
        r.err = r.body.empty() ? ("status " + std::to_string(r.status)) : r.body;
        return r;
    }

    cJSON* root = cJSON_Parse(r.body.c_str());
    if (root != nullptr) {
        if (parse_api_code_200(root)) {
            r.ok = true;
            cJSON_Delete(root);
            return r;
        }
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(msg) && msg->valuestring != nullptr &&
            msg->valuestring[0] != '\0') {
            r.err = msg->valuestring;
        } else {
            r.err = "code != 200";
        }
        cJSON_Delete(root);
        return r;
    }

    r.ok = true;
    return r;
}

void execute_clear_all() {
    const std::string url = get_remove_all_url();
    ESP_LOGI(TAG, "remove all conversations");

    HttpDeleteResult http_res = http_remove_all_conversations();

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (is_screen_alive()) {
        if (http_res.ok) {
            s_conversation_id.clear();
            clear_messages();
            save_refresh_snapshot(true, true, true, "", "");
            if (s_status_lbl != nullptr) {
                lv_label_set_text(s_status_lbl, "已清空全部会话");
                lv_obj_set_style_text_color(s_status_lbl,
                                            lv_color_hex(kColorHintText),
                                            LV_PART_MAIN);
            }
            ESP_LOGI(TAG, "remove all conversations ok");
        } else if (s_status_lbl != nullptr) {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "清空失败: %s",
                          http_res.err.c_str());
            lv_label_set_text(s_status_lbl, buf);
            lv_obj_set_style_text_color(s_status_lbl,
                                        lv_color_hex(kColorErrorText),
                                        LV_PART_MAIN);
            const std::string safe_body =
                api::RedactClawUrlsForLog(http_res.body);
            ESP_LOGW(TAG, "remove all conversations failed: status=%d err=%s body=%s",
                     http_res.status, http_res.err.c_str(),
                     safe_body.c_str());
        }
    }
    esp_lv_adapter_unlock();
}

void trigger_clear_all() {
    if (!is_screen_alive() || s_state.load() == State::Closing) {
        return;
    }
    if (!s_service_available.load()) {
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, "龙虾服务不可用");
            lv_obj_set_style_text_color(s_status_lbl,
                                        lv_color_hex(kColorErrorText),
                                        LV_PART_MAIN);
        }
        return;
    }

    s_history_session.fetch_add(1, std::memory_order_relaxed);

    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, "正在清空…");
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
    }

    if (!submit_worker_job(WorkerJob::ClearAll)) {
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, "无法启动清空任务");
            lv_obj_set_style_text_color(s_status_lbl,
                                        lv_color_hex(kColorErrorText),
                                        LV_PART_MAIN);
        }
    }
}

// ---------------------------------------------------------------------------
// 录音按钮 UI 同步
//
// 任何状态切换都通过这一函数刷新底部按钮的颜色、文字和可点击性。
// 调用前需要确保已经持有 LVGL 锁（按钮 / lv_timer / 录音 task 三类入口都
// 满足这一点，分别由：lvgl 主线程触发、lv_timer 回调天然在锁内、worker
// task 经 post_status_from_worker 上锁后再调用）。
// ---------------------------------------------------------------------------
void update_button_ui_locked(State st) {
    if (s_record_btn == nullptr || s_record_lbl == nullptr) return;
    switch (st) {
        case State::Idle:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(
                                          s_service_available.load()
                                              ? kColorRecordBtnIdle
                                              : kColorRecordBtnBusy),
                                      LV_PART_MAIN);
            lv_label_set_text(s_record_lbl, "按住说话");
            if (s_service_available.load()) {
                lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_remove_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            }
            break;
        case State::Recording:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(kColorRecordBtnActive),
                                      LV_PART_MAIN);
            lv_label_set_text(s_record_lbl, "已录 0.0 秒");
            lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            break;
        case State::Uploading:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(kColorRecordBtnBusy),
                                      LV_PART_MAIN);
            lv_label_set_text(s_record_lbl, "上传中...");
            lv_obj_remove_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            break;
        case State::Closing:
            break;
    }
}

// LVGL 主线程里跑的 100ms 周期 timer，专门用来更新「已录 X.X 秒」。
// 录音 task 自己不动 UI 是为了避免锁竞争，秒数显示也不会卡。
void tick_timer_cb(lv_timer_t* /*t*/) {
    if (s_state.load() != State::Recording) return;
    if (s_record_lbl == nullptr) return;
    const int64_t start = s_record_start_us.load();
    if (start <= 0) return;
    const int64_t now = esp_timer_get_time();
    const int ms = static_cast<int>((now - start) / 1000);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "已录 %d.%d 秒", ms / 1000,
                  (ms / 100) % 10);
    lv_label_set_text(s_record_lbl, buf);
}

// ---------------------------------------------------------------------------
// HTTP 上传
//
// POST 原始 WAV 字节（Content-Type: audio/wav），与 express.raw() 后端对齐。
UploadResult upload_wav(const uint8_t* wav_header, size_t header_size,
                        const uint8_t* pcm, size_t pcm_size,
                        const std::string& conversation_id) {
    UploadResult r;
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        r.err = "no network";
        return r;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        r.err = "create http failed";
        return r;
    }

    std::string wav_body;
    wav_body.reserve(header_size + pcm_size);
    wav_body.assign(reinterpret_cast<const char*>(wav_header), header_size);
    wav_body.append(reinterpret_cast<const char*>(pcm), pcm_size);

    http->SetContent(std::move(wav_body));
    http->SetHeader("Content-Type", "audio/wav");
    http->SetHeader("Connection", "close");
    http->SetHeader("X-Device-Id", device_id_header());
    if (!conversation_id.empty()) {
        http->SetHeader("conversationId", conversation_id.c_str());
    }

    const std::string upload_url = get_upload_url();
    if (!http->Open("POST", upload_url)) {
        r.err = "open failed";
        return r;
    }

    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();
    http->Close();
    parse_upload_response(status, body, r);
    return r;
}

// ---------------------------------------------------------------------------
// 录音 + 上传 worker
//
// 1) 关掉 wake word，等 AudioInputTask 让出 mic
// 2) 循环 ReadAudioData 直到 s_stop_requested 或者达到 60s 上限
// 3) 还原 wake word
// 4) 录音过短 -> 提示丢弃；否则 -> 拼 WAV + 上传
// 5) 上传成功 -> add right bubble；失败 -> status_lbl 显示错误
//
// 退出条件：录音/上传都跑完一遍才退出；屏幕被卸载时通过 s_stop_requested
// 让录音循环尽快结束，上传部分一旦发起就会跑完（不强行中断 HTTP）。
// ---------------------------------------------------------------------------
void record_and_upload_task(void* /*arg*/) {
    auto& app = Application::GetInstance();
    auto& as = app.GetAudioService();

    // 这一次操作期间是否由我们关掉了 wake word；只在我们关掉的情况下负
    // 责恢复，避免误关用户已经在别处 disable 的开关。
    bool wake_disabled_by_us = false;
    if (as.IsWakeWordRunning()) {
        as.EnableWakeWordDetection(false);
        wake_disabled_by_us = true;
        // 等 AudioInputTask 走完当前 ReadAudioData 迭代并回到事件 wait。
        // 最坏情况 wake word feed_size 是 60ms 一帧，留 100ms 余量足够。
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // PSRAM 上分配一次性大缓冲。最坏情况 60s = 1.92MB。
    uint8_t* buffer = static_cast<uint8_t*>(
        heap_caps_malloc(kMaxRecordBytes, MALLOC_CAP_SPIRAM));
    if (buffer == nullptr) {
        // 没 PSRAM 兜底到普通堆，长度上限自动被堆大小限制
        buffer = static_cast<uint8_t*>(heap_caps_malloc(
            kMaxRecordBytes / 4, MALLOC_CAP_8BIT));
    }
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "no memory for record buffer");
        post_status_from_worker("内存不足", kColorHintText);
        if (wake_disabled_by_us) as.EnableWakeWordDetection(true);
        s_state.store(State::Idle);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (is_screen_alive()) update_button_ui_locked(State::Idle);
            esp_lv_adapter_unlock();
        }
        s_record_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    size_t written = 0;
    std::vector<int16_t> frame;
    frame.reserve(kSamplesPerFrame * 2);

    const int64_t start_us = esp_timer_get_time();
    s_record_start_us.store(start_us);

    while (!s_stop_requested.load()) {
        if (!as.ReadAudioData(frame, kSampleRate, kSamplesPerFrame)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        AudioCodec* codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr && codec->input_channels() == 2) {
            keep_left_channel_only(frame);
        }
        const size_t bytes = frame.size() * sizeof(int16_t);
        if (written + bytes > kMaxRecordBytes) {
            ESP_LOGW(TAG, "record buffer full, force stop");
            break;
        }
        std::memcpy(buffer + written, frame.data(), bytes);
        written += bytes;
        if ((esp_timer_get_time() - start_us) / 1000 >=
            kMaxRecordSeconds * 1000) {
            break;
        }
    }
    const int64_t end_us = esp_timer_get_time();
    const int duration_ms = static_cast<int>((end_us - start_us) / 1000);

    if (wake_disabled_by_us) {
        as.EnableWakeWordDetection(true);
    }

    // 录音过短 -> 提示并丢弃
    if (duration_ms < kMinRecordMs || written < 1024) {
        ESP_LOGI(TAG, "discard short recording: %d ms / %u bytes",
                 duration_ms, static_cast<unsigned>(written));
        post_status_from_worker("录音太短，再试一次", kColorHintText);
        heap_caps_free(buffer);
        s_state.store(State::Idle);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (is_screen_alive()) update_button_ui_locked(State::Idle);
            esp_lv_adapter_unlock();
        }
        s_record_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // 切到 Uploading 状态，刷新按钮
    s_state.store(State::Uploading);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (is_screen_alive()) update_button_ui_locked(State::Uploading);
        esp_lv_adapter_unlock();
    }
    char hint[64];
    std::snprintf(hint, sizeof(hint), "上传中… (%.1fs)", duration_ms / 1000.0f);
    post_status_from_worker(hint, kColorHintText);

    uint8_t wav_header[44];
    fill_wav_header(wav_header, static_cast<uint32_t>(written));
    ESP_LOGI(TAG, "uploading WAV %u bytes (%d ms)",
             static_cast<unsigned>(written + sizeof(wav_header)), duration_ms);

    UploadResult res =
        upload_wav(wav_header, sizeof(wav_header), buffer, written,
                   s_conversation_id);
    heap_caps_free(buffer);

    if (res.ok) {
        if (!res.conversation_id.empty()) {
            s_conversation_id = res.conversation_id;
        }
        ESP_LOGI(TAG, "upload ok, asr=%s conv=%s", res.text.c_str(),
                 s_conversation_id.c_str());

        std::vector<HistoryMessage> messages;
        std::string messages_json;
        std::string fetch_err;
        const bool refreshed = fetch_messages(s_conversation_id, messages,
                                              messages_json, fetch_err);
        if (refreshed) {
            const bool unchanged = refresh_snapshot_unchanged(
                s_service_available.load(), true, true, s_conversation_id,
                messages_json);
            if (!unchanged) {
                if (esp_lv_adapter_lock(-1) == ESP_OK) {
                    if (is_screen_alive()) {
                        apply_history_locked(messages);
                    }
                    esp_lv_adapter_unlock();
                }
                save_refresh_snapshot(s_service_available.load(), true, true,
                                      s_conversation_id, messages_json);
            }
            post_status_from_worker("按住说话", kColorHintText);
        } else {
            ESP_LOGW(TAG, "refresh messages failed: %s", fetch_err.c_str());
            post_status_from_worker("刷新消息失败", kColorErrorText);
        }
    } else {
        ESP_LOGW(TAG, "upload failed: %s", res.err.c_str());
        std::string msg = "上传失败: " + res.err;
        post_status_from_worker(msg.c_str(), kColorErrorText);
    }

    s_state.store(State::Idle);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (is_screen_alive()) update_button_ui_locked(State::Idle);
        esp_lv_adapter_unlock();
    }
    s_record_task = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// 按钮事件
// ---------------------------------------------------------------------------
void on_record_pressed(lv_event_t* /*e*/) {
    if (s_activation_blocked) {
        ESP_LOGW(TAG, "record ignored: device not activated");
        ensure_activation_blocked_dialog();
        return;
    }
    if (!s_service_available.load()) {
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, "龙虾服务不可用");
            lv_obj_set_style_text_color(s_status_lbl,
                                        lv_color_hex(kColorErrorText),
                                        LV_PART_MAIN);
        }
        return;
    }
    if (s_state.load() != State::Idle) return;
    // 拒绝那些会真正抢占 mic / 不该打扰的状态：
    //   - Connecting / Listening / Speaking：正在和 AI 对话，抢走 mic 会
    //     破坏会话
    //   - Upgrading：固件升级中，不能动音频外设
    // 其它状态（包括 Activating「等待激活码输入」、Idle、Unknown/Starting
    // 等过渡态）允许直接录音上传，不打扰用户。
    auto& app = Application::GetInstance();
    const DeviceState ds = app.GetDeviceState();
    if (ds == kDeviceStateConnecting || ds == kDeviceStateListening ||
        ds == kDeviceStateSpeaking || ds == kDeviceStateUpgrading) {
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, "请先结束当前对话");
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorErrorText),
                                        LV_PART_MAIN);
        }
        return;
    }

    s_stop_requested.store(false);
    s_state.store(State::Recording);
    update_button_ui_locked(State::Recording);
    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, "正在录音…松开结束");
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
    }

    // 启动 worker。栈 8KB（HTTP 客户端栈占用较大），优先级 5。
    BaseType_t ok = xTaskCreate(record_and_upload_task, "openclaw_rec",
                                8192, nullptr, 5, &s_record_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create record task failed");
        s_state.store(State::Idle);
        update_button_ui_locked(State::Idle);
    }
}

void on_record_released(lv_event_t* /*e*/) {
    // 把停止信号交给 worker；UI 立刻显示「上传中」，worker 真正收尾后还
    // 会再刷一次。
    if (s_state.load() == State::Recording) {
        s_stop_requested.store(true);
    }
}

// ---------------------------------------------------------------------------
// 屏幕导航 / 生命周期
// ---------------------------------------------------------------------------
void clear_messages() {
    if (s_msg_list == nullptr) return;
    const uint32_t count = lv_obj_get_child_count(s_msg_list);
    for (int32_t i = static_cast<int32_t>(count) - 1; i >= 0; --i) {
        lv_obj_delete(lv_obj_get_child(s_msg_list, i));
    }
    update_empty_hint();
}

void on_clear_clicked(lv_event_t* /*e*/) {
    if (s_activation_blocked) {
        return;
    }
    open_clear_confirm_dialog();
}

// ---------------------------------------------------------------------------
// 清空全部会话确认对话框
// ---------------------------------------------------------------------------
void on_clear_dialog_mask_clicked(lv_event_t* e) {
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    close_clear_dialog();
}

void on_clear_cancel_clicked(lv_event_t* /*e*/) { close_clear_dialog(); }

void on_clear_confirm_clicked(lv_event_t* /*e*/) {
    close_clear_dialog();
    trigger_clear_all();
}

void close_clear_dialog() {
    if (s_clear_dlg.mask != nullptr) {
        lv_obj_delete(s_clear_dlg.mask);
    }
    s_clear_dlg = ClearDialogUi{};
}

void open_clear_confirm_dialog() {
    if (s_screen == nullptr || s_clear_dlg.mask != nullptr) {
        return;
    }

    constexpr int32_t kCardW = 480;
    constexpr int32_t kCardH = 280;
    constexpr int32_t kBtnW = 200;
    constexpr int32_t kBtnH = 80;

    lv_obj_t* mask = lv_obj_create(s_screen);
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    lv_obj_add_event_cb(mask, on_clear_dialog_mask_clicked, LV_EVENT_CLICKED,
                        nullptr);
    s_clear_dlg.mask = mask;

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "清空会话");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* desc = lv_label_create(card);
    lv_label_set_text(desc, "将删除设备下全部会话，是否确定？");
    lv_obj_set_style_text_color(desc, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, -10);
    lv_obj_remove_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cancel = lv_button_create(card);
    lv_obj_remove_style_all(cancel);
    lv_obj_set_size(cancel, kBtnW, kBtnH);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(cancel, 16, LV_PART_MAIN);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(cancel, on_clear_cancel_clicked, LV_EVENT_CLICKED,
                        nullptr);
    {
        lv_obj_t* lbl = lv_label_create(cancel);
        lv_label_set_text(lbl, "取消");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_center(lbl);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* ok = lv_button_create(card);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, kBtnW, kBtnH);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0xDC2626), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ok, 16, LV_PART_MAIN);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(ok, on_clear_confirm_clicked, LV_EVENT_CLICKED,
                        nullptr);
    {
        lv_obj_t* lbl = lv_label_create(ok);
        lv_label_set_text(lbl, "确定");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_center(lbl);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }
}

// ---------------------------------------------------------------------------
// Header 按钮样式
// ---------------------------------------------------------------------------
void style_header_btn(lv_obj_t* btn) {
    lv_obj_set_style_radius(btn, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorHeaderBtn), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorHeaderBtnBorder),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3B4556),
                              LV_PART_MAIN | LV_STATE_PRESSED);
}

void on_swipe_back() {
    if (!s_activation_blocked) {
        if (s_clear_dlg.mask != nullptr) {
            close_clear_dialog();
            return;
        }
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home    = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void on_screen_unloaded(lv_event_t* /*e*/) {
    // 通知 worker 尽快收尾；不阻塞地等它退出。LVGL 锁里只是把指针清空，
    // worker 后续若试图碰 UI 会通过 is_screen_alive() 判空被挡掉。
    s_stop_requested.store(true);
    s_state.store(State::Closing);
    s_history_session.fetch_add(1, std::memory_order_relaxed);
    s_history_loading.store(false);
    s_service_available.store(false);
    s_conversation_id.clear();
    invalidate_refresh_snapshot();
    stop_openclaw_worker();

    if (s_tick_timer != nullptr) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = nullptr;
    }
    if (s_activation_guard_timer != nullptr) {
        lv_timer_delete(s_activation_guard_timer);
        s_activation_guard_timer = nullptr;
    }
    stop_auto_refresh_timer();
    s_clear_dlg = ClearDialogUi{};
    s_activation_dlg = ActivationBlockedDialogUi{};
    s_activation_blocked = false;
    s_screen     = nullptr;
    s_msg_list   = nullptr;
    s_empty_hint = nullptr;
    s_record_btn = nullptr;
    s_record_lbl = nullptr;
    s_status_lbl = nullptr;
}

// ---------------------------------------------------------------------------
// UI 组装
// ---------------------------------------------------------------------------
void build_header(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kColorHeaderBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* divider = lv_obj_create(header);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

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
    lv_obj_add_event_cb(back,
                        [](lv_event_t* /*e*/) { on_swipe_back(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "OpenClaw");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorHeaderText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 12, 0);

    // 右上角：清空
    constexpr int32_t kHdrBtnW = 88;
    constexpr int32_t kHdrBtnH = 56;
    constexpr int32_t kHdrRightPad = 12;

    lv_obj_t* clear = lv_button_create(header);
    lv_obj_set_size(clear, kHdrBtnW, kHdrBtnH);
    lv_obj_align(clear, LV_ALIGN_RIGHT_MID, -kHdrRightPad, 0);
    style_header_btn(clear);
    lv_obj_add_event_cb(clear, on_clear_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* clear_lbl = lv_label_create(clear);
    lv_label_set_text(clear_lbl, "清空");
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(kColorHeaderBtnText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(clear_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(clear_lbl);
}

void build_message_list(lv_obj_t* parent) {
    s_msg_list = lv_obj_create(parent);
    lv_obj_set_size(s_msg_list, kPanelW, kListH);
    lv_obj_set_pos(s_msg_list, 0, kHeaderH);
    screen_strip_obj_chrome(s_msg_list);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_msg_list, kListPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_msg_list, kListPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_msg_list, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_msg_list, 16, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_msg_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    s_empty_hint = lv_label_create(parent);
    lv_label_set_text(s_empty_hint, kEmptyHint);
    lv_obj_set_width(s_empty_hint, kPanelW * 80 / 100);
    lv_label_set_long_mode(s_empty_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_empty_hint, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_empty_hint, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_empty_hint, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_align(s_empty_hint, LV_ALIGN_TOP_MID, 0,
                 kHeaderH + (kListH / 2) - 50);
    screen_make_input_passive(s_empty_hint);
    update_empty_hint();
}

void build_footer(lv_obj_t* parent) {
    lv_obj_t* footer = lv_obj_create(parent);
    screen_strip_obj_chrome(footer);
    lv_obj_set_size(footer, kPanelW, kFooterH);
    lv_obj_set_pos(footer, 0, kPanelH - kFooterH);
    lv_obj_set_style_bg_color(footer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    s_status_lbl = lv_label_create(footer);
    lv_label_set_text(s_status_lbl, "按住下面的按钮说话");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 10);
    screen_make_input_passive(s_status_lbl);

    constexpr int32_t kBtnW = 400;
    constexpr int32_t kBtnH = 72;
    s_record_btn = lv_button_create(footer);
    lv_obj_set_size(s_record_btn, kBtnW, kBtnH);
    lv_obj_align(s_record_btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_radius(s_record_btn, kBtnH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(kColorRecordBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_record_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_record_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_record_btn, 0, LV_PART_MAIN);
    // 按下时背景再深一层，给一个明确的「正在按」反馈。
    lv_obj_set_style_bg_color(s_record_btn,
                              lv_color_hex(kColorRecordBtnActive),
                              LV_PART_MAIN | LV_STATE_PRESSED);

    s_record_lbl = lv_label_create(s_record_btn);
    lv_label_set_text(s_record_lbl, "按住说话");
    lv_obj_set_style_text_color(s_record_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_record_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(s_record_lbl);

    lv_obj_add_event_cb(s_record_btn, on_record_pressed, LV_EVENT_PRESSED,
                        nullptr);
    lv_obj_add_event_cb(s_record_btn, on_record_released, LV_EVENT_RELEASED,
                        nullptr);
    // 用户在按钮上做小幅水平拖动时不应被识别成「右滑返回」。slider/arc
    // 的判定逻辑在 screen_util 里，对这种长按场景我们走一样的豁免：录音
    // 按钮整体不参与 swipe-back 计算。代价是从按钮处开始的右滑无法返
    // 回 home，用户可以从按钮外的空白区滑动。
    screen_swipe_back_ignore(s_record_btn, true);
}

}  // namespace

// ===========================================================================
// 公共接口
// ===========================================================================
lv_obj_t* OpenClawScreen::Create() {
    s_activation_blocked = !is_device_activated();
    s_worker_shutdown.store(false, std::memory_order_release);
    if (s_activation_blocked) {
        log_activation_blocked();
    }

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_message_list(scr);
    build_footer(scr);

    if (s_activation_blocked) {
        open_activation_blocked_dialog();
    } else if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, "正在检查龙虾状态…");
    }

    // 100ms 周期 timer 专门刷「已录 N.N 秒」。timer 销毁交给
    // on_screen_unloaded，避免屏幕已经销毁后 timer 仍试图访问 label。
    s_tick_timer = lv_timer_create(tick_timer_cb, 100, nullptr);
    if (s_activation_blocked) {
        s_activation_guard_timer =
            lv_timer_create(on_activation_guard_timer, 1000, nullptr);
    }

    s_state.store(State::Idle);
    s_stop_requested.store(false);
    s_record_start_us.store(0);

    screen_attach_swipe_back(scr, on_swipe_back);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
            if (s_activation_blocked) {
                ESP_LOGW(TAG, "screen loaded while not activated, keep dialog");
                ensure_activation_blocked_dialog();
                return;
            }
            trigger_fetch_history(true);
            start_auto_refresh_timer();
        }
    }, LV_EVENT_SCREEN_LOADED, nullptr);
    return scr;
}

void OpenClawScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        if (!is_device_activated()) {
            ESP_LOGW(TAG, "load: openclaw_screen blocked (device not activated)");
            log_activation_blocked();
        } else {
            ESP_LOGI(TAG, "load: openclaw_screen");
        }
    } else {
        ESP_LOGI(TAG, "unload: openclaw_screen");
        auto& audio_service = Application::GetInstance().GetAudioService();
        Application::GetInstance().ForceReturnToIdle();
        audio_service.EnableWakeWordDetection(false);
    }
}
