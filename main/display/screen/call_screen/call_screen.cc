#include "call_screen.h"

#include "home_screen/home_screen.h"
#include "screen_util.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "lvgl.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "IOExpander.hpp"
#include "board.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "settings.h"

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);

// ---------------------------------------------------------------------------
// 720x720 layout
//
//  +-----------------------------------------------+ y=0
//  |  [◀]  电话                                    |  header (h=88)
//  +-----------------------------------------------+ y=88
//  |              138 1234 5678          ⌫         |  number display (h=88)
//  |              (status line)                     |
//  +-----------------------------------------------+ y=176
//  |   [1]   [2 ABC]   [3 DEF]                      |
//  |   [4 GHI] [5 JKL] [6 MNO]                      |  keypad 4x3
//  |   [7 PQRS][8 TUV] [9 WXYZ]                     |  buttons d=96
//  |   [*]    [0 +]    [#]                          |
//  +-----------------------------------------------+ y=~586
//  |         [  call/hangup btn  ]                  |  action bar (h=128)
//  +-----------------------------------------------+ y=720
// ---------------------------------------------------------------------------

namespace {

constexpr const char* TAG = "CallScreen";

constexpr int kPanelSize    = 720;
constexpr int kPad          = 16;
constexpr int kBackBtnSize  = 72;   // 与其他页面一致的返回按钮点击区域
constexpr int kHeaderH      = 88;   // 容纳 72px 返回按钮 + 上下留白
// 号码区略压缩，把空间让给拨号盘上移并与底部拨打键拉开间距。
constexpr int kNumberAreaH  = 88;
constexpr int kKeypadY      = kHeaderH + kNumberAreaH;
constexpr int kActionH      = 128;
constexpr int kKeypadH      = kPanelSize - kKeypadY - kActionH;
constexpr int kKeypadTopPad = 6;   // 拨号盘顶对齐，不再垂直居中下压

// ----- number area sub-layout ----------------------------------------------
// We hand-place the number label, the status line, and the backspace button
// so the backspace's vertical center sits exactly on the number's vertical
// center -- making the trio read as a single horizontal row.
constexpr int kNumberLblTop = kHeaderH + 8;
constexpr int kNumberLblH   = 44;                  // hugs the 30px font
constexpr int kStatusLblTop = kNumberLblTop + kNumberLblH + 6;
constexpr int kStatusLblH   = 28;
constexpr int kBackspaceBtnSize  = 80;             // visible diameter
constexpr int kBackspaceClickExt = 16;             // adds 16px to every side
                                                   // -> 112x112 effective tap
constexpr int kBackspaceY   =
    kNumberLblTop + (kNumberLblH - kBackspaceBtnSize) / 2;  // 64

constexpr int kKeypadRows   = 4;
constexpr int kKeypadCols   = 3;
constexpr int kKeypadColGap = 40;
constexpr int kKeypadRowGap = 6;
constexpr int kKeypadHorPad = 80;  // outer padding inside the keypad strip
constexpr int kKeyDiameter  = 96;  // circular digit buttons (was 80, ↑ 20%)

// Action button (call / hangup) -- 贴近屏幕底部，与拨号盘留出空隙。
constexpr int kActionBtnD          = 96;
constexpr int kActionBtnBottomPad  = 18;

// ----- iOS-inspired dark phone palette -------------------------------------
constexpr uint32_t kColorBg            = 0x000000;
constexpr uint32_t kColorTextPrimary   = 0xFFFFFF;
constexpr uint32_t kColorTextSecondary = 0x9A9A9A;
constexpr uint32_t kColorHintText      = 0x6E6E70;

constexpr uint32_t kKeyBg          = 0x303030;
constexpr uint32_t kKeyBgPress     = 0x595959;
constexpr uint32_t kKeyText        = 0xFFFFFF;
constexpr uint32_t kKeySubText     = 0xB0B0B0;

constexpr uint32_t kCallBg         = 0x34C759;   // iOS green
constexpr uint32_t kCallBgPress    = 0x66D684;
constexpr uint32_t kHangupBg       = 0xFF3B30;   // iOS red
constexpr uint32_t kHangupBgPress  = 0xFF6F66;

// ----- keypad table ---------------------------------------------------------
struct KeyDef {
    const char* digit;
    const char* sub;     // letters (e.g. "ABC"), may be empty
    int row, col;
};

const KeyDef kKeys[] = {
    {"1", "",     0, 0}, {"2", "ABC",  0, 1}, {"3", "DEF",  0, 2},
    {"4", "GHI",  1, 0}, {"5", "JKL",  1, 1}, {"6", "MNO",  1, 2},
    {"7", "PQRS", 2, 0}, {"8", "TUV",  2, 1}, {"9", "WXYZ", 2, 2},
    {"*", "",     3, 0}, {"0", "+",    3, 1}, {"#", "",     3, 2},
};

// ----- dialer state ---------------------------------------------------------
enum class CallState { kIdle, kCalling };

constexpr int  kMaxDigits   = 24;
char           s_number[kMaxDigits + 1];
CallState      s_call_state;

lv_obj_t* s_number_lbl;
lv_obj_t* s_status_lbl;
lv_obj_t* s_backspace_btn;
lv_obj_t* s_action_btn;        // single circular button at bottom
lv_obj_t* s_action_icon;       // image inside action_btn (dial / hangup)

// Tracks whether the call screen is currently mounted. lv_async_call() trampolines
// from the AT-task thread back to the LVGL thread; if the user already swiped back
// we MUST NOT touch the now-deleted UI objects.
bool s_screen_active = false;

// Bumped on every call-state transition. Carried by the AT task; on completion
// we drop the result if the epoch no longer matches (e.g. user swiped back, or
// hung up before ATD returned). This avoids a stale "dial OK" overwriting a
// freshly-idle UI.
uint32_t s_call_epoch = 0;

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

// Format a Chinese-style phone number: "13812345678" -> "138 1234 5678".
// For non-11-digit input we just show it as typed (still grouped lightly).
void FormatNumberForDisplay(const char* in, char* out, size_t out_sz) {
    size_t len = std::strlen(in);
    if (len == 0) {
        if (out_sz > 0) out[0] = '\0';
        return;
    }
    // Mobile-style: 3-4-4 grouping when exactly 11 digits.
    if (len == 11) {
        snprintf(out, out_sz, "%.3s %.4s %.4s", in, in + 3, in + 7);
        return;
    }
    // Otherwise insert a space every 4 chars from the left for readability.
    size_t out_len = 0;
    for (size_t i = 0; i < len && out_len + 1 < out_sz; ++i) {
        if (i > 0 && i % 4 == 0 && out_len + 1 < out_sz) {
            out[out_len++] = ' ';
        }
        out[out_len++] = in[i];
    }
    if (out_len < out_sz) out[out_len] = '\0';
}

void RefreshNumberDisplay() {
    char formatted[40];
    FormatNumberForDisplay(s_number, formatted, sizeof(formatted));
    if (formatted[0] == '\0') {
        // Placeholder hint when nothing has been typed.
        lv_label_set_text(s_number_lbl, "");
    } else {
        lv_label_set_text(s_number_lbl, formatted);
    }
    // Backspace only shown when there is something to delete and we are
    // not currently in a "call in progress" state.
    bool show_back = (s_number[0] != '\0' && s_call_state == CallState::kIdle);
    if (show_back) {
        lv_obj_remove_flag(s_backspace_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_backspace_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void RefreshActionButton() {
    if (s_call_state == CallState::kCalling) {
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kHangupBg),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kHangupBgPress),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
    } else {
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kCallBg),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kCallBgPress),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
    }
}

void RefreshStatus() {
    if (s_call_state == CallState::kCalling) {
        if (s_number[0] != '\0') {
            lv_label_set_text(s_status_lbl, "拨号中...");
        } else {
            lv_label_set_text(s_status_lbl, "");
        }
    } else {
        lv_label_set_text(s_status_lbl, "");
    }
}

void SetStatusText(const char* txt) {
    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, txt);
    }
}

// ---------------------------------------------------------------------------
// AT command plumbing
//
// 拨号 / 挂断按钮按下后，要把 AT 命令丢到一个临时 FreeRTOS task 里跑，
// 因为 UartEthModem::SendAt() 是同步阻塞的（最多 timeout_ms），不能在
// LVGL 主线程里直接调用，否则界面会卡住。
//
// 拨号流程会先发一条 AT+CPIN? 体检：
//   - 模组返回 "+CPIN: READY" 且带 OK 才继续 ATD<num>
//   - 否则提示 "请检查移动网络"，回 idle
//
// 任务结果通过 lv_async_call() 切回 LVGL 线程刷新 UI，
// 同时用 s_screen_active + s_call_epoch 双重保护，避免在屏幕已经销毁
// 或者状态已经变化（用户中途挂断/退出）时还去操作野指针。
// ---------------------------------------------------------------------------

Nt26Board* GetNt26Board() {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    if (dual != nullptr) {
        return dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    }
    return dynamic_cast<Nt26Board*>(&board);
}

// 读取 network_screen 写入的 NVS 偏好。0 = 外置卡（默认）、1 = 内置卡。
// 该值在 NetworkScreen 加载时通过 AT+ECSIMCFG? 与模组真实状态同步，所以
// 这里直接读 NVS 即可，无需在拨号关键路径上再发一次 AT 查询。
int GetSavedSimSlot() {
    Settings settings("network", true);
    int v = settings.GetInt("sim_slot", 0);
    return (v == 1) ? 1 : 0;
}

// 内置卡（SimSlot=1）出厂被定义为「数据卡」，没开 CS 语音业务，
// 直接 ATD 会被模组拒（NO CARRIER）。拨号前在 UI 层就拦下来，给用户
// 一条明确指引。
bool IsInternalSimActive() {
    return GetSavedSimSlot() == 1;
}

enum class AtJobKind : uint8_t {
    kDial,    // AT+CPIN? -> ATD<number>
    kHangup,  // ATH
};

enum class AtOutcome : uint8_t {
    kDialOk,         // ATD 收到 OK
    kSimNotReady,    // AT+CPIN? 没回 READY / 超时 / 模组不在
    kDialFailed,     // ATD 没回 OK（ERROR / NO CARRIER / 超时）
    kHangupDone,     // ATH 完成（成功与否都视作完成）
    kNo4G,           // 当前不是 4G 板（WiFi 模式）
};

struct AtJob {
    AtJobKind   kind;
    std::string number;   // only used for kDial
    uint32_t    epoch;    // 触发时记录的 s_call_epoch
};

struct AtResult {
    AtJobKind   kind;
    AtOutcome   outcome;
    uint32_t    epoch;
};

// 在 LVGL 线程里更新 UI（lv_async_call 的回调）。
void OnAtResult(void* user_data) {
    auto* res = static_cast<AtResult*>(user_data);
    // 屏幕已销毁，直接丢弃。
    if (!s_screen_active) {
        delete res;
        return;
    }
    // 状态已经变了（比如拨号还没回 OK 用户就按了挂断/返回），结果作废。
    if (res->epoch != s_call_epoch) {
        delete res;
        return;
    }

    if (res->kind == AtJobKind::kDial) {
        switch (res->outcome) {
            case AtOutcome::kDialOk:
                // ATD 已经收到 OK，正在通话中。
                SetStatusText("通话中");
                break;
            case AtOutcome::kSimNotReady:
                s_call_state = CallState::kIdle;
                ++s_call_epoch;
                RefreshActionButton();
                RefreshNumberDisplay();
                SetStatusText("请检查移动网络");
                break;
            case AtOutcome::kNo4G:
                s_call_state = CallState::kIdle;
                ++s_call_epoch;
                RefreshActionButton();
                RefreshNumberDisplay();
                SetStatusText("无 4G 模块");
                break;
            case AtOutcome::kDialFailed:
            default:
                s_call_state = CallState::kIdle;
                ++s_call_epoch;
                RefreshActionButton();
                RefreshNumberDisplay();
                SetStatusText("拨号失败");
                break;
        }
    } else {
        // 挂断的反馈不是必须展示的，简单清空状态行即可。
        if (s_call_state == CallState::kIdle) {
            SetStatusText("");
        }
    }
    delete res;
}

void AtJobTask(void* arg) {
    auto* job = static_cast<AtJob*>(arg);
    auto* result = new AtResult{};
    result->kind  = job->kind;
    result->epoch = job->epoch;

    auto* nt26 = GetNt26Board();
    if (nt26 == nullptr) {
        ESP_LOGW(TAG, "AT job kind=%d: 当前不在 4G 模式，跳过",
                 (int)job->kind);
        result->outcome = (job->kind == AtJobKind::kDial)
                              ? AtOutcome::kNo4G
                              : AtOutcome::kHangupDone;
    } else if (job->kind == AtJobKind::kHangup) {
        std::string resp;
        esp_err_t err = nt26->SendAtCommand("ATH", resp, 5000);
        ESP_LOGI(TAG, "AT 'ATH' -> err=%d resp='%s'", (int)err, resp.c_str());
        result->outcome = AtOutcome::kHangupDone;
    } else {
        // === Dial flow: AT+CPIN? -> ATD<number> ===
        std::string cpin_resp;
        esp_err_t cpin_err =
            nt26->SendAtCommand("AT+CPIN?", cpin_resp, 3000);
        ESP_LOGI(TAG, "AT 'AT+CPIN?' -> err=%d resp='%s'",
                 (int)cpin_err, cpin_resp.c_str());

        const bool sim_ready =
            (cpin_err == ESP_OK &&
             cpin_resp.find("READY") != std::string::npos &&
             cpin_resp.find("OK") != std::string::npos);

        if (!sim_ready) {
            result->outcome = AtOutcome::kSimNotReady;
        } else {
            std::string atd = "ATD";
            atd += job->number;
            std::string atd_resp;
            // ATD 通常 1-3s 回 OK；放宽到 10s 给模组留余量。
            esp_err_t err = nt26->SendAtCommand(atd, atd_resp, 10000);
            ESP_LOGI(TAG, "AT '%s' -> err=%d resp='%s'",
                     atd.c_str(), (int)err, atd_resp.c_str());
            result->outcome = (err == ESP_OK &&
                               atd_resp.find("OK") != std::string::npos)
                                  ? AtOutcome::kDialOk
                                  : AtOutcome::kDialFailed;
        }
    }

    lv_async_call(OnAtResult, result);
    delete job;
    vTaskDelete(nullptr);
}

void DispatchDial(const std::string& number) {
    auto* job = new AtJob{AtJobKind::kDial, number, s_call_epoch};
    BaseType_t r = xTaskCreate(AtJobTask, "call_at", 4096, job,
                               tskIDLE_PRIORITY + 2, nullptr);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(call_at dial) failed");
        delete job;
        SetStatusText("系统忙");
    }
}

void DispatchHangup() {
    auto* job = new AtJob{AtJobKind::kHangup, "", s_call_epoch};
    BaseType_t r = xTaskCreate(AtJobTask, "call_at", 4096, job,
                               tskIDLE_PRIORITY + 2, nullptr);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(call_at hangup) failed");
        delete job;
    }
}

// ---------------------------------------------------------------------------
// State mutations
// ---------------------------------------------------------------------------

void AppendDigit(const char* d) {
    if (s_call_state == CallState::kCalling) return;
    size_t len = std::strlen(s_number);
    if (len >= kMaxDigits) return;
    s_number[len]     = d[0];
    s_number[len + 1] = '\0';
    RefreshNumberDisplay();
}

void Backspace() {
    if (s_call_state == CallState::kCalling) return;
    size_t len = std::strlen(s_number);
    if (len == 0) return;
    s_number[len - 1] = '\0';
    RefreshNumberDisplay();
}

void StartCall() {
    if (s_number[0] == '\0') return;

    // 内置卡（数据卡）不能拨号。保持 idle 状态，只在状态栏给出指引，
    // 让用户去 网络配置 → SIM 卡切换 把卡换成外置卡再拨。
    if (IsInternalSimActive()) {
        SetStatusText("内置卡无法拨打电话，请切换到外置卡");
        return;
    }

    s_call_state = CallState::kCalling;
    ++s_call_epoch;
    RefreshActionButton();
    RefreshNumberDisplay();
    // 拨号前要先体检 SIM，UI 上先告诉用户在做什么，避免好像“按下没反应”。
    SetStatusText("正在检查网络...");

    DispatchDial(s_number);
}

void HangupCall() {
    const bool was_calling = (s_call_state == CallState::kCalling);
    s_call_state = CallState::kIdle;
    ++s_call_epoch;
    RefreshActionButton();
    RefreshStatus();
    RefreshNumberDisplay();

    if (was_calling) {
        // 主动挂断: 通知模组释放当前通话。即便 ATD 还没回 OK，
        // 这条 ATH 也能被串行化进 modem 的 AT 队列（at_mutex_）。
        DispatchHangup();
    }
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

void KeyEventCb(lv_event_t* e) {
    const KeyDef* k = static_cast<const KeyDef*>(lv_event_get_user_data(e));
    if (k == nullptr) return;
    AppendDigit(k->digit);
}

void BackspaceEventCb(lv_event_t* e) {
    (void)e;
    Backspace();
}

// Long-press on backspace clears the whole number, matching iPhone behaviour.
void BackspaceLongPressCb(lv_event_t* e) {
    (void)e;
    if (s_call_state == CallState::kCalling) return;
    s_number[0] = '\0';
    RefreshNumberDisplay();
}

void ActionEventCb(lv_event_t* e) {
    (void)e;
    if (s_call_state == CallState::kIdle) {
        StartCall();
    } else {
        HangupCall();
    }
}

void OnSwipeBack() {
    // 函数名保留 OnSwipeBack 是历史叫法；现在只由左上角返回按钮触发。
    // 如果用户在通话中点返回，主动给模组挂断一下，避免通话还挂着。
    if (s_call_state == CallState::kCalling) {
        DispatchHangup();
    }
    s_call_state = CallState::kIdle;
    ++s_call_epoch;
    s_screen_active = false;

    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnScreenUnloaded(lv_event_t* e) {
    (void)e;
    // 屏幕被 LVGL 卸载（无论是 swipe-back 还是别的路径），统一在这里把
    // active 标记关掉。AT 任务回来时会看到 false 并丢弃 UI 更新。
    s_screen_active = false;
    s_number_lbl     = nullptr;
    s_status_lbl     = nullptr;
    s_backspace_btn  = nullptr;
    s_action_btn     = nullptr;
    s_action_icon    = nullptr;
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

void BuildHeader(lv_obj_t* parent) {
    // 左上角返回按钮：72x72 透明圆形按钮 + ic_app_back 图标（与其它页面一致）。
    // 点击复用 OnSwipeBack —— 它已经处理了"通话中点返回先挂断再退出"。
    lv_obj_t* back_btn = lv_button_create(parent);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, kBackBtnSize, kBackBtnSize);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, kPad,
                 (kHeaderH - kBackBtnSize) / 2);
    lv_obj_add_event_cb(
        back_btn, [](lv_event_t*) { OnSwipeBack(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    // 标题放在按钮右侧，与按钮垂直居中对齐。
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "电话");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, kPad + kBackBtnSize + 16,
                 (kHeaderH - 30) / 2);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // 注：本屏不挂右滑返回手势——拨号过程中横滑数字键太容易被误判成返
    // 回手势，所以只保留左上角的明确按钮作为唯一返回入口。
}

void BuildNumberArea(lv_obj_t* parent) {
    // Big number label, centered.  Sized to hug the font height so the
    // backspace button can vertically align with the actual text rather
    // than a tall padded box.
    s_number_lbl = lv_label_create(parent);
    lv_label_set_text(s_number_lbl, "");
    lv_obj_set_style_text_color(s_number_lbl,
                                lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_number_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_number_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_number_lbl, LV_LABEL_LONG_SCROLL);
    lv_obj_set_size(s_number_lbl,
                    kPanelSize - 2 * kPad - kBackspaceBtnSize - 24,
                    kNumberLblH);
    lv_obj_align(s_number_lbl, LV_ALIGN_TOP_MID, 0, kNumberLblTop);
    lv_obj_remove_flag(s_number_lbl, LV_OBJ_FLAG_CLICKABLE);

    // Status line under the number ("拨号中...", etc.)
    s_status_lbl = lv_label_create(parent);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl,
                                lv_color_hex(kColorTextSecondary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_size(s_status_lbl, kPanelSize - 2 * kPad, kStatusLblH);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, kStatusLblTop);
    lv_obj_remove_flag(s_status_lbl, LV_OBJ_FLAG_CLICKABLE);

    // Backspace button on the right of the number, centered on the number's
    // vertical midline.  We keep the visual circle at 80px but extend the
    // hit-test area by 16px on every side so the effective tap target is
    // 112x112 -- much easier to land than the previous 64px region.
    s_backspace_btn = lv_button_create(parent);
    lv_obj_set_size(s_backspace_btn, kBackspaceBtnSize, kBackspaceBtnSize);
    lv_obj_align(s_backspace_btn, LV_ALIGN_TOP_RIGHT, -kPad - 4, kBackspaceY);
    lv_obj_set_ext_click_area(s_backspace_btn, kBackspaceClickExt);
    lv_obj_set_style_bg_opa(s_backspace_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_backspace_btn, LV_OPA_30,
                            LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_backspace_btn, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_backspace_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_backspace_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_backspace_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_backspace_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_backspace_btn, BackspaceEventCb, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_add_event_cb(s_backspace_btn, BackspaceLongPressCb,
                        LV_EVENT_LONG_PRESSED, nullptr);

    lv_obj_t* back_lbl = lv_image_create(s_backspace_btn);
    lv_image_set_src(back_lbl, "A:ic_s_call_delete.spng");
    lv_obj_remove_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_lbl);

    // Hidden until the user types a digit.
    lv_obj_add_flag(s_backspace_btn, LV_OBJ_FLAG_HIDDEN);
}

void StyleKeyButton(lv_obj_t* btn) {
    lv_obj_set_style_bg_color(btn, lv_color_hex(kKeyBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kKeyBgPress),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
}

void BuildKeypad(lv_obj_t* parent) {
    // Total used width inside the keypad strip:
    //   3 * kKeyDiameter + 2 * kKeypadColGap = 3*96 + 2*40 = 368
    // Total used height:
    //   4 * kKeyDiameter + 3 * kKeypadRowGap = 4*96 + 3*6 = 402
    // 顶对齐并留 kKeypadTopPad，避免拨号盘被垂直居中后贴住底部拨打键。
    const int row_w =
        kKeypadCols * kKeyDiameter + (kKeypadCols - 1) * kKeypadColGap;
    const int col_h =
        kKeypadRows * kKeyDiameter + (kKeypadRows - 1) * kKeypadRowGap;
    const int x_origin = (kPanelSize - row_w) / 2;
    const int y_origin = kKeypadY + kKeypadTopPad;
    (void)kKeypadHorPad;  // not needed once we center exactly

    for (const auto& k : kKeys) {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_set_size(btn, kKeyDiameter, kKeyDiameter);
        lv_obj_set_pos(btn,
                       x_origin + k.col * (kKeyDiameter + kKeypadColGap),
                       y_origin + k.row * (kKeyDiameter + kKeypadRowGap));
        StyleKeyButton(btn);
        lv_obj_add_event_cb(btn, KeyEventCb, LV_EVENT_CLICKED,
                            const_cast<KeyDef*>(&k));

        // Inner column: digit on top, sub-letters below.
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t* digit = lv_label_create(btn);
        lv_label_set_text(digit, k.digit);
        lv_obj_set_style_text_color(digit, lv_color_hex(kKeyText), LV_PART_MAIN);
        lv_obj_set_style_text_font(digit, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_remove_flag(digit, LV_OBJ_FLAG_CLICKABLE);

        if (k.sub != nullptr && k.sub[0] != '\0') {
            lv_obj_t* sub = lv_label_create(btn);
            lv_label_set_text(sub, k.sub);
            lv_obj_set_style_text_color(sub, lv_color_hex(kKeySubText),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(sub, &font_puhui_20_4, LV_PART_MAIN);
            lv_obj_set_style_pad_top(sub, 2, LV_PART_MAIN);
            lv_obj_remove_flag(sub, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

void BuildActionBar(lv_obj_t* parent) {
    s_action_btn = lv_button_create(parent);
    lv_obj_set_size(s_action_btn, kActionBtnD, kActionBtnD);
    lv_obj_align(s_action_btn, LV_ALIGN_BOTTOM_MID, 0, -kActionBtnBottomPad);
    lv_obj_set_style_radius(s_action_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_action_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_action_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_action_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_action_btn, ActionEventCb, LV_EVENT_CLICKED, nullptr);

    s_action_icon = lv_image_create(s_action_btn);
    lv_image_set_src(s_action_icon, "A:ic_s_call_diall.spng");
    lv_image_set_inner_align(s_action_icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(s_action_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_action_icon);
}

}  // namespace

lv_obj_t* CallScreen::Create() {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);

    s_number[0] = '\0';
    s_call_state = CallState::kIdle;
    ++s_call_epoch;
    s_screen_active = true;

    BuildHeader(scr);
    BuildNumberArea(scr);
    BuildKeypad(scr);
    BuildActionBar(scr);

    RefreshNumberDisplay();
    RefreshActionButton();
    RefreshStatus();

    // 屏幕被卸载时清理 active 标志，确保 lv_async_call 不会回到野指针。
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    // 注：本屏不再挂右滑返回手势——拨号过程中误触概率太高（横滑数字键
    // 容易扫成返回）。返回入口只保留左上角的明确按钮 OnSwipeBack()。

    return scr;
}

void CallScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    auto& io_expander = IOExpander::getInstance();
    if (event == SCREEN_LIFECYCLE_LOAD) {
        // 进入拨号界面：把功放切到 4G 通话路径。
        ESP_LOGI(TAG, "load: PA_SWITCH=false (route to 4G)");
        io_expander.setLevel(IOExpander::Pin::PA_SWITCH, false);
    } else {
        // 退出拨号界面：恢复默认 WIFI/本地音频路径。
        ESP_LOGI(TAG, "unload: PA_SWITCH=true (route to WIFI)");
        io_expander.setLevel(IOExpander::Pin::PA_SWITCH, true);

        // 兜底：如果离开时仍处于通话态（理论上 OnSwipeBack 已经处理过，
        // 但如果是别的路径触发的卸载，这里保险一下），主动挂断。
        if (s_call_state == CallState::kCalling) {
            DispatchHangup();
            s_call_state = CallState::kIdle;
            ++s_call_epoch;
        }
        s_screen_active = false;
    }
}
