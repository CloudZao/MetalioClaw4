#include "translate_screen.h"
#include "i18n.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "api_endpoints.h"
#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "http.h"
#include "screen_util.h"
#include "system_info.h"
#include <web_socket.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "TranslateScreen";

constexpr int kSampleRate = 16000;
constexpr int kFrameMs = 100;
constexpr int kSamplesPerFrame = kSampleRate * kFrameMs / 1000;  // 1600
constexpr int kMinPcmBytes = 80;
constexpr int kHttpTimeoutMs = 20000;
constexpr int kEndWaitMs = 800;

constexpr int32_t kPanelW = 720;
constexpr int32_t kPanelH = 720;
constexpr int32_t kHeaderH = 88;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kFooterH = 108;
constexpr int32_t kLangRowH = 64;
constexpr int32_t kStatusH = 36;
constexpr int32_t kBodyH = kPanelH - kHeaderH - kFooterH;
constexpr int32_t kHeaderSidePad = 8;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorHeaderBg = 0x12151C;
constexpr uint32_t kColorDivider = 0x2A2F3A;
constexpr uint32_t kColorCard = 0x1A1F2A;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorHintText = 0x9AA3B2;
constexpr uint32_t kColorTrans = 0x93C5FD;
constexpr uint32_t kColorBtnIdle = 0x2563EB;
constexpr uint32_t kColorBtnActive = 0xDC2626;
constexpr uint32_t kColorBtnBusy = 0x4B5563;

// Sonicloud 语种表 lanid 0–99（fromlan/tolan 不支持 >=100）
// https://open.sinicloud.com/documents/webapi/stream-asr/#语种编号列表lanid
struct LangEntry {
    int lanid;
    const char* code;
    const char* name_zh;
};

constexpr LangEntry kLangs[] = {
#include "sinicloud_lang_table.inc"
};
constexpr size_t kLangCount = sizeof(kLangs) / sizeof(kLangs[0]);
constexpr int kDefaultFromIdx = 0;  // zh
constexpr int kDefaultToIdx = 1;    // en

enum class State : uint8_t {
    Idle,
    Connecting,
    Streaming,
    Stopping,
    Closing,
};

std::atomic<State> s_state{State::Idle};
std::atomic<bool> s_stop_requested{false};
std::atomic<bool> s_can_send_audio{false};
std::atomic<uint32_t> s_session{0};
std::mutex s_http_mutex;
std::mutex s_text_mutex;
std::mutex s_ws_mutex;

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_from_dd = nullptr;
lv_obj_t* s_to_dd = nullptr;
lv_obj_t* s_status_lbl = nullptr;
lv_obj_t* s_source_lbl = nullptr;
lv_obj_t* s_trans_lbl = nullptr;
lv_obj_t* s_action_btn = nullptr;
lv_obj_t* s_action_lbl = nullptr;
TaskHandle_t s_worker_task = nullptr;

std::unique_ptr<WebSocket> s_ws;
std::string s_source_text;
std::string s_trans_text;
std::string s_lang_options;

const char* kFatalCodes[] = {
    "1",    "2",    "-1",   "-2",   "-3",   "-4",   "16",   "4000",
    "4001", "4002", "4003", "4004", "4005", "4006", "4007", "4010",
    "4011", "4012", "4013", "4014", "4015", "4016", "5000", "5001",
    "5002",
};

bool screen_alive() { return s_screen != nullptr; }

bool session_alive(uint32_t session) {
    return session == s_session.load(std::memory_order_acquire) &&
           s_state.load(std::memory_order_acquire) != State::Closing;
}

bool is_fatal_code(const char* code) {
    if (code == nullptr) {
        return false;
    }
    for (const char* c : kFatalCodes) {
        if (std::strcmp(c, code) == 0) {
            return true;
        }
    }
    return false;
}

struct WakeWordGuard {
    AudioService& as;
    bool disabled = false;
    explicit WakeWordGuard(AudioService& audio) : as(audio) {
        if (as.IsWakeWordRunning()) {
            as.EnableWakeWordDetection(false);
            disabled = true;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    ~WakeWordGuard() {
        if (disabled) {
            as.EnableWakeWordDetection(true);
        }
    }
    WakeWordGuard(const WakeWordGuard&) = delete;
    WakeWordGuard& operator=(const WakeWordGuard&) = delete;
};

const std::string& device_user_id() {
    static const std::string kId = [] {
        std::string mac = SystemInfo::GetMacAddress();
        for (char& c : mac) {
            if (c == ':') {
                c = '-';
            }
        }
        return mac;
    }();
    return kId;
}

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

void build_lang_options() {
    if (!s_lang_options.empty()) {
        return;
    }
    s_lang_options.reserve(kLangCount * 18);
    for (size_t i = 0; i < kLangCount; ++i) {
        if (i > 0) {
            s_lang_options.push_back('\n');
        }
        s_lang_options.append(kLangs[i].name_zh);
        s_lang_options.append(" (");
        s_lang_options.append(kLangs[i].code);
        s_lang_options.push_back(')');
    }
}

int selected_lang_index(lv_obj_t* dd, int fallback) {
    if (dd == nullptr) {
        return fallback;
    }
    const uint32_t sel = lv_dropdown_get_selected(dd);
    if (sel >= kLangCount) {
        return fallback;
    }
    return static_cast<int>(sel);
}

void post_status(const char* text, uint32_t session) {
    if (text == nullptr || !session_alive(session)) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (session_alive(session) && screen_alive() && s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, text);
    }
    esp_lv_adapter_unlock();
}

void refresh_result_ui_locked() {
    if (s_source_lbl == nullptr || s_trans_lbl == nullptr) {
        return;
    }
    std::string source;
    std::string trans;
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        source = s_source_text;
        trans = s_trans_text;
    }
    if (source.empty()) {
        lv_label_set_text(s_source_lbl, I18n::T("识别原文将显示在这里"));
        lv_obj_set_style_text_color(s_source_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
    } else {
        lv_label_set_text(s_source_lbl, source.c_str());
        lv_obj_set_style_text_color(s_source_lbl, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
    }
    if (trans.empty()) {
        lv_label_set_text(s_trans_lbl, I18n::T("译文将显示在这里"));
        lv_obj_set_style_text_color(s_trans_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
    } else {
        lv_label_set_text(s_trans_lbl, trans.c_str());
        lv_obj_set_style_text_color(s_trans_lbl, lv_color_hex(kColorTrans),
                                    LV_PART_MAIN);
    }
}

void post_result_refresh(uint32_t session) {
    if (!session_alive(session)) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (session_alive(session) && screen_alive()) {
        refresh_result_ui_locked();
    }
    esp_lv_adapter_unlock();
}

void clear_results_locked() {
    std::lock_guard<std::mutex> lock(s_text_mutex);
    s_source_text.clear();
    s_trans_text.clear();
}

void apply_recognition(const char* result, const char* trans) {
    std::lock_guard<std::mutex> lock(s_text_mutex);
    // 识别原文覆盖显示（不追加历史句）
    if (result != nullptr && result[0] != '\0') {
        s_source_text = result;
    }
    if (trans != nullptr && trans[0] != '\0') {
        s_trans_text = trans;
    }
}

void update_action_ui_locked(State st) {
    if (s_action_btn == nullptr || s_action_lbl == nullptr) {
        return;
    }
    switch (st) {
        case State::Idle:
            lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kColorBtnIdle),
                                      LV_PART_MAIN);
            lv_label_set_text(s_action_lbl, I18n::T("实时翻译"));
            lv_obj_add_flag(s_action_btn, LV_OBJ_FLAG_CLICKABLE);
            if (s_from_dd != nullptr) {
                lv_obj_remove_state(s_from_dd, LV_STATE_DISABLED);
            }
            if (s_to_dd != nullptr) {
                lv_obj_remove_state(s_to_dd, LV_STATE_DISABLED);
            }
            break;
        case State::Connecting:
            lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kColorBtnBusy),
                                      LV_PART_MAIN);
            lv_label_set_text(s_action_lbl, I18n::T("停止翻译"));
            lv_obj_add_flag(s_action_btn, LV_OBJ_FLAG_CLICKABLE);
            if (s_from_dd != nullptr) {
                lv_obj_add_state(s_from_dd, LV_STATE_DISABLED);
            }
            if (s_to_dd != nullptr) {
                lv_obj_add_state(s_to_dd, LV_STATE_DISABLED);
            }
            break;
        case State::Streaming:
            lv_obj_set_style_bg_color(s_action_btn,
                                      lv_color_hex(kColorBtnActive),
                                      LV_PART_MAIN);
            lv_label_set_text(s_action_lbl, I18n::T("停止翻译"));
            lv_obj_add_flag(s_action_btn, LV_OBJ_FLAG_CLICKABLE);
            break;
        case State::Stopping:
            lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kColorBtnBusy),
                                      LV_PART_MAIN);
            lv_label_set_text(s_action_lbl, I18n::T("停止中…"));
            lv_obj_remove_flag(s_action_btn, LV_OBJ_FLAG_CLICKABLE);
            break;
        case State::Closing:
            break;
    }
}

void post_state(State st, uint32_t session) {
    s_state.store(st, std::memory_order_release);
    if (!session_alive(session) && st != State::Closing) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (session == s_session.load(std::memory_order_acquire) &&
        screen_alive()) {
        update_action_ui_locked(st);
    }
    esp_lv_adapter_unlock();
}

void close_websocket() {
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    if (s_ws) {
        s_ws->Close();
        s_ws.reset();
    }
    s_can_send_audio.store(false, std::memory_order_release);
}

struct HttpJsonResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string err;
};

std::unique_ptr<Http> create_http_client(std::string& err_out) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = "no network";
        return nullptr;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = "create http failed";
        return nullptr;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
    http->SetHeader("X-Device-Id", SystemInfo::GetMacAddress().c_str());
    return http;
}

HttpJsonResult http_post_json(const std::string& url, std::string json) {
    HttpJsonResult r;
    std::lock_guard<std::mutex> lock(s_http_mutex);
    auto http = create_http_client(r.err);
    if (http == nullptr) {
        return r;
    }
    http->SetHeader("Content-Type", "application/json");
    api::LogHttpRequest(TAG, "POST", url, json);
    http->SetContent(std::move(json));
    if (!http->Open("POST", url)) {
        r.err = "open failed";
        api::LogHttpResponse(TAG, -1, r.err);
        return r;
    }
    r.status = http->GetStatusCode();
    r.body = http->ReadAll();
    api::LogHttpResponse(TAG, r.status, r.body);
    http->Close();
    if (r.status != 200) {
        r.err = r.body.empty() ? ("HTTP " + std::to_string(r.status)) : r.body;
        return r;
    }
    r.ok = true;
    return r;
}

bool fetch_ws_url(int lanid, const char* fromlan, const char* tolan,
                  std::string& ws_url_out, std::string& err_out) {
    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "userId", device_user_id().c_str());
    char lanid_buf[16];
    std::snprintf(lanid_buf, sizeof(lanid_buf), "%d", lanid);
    cJSON_AddStringToObject(req, "lanid", lanid_buf);
    cJSON_AddStringToObject(req, "lid", "1");
    cJSON_AddStringToObject(req, "fromlan", fromlan);
    cJSON_AddStringToObject(req, "tolan", tolan);
    char* raw = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (raw == nullptr) {
        err_out = "json alloc failed";
        return false;
    }
    std::string body(raw);
    cJSON_free(raw);

    auto resp = http_post_json(api::Url(api::kSinicloudToken), std::move(body));
    if (!resp.ok) {
        err_out = resp.err.empty() ? "token request failed" : resp.err;
        return false;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (root == nullptr) {
        err_out = "token json parse failed";
        return false;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        err_out = cJSON_IsString(msg) && msg->valuestring
                      ? msg->valuestring
                      : "token api error";
        cJSON_Delete(root);
        return false;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* ws = data != nullptr
                    ? cJSON_GetObjectItemCaseSensitive(data, "wsUrl")
                    : nullptr;
    if (!cJSON_IsString(ws) || ws->valuestring == nullptr ||
        ws->valuestring[0] == '\0') {
        err_out = "wsUrl missing";
        cJSON_Delete(root);
        return false;
    }
    ws_url_out = ws->valuestring;
    cJSON_Delete(root);
    return true;
}

void handle_server_json(const char* data, size_t len, uint32_t session) {
    if (data == nullptr || len == 0 || !session_alive(session)) {
        return;
    }
    std::string payload(data, len);
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "bad json: %.80s", payload.c_str());
        return;
    }

    cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "errCode");
    std::string code;
    if (cJSON_IsString(err) && err->valuestring != nullptr) {
        code = err->valuestring;
    } else if (cJSON_IsNumber(err)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", err->valueint);
        code = buf;
    }

    cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON* trans = cJSON_GetObjectItemCaseSensitive(root, "trans");
    const char* result_s =
        cJSON_IsString(result) && result->valuestring ? result->valuestring
                                                      : nullptr;
    const char* trans_s =
        cJSON_IsString(trans) && trans->valuestring ? trans->valuestring
                                                    : nullptr;

    if (code == "1000") {
        s_can_send_audio.store(true, std::memory_order_release);
        post_status(I18n::T("翻译中"), session);
        post_state(State::Streaming, session);
        cJSON_Delete(root);
        return;
    }
    if (code == "1001") {
        ESP_LOGW(TAG, "low balance warning");
        cJSON_Delete(root);
        return;
    }
    if (code == "0" || code == "3") {
        apply_recognition(result_s, trans_s);
        post_result_refresh(session);
        cJSON_Delete(root);
        return;
    }
    if (is_fatal_code(code.c_str())) {
        ESP_LOGE(TAG, "fatal errCode=%s result=%s", code.c_str(),
                 result_s ? result_s : "");
        post_status(I18n::T("翻译出错"), session);
        s_stop_requested.store(true, std::memory_order_release);
        cJSON_Delete(root);
        return;
    }
    if (result_s != nullptr && result_s[0] != '\0') {
        apply_recognition(result_s, trans_s);
        post_result_refresh(session);
    } else {
        ESP_LOGI(TAG, "unhandled errCode=%s", code.c_str());
    }
    cJSON_Delete(root);
}

bool connect_websocket(const std::string& ws_url, uint32_t session,
                       std::string& err_out) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = "no network";
        return false;
    }
    auto ws = network->CreateWebSocket(1);
    if (!ws) {
        err_out = "create websocket failed";
        return false;
    }
    ws->SetReceiveBufferSize(4096);
    ws->OnData([session](const char* data, size_t len, bool binary) {
        if (binary || !session_alive(session)) {
            return;
        }
        handle_server_json(data, len, session);
    });
    ws->OnDisconnected([session]() {
        ESP_LOGI(TAG, "websocket disconnected");
        s_can_send_audio.store(false, std::memory_order_release);
        if (session_alive(session) &&
            s_state.load(std::memory_order_acquire) == State::Streaming) {
            s_stop_requested.store(true, std::memory_order_release);
        }
    });
    ws->OnError([session](int err) {
        ESP_LOGE(TAG, "websocket error=%d", err);
        if (session_alive(session)) {
            s_stop_requested.store(true, std::memory_order_release);
        }
    });

    ESP_LOGI(TAG, "connecting ws…");
    if (!ws->Connect(ws_url.c_str())) {
        err_out = "websocket connect failed";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        s_ws = std::move(ws);
    }
    return true;
}

bool send_pcm_frame(const int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return false;
    }
    const size_t bytes = count * sizeof(int16_t);
    if (bytes <= static_cast<size_t>(kMinPcmBytes)) {
        return true;
    }
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    if (!s_ws || !s_ws->IsConnected()) {
        return false;
    }
    return s_ws->Send(samples, bytes, true);
}

void send_end_and_close() {
    {
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        if (s_ws && s_ws->IsConnected()) {
            s_ws->Send(std::string("end"));
            ESP_LOGI(TAG, "sent end");
        }
    }
    vTaskDelay(pdMS_TO_TICKS(kEndWaitMs));
    close_websocket();
}

void stream_pcm_loop(uint32_t session) {
    auto& as = Application::GetInstance().GetAudioService();
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    WakeWordGuard wake_guard(as);

    std::vector<int16_t> frame;
    frame.reserve(static_cast<size_t>(kSamplesPerFrame) * 2);

    while (!s_stop_requested.load(std::memory_order_acquire) &&
           session_alive(session)) {
        if (!s_can_send_audio.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!as.ReadAudioData(frame, kSampleRate, kSamplesPerFrame)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (codec != nullptr && codec->input_channels() == 2) {
            keep_left_channel_only(frame);
        }
        if (!send_pcm_frame(frame.data(), frame.size())) {
            ESP_LOGW(TAG, "send pcm failed");
            break;
        }
    }
}

void worker_task(void* /*arg*/) {
    const uint32_t session = s_session.load(std::memory_order_acquire);
    int from_idx = kDefaultFromIdx;
    int to_idx = kDefaultToIdx;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        from_idx = selected_lang_index(s_from_dd, kDefaultFromIdx);
        to_idx = selected_lang_index(s_to_dd, kDefaultToIdx);
        esp_lv_adapter_unlock();
    }

    if (from_idx == to_idx) {
        post_status(I18n::T("源语言与目标语言不能相同"), session);
        post_state(State::Idle, session);
        s_worker_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    auto& as = Application::GetInstance().GetAudioService();
    if (!as.IsStarted()) {
        post_status(I18n::T("音频未就绪"), session);
        post_state(State::Idle, session);
        s_worker_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    if (as.IsAudioProcessorRunning()) {
        post_status(I18n::T("音频忙，稍后再试"), session);
        post_state(State::Idle, session);
        s_worker_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    clear_results_locked();
    post_result_refresh(session);
    post_status(I18n::T("获取令牌…"), session);

    const LangEntry& from = kLangs[from_idx];
    const LangEntry& to = kLangs[to_idx];
    std::string ws_url;
    std::string err;
    if (!fetch_ws_url(from.lanid, from.code, to.code, ws_url, err)) {
        ESP_LOGE(TAG, "token failed: %s", err.c_str());
        post_status(I18n::T("获取令牌失败"), session);
        post_state(State::Idle, session);
        s_worker_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (s_stop_requested.load(std::memory_order_acquire) ||
        !session_alive(session)) {
        post_state(State::Idle, session);
        s_worker_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    post_status(I18n::T("连接中…"), session);
    if (!connect_websocket(ws_url, session, err)) {
        ESP_LOGE(TAG, "ws failed: %s", err.c_str());
        post_status(I18n::T("连接失败"), session);
        post_state(State::Idle, session);
        s_worker_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // 等待 errCode=1000；超时则退出
    for (int i = 0; i < 100 && session_alive(session) &&
                    !s_stop_requested.load(std::memory_order_acquire);
         ++i) {
        if (s_can_send_audio.load(std::memory_order_acquire)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_can_send_audio.load(std::memory_order_acquire)) {
        post_status(I18n::T("等待就绪超时"), session);
        close_websocket();
        post_state(State::Idle, session);
        s_worker_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    stream_pcm_loop(session);

    post_state(State::Stopping, session);
    post_status(I18n::T("停止中…"), session);
    send_end_and_close();

    if (session_alive(session)) {
        post_status(I18n::T("已停止"), session);
        post_state(State::Idle, session);
    }
    s_worker_task = nullptr;
    vTaskDelete(nullptr);
}

void start_session() {
    if (s_worker_task != nullptr) {
        return;
    }
    State st = s_state.load(std::memory_order_acquire);
    if (st != State::Idle) {
        return;
    }
    s_stop_requested.store(false, std::memory_order_release);
    s_can_send_audio.store(false, std::memory_order_release);
    post_state(State::Connecting, s_session.load(std::memory_order_acquire));

    BaseType_t ok = xTaskCreatePinnedToCore(
        worker_task, "translate_ws", 8192, nullptr, 5, &s_worker_task, 0);
    if (ok != pdPASS) {
        s_worker_task = nullptr;
        post_status(I18n::T("启动失败"), s_session.load());
        post_state(State::Idle, s_session.load());
    }
}

void request_stop() {
    State st = s_state.load(std::memory_order_acquire);
    if (st != State::Streaming && st != State::Connecting) {
        return;
    }
    s_stop_requested.store(true, std::memory_order_release);
    if (st == State::Streaming) {
        post_state(State::Stopping, s_session.load(std::memory_order_acquire));
    }
}

void on_action_clicked(lv_event_t* /*e*/) {
    State st = s_state.load(std::memory_order_acquire);
    if (st == State::Idle) {
        start_session();
    } else if (st == State::Streaming || st == State::Connecting) {
        request_stop();
    }
}

void stop_session_for_exit() {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Closing, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    s_can_send_audio.store(false, std::memory_order_release);
    close_websocket();
}

void go_home() {
    stop_session_for_exit();
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void on_back_clicked(lv_event_t* /*e*/) { go_home(); }

void on_swipe_back() { go_home(); }

void on_screen_unloaded(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_target_obj(e);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Closing, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    s_can_send_audio.store(false, std::memory_order_release);
    close_websocket();

    // 若本屏已被更新的 Create() 替换，静态指针属于新实例，不能在这里清掉。
    if (scr != s_screen) {
        ESP_LOGI(TAG, "unload: stale screen instance ignored");
        return;
    }

    s_screen = nullptr;
    s_from_dd = nullptr;
    s_to_dd = nullptr;
    s_status_lbl = nullptr;
    s_source_lbl = nullptr;
    s_trans_lbl = nullptr;
    s_action_btn = nullptr;
    s_action_lbl = nullptr;
}

void on_dropdown_ready(lv_event_t* e) {
    lv_obj_t* dd = lv_event_get_target_obj(e);
    if (dd == nullptr) {
        return;
    }
    lv_obj_t* list = lv_dropdown_get_list(dd);
    if (list == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 10, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 360, LV_PART_MAIN);
    screen_swipe_back_ignore(list, true);
}

void style_dropdown(lv_obj_t* dd) {
    lv_obj_set_style_radius(dd, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dd, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, lv_color_hex(0x4B5563), LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, &font_puhui_20_4, LV_PART_MAIN);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_DOWN);
    lv_obj_add_event_cb(dd, on_dropdown_ready, LV_EVENT_READY, nullptr);
    screen_swipe_back_ignore(dd, true);
}

lv_obj_t* make_text_card(lv_obj_t* parent, const char* title,
                         lv_obj_t** out_label, uint32_t title_color) {
    lv_obj_t* card = lv_obj_create(parent);
    screen_strip_obj_chrome(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(title_color),
                                LV_PART_MAIN);
    screen_make_input_passive(title_lbl);

    lv_obj_t* body = lv_label_create(card);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(body, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    screen_make_input_passive(body);
    *out_label = body;
    return card;
}

void build_header(lv_obj_t* parent) {
    lv_obj_t* top = lv_obj_create(parent);
    screen_strip_obj_chrome(top);
    lv_obj_set_size(top, kPanelW, kHeaderH);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* divider = lv_obj_create(top);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

    lv_obj_t* back = lv_button_create(top);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, kHeaderSidePad, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_back_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(top);
    lv_label_set_text(title, I18n::T("翻译"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, kHeaderSidePad + kBackBtnSize + 8,
                 0);
    screen_make_input_passive(title);
}

void build_body(lv_obj_t* parent) {
    build_lang_options();

    lv_obj_t* body = lv_obj_create(parent);
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kPanelW, kBodyH);
    lv_obj_set_pos(body, 0, kHeaderH);
    lv_obj_set_style_bg_color(body, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_right(body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_top(body, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(body, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, LV_PART_MAIN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lang_row = lv_obj_create(body);
    screen_strip_obj_chrome(lang_row);
    lv_obj_set_size(lang_row, LV_PCT(100), kLangRowH);
    lv_obj_set_style_bg_opa(lang_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(lang_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lang_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(lang_row, LV_OBJ_FLAG_SCROLLABLE);

    s_from_dd = lv_dropdown_create(lang_row);
    lv_obj_set_size(s_from_dd, 280, 52);
    style_dropdown(s_from_dd);
    lv_dropdown_set_options(s_from_dd, s_lang_options.c_str());
    lv_dropdown_set_selected(s_from_dd, kDefaultFromIdx);

    lv_obj_t* arrow = lv_label_create(lang_row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(arrow, &font_puhui_20_4, LV_PART_MAIN);
    screen_make_input_passive(arrow);

    s_to_dd = lv_dropdown_create(lang_row);
    lv_obj_set_size(s_to_dd, 280, 52);
    style_dropdown(s_to_dd);
    lv_dropdown_set_options(s_to_dd, s_lang_options.c_str());
    lv_dropdown_set_selected(s_to_dd, kDefaultToIdx);

    s_status_lbl = lv_label_create(body);
    lv_obj_set_width(s_status_lbl, LV_PCT(100));
    lv_obj_set_height(s_status_lbl, kStatusH);
    lv_label_set_text(s_status_lbl, I18n::T("选择语言后点击实时翻译"));
    lv_obj_set_style_text_font(s_status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    screen_make_input_passive(s_status_lbl);

    make_text_card(body, I18n::T("识别原文"), &s_source_lbl, kColorHintText);
    make_text_card(body, I18n::T("译文"), &s_trans_lbl, kColorTrans);
    refresh_result_ui_locked();
}

void build_footer(lv_obj_t* parent) {
    lv_obj_t* footer = lv_obj_create(parent);
    screen_strip_obj_chrome(footer);
    lv_obj_set_size(footer, kPanelW, kFooterH);
    lv_obj_set_pos(footer, 0, kPanelH - kFooterH);
    lv_obj_set_style_bg_color(footer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int32_t kBtnW = 400;
    constexpr int32_t kBtnH = 72;
    s_action_btn = lv_button_create(footer);
    lv_obj_set_size(s_action_btn, kBtnW, kBtnH);
    lv_obj_align(s_action_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_action_btn, kBtnH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kColorBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_action_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_action_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_action_btn, 0, LV_PART_MAIN);

    s_action_lbl = lv_label_create(s_action_btn);
    lv_label_set_text(s_action_lbl, I18n::T("实时翻译"));
    lv_obj_set_style_text_color(s_action_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_action_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(s_action_lbl);

    lv_obj_add_event_cb(s_action_btn, on_action_clicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(s_action_btn, true);
}

}  // namespace

lv_obj_t* TranslateScreen::Create() {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Idle, std::memory_order_release);
    s_stop_requested.store(false, std::memory_order_release);
    s_can_send_audio.store(false, std::memory_order_release);
    clear_results_locked();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_body(scr);
    build_footer(scr);

    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    screen_attach_swipe_back(scr, on_swipe_back);
    return scr;
}

void TranslateScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: translate_screen");
        return;
    }
    ESP_LOGI(TAG, "unload: translate_screen");
    s_state.store(State::Closing, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    close_websocket();
    auto& audio_service = Application::GetInstance().GetAudioService();
    Application::GetInstance().ForceReturnToIdle();
    audio_service.EnableWakeWordDetection(false);
}
