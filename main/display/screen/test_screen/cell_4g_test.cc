#include "cell_4g_test.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "board.h"
#include "dual_network_board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nt26_board.h"
#include "screen_util.h"
#include "test_ui_common.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "Cell4gTest";

constexpr int  kSimSlotExternal = 0;
constexpr int  kSimSlotInternal = 1;
constexpr uint32_t kAutoStartDelayMs = 1000;

// 手册：AT+ECPING="url",number,size,delay(ms)
constexpr int      kPingCount    = 3;
constexpr int      kPingSize     = 32;
constexpr int      kPingDelayMs  = 15000;
constexpr uint32_t kEcpingTimeoutMs =
    static_cast<uint32_t>(kPingCount) * kPingDelayMs + 20000;
constexpr uint32_t kSwitchRegWaitMs  = 30000;
constexpr uint32_t kAtShortMs        = 3000;
constexpr uint32_t kAtMediumMs       = 8000;
constexpr uint32_t kAtLongMs         = 12000;

enum class TestState {
    Idle,
    TestingInternal,
    TestingExternal,
};

struct SimRowUi {
    lv_obj_t* status_icon = nullptr;
    lv_obj_t* hint_lbl    = nullptr;
    int       slot        = 0;
};

struct TestDoneMsg {
    int  slot;
    bool pass;
    char detail[64];
};

TestState    s_state            = TestState::Idle;
bool         s_loaded           = false;
bool         s_auto_sequence    = false;
bool         s_modem_busy       = false;
uint32_t     s_last_cfun_ms     = 0;
lv_timer_t*  s_auto_start_timer = nullptr;
SimRowUi     s_internal{};
SimRowUi     s_external{};

constexpr uint32_t kPostCfunSettleMs = 40000;

void MarkModemRadioReset() {
    s_last_cfun_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void ClearModemRadioReset() {
    s_last_cfun_ms = 0;
}

bool NeedsRegistrationWait(int target_slot, int current_slot) {
    if (current_slot != target_slot) {
        return true;
    }
    if (s_last_cfun_ms == 0) {
        return false;
    }
    const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    return (now - s_last_cfun_ms) < kPostCfunSettleMs;
}

Nt26Board* GetNt26Board() {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    if (dual != nullptr) {
        return dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    }
    return dynamic_cast<Nt26Board*>(&board);
}

bool HasAtError(const std::string& resp) {
    if (resp.find("+CME ERROR") != std::string::npos ||
        resp.find("+CMS ERROR") != std::string::npos) {
        return true;
    }
    if (resp.find("ERROR") != std::string::npos &&
        resp.find("OK") == std::string::npos) {
        return true;
    }
    return false;
}

bool RunAtOk(Nt26Board* nt26, const std::string& cmd, std::string& resp,
             uint32_t timeout_ms) {
    resp.clear();
    const esp_err_t err =
        nt26->SendAtCommand(cmd, resp, timeout_ms, true);
    ESP_LOGI(TAG, "AT '%s' err=%d resp_len=%u",
             cmd.c_str(), static_cast<int>(err),
             static_cast<unsigned>(resp.size()));
    if (err != ESP_OK) {
        return false;
    }
    return resp.find("OK") != std::string::npos && !HasAtError(resp);
}

void RunAt(Nt26Board* nt26, const std::string& cmd, std::string& resp,
           uint32_t timeout_ms) {
    resp.clear();
    const esp_err_t err =
        nt26->SendAtCommand(cmd, resp, timeout_ms, true);
    ESP_LOGI(TAG, "AT '%s' err=%d resp_len=%u",
             cmd.c_str(), static_cast<int>(err),
             static_cast<unsigned>(resp.size()));
}

bool IsSimAbsent(const std::string& resp) {
    return resp.find("+CME ERROR: 10") != std::string::npos ||
           resp.find("+CME ERROR: 13") != std::string::npos ||
           resp.find("+CME ERROR: 14") != std::string::npos;
}

int ParseSimSlotFromEcsimcfg(const std::string& resp) {
    constexpr const char* kKey = "\"SimSlot\"";
    size_t pos = 0;
    while ((pos = resp.find(kKey, pos)) != std::string::npos) {
        const size_t comma = resp.find(',', pos);
        if (comma == std::string::npos) {
            return -1;
        }
        size_t i = comma + 1;
        while (i < resp.size() &&
               (resp[i] == ' ' || resp[i] == '\t')) {
            ++i;
        }
        if (i >= resp.size() ||
            !std::isdigit(static_cast<unsigned char>(resp[i]))) {
            pos = comma + 1;
            continue;
        }
        int slot = 0;
        while (i < resp.size() &&
               std::isdigit(static_cast<unsigned char>(resp[i]))) {
            slot = slot * 10 + (resp[i] - '0');
            ++i;
        }
        return slot;
    }
    return -1;
}

int QueryCurrentSimSlot(Nt26Board* nt26) {
    std::string resp;
    if (!RunAtOk(nt26, "AT+ECSIMCFG?", resp, kAtShortMs)) {
        return -1;
    }
    return ParseSimSlotFromEcsimcfg(resp);
}

int ParseCeregStat(const std::string& resp) {
    const size_t pos = resp.find("+CEREG:");
    if (pos == std::string::npos) {
        return -1;
    }
    int n = 0;
    int stat = 0;
    const char* p = resp.c_str() + pos;
    if (std::sscanf(p, "+CEREG: %d,%d", &n, &stat) >= 2 && n <= 3) {
        return stat;
    }
    if (std::sscanf(p, "+CEREG: %d", &stat) == 1) {
        return stat;
    }
    return -1;
}

bool IsSimReady(const std::string& resp) {
    return resp.find("+CPIN: READY") != std::string::npos;
}

bool HasIpv4Address(const std::string& resp) {
    if (resp.find("+CGPADDR:") == std::string::npos) {
        return false;
    }
    int a = 0;
    int b = 0;
    int c = 0;
    int d = 0;
    return std::sscanf(resp.c_str(), "+CGPADDR: %*d,\"%d.%d.%d.%d", &a, &b, &c,
                      &d) == 4 ||
           std::sscanf(resp.c_str(), "\r\n+CGPADDR: %*d,\"%d.%d.%d.%d", &a, &b,
                       &c, &d) == 4;
}

bool WaitSimRegisteredForPing(Nt26Board* nt26, char* detail, size_t detail_len,
                              uint32_t max_wait_ms) {
    const uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    std::string resp;

    RunAtOk(nt26, "AT+CEREG=2", resp, kAtShortMs);

    while ((xTaskGetTickCount() * portTICK_PERIOD_MS - start) < max_wait_ms) {
        if (!s_loaded) {
            std::snprintf(detail, detail_len, "测试已取消");
            return false;
        }

        RunAt(nt26, "AT+CPIN?", resp, kAtShortMs);
        if (IsSimAbsent(resp)) {
            std::snprintf(detail, detail_len, "未检测到SIM卡");
            return false;
        }
        if (!IsSimReady(resp)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (RunAtOk(nt26, "AT+CEREG?", resp, kAtShortMs)) {
            const int stat = ParseCeregStat(resp);
            if (stat == 3) {
                std::snprintf(detail, detail_len, "网络注册被拒绝");
                return false;
            }
            if (stat == 1 || stat == 5) {
                return true;
            }
        }

        if (RunAtOk(nt26, "AT+CGPADDR=1", resp, kAtShortMs) &&
            HasIpv4Address(resp)) {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    std::snprintf(detail, detail_len, "搜网超时");
    return false;
}

bool ApplySimSlot(Nt26Board* nt26, int target_slot, char* detail,
                  size_t detail_len) {
    std::string resp;
    if (!RunAtOk(nt26, "AT+CFUN=0", resp, kAtMediumMs)) {
        std::snprintf(detail, detail_len, "CFUN=0失败");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    char buf[64];
    std::snprintf(buf, sizeof(buf), "AT+ECSIMCFG=SimSlot,%d", target_slot);
    if (!RunAtOk(nt26, buf, resp, kAtShortMs)) {
        RunAtOk(nt26, "AT+CFUN=1", resp, kAtLongMs);
        std::snprintf(detail, detail_len, "切换SIM失败");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    RunAtOk(nt26, "AT+CFUN=1", resp, kAtLongMs);
    vTaskDelay(pdMS_TO_TICKS(2000));

    const int slot = QueryCurrentSimSlot(nt26);
    if (slot != target_slot) {
        std::snprintf(detail, detail_len, "SIM槽位未生效");
        return false;
    }
    MarkModemRadioReset();
    return true;
}

void RestoreSimSlotQuiet(Nt26Board* nt26, int target_slot) {
    char detail[64];
    ApplySimSlot(nt26, target_slot, detail, sizeof(detail));
}

bool ParseEcpingSummary(const std::string& resp, int* tx, int* rx) {
    if (tx == nullptr || rx == nullptr) {
        return false;
    }

    size_t pos = 0;
    while ((pos = resp.find("+ECPING:", pos)) != std::string::npos) {
        const char* p = resp.c_str() + pos;
        int local_tx = 0;
        int local_rx = 0;
        if (std::sscanf(p,
                        "+ECPING: dest: %*[^,], %d packets transmitted, %d received",
                        &local_tx, &local_rx) == 2 ||
            std::sscanf(p,
                        "+ECPING: dest: %*[^,], %d packets transmittted, %d received",
                        &local_tx, &local_rx) == 2) {
            *tx = local_tx;
            *rx = local_rx;
            return true;
        }
        pos += 8;
    }
    return false;
}

bool IsEcpingSuccess(const std::string& resp) {
    if (HasAtError(resp)) {
        return false;
    }
    if (resp.find("+ECPING: FAIL") != std::string::npos ||
        resp.find("+ECPING: TIMEOUT") != std::string::npos) {
        return false;
    }
    if (resp.find("+ECPING: SUCC") != std::string::npos) {
        return true;
    }
    if (resp.find("+ECPING: DONE") != std::string::npos) {
        return true;
    }
    if (resp.find("0% packet loss") != std::string::npos ||
        resp.find("0 % packet loss") != std::string::npos) {
        return true;
    }

    int tx = 0;
    int rx = 0;
    if (ParseEcpingSummary(resp, &tx, &rx)) {
        return rx > 0 && (tx <= 0 || rx >= tx);
    }

    return false;
}

bool RunEcpingTest(Nt26Board* nt26, char* detail, size_t detail_len) {
    char cmd[96];
    std::snprintf(cmd, sizeof(cmd), "AT+ECPING=\"www.baidu.com\",%d,%d,%d",
                  kPingCount, kPingSize, kPingDelayMs);

    std::string resp;
    const esp_err_t err = nt26->SendAtCommandCollectUntil(
        cmd, resp, kEcpingTimeoutMs, "+ECPING: DONE", true);
    ESP_LOGI(TAG, "ECPING err=%d resp_len=%u resp='%s'",
             static_cast<int>(err), static_cast<unsigned>(resp.size()),
             resp.c_str());

    if (IsEcpingSuccess(resp)) {
        return true;
    }

    if (HasAtError(resp)) {
        std::snprintf(detail, detail_len, "PING命令失败");
    } else if (err == ESP_ERR_TIMEOUT) {
        std::snprintf(detail, detail_len, "PING等待超时");
    } else {
        int tx = 0;
        int rx = 0;
        if (ParseEcpingSummary(resp, &tx, &rx)) {
            std::snprintf(detail, detail_len, "PING丢包 %d/%d", rx, tx);
        } else {
            std::snprintf(detail, detail_len, "PING失败");
        }
    }
    return false;
}

SimRowUi* RowForSlot(int slot) {
    return slot == kSimSlotInternal ? &s_internal : &s_external;
}

void SetHint(SimRowUi* row, const char* text, bool error) {
    if (row == nullptr || row->hint_lbl == nullptr) {
        return;
    }
    lv_label_set_text(row->hint_lbl, text);
    lv_obj_set_style_text_color(
        row->hint_lbl,
        lv_color_hex(error ? kTestColorError : kTestColorTextDim),
        LV_PART_MAIN);
}

void StopAutoStartTimer() {
    if (s_auto_start_timer != nullptr) {
        lv_timer_delete(s_auto_start_timer);
        s_auto_start_timer = nullptr;
    }
}

void StartTest(int slot);

void OnTestDoneAsync(void* user_data) {
    auto* msg = static_cast<TestDoneMsg*>(user_data);
    if (!s_loaded) {
        delete msg;
        return;
    }

    SimRowUi* row = RowForSlot(msg->slot);
    s_state = TestState::Idle;

    if (row != nullptr) {
        if (msg->pass) {
            SetHint(row, "PING成功", false);
            TestUiUpdateStatus(row->status_icon, true);
        } else {
            SetHint(row, msg->detail, true);
            TestUiUpdateStatus(row->status_icon, false);
        }
    }

    if (s_auto_sequence && msg->slot == kSimSlotInternal) {
        StartTest(kSimSlotExternal);
    } else if (s_auto_sequence && msg->slot == kSimSlotExternal) {
        s_auto_sequence = false;
        ESP_LOGI(TAG, "auto 4G test sequence finished");
    }

    delete msg;
}

void Cell4gTestTask(void* arg) {
    s_modem_busy = true;
    const int target_slot = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    auto* msg = new TestDoneMsg{};
    msg->slot = target_slot;
    msg->pass = false;
    std::snprintf(msg->detail, sizeof(msg->detail), "测试失败");

    Nt26Board* nt26 = GetNt26Board();
    if (nt26 == nullptr) {
        std::snprintf(msg->detail, sizeof(msg->detail), "4G模块不可用");
        lv_async_call(OnTestDoneAsync, msg);
        s_modem_busy = false;
        vTaskDelete(nullptr);
        return;
    }

    std::string resp;
    if (!RunAtOk(nt26, "AT", resp, kAtShortMs)) {
        std::snprintf(msg->detail, sizeof(msg->detail), "模组无响应");
        lv_async_call(OnTestDoneAsync, msg);
        s_modem_busy = false;
        vTaskDelete(nullptr);
        return;
    }

    const int original_slot = QueryCurrentSimSlot(nt26);
    bool switched = false;
    const int current_slot =
        (original_slot == kSimSlotExternal ||
         original_slot == kSimSlotInternal)
            ? original_slot
            : -1;

    if (current_slot != target_slot) {
        if (!ApplySimSlot(nt26, target_slot, msg->detail,
                          sizeof(msg->detail))) {
            lv_async_call(OnTestDoneAsync, msg);
            s_modem_busy = false;
            vTaskDelete(nullptr);
            return;
        }
        switched = true;
    }

    if (NeedsRegistrationWait(target_slot, current_slot) || switched) {
        char wait_detail[64];
        if (!WaitSimRegisteredForPing(nt26, wait_detail, sizeof(wait_detail),
                                      kSwitchRegWaitMs)) {
            if (std::strstr(wait_detail, "未检测到SIM") != nullptr) {
                std::snprintf(msg->detail, sizeof(msg->detail), "%s",
                              wait_detail);
                if (switched && current_slot >= 0) {
                    RestoreSimSlotQuiet(nt26, current_slot);
                }
                lv_async_call(OnTestDoneAsync, msg);
                s_modem_busy = false;
                vTaskDelete(nullptr);
                return;
            }
            ESP_LOGW(TAG, "reg wait: %s, try ping anyway", wait_detail);
        }
    }

    if (RunEcpingTest(nt26, msg->detail, sizeof(msg->detail))) {
        msg->pass = true;
        std::snprintf(msg->detail, sizeof(msg->detail), "PING成功");
        ClearModemRadioReset();
    }

    if (switched && current_slot >= 0 && current_slot != target_slot) {
        RestoreSimSlotQuiet(nt26, current_slot);
    }

    lv_async_call(OnTestDoneAsync, msg);
    s_modem_busy = false;
    vTaskDelete(nullptr);
}

void StartTest(int slot) {
    if (s_state != TestState::Idle || s_modem_busy) {
        return;
    }

    s_state = slot == kSimSlotInternal ? TestState::TestingInternal
                                     : TestState::TestingExternal;
    SimRowUi* row = RowForSlot(slot);
    if (row != nullptr) {
        SetHint(row, "测试中…", false);
    }

    if (xTaskCreate(Cell4gTestTask, "cell4g_test", 8192,
                    reinterpret_cast<void*>(static_cast<intptr_t>(slot)), 5,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG, "create test task failed");
        s_state = TestState::Idle;
        if (row != nullptr) {
            SetHint(row, "任务创建失败", true);
            TestUiUpdateStatus(row->status_icon, false);
        }
        if (s_auto_sequence && slot == kSimSlotInternal) {
            StartTest(kSimSlotExternal);
        } else if (s_auto_sequence) {
            s_auto_sequence = false;
        }
    }
}

void OnAutoStartTimer(lv_timer_t* /*t*/) {
    s_auto_start_timer = nullptr;
    if (!s_loaded) {
        return;
    }
    ESP_LOGI(TAG, "auto start: internal SIM first");
    StartTest(kSimSlotInternal);
}

lv_obj_t* CreateSubRow(lv_obj_t* parent, const char* title, int slot,
                       SimRowUi* ui) {
    lv_obj_t* row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_set_size(row, kTestPanelW - 2 * kTestSideMargin - 32, kTestRowH - 8);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);

    lv_obj_t* status = lv_image_create(row);
    lv_obj_set_size(status, 36, 36);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_width(label, 200);

    lv_obj_t* hint = lv_label_create(row);
    lv_label_set_text(hint, "等待测试…");
    lv_obj_set_style_text_color(hint, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_flex_grow(hint, 1);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    ui->status_icon = status;
    ui->hint_lbl    = hint;
    ui->slot        = slot;
    return row;
}

}  // namespace

namespace Cell4gTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* card = lv_obj_create(list);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kTestPanelW - 2 * kTestSideMargin, kTestRowH * 2 + 12);
    lv_obj_set_style_bg_color(card, lv_color_hex(kTestColorCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    CreateSubRow(card, "4G·内置卡", kSimSlotInternal, &s_internal);
    CreateSubRow(card, "4G·外置卡", kSimSlotExternal, &s_external);
}

void OnLoad() {
    s_loaded = true;
    s_state = TestState::Idle;
    s_auto_sequence = true;
    SetHint(&s_internal, "等待测试…", false);
    SetHint(&s_external, "等待测试…", false);

    StopAutoStartTimer();
    s_auto_start_timer =
        lv_timer_create(OnAutoStartTimer, kAutoStartDelayMs, nullptr);
    lv_timer_set_repeat_count(s_auto_start_timer, 1);
}

void OnUnload() {
    s_loaded = false;
    s_auto_sequence = false;
    s_state = TestState::Idle;
    s_modem_busy = false;
    s_last_cfun_ms = 0;
    StopAutoStartTimer();
    s_internal = SimRowUi{};
    s_external = SimRowUi{};
}

}  // namespace Cell4gTest
