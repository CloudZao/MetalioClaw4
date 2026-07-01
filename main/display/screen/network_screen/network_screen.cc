#include "network_screen.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "ssid_manager.h"
#include "wifi_station.h"

#include "board.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "settings.h"

#include "home_screen/home_screen.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "NetworkScreen";

// ---------------------------------------------------------------------------
// 视觉常量
// ---------------------------------------------------------------------------
constexpr int kPanelW = 720;
constexpr int kPanelH = 720;
constexpr int kHeaderH = 90;

constexpr uint32_t kColorBg         = 0x0E1116;
constexpr uint32_t kColorCard       = 0x1B2030;
constexpr uint32_t kColorBtn        = 0x2A2F3A;
constexpr uint32_t kColorBtnActive  = 0x3B82F6;
constexpr uint32_t kColorBtnDanger  = 0xDC2626;
constexpr uint32_t kColorBtnAccent  = 0x10B981;
constexpr uint32_t kColorText       = 0xFFFFFF;
constexpr uint32_t kColorSubtle     = 0x9AA3B2;
constexpr uint32_t kColorSuccess    = 0x34C759;
constexpr uint32_t kColorError      = 0xFF3B30;
constexpr uint32_t kColorScanning   = 0xF59E0B;
constexpr uint32_t kColorListBg     = 0x12151C;
constexpr uint32_t kColorItem       = 0x202736;
constexpr uint32_t kColorItemSel    = 0x2F3A52;

constexpr size_t kMaxSsidLen = 32;
constexpr size_t kMaxPasswordLen = 64;

// ---------------------------------------------------------------------------
// FreeRTOS / WiFi 事件位
// ---------------------------------------------------------------------------
constexpr EventBits_t kBitScanDone     = BIT0;
constexpr EventBits_t kBitConnected    = BIT1;
constexpr EventBits_t kBitDisconnected = BIT2;

// ---------------------------------------------------------------------------
// 数据
// ---------------------------------------------------------------------------
struct ApItem {
    std::string ssid;
    int8_t rssi = -127;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
};

struct UiState {
    lv_obj_t* screen        = nullptr;
    lv_obj_t* status_label  = nullptr;
    lv_obj_t* back_btn      = nullptr;
    lv_obj_t* scan_btn      = nullptr;
    lv_obj_t* scan_btn_lbl  = nullptr;
    lv_obj_t* tabview       = nullptr;
    lv_obj_t* nearby_tab    = nullptr;  // tabview 的 tab 容器
    lv_obj_t* saved_tab     = nullptr;
    lv_obj_t* network_tab   = nullptr;
    lv_obj_t* sim_tab       = nullptr;
    lv_obj_t* nearby_list   = nullptr;  // 实际放 item 的 flex 容器
    lv_obj_t* nearby_spinner = nullptr; // 扫描中悬浮在列表中央的圆环 spinner
    lv_obj_t* saved_list    = nullptr;
    lv_obj_t* clear_btn     = nullptr;
    // 上网方式切换（WiFi / 4G）：和 SIM 卡切换一致的两按钮选择器
    lv_obj_t* network_wifi_btn    = nullptr;
    lv_obj_t* network_wifi_lbl    = nullptr;
    lv_obj_t* network_cell_btn    = nullptr;
    lv_obj_t* network_cell_lbl    = nullptr;
    lv_obj_t* network_current_lbl = nullptr;
    // SIM 卡切换（仅 4G 模式）
    lv_obj_t* sim_external_btn    = nullptr;
    lv_obj_t* sim_external_lbl    = nullptr;
    lv_obj_t* sim_internal_btn    = nullptr;
    lv_obj_t* sim_internal_lbl    = nullptr;
    lv_obj_t* sim_current_lbl     = nullptr;
    // 密码键盘弹窗
    lv_obj_t* pwd_overlay   = nullptr;
    lv_obj_t* pwd_textarea  = nullptr;
    lv_obj_t* pwd_keyboard  = nullptr;
    lv_obj_t* pwd_title     = nullptr;
    lv_obj_t* pwd_show_chk  = nullptr;
    // 连接进度 / 成功提示弹窗（盖在 pwd_overlay 之上的全屏模态）
    lv_obj_t* status_overlay     = nullptr;
    lv_obj_t* status_message_lbl = nullptr;
};

UiState s_ui;

std::vector<ApItem>  s_scan_results;
bool                 s_screen_active = false;
bool                 s_wifi_initialized = false;
bool                 s_scan_in_progress = false;
bool                 s_connect_in_progress = false;
std::string          s_pending_ssid;        // 当前正在弹键盘 / 连接的 SSID
wifi_auth_mode_t     s_pending_authmode = WIFI_AUTH_OPEN;

esp_netif_t*         s_netif = nullptr;
esp_event_handler_instance_t s_wifi_evt_inst = nullptr;
esp_event_handler_instance_t s_ip_evt_inst   = nullptr;
EventGroupHandle_t   s_evt_group = nullptr;
// 最近一次 STA_DISCONNECTED 事件携带的 reason 码，用来判断是密码错误还是 AP
// 拒绝等。0 表示「没拿到原因」。
uint8_t              s_last_disconnect_reason = 0;

// 重启倒计时（连接成功后用）。kRestartCountdownSec 秒后自动 esp_restart。
constexpr int        kRestartCountdownSec = 3;
lv_timer_t*          s_restart_timer = nullptr;
int                  s_restart_remaining = 0;
std::string          s_restart_ssid;
// 记录进入页面前 WifiStation 是否已经在跑（即设备网络模式是 WiFi），
// 用来决定离开时是否恢复 WifiStation::Start()。ML307 模式下 WifiStation
// 根本没起过，恢复时跳过即可，避免空跑一份 wifi 栈。
bool                 s_wifi_station_was_active = false;
bool                 s_network_switch_pending = false;

// 上网方式（network/type NVS key）：与 DualNetworkBoard::LoadNetworkTypeFromSettings
// 一致。0 = WiFi，1 = 4G（蜂窝模组）。切换由 DualNetworkBoard::SwitchNetworkType()
// 主导，并触发设备重启。
constexpr int        kNetTypeWifi     = 0;
constexpr int        kNetTypeCellular = 1;

// SIM 卡相关。
//   - kSimSlotExternal = 0：外置卡（默认）
//   - kSimSlotInternal = 1：内置卡
// 切换通过 AT+ECSIMCFG=SimSlot,X 完成，需要 AT+CFUN=0 / AT+CFUN=1 包夹。
// s_sim_switch_pending 用来在切换期间禁止重复按按钮。
constexpr int        kSimSlotExternal = 0;
constexpr int        kSimSlotInternal = 1;
bool                 s_sim_switch_pending = false;

// 前向声明
void post_status(const char* text, uint32_t color = kColorText);
void refresh_saved_list();
void refresh_nearby_list();
void rebuild_nearby_list_now();
void rebuild_saved_list_now();
void open_password_popup(const std::string& ssid, wifi_auth_mode_t authmode);
void close_password_popup();
void schedule_scan();
void schedule_connect(const std::string& ssid, const std::string& password);
void open_connecting_popup(const std::string& ssid);
void open_success_popup(const std::string& ssid);
void show_failure_in_status_popup(const std::string& title,
                                  const std::string& detail,
                                  uint32_t auto_close_ms = 2500);
void close_status_popup();
void refresh_network_switch_ui();
void open_switch_reboot_popup(const char* target_name);
void schedule_network_switch(int target_type);
void refresh_sim_slot_ui();
void open_sim_switching_popup(int target_slot);
void schedule_sim_slot_query();

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
const char* auth_label(wifi_auth_mode_t mode) {
    return (mode == WIFI_AUTH_OPEN) ? "[开放]" : "[加密]";
}

// 把 STA_DISCONNECTED 的 reason 码翻成人话。常见值对应密码错误 / AP 找不到 /
// 鉴权 / 关联超时等几大类。
const char* disconnect_reason_text(uint8_t reason) {
    switch (reason) {
        // —— 密码 / 鉴权类 —— 用户最关心的「密码错了」全在这里
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_LEAVE:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        case WIFI_REASON_GROUP_CIPHER_INVALID:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return "密码错误，请重新输入";
        case WIFI_REASON_NO_AP_FOUND:
            return "未找到该 WiFi（信号丢失）";
        case WIFI_REASON_ASSOC_EXPIRE:
        case WIFI_REASON_ASSOC_TOOMANY:
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_NOT_ASSOCED:
            return "关联失败，路由器拒绝连接";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "信号太弱，连接超时";
        case 0:
            return "连接失败";
        default:
            return nullptr;  // 调用方自己拼 reason=xx
    }
}

const char* rssi_quality_text(int8_t rssi) {
    if (rssi >= -55) return "信号强";
    if (rssi >= -65) return "信号较强";
    if (rssi >= -75) return "信号中";
    if (rssi >= -85) return "信号弱";
    return "信号很弱";
}

bool screen_alive() { return s_screen_active && s_ui.screen != nullptr; }

int GetSavedNetworkType() {
    // 与 DualNetworkBoard 启动时读 NVS 的逻辑一致（默认 4G=1，与 xingzhi-395 板级一致）
    const NetworkType type =
        DualNetworkBoard::LoadNetworkTypeFromSettings(kNetTypeCellular);
    return type == NetworkType::ML307 ? kNetTypeCellular : kNetTypeWifi;
}

DualNetworkBoard* GetDualNetworkBoard() {
    return dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
}

// 当前是否处于 4G（蜂窝）模式。Settings 中 "network/type" 的语义：
// 0 = WiFi、1 = 4G/ML307。4G 模式下我们不展示「附近 WiFi」「已保存 WiFi」
// 两个 Tab，也不会启动本地 STA 栈做扫描。
bool IsCellularMode() {
    return GetSavedNetworkType() == 1;
}

// 返回当前使用的 4G 模组 Board，便于直接调用 SendAtCommand。
// 在 WiFi 模式 / 未配 4G 板时返回 nullptr。
Nt26Board* GetNt26Board() {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    if (dual != nullptr) {
        return dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    }
    return dynamic_cast<Nt26Board*>(&board);
}

// 读取/写入用户选择的 SIM 槽位。默认 0 = 外置卡。这个值只用于 UI 高亮，
// 模组本身的当前槽位由 AT+ECSIMCFG 命令直接持久化在模组里。
int GetSavedSimSlot() {
    Settings settings("network", true);
    int v = settings.GetInt("sim_slot", kSimSlotExternal);
    return (v == kSimSlotInternal) ? kSimSlotInternal : kSimSlotExternal;
}

void SaveSimSlot(int slot) {
    Settings settings("network", true);
    settings.SetInt("sim_slot", slot);
}

const char* SimSlotName(int slot) {
    return (slot == kSimSlotInternal) ? "内置卡" : "外置卡";
}

// ---------------------------------------------------------------------------
// 异步 UI 更新（worker task -> LVGL 线程）
// ---------------------------------------------------------------------------
struct AsyncStatusMsg {
    char text[160];
    uint32_t color;
};

void async_update_status(void* user_data) {
    auto* msg = static_cast<AsyncStatusMsg*>(user_data);
    if (screen_alive() && s_ui.status_label != nullptr) {
        lv_label_set_text(s_ui.status_label, msg->text);
        lv_obj_set_style_text_color(s_ui.status_label, lv_color_hex(msg->color), LV_PART_MAIN);
    }
    delete msg;
}

void post_status(const char* text, uint32_t color) {
    if (!s_screen_active) return;
    auto* msg = new AsyncStatusMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    msg->color = color;
    lv_async_call(async_update_status, msg);
}

void async_rebuild_nearby(void* /*user_data*/) {
    if (screen_alive()) {
        rebuild_nearby_list_now();
    }
}

void async_rebuild_saved(void* /*user_data*/) {
    if (screen_alive()) {
        rebuild_saved_list_now();
    }
}

void refresh_nearby_list() {
    if (!s_screen_active) return;
    lv_async_call(async_rebuild_nearby, nullptr);
}

void refresh_saved_list() {
    if (!s_screen_active) return;
    lv_async_call(async_rebuild_saved, nullptr);
}

void async_set_scan_btn_enabled(void* user_data) {
    if (!screen_alive() || s_ui.scan_btn == nullptr) return;
    const bool enabled = (user_data != nullptr);
    if (enabled) {
        lv_obj_remove_state(s_ui.scan_btn, LV_STATE_DISABLED);
        if (s_ui.scan_btn_lbl != nullptr) lv_label_set_text(s_ui.scan_btn_lbl, "扫描");
    } else {
        lv_obj_add_state(s_ui.scan_btn, LV_STATE_DISABLED);
        if (s_ui.scan_btn_lbl != nullptr) lv_label_set_text(s_ui.scan_btn_lbl, "扫描中…");
    }
}

void post_scan_btn_enabled(bool enabled) {
    if (!s_screen_active) return;
    lv_async_call(async_set_scan_btn_enabled,
                  reinterpret_cast<void*>(static_cast<intptr_t>(enabled ? 1 : 0)));
}

// ---- 「附近 WiFi」列表中央的圆环 spinner --------------------------------
// scan_task 在初始化 / 扫描期间打开，列表填充完成 / 出错时关闭。spinner 自
// 身用 LVGL 的内置动画，不需要我们手动驱动。
void async_set_nearby_spinner(void* user_data) {
    if (!screen_alive() || s_ui.nearby_spinner == nullptr) return;
    const bool show = (user_data != nullptr);
    if (show) {
        lv_obj_remove_flag(s_ui.nearby_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ui.nearby_spinner);
    } else {
        lv_obj_add_flag(s_ui.nearby_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

void post_nearby_spinner(bool show) {
    if (!s_screen_active) return;
    lv_async_call(async_set_nearby_spinner,
                  reinterpret_cast<void*>(static_cast<intptr_t>(show ? 1 : 0)));
}

struct AsyncStringMsg {
    std::string text;
};

void async_open_connecting(void* user_data) {
    auto* msg = static_cast<AsyncStringMsg*>(user_data);
    if (screen_alive()) open_connecting_popup(msg->text);
    delete msg;
}

void post_open_connecting(const std::string& ssid) {
    if (!s_screen_active) return;
    lv_async_call(async_open_connecting, new AsyncStringMsg{ssid});
}

void async_open_success_and_reboot(void* user_data) {
    auto* msg = static_cast<AsyncStringMsg*>(user_data);
    if (screen_alive()) {
        // 成功后把密码弹窗也关掉，让 success 弹窗成为唯一顶层 UI
        close_password_popup();
        open_success_popup(msg->text);
    }
    delete msg;
}

void post_open_success_and_reboot(const std::string& ssid) {
    if (!s_screen_active) return;
    lv_async_call(async_open_success_and_reboot, new AsyncStringMsg{ssid});
}

void async_close_status(void* /*user_data*/) {
    if (screen_alive()) close_status_popup();
}

void post_close_status_popup() {
    if (!s_screen_active) return;
    lv_async_call(async_close_status, nullptr);
}

struct AsyncFailureMsg {
    std::string title;
    std::string detail;
    uint32_t    auto_close_ms;
};

void async_show_failure(void* user_data) {
    auto* msg = static_cast<AsyncFailureMsg*>(user_data);
    if (screen_alive()) {
        show_failure_in_status_popup(msg->title, msg->detail, msg->auto_close_ms);
    }
    delete msg;
}

void post_show_failure(const std::string& title, const std::string& detail,
                       uint32_t auto_close_ms = 2500) {
    if (!s_screen_active) return;
    lv_async_call(async_show_failure,
                  new AsyncFailureMsg{title, detail, auto_close_ms});
}

// ---------------------------------------------------------------------------
// WiFi 事件
// ---------------------------------------------------------------------------
void wifi_evt_handler(void* /*arg*/, esp_event_base_t base, int32_t id,
                      void* data) {
    if (s_evt_group == nullptr) return;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_SCAN_DONE) {
            xEventGroupSetBits(s_evt_group, kBitScanDone);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            auto* evt = static_cast<wifi_event_sta_disconnected_t*>(data);
            s_last_disconnect_reason = (evt != nullptr) ? evt->reason : 0;
            ESP_LOGW(TAG, "STA_DISCONNECTED reason=%u", s_last_disconnect_reason);
            xEventGroupSetBits(s_evt_group, kBitDisconnected);
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            xEventGroupSetBits(s_evt_group, kBitConnected);
        }
    }
}

// 初始化我们自己的 STA 栈。如果设备处于 WiFi 模式（WifiStation 已经在跑），
// 先把它停掉避免事件回调互相打架。返回是否成功。
bool wifi_init_for_screen() {
    if (s_wifi_initialized) return true;

    // 判断当前 wifi 栈是否已经在跑。esp_wifi_get_mode 在未初始化时会返回
    // ESP_ERR_WIFI_NOT_INIT —— 那种情况下我们不能调用 WifiStation::Stop()
    // （它内部走 ESP_ERROR_CHECK(esp_wifi_stop) 会触发 abort）。
    wifi_mode_t mode_before = WIFI_MODE_NULL;
    esp_err_t mode_err = esp_wifi_get_mode(&mode_before);
    s_wifi_station_was_active = (mode_err == ESP_OK && mode_before != WIFI_MODE_NULL);
    if (s_wifi_station_was_active) {
        WifiStation::GetInstance().Stop();
    }

    if (s_evt_group == nullptr) {
        s_evt_group = xEventGroupCreate();
    } else {
        xEventGroupClearBits(s_evt_group, 0xFFFFFF);
    }

    // 注意：netif / event loop 已经在更早阶段初始化，这里只补一下 default sta。
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %d", err);
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %d", err);
        return false;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == nullptr) {
        ESP_LOGE(TAG, "create wifi netif failed");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %d", err);
        return false;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt_handler, nullptr, &s_wifi_evt_inst);
    if (err != ESP_OK) ESP_LOGE(TAG, "wifi handler register failed: %d", err);
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_evt_handler, nullptr, &s_ip_evt_inst);
    if (err != ESP_OK) ESP_LOGE(TAG, "ip handler register failed: %d", err);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "wifi stack initialized for screen");
    return true;
}

void wifi_teardown_for_screen() {
    if (!s_wifi_initialized) return;
    esp_wifi_scan_stop();
    esp_wifi_disconnect();

    if (s_wifi_evt_inst != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt_inst);
        s_wifi_evt_inst = nullptr;
    }
    if (s_ip_evt_inst != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt_inst);
        s_ip_evt_inst = nullptr;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_netif != nullptr) {
        esp_netif_destroy(s_netif);
        s_netif = nullptr;
    }
    s_wifi_initialized = false;
    ESP_LOGI(TAG, "wifi stack torn down");

    // 只有进入页面前 WifiStation 在跑时（即 WiFi 模式）才恢复它；ML307
    // 模式下进入本页面前 wifi 栈本来就没起，不要无中生有起一份。
    if (s_wifi_station_was_active) {
        WifiStation::GetInstance().Start();
    }
    s_wifi_station_was_active = false;
}

// ---------------------------------------------------------------------------
// 扫描任务
// ---------------------------------------------------------------------------
void scan_task(void* /*arg*/) {
    s_scan_in_progress = true;
    post_scan_btn_enabled(false);
    post_nearby_spinner(true);
    // 扫描开始：触发一次列表重建，让 rebuild_nearby_list_now() 看到
    // s_scan_in_progress=true 后把「未发现网络…」那行旧提示清掉，
    // 只留 spinner 单独转。
    refresh_nearby_list();

    // 首次进入页面时 wifi 栈还没初始化，放到 task 内部完成，避免阻塞 LVGL
    // 线程导致 "点开 app 卡几秒才看到页面" 的体验问题。
    if (!s_wifi_initialized) {
        post_status("正在初始化 WiFi…", kColorScanning);
        if (!wifi_init_for_screen()) {
            post_status("WiFi 初始化失败", kColorError);
            s_scan_in_progress = false;
            refresh_nearby_list();      // 把「未发现网络…」提示画回来
            post_nearby_spinner(false);
            post_scan_btn_enabled(true);
            vTaskDelete(nullptr);
            return;
        }
    }

    post_status("正在扫描附近 WiFi…", kColorScanning);

    xEventGroupClearBits(s_evt_group, kBitScanDone);

    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %d", err);
        char buf[64];
        snprintf(buf, sizeof(buf), "启动扫描失败 (err=%d)", err);
        post_status(buf, kColorError);
        s_scan_in_progress = false;
        refresh_nearby_list();
        post_nearby_spinner(false);
        post_scan_btn_enabled(true);
        vTaskDelete(nullptr);
        return;
    }

    auto bits = xEventGroupWaitBits(s_evt_group, kBitScanDone, pdTRUE, pdTRUE,
                                    pdMS_TO_TICKS(15000));
    if (!(bits & kBitScanDone)) {
        post_status("扫描超时", kColorError);
        s_scan_in_progress = false;
        refresh_nearby_list();
        post_nearby_spinner(false);
        post_scan_btn_enabled(true);
        vTaskDelete(nullptr);
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);

    std::vector<wifi_ap_record_t> records;
    if (ap_num > 0) {
        records.resize(ap_num);
        uint16_t got = ap_num;
        esp_wifi_scan_get_ap_records(&got, records.data());
        records.resize(got);
    }

    // 排序：RSSI 降序
    std::sort(records.begin(), records.end(),
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
                  return a.rssi > b.rssi;
              });

    // 去重（同名只保留信号最强的）
    s_scan_results.clear();
    s_scan_results.reserve(records.size());
    for (auto& r : records) {
        const char* ssid = reinterpret_cast<const char*>(r.ssid);
        if (ssid[0] == '\0') continue;
        bool dup = false;
        for (auto& existing : s_scan_results) {
            if (existing.ssid == ssid) { dup = true; break; }
        }
        if (dup) continue;
        ApItem it;
        it.ssid = ssid;
        it.rssi = r.rssi;
        it.authmode = r.authmode;
        s_scan_results.push_back(std::move(it));
    }

    char status[64];
    snprintf(status, sizeof(status), "扫描完成，共 %d 个网络",
             static_cast<int>(s_scan_results.size()));
    post_status(status, kColorSuccess);

    // 关键顺序：先把 s_scan_in_progress 置 false，再 refresh_nearby_list()。
    // refresh_nearby_list 是 async，跑到 LVGL 那边时如果 flag 还是 true，
    // 在 0 结果场景下会因「扫描中不画提示」而留下一片空白。
    s_scan_in_progress = false;
    refresh_nearby_list();
    post_nearby_spinner(false);
    post_scan_btn_enabled(true);
    vTaskDelete(nullptr);
}

void schedule_scan() {
    if (s_scan_in_progress) {
        ESP_LOGW(TAG, "scan already in progress");
        return;
    }
    if (s_connect_in_progress) {
        post_status("正在连接，请稍后再扫描", kColorScanning);
        return;
    }
    if (xTaskCreate(scan_task, "wifi_scan", 4096, nullptr, 5, nullptr) != pdPASS) {
        post_status("无法启动扫描任务", kColorError);
    }
}

// ---------------------------------------------------------------------------
// 连接任务
// ---------------------------------------------------------------------------
struct ConnectCtx {
    std::string ssid;
    std::string password;
};

void connect_task(void* arg) {
    auto* ctx = static_cast<ConnectCtx*>(arg);

    char buf[128];
    snprintf(buf, sizeof(buf), "正在连接 %s …", ctx->ssid.c_str());
    post_status(buf, kColorScanning);

    // 准备：停止扫描 + 断开当前连接，然后写新配置 + connect
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_config_t wc = {};
    strlcpy(reinterpret_cast<char*>(wc.sta.ssid), ctx->ssid.c_str(), 32);
    strlcpy(reinterpret_cast<char*>(wc.sta.password), ctx->password.c_str(), 64);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.failure_retry_cnt = 1;

    xEventGroupClearBits(s_evt_group, kBitConnected | kBitDisconnected);
    s_last_disconnect_reason = 0;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        snprintf(buf, sizeof(buf), "set_config 失败 (err=%d)", err);
        post_status(buf, kColorError);
        post_show_failure("连接失败", buf);
        s_connect_in_progress = false;
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        snprintf(buf, sizeof(buf), "esp_wifi_connect 失败 (err=%d)", err);
        post_status(buf, kColorError);
        post_show_failure("连接失败", buf);
        s_connect_in_progress = false;
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    // 等待 15 秒看是否拿到 IP 或被断开
    auto bits = xEventGroupWaitBits(s_evt_group,
                                    kBitConnected | kBitDisconnected, pdTRUE,
                                    pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & kBitConnected) {
        // 拿到 IP，记一笔到 SsidManager（SaveToNvs 内已经持久化，重启后生效）
        SsidManager::GetInstance().AddSsid(ctx->ssid, ctx->password);
        snprintf(buf, sizeof(buf), "连接 %s 成功，准备重启…", ctx->ssid.c_str());
        post_status(buf, kColorSuccess);
        refresh_saved_list();
        // 弹出「连接成功 + 倒计时重启」覆盖层，倒计时归零后自动 esp_restart()
        post_open_success_and_reboot(ctx->ssid);
    } else if (bits & kBitDisconnected) {
        const uint8_t reason = s_last_disconnect_reason;
        const char* mapped = disconnect_reason_text(reason);
        std::string detail;
        if (mapped != nullptr) {
            detail = mapped;
        } else {
            char tmp[96];
            snprintf(tmp, sizeof(tmp), "连接被拒绝 (reason=%u)", reason);
            detail = tmp;
        }
        snprintf(buf, sizeof(buf), "连接 %s 失败：%s",
                 ctx->ssid.c_str(), detail.c_str());
        post_status(buf, kColorError);
        // 失败弹窗保留 2.5 秒让用户看清原因，然后自动收起回到密码键盘
        post_show_failure("连接失败", detail);
    } else {
        snprintf(buf, sizeof(buf), "连接 %s 超时", ctx->ssid.c_str());
        post_status(buf, kColorError);
        esp_wifi_disconnect();
        post_show_failure("连接超时", "未能在 15 秒内完成连接，请检查网络后重试");
    }

    s_connect_in_progress = false;
    delete ctx;
    vTaskDelete(nullptr);
}

void schedule_connect(const std::string& ssid, const std::string& password) {
    if (s_connect_in_progress) {
        post_status("已有正在进行的连接任务", kColorScanning);
        return;
    }
    if (ssid.empty() || ssid.size() > kMaxSsidLen) {
        post_status("SSID 不合法", kColorError);
        return;
    }
    if (password.size() > kMaxPasswordLen) {
        post_status("密码超长", kColorError);
        return;
    }
    auto* ctx = new ConnectCtx{ssid, password};
    s_connect_in_progress = true;
    // 立刻弹出转圈遮罩，覆盖密码键盘，让用户得到「我点了连接」的视觉反馈。
    open_connecting_popup(ssid);
    if (xTaskCreate(connect_task, "wifi_connect", 4096, ctx, 5, nullptr) != pdPASS) {
        delete ctx;
        s_connect_in_progress = false;
        post_status("无法启动连接任务", kColorError);
        close_status_popup();
    }
}

// ---------------------------------------------------------------------------
// 列表渲染
// ---------------------------------------------------------------------------
struct NearbyClickCtx {
    char ssid[kMaxSsidLen + 1];
    int authmode;  // wifi_auth_mode_t
};

void on_nearby_item_clicked(lv_event_t* e) {
    auto* ctx = static_cast<NearbyClickCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    open_password_popup(ctx->ssid, static_cast<wifi_auth_mode_t>(ctx->authmode));
}

void on_nearby_item_delete(lv_event_t* e) {
    auto* ctx = static_cast<NearbyClickCtx*>(lv_event_get_user_data(e));
    delete ctx;
}

void rebuild_nearby_list_now() {
    if (s_ui.nearby_list == nullptr) return;
    // 不能用 lv_obj_clean()：会把 nearby_spinner 这个浮层子物件也一起删掉。
    // 改成倒序遍历，只删非 spinner 的旧 item / 提示文本。
    for (int32_t i = static_cast<int32_t>(
                         lv_obj_get_child_count(s_ui.nearby_list)) - 1;
         i >= 0; --i) {
        lv_obj_t* child = lv_obj_get_child(s_ui.nearby_list, i);
        if (child != nullptr && child != s_ui.nearby_spinner) {
            lv_obj_delete(child);
        }
    }

    if (s_scan_results.empty()) {
        // 扫描中（spinner 在转）就别再叠一句「未发现网络…」了，那句话只在
        // 真正扫完发现空结果 / 还没开过扫描的时候才有意义。
        if (s_scan_in_progress) {
            return;
        }
        lv_obj_t* hint = lv_label_create(s_ui.nearby_list);
        lv_label_set_text(hint, "未发现网络，点右上「扫描」试试");
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_pad_all(hint, 16, LV_PART_MAIN);
        return;
    }

    for (auto& ap : s_scan_results) {
        lv_obj_t* item = lv_button_create(s_ui.nearby_list);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, 60);
        lv_obj_set_style_radius(item, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(item, lv_color_hex(kColorItem), LV_PART_MAIN);
        lv_obj_set_style_bg_color(item, lv_color_hex(kColorItemSel),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(item, 14, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(item, 0, LV_PART_MAIN);

        auto* ctx = new NearbyClickCtx{};
        snprintf(ctx->ssid, sizeof(ctx->ssid), "%s", ap.ssid.c_str());
        ctx->authmode = static_cast<int>(ap.authmode);
        lv_obj_add_event_cb(item, on_nearby_item_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(item, on_nearby_item_delete, LV_EVENT_DELETE, ctx);

        // 左侧：加密标记 + SSID
        lv_obj_t* left = lv_label_create(item);
        char ltext[96];
        snprintf(ltext, sizeof(ltext), "%s %s", auth_label(ap.authmode),
                 ap.ssid.c_str());
        lv_label_set_text(left, ltext);
        lv_label_set_long_mode(left, LV_LABEL_LONG_DOT);
        lv_obj_set_width(left, kPanelW - 40 - 200);
        lv_obj_set_style_text_color(left, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(left, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(left, LV_ALIGN_LEFT_MID, 0, 0);

        // 右侧：信号描述 + dBm
        lv_obj_t* right = lv_label_create(item);
        char rtext[48];
        snprintf(rtext, sizeof(rtext), "%s  %d dBm",
                 rssi_quality_text(ap.rssi), ap.rssi);
        lv_label_set_text(right, rtext);
        lv_obj_set_style_text_color(right, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(right, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

// 已保存 WiFi 列表项的按钮上下文（每行三个：置顶 / 删除）
struct SavedActionCtx {
    int index;
};

void on_saved_set_default(lv_event_t* e) {
    auto* ctx = static_cast<SavedActionCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    SsidManager::GetInstance().SetDefaultSsid(ctx->index);
    post_status("已设置为默认网络", kColorSuccess);
    refresh_saved_list();
}

void on_saved_remove(lv_event_t* e) {
    auto* ctx = static_cast<SavedActionCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    SsidManager::GetInstance().RemoveSsid(ctx->index);
    post_status("已删除该网络", kColorSuccess);
    refresh_saved_list();
}

void on_saved_btn_delete(lv_event_t* e) {
    delete static_cast<SavedActionCtx*>(lv_event_get_user_data(e));
}

void on_clear_all_saved(lv_event_t* /*e*/) {
    SsidManager::GetInstance().Clear();
    post_status("已清空所有已保存网络", kColorSuccess);
    refresh_saved_list();
}

void rebuild_saved_list_now() {
    if (s_ui.saved_list == nullptr) return;
    lv_obj_clean(s_ui.saved_list);

    auto& mgr = SsidManager::GetInstance();
    const auto& list = mgr.GetSsidList();

    if (list.empty()) {
        lv_obj_t* hint = lv_label_create(s_ui.saved_list);
        lv_label_set_text(hint, "暂无已连接过的 WiFi");
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_pad_all(hint, 16, LV_PART_MAIN);
        // 同时禁用「清空」按钮
        if (s_ui.clear_btn != nullptr) {
            lv_obj_add_state(s_ui.clear_btn, LV_STATE_DISABLED);
        }
        return;
    }
    if (s_ui.clear_btn != nullptr) {
        lv_obj_remove_state(s_ui.clear_btn, LV_STATE_DISABLED);
    }

    for (size_t i = 0; i < list.size(); ++i) {
        const auto& item = list[i];

        lv_obj_t* row = lv_obj_create(s_ui.saved_list);
        screen_strip_obj_chrome(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 60);
        lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorItem), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 14, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // SSID 文本（带「默认」标记，列表中第 0 项是默认）
        lv_obj_t* lbl = lv_label_create(row);
        char ttext[96];
        if (i == 0) {
            snprintf(ttext, sizeof(ttext), "%s  (默认)", item.ssid.c_str());
        } else {
            snprintf(ttext, sizeof(ttext), "%s", item.ssid.c_str());
        }
        lv_label_set_text(lbl, ttext);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, kPanelW - 40 - 260);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        // 「置顶」按钮（i == 0 时禁用）
        lv_obj_t* def_btn = lv_button_create(row);
        lv_obj_set_size(def_btn, 110, 44);
        lv_obj_align(def_btn, LV_ALIGN_RIGHT_MID, -120, 0);
        lv_obj_set_style_radius(def_btn, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(def_btn, lv_color_hex(kColorBtnAccent), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(def_btn, 0, LV_PART_MAIN);
        auto* def_ctx = new SavedActionCtx{static_cast<int>(i)};
        lv_obj_add_event_cb(def_btn, on_saved_set_default, LV_EVENT_CLICKED, def_ctx);
        lv_obj_add_event_cb(def_btn, on_saved_btn_delete, LV_EVENT_DELETE, def_ctx);
        if (i == 0) lv_obj_add_state(def_btn, LV_STATE_DISABLED);
        lv_obj_t* def_lbl = lv_label_create(def_btn);
        lv_label_set_text(def_lbl, "设为默认");
        lv_obj_set_style_text_color(def_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(def_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(def_lbl);

        // 「删除」按钮
        lv_obj_t* del_btn = lv_button_create(row);
        lv_obj_set_size(del_btn, 100, 44);
        lv_obj_align(del_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(del_btn, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del_btn, lv_color_hex(kColorBtnDanger), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(del_btn, 0, LV_PART_MAIN);
        auto* del_ctx = new SavedActionCtx{static_cast<int>(i)};
        lv_obj_add_event_cb(del_btn, on_saved_remove, LV_EVENT_CLICKED, del_ctx);
        lv_obj_add_event_cb(del_btn, on_saved_btn_delete, LV_EVENT_DELETE, del_ctx);
        lv_obj_t* del_lbl = lv_label_create(del_btn);
        lv_label_set_text(del_lbl, "删除");
        lv_obj_set_style_text_color(del_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(del_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(del_lbl);
    }
}

// ---------------------------------------------------------------------------
// 密码输入弹窗（模态遮罩 + 文本框 + 键盘）
// ---------------------------------------------------------------------------
// lv_keyboard_create 内部已经把默认 VALUE_CHANGED 回调挂上去（负责把按键
// 回填进 textarea、切换大小写 / 数字键盘 / 关闭等）。这里只补一下用户行为：
//   - LV_EVENT_READY  : 用户按 OK，按当前 textarea 内容发起连接
//   - LV_EVENT_CANCEL : 用户按键盘的关闭按钮，等同于点取消
void on_kb_event(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* kb = lv_event_get_target_obj(e);

    if (code == LV_EVENT_CANCEL) {
        close_password_popup();
    } else if (code == LV_EVENT_READY) {
        lv_obj_t* ta = lv_keyboard_get_textarea(kb);
        const char* pwd = (ta != nullptr) ? lv_textarea_get_text(ta) : "";
        // 开放网络若用户没输入密码也允许（password 留空）
        schedule_connect(s_pending_ssid, pwd ? pwd : "");
    }
}

void on_show_pwd_changed(lv_event_t* e) {
    lv_obj_t* chk = lv_event_get_target_obj(e);
    if (s_ui.pwd_textarea == nullptr) return;
    const bool checked = lv_obj_has_state(chk, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(s_ui.pwd_textarea, !checked);
}

void on_pwd_connect_btn(lv_event_t* /*e*/) {
    if (s_ui.pwd_textarea == nullptr) return;
    const char* pwd = lv_textarea_get_text(s_ui.pwd_textarea);
    schedule_connect(s_pending_ssid, pwd ? pwd : "");
}

void on_pwd_cancel_btn(lv_event_t* /*e*/) { close_password_popup(); }

void open_password_popup(const std::string& ssid, wifi_auth_mode_t authmode) {
    if (s_ui.screen == nullptr) return;
    close_password_popup();

    s_pending_ssid = ssid;
    s_pending_authmode = authmode;

    // 全屏半透明遮罩
    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    // 让遮罩拦截所有点击 / 滑动（包括屏幕右滑返回手势）
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.pwd_overlay = mask;

    // 顶部信息卡
    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kPanelW - 60, 260);
    lv_obj_set_pos(card, 30, 30);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(card);
    s_ui.pwd_title = title;
    char ttext[128];
    snprintf(ttext, sizeof(ttext), "连接到: %s", ssid.c_str());
    lv_label_set_text(title, ttext);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, kPanelW - 60 - 40);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint,
                      authmode == WIFI_AUTH_OPEN
                          ? "该网络无需密码，可直接连接"
                          : "请输入 WiFi 密码（8~63 字符）");
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align_to(hint, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);

    // 密码输入框
    lv_obj_t* ta = lv_textarea_create(card);
    s_ui.pwd_textarea = ta;
    lv_obj_set_size(ta, kPanelW - 60 - 40, 60);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, 92);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_max_length(ta, kMaxPasswordLen);
    lv_textarea_set_placeholder_text(ta, "WiFi 密码");
    lv_obj_set_style_text_font(ta, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x121726), LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_radius(ta, 10, LV_PART_MAIN);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    screen_swipe_back_ignore(ta, true);

    // 显示密码 checkbox
    lv_obj_t* chk = lv_checkbox_create(card);
    s_ui.pwd_show_chk = chk;
    lv_checkbox_set_text(chk, "显示密码");
    lv_obj_align(chk, LV_ALIGN_TOP_LEFT, 0, 168);
    lv_obj_set_style_text_color(chk, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(chk, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_add_event_cb(chk, on_show_pwd_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    screen_swipe_back_ignore(chk, true);

    // 取消 / 连接按钮
    lv_obj_t* cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 160, 56);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -180, 160);
    lv_obj_set_style_radius(cancel, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, on_pwd_cancel_btn, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "取消");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(cancel_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
    screen_swipe_back_ignore(cancel, true);

    lv_obj_t* connect = lv_button_create(card);
    lv_obj_set_size(connect, 160, 56);
    lv_obj_align(connect, LV_ALIGN_TOP_RIGHT, 0, 160);
    lv_obj_set_style_radius(connect, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(connect, lv_color_hex(kColorBtnActive), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(connect, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(connect, on_pwd_connect_btn, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* connect_lbl = lv_label_create(connect);
    lv_label_set_text(connect_lbl, "连接");
    lv_obj_set_style_text_color(connect_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(connect_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(connect_lbl);
    screen_swipe_back_ignore(connect, true);

    // 屏幕底部键盘（吃满底部 ~390px）
    lv_obj_t* kb = lv_keyboard_create(mask);
    s_ui.pwd_keyboard = kb;
    lv_obj_set_size(kb, kPanelW, 390);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_CANCEL, nullptr);
    // 键盘是触摸密集型控件——禁止屏幕级右滑返回手势把它的拖动判定为返回
    screen_swipe_back_ignore(kb, true);
}

void close_password_popup() {
    if (s_ui.pwd_overlay != nullptr) {
        lv_obj_delete(s_ui.pwd_overlay);
    }
    s_ui.pwd_overlay  = nullptr;
    s_ui.pwd_textarea = nullptr;
    s_ui.pwd_keyboard = nullptr;
    s_ui.pwd_title    = nullptr;
    s_ui.pwd_show_chk = nullptr;
    s_pending_ssid.clear();
}

// ---------------------------------------------------------------------------
// 连接进度 / 成功弹窗
//
// 连接发起后用一个独立的全屏遮罩盖住整个屏幕：
//   - 连接中：居中显示一个转圈 spinner + 状态文本
//   - 成功后：spinner 消失，文本变成「连接成功，N 秒后重启…」并倒计时，
//     倒计时归零后异步触发 esp_restart()
// 失败时调用 close_status_popup() 把这一层撤掉，让用户回到密码弹窗重试。
// ---------------------------------------------------------------------------
void close_status_popup() {
    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    if (s_ui.status_overlay != nullptr) {
        lv_obj_delete(s_ui.status_overlay);
    }
    s_ui.status_overlay     = nullptr;
    s_ui.status_message_lbl = nullptr;
    s_restart_remaining = 0;
    s_restart_ssid.clear();
}

void open_connecting_popup(const std::string& ssid) {
    if (s_ui.screen == nullptr) return;
    close_status_popup();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 360);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* spin = lv_spinner_create(card);
    lv_obj_set_size(spin, 140, 140);
    lv_obj_align(spin, LV_ALIGN_TOP_MID, 0, 20);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtnActive), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);

    lv_obj_t* lbl = lv_label_create(card);
    s_ui.status_message_lbl = lbl;
    char buf[160];
    snprintf(buf, sizeof(buf), "正在连接\n%s …", ssid.c_str());
    lv_label_set_text(lbl, buf);
    lv_obj_set_width(lbl, 520 - 48);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -16);
}

void reboot_task(void* /*arg*/) {
    ESP_LOGI(TAG, "wifi configured -> esp_restart");
    // 给 LVGL 一点点时间把 「正在重启…」 渲染出来
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void restart_timer_cb(lv_timer_t* /*timer*/) {
    s_restart_remaining--;
    if (s_restart_remaining > 0) {
        if (s_ui.status_message_lbl != nullptr) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "%s 连接成功！\n设备将在 %d 秒后自动重启…",
                     s_restart_ssid.c_str(), s_restart_remaining);
            lv_label_set_text(s_ui.status_message_lbl, buf);
        }
        return;
    }

    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    if (s_ui.status_message_lbl != nullptr) {
        lv_label_set_text(s_ui.status_message_lbl, "正在重启…");
    }
    xTaskCreate(reboot_task, "wifi_reboot", 2048, nullptr, 5, nullptr);
}

// 失败提示自动关闭：复用 s_restart_timer 槽位，到期回调里只做收掉遮罩，
// 重启逻辑走的是另一个 cb（restart_timer_cb），两者互斥不会同时存在。
// 注意：repeat_count=1 的 lv_timer 触发后会自动 delete，所以这里要先把
// s_restart_timer 置空，避免 close_status_popup 再去 lv_timer_delete 一个野指针。
void failure_close_timer_cb(lv_timer_t* /*timer*/) {
    s_restart_timer = nullptr;
    close_status_popup();
}

// 把当前的 status_overlay 替换成「失败」卡片，N 毫秒后自动关闭遮罩，让用户
// 回到密码键盘重新输入。如果遮罩已经被关掉了（极端情况），就新建一个。
void show_failure_in_status_popup(const std::string& title,
                                  const std::string& detail,
                                  uint32_t auto_close_ms) {
    if (s_ui.screen == nullptr) return;

    // 复用 / 新建遮罩
    if (s_ui.status_overlay == nullptr) {
        lv_obj_t* mask = lv_obj_create(s_ui.screen);
        screen_strip_obj_chrome(mask);
        lv_obj_set_size(mask, kPanelW, kPanelH);
        lv_obj_set_pos(mask, 0, 0);
        lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
        lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
        screen_swipe_back_ignore(mask, true);
        s_ui.status_overlay = mask;
    } else {
        lv_obj_clean(s_ui.status_overlay);
        s_ui.status_message_lbl = nullptr;
    }

    lv_obj_t* card = lv_obj_create(s_ui.status_overlay);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 320);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* head = lv_label_create(card);
    lv_label_set_text(head, title.c_str());
    lv_obj_set_style_text_color(head, lv_color_hex(kColorBtnDanger), LV_PART_MAIN);
    lv_obj_set_style_text_font(head, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t* body = lv_label_create(card);
    s_ui.status_message_lbl = body;
    lv_label_set_text(body, detail.c_str());
    lv_obj_set_width(body, 520 - 48);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 20);

    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    s_restart_timer = lv_timer_create(failure_close_timer_cb, auto_close_ms, nullptr);
    lv_timer_set_repeat_count(s_restart_timer, 1);
}

void open_success_popup(const std::string& ssid) {
    if (s_ui.screen == nullptr) return;
    close_status_popup();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 320);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* check = lv_label_create(card);
    lv_label_set_text(check, "成功");
    lv_obj_set_style_text_color(check, lv_color_hex(kColorBtnAccent), LV_PART_MAIN);
    lv_obj_set_style_text_font(check, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(check, LV_ALIGN_TOP_MID, 0, 20);

    s_restart_ssid = ssid;
    s_restart_remaining = kRestartCountdownSec;

    lv_obj_t* lbl = lv_label_create(card);
    s_ui.status_message_lbl = lbl;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s 连接成功！\n设备将在 %d 秒后自动重启…",
             s_restart_ssid.c_str(), s_restart_remaining);
    lv_label_set_text(lbl, buf);
    lv_obj_set_width(lbl, 520 - 48);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -16);

    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
    }
    s_restart_timer = lv_timer_create(restart_timer_cb, 1000, nullptr);
}

// ---------------------------------------------------------------------------
// 头部按钮 / 屏幕生命周期
// ---------------------------------------------------------------------------
void on_scan_clicked(lv_event_t* /*e*/) { schedule_scan(); }

void on_back_clicked(lv_event_t* /*e*/);

void on_swipe_back() {
    // 弹窗存在时优先关闭弹窗，不触发返回。
    // 连接 / 成功倒计时遮罩不允许通过滑动关闭——前者要等结果，后者会自动重启。
    if (s_ui.status_overlay != nullptr) {
        return;
    }
    if (s_ui.pwd_overlay != nullptr) {
        close_password_popup();
        return;
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

// 返回按钮：复用右滑返回的退出路径（包含弹窗保护）。
void on_back_clicked(lv_event_t* /*e*/) { on_swipe_back(); }

void on_screen_unloaded(lv_event_t* /*e*/) {
    s_screen_active = false;
    s_ui.screen        = nullptr;
    s_ui.status_label  = nullptr;
    s_ui.back_btn      = nullptr;
    s_ui.scan_btn      = nullptr;
    s_ui.scan_btn_lbl  = nullptr;
    s_ui.tabview       = nullptr;
    s_ui.nearby_tab    = nullptr;
    s_ui.saved_tab       = nullptr;
    s_ui.network_tab     = nullptr;
    s_ui.sim_tab         = nullptr;
    s_ui.nearby_list     = nullptr;
    s_ui.nearby_spinner  = nullptr;
    s_ui.saved_list      = nullptr;
    s_ui.clear_btn       = nullptr;
    s_ui.network_wifi_btn    = nullptr;
    s_ui.network_wifi_lbl    = nullptr;
    s_ui.network_cell_btn    = nullptr;
    s_ui.network_cell_lbl    = nullptr;
    s_ui.network_current_lbl = nullptr;
    s_ui.sim_external_btn = nullptr;
    s_ui.sim_external_lbl = nullptr;
    s_ui.sim_internal_btn = nullptr;
    s_ui.sim_internal_lbl = nullptr;
    s_ui.sim_current_lbl  = nullptr;
    s_network_switch_pending = false;
    // 注意：s_sim_switch_pending 不在这里清零——AT 任务可能还在后台跑，
    // 它结束后回调里会检测 screen_alive() 并自行复位。
    s_ui.pwd_overlay   = nullptr;
    s_ui.pwd_textarea  = nullptr;
    s_ui.pwd_keyboard  = nullptr;
    s_ui.pwd_title     = nullptr;
    s_ui.pwd_show_chk  = nullptr;
    // 屏幕销毁时 lvgl 会随父对象一起把 overlay 删掉；但 lv_timer 不挂在
    // 对象树上，必须显式 delete，否则到期后会回调到野指针。
    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    s_ui.status_overlay     = nullptr;
    s_ui.status_message_lbl = nullptr;
    s_restart_remaining = 0;
    s_restart_ssid.clear();
    s_pending_ssid.clear();
    s_scan_results.clear();
}

// ---------------------------------------------------------------------------
// 网络切换（WiFi <-> 4G）
// ---------------------------------------------------------------------------
void switch_network_task(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (auto* dual = GetDualNetworkBoard()) {
        dual->SwitchNetworkType();
    } else {
        ESP_LOGE(TAG, "DualNetworkBoard not available, reboot anyway");
        esp_restart();
    }
    vTaskDelete(nullptr);
}

void open_switch_reboot_popup(const char* target_name) {
    if (s_ui.screen == nullptr) {
        return;
    }
    close_status_popup();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 320);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* head = lv_label_create(card);
    lv_label_set_text(head, "切换网络");
    lv_obj_set_style_text_color(head, lv_color_hex(kColorBtnActive), LV_PART_MAIN);
    lv_obj_set_style_text_font(head, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t* body = lv_label_create(card);
    s_ui.status_message_lbl = body;
    char buf[160];
    snprintf(buf, sizeof(buf), "正在切换到 %s\n设备即将重启…", target_name);
    lv_label_set_text(body, buf);
    lv_obj_set_width(body, 520 - 48);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 20);

    if (xTaskCreate(switch_network_task, "net_switch", 4096, nullptr, 5,
                    nullptr) != pdPASS) {
        s_network_switch_pending = false;
        post_status("无法启动切换任务", kColorError);
        close_status_popup();
        refresh_network_switch_ui();
    }
}

// 把"按钮 → 切换"逻辑收成一个函数，两个上网方式按钮共用：
//   * 同一目标点两次 / 当前已经是该模式：no-op，避免重复进入重启流程
//   * 没有 4G 板（DualNetworkBoard 不可用）：状态栏报错并把高亮拉回真值
//   * 否则进入重启确认弹窗，由 switch_network_task 异步触发设备重启
void schedule_network_switch(int target_type) {
    if (s_network_switch_pending) {
        return;
    }
    if (target_type != kNetTypeWifi && target_type != kNetTypeCellular) {
        return;
    }
    if (GetSavedNetworkType() == target_type) {
        return;
    }

    if (GetDualNetworkBoard() == nullptr) {
        post_status("当前设备不支持网络切换", kColorError);
        refresh_network_switch_ui();
        return;
    }

    s_network_switch_pending = true;
    const char* target = target_type == kNetTypeCellular ? "4G" : "WiFi";
    post_status(target_type == kNetTypeCellular ? "准备切换到 4G…"
                                                 : "准备切换到 WiFi…",
                kColorScanning);
    open_switch_reboot_popup(target);
}

void on_network_wifi_clicked(lv_event_t* /*e*/) {
    schedule_network_switch(kNetTypeWifi);
}

void on_network_cell_clicked(lv_event_t* /*e*/) {
    schedule_network_switch(kNetTypeCellular);
}

// 把两个上网方式按钮的高亮状态同步成「当前模式高亮、另一个置灰」。
// 行为完全对应 refresh_sim_slot_ui()，方便直观比对维护。
void refresh_network_switch_ui() {
    if (s_ui.network_wifi_btn == nullptr ||
        s_ui.network_cell_btn == nullptr) {
        return;
    }
    const int type = GetSavedNetworkType();
    const bool is_wifi = (type != kNetTypeCellular);

    lv_obj_set_style_bg_color(s_ui.network_wifi_btn,
                              lv_color_hex(is_wifi ? kColorBtnActive
                                                   : kColorBtn),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.network_cell_btn,
                              lv_color_hex(is_wifi ? kColorBtn
                                                   : kColorBtnActive),
                              LV_PART_MAIN);

    if (s_ui.network_current_lbl != nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "当前：%s", is_wifi ? "WiFi" : "4G");
        lv_label_set_text(s_ui.network_current_lbl, buf);
    }
}

// ---------------------------------------------------------------------------
// SIM 卡切换（仅 4G 模式）
//
// 切换序列（参考 4G 模组手册）：
//   1) AT+CFUN=0           关闭射频，进入最小功能模式
//   2) AT+ECSIMCFG=SimSlot,X   X=0 外置卡 / X=1 内置卡
//   3) AT+CFUN=1           重新打开射频
// 整个过程模组会重新搜网，期间网络短时中断；我们用一个全屏遮罩展示进度。
// 成功之后只保存 UI 偏好，不重启设备（模组内部已经持久化新槽位）。
// ---------------------------------------------------------------------------

// 把两个 SIM 槽位按钮的高亮状态同步成「当前槽位高亮、另一个置灰」。
void refresh_sim_slot_ui() {
    if (s_ui.sim_external_btn == nullptr || s_ui.sim_internal_btn == nullptr) {
        return;
    }
    const int slot = GetSavedSimSlot();
    const bool is_external = (slot == kSimSlotExternal);

    lv_obj_set_style_bg_color(s_ui.sim_external_btn,
                              lv_color_hex(is_external ? kColorBtnActive
                                                       : kColorBtn),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.sim_internal_btn,
                              lv_color_hex(is_external ? kColorBtn
                                                       : kColorBtnActive),
                              LV_PART_MAIN);

    if (s_ui.sim_current_lbl != nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "当前：%s", SimSlotName(slot));
        lv_label_set_text(s_ui.sim_current_lbl, buf);
    }
}

// 进度文本通过 status_message_lbl 异步刷新（在 LVGL 线程）
void async_sim_set_progress(void* user_data) {
    auto* msg = static_cast<AsyncStringMsg*>(user_data);
    if (screen_alive() && s_ui.status_message_lbl != nullptr) {
        lv_label_set_text(s_ui.status_message_lbl, msg->text.c_str());
    }
    delete msg;
}

void post_sim_progress(const std::string& text) {
    if (!s_screen_active) return;
    lv_async_call(async_sim_set_progress, new AsyncStringMsg{text});
}

// 从 AT+ECSIMCFG? 的响应里解析出 SimSlot 行。完整响应类似：
//   +ECSIMCFG: "SimSimulator",0
//   +ECSIMCFG: "SimSlot",1
//   ...
//   OK
// 返回 -1 表示没解析到（响应不完整 / 没回 OK / 模组没回这一行）。
int parse_sim_slot_from_ecsimcfg(const std::string& resp) {
    constexpr const char* kKey = "\"SimSlot\"";
    size_t pos = 0;
    while ((pos = resp.find(kKey, pos)) != std::string::npos) {
        size_t comma = resp.find(',', pos);
        if (comma == std::string::npos) return -1;
        // 跳过逗号后的空白
        size_t i = comma + 1;
        while (i < resp.size() && (resp[i] == ' ' || resp[i] == '\t')) ++i;
        if (i >= resp.size() || !std::isdigit(static_cast<unsigned char>(resp[i]))) {
            pos = comma + 1;
            continue;
        }
        int slot = 0;
        while (i < resp.size() && std::isdigit(static_cast<unsigned char>(resp[i]))) {
            slot = slot * 10 + (resp[i] - '0');
            ++i;
        }
        return slot;
    }
    return -1;
}

// 把异步查到的槽位投递回 LVGL 线程：写 NVS 并刷新 UI 高亮 / "当前" 文字。
struct SimSlotQueryMsg {
    int slot;  // 0 / 1，-1 表示查询失败（不写 NVS，只刷新一下显示）
};

void async_sim_slot_queried(void* user_data) {
    auto* msg = static_cast<SimSlotQueryMsg*>(user_data);
    if (screen_alive() && msg->slot >= 0) {
        if (GetSavedSimSlot() != msg->slot) {
            SaveSimSlot(msg->slot);
            ESP_LOGI(TAG, "sim_slot synced from modem: %d", msg->slot);
        }
        refresh_sim_slot_ui();
    }
    delete msg;
}

void sim_slot_query_task(void* /*arg*/) {
    auto* msg = new SimSlotQueryMsg{-1};
    Nt26Board* nt26 = GetNt26Board();
    if (nt26 != nullptr) {
        std::string resp;
        // 同样要 bypass 初始化检查：外置卡缺失时也得能读到模组当前槽位，
        // 否则 UI 一直不知道自己卡在哪里。
        esp_err_t err = nt26->SendAtCommand("AT+ECSIMCFG?", resp, 5000,
                                            /*bypass_init_check=*/true);
        ESP_LOGI(TAG, "AT 'AT+ECSIMCFG?' -> err=%d resp_len=%u",
                 (int)err, (unsigned)resp.size());
        if (err == ESP_OK && resp.find("OK") != std::string::npos) {
            int slot = parse_sim_slot_from_ecsimcfg(resp);
            if (slot == kSimSlotExternal || slot == kSimSlotInternal) {
                msg->slot = slot;
            } else {
                ESP_LOGW(TAG, "ECSIMCFG: SimSlot 行未解析到，resp='%s'",
                         resp.c_str());
            }
        }
    }
    lv_async_call(async_sim_slot_queried, msg);
    vTaskDelete(nullptr);
}

void schedule_sim_slot_query() {
    if (GetNt26Board() == nullptr) {
        return;
    }
    if (xTaskCreate(sim_slot_query_task, "sim_query", 4096, nullptr, 5,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(sim_query) failed");
    }
}

struct SimSwitchResultMsg {
    bool        success;
    int         target_slot;
    std::string detail;   // 失败时的提示语
};

void async_sim_switch_done(void* user_data) {
    auto* msg = static_cast<SimSwitchResultMsg*>(user_data);
    if (screen_alive()) {
        s_sim_switch_pending = false;
        close_status_popup();
        if (msg->success) {
            char buf[96];
            snprintf(buf, sizeof(buf), "已切换到%s",
                     SimSlotName(msg->target_slot));
            post_status(buf, kColorSuccess);
            // 再向模组发一次 AT+ECSIMCFG? 确认实际生效的槽位，避免「以为
            // 切了但模组没切成」的脏数据。结果通过 NVS 修正回 UI。
            schedule_sim_slot_query();
        } else {
            post_status(msg->detail.c_str(), kColorError);
            show_failure_in_status_popup("SIM 卡切换失败", msg->detail, 3000);
            // 失败也查一次，保证 UI 与模组当前实际状态对齐
            schedule_sim_slot_query();
        }
        refresh_sim_slot_ui();
    } else {
        s_sim_switch_pending = false;
    }
    delete msg;
}

struct SimSwitchCtx {
    int target_slot;
};

void sim_switch_task(void* arg) {
    auto* ctx = static_cast<SimSwitchCtx*>(arg);
    auto* result = new SimSwitchResultMsg{};
    result->target_slot = ctx->target_slot;
    result->success = false;

    Nt26Board* nt26 = GetNt26Board();
    if (nt26 == nullptr) {
        result->detail = "未检测到 4G 模块";
        lv_async_call(async_sim_switch_done, result);
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    auto run_at = [&](const std::string& cmd, std::string& resp,
                      uint32_t timeout_ms) -> esp_err_t {
        resp.clear();
        // SIM 切换序列必须能在「modem 没完成初始化」时也执行：当前活跃槽位
        // 上的物理卡没插时 modem 永远不会切到 IsInitialized=true，但 AT
        // 通道是通的，bypass 本地状态检查直接下发。
        esp_err_t e = nt26->SendAtCommand(cmd, resp, timeout_ms,
                                          /*bypass_init_check=*/true);
        ESP_LOGI(TAG, "AT '%s' -> err=%d resp='%s'", cmd.c_str(), (int)e,
                 resp.c_str());
        return e;
    };

    // —— Step 1：AT+CFUN=0
    post_sim_progress("正在关闭射频…\nAT+CFUN=0");
    std::string resp;
    esp_err_t e1 = run_at("AT+CFUN=0", resp, 8000);
    if (e1 != ESP_OK || resp.find("OK") == std::string::npos) {
        result->detail = "AT+CFUN=0 执行失败";
        lv_async_call(async_sim_switch_done, result);
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // —— Step 2：AT+ECSIMCFG=SimSlot,X
    char buf[64];
    snprintf(buf, sizeof(buf), "AT+ECSIMCFG=SimSlot,%d", ctx->target_slot);
    char prog[128];
    snprintf(prog, sizeof(prog), "正在切换到%s…\n%s",
             SimSlotName(ctx->target_slot), buf);
    post_sim_progress(prog);
    esp_err_t e2 = run_at(buf, resp, 5000);
    if (e2 != ESP_OK || resp.find("OK") == std::string::npos) {
        // 切换失败：尝试把射频打回去，不然 4G 网络彻底掉线
        std::string tmp;
        run_at("AT+CFUN=1", tmp, 10000);
        result->detail = "AT+ECSIMCFG 执行失败";
        lv_async_call(async_sim_switch_done, result);
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // —— Step 3：AT+CFUN=1
    post_sim_progress("正在重新搜网…\nAT+CFUN=1");
    esp_err_t e3 = run_at("AT+CFUN=1", resp, 15000);
    // CFUN=1 个别模组在重启完成前会先回 OK，少数情况会超时但实际生效；
    // 这里只把它当作「尽力而为」，主要以 ECSIMCFG 的 OK 为准。
    (void)e3;

    SaveSimSlot(ctx->target_slot);
    result->success = true;
    lv_async_call(async_sim_switch_done, result);
    delete ctx;
    vTaskDelete(nullptr);
}

// 弹出一个全屏遮罩告诉用户正在切换 SIM 卡。复用 status_overlay 槽位。
void open_sim_switching_popup(int target_slot) {
    if (s_ui.screen == nullptr) return;
    close_status_popup();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 360);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* spin = lv_spinner_create(card);
    lv_obj_set_size(spin, 120, 120);
    lv_obj_align(spin, LV_ALIGN_TOP_MID, 0, 16);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtnActive),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);

    lv_obj_t* lbl = lv_label_create(card);
    s_ui.status_message_lbl = lbl;
    char buf[160];
    snprintf(buf, sizeof(buf), "正在切换到%s…\nAT+CFUN=0",
             SimSlotName(target_slot));
    lv_label_set_text(lbl, buf);
    lv_obj_set_width(lbl, 520 - 48);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -16);
}

void schedule_sim_switch(int target_slot) {
    if (s_sim_switch_pending) {
        return;
    }
    if (target_slot != kSimSlotExternal && target_slot != kSimSlotInternal) {
        return;
    }
    if (GetSavedSimSlot() == target_slot) {
        // 同一张卡，无需切换
        return;
    }
    if (GetNt26Board() == nullptr) {
        post_status("当前不在 4G 模式，无法切换 SIM 卡", kColorError);
        return;
    }

    s_sim_switch_pending = true;
    auto* ctx = new SimSwitchCtx{target_slot};
    open_sim_switching_popup(target_slot);
    if (xTaskCreate(sim_switch_task, "sim_switch", 4096, ctx, 5, nullptr) !=
        pdPASS) {
        delete ctx;
        s_sim_switch_pending = false;
        close_status_popup();
        post_status("无法启动 SIM 切换任务", kColorError);
    }
}

void on_sim_external_clicked(lv_event_t* /*e*/) {
    schedule_sim_switch(kSimSlotExternal);
}

void on_sim_internal_clicked(lv_event_t* /*e*/) {
    schedule_sim_switch(kSimSlotInternal);
}

// ---------------------------------------------------------------------------
// UI 组装
// ---------------------------------------------------------------------------
void build_header(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // 最左侧返回按钮：透明按钮 + ic_app_back 图标。按下时复用右滑返回逻辑，
    // 弹窗存在时会先关闭弹窗而不是直接退屏。
    constexpr int kBackBtnSize = 72;
    lv_obj_t* back = lv_button_create(header);
    s_ui.back_btn = back;
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_back_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "网络配置");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    // 标题让到返回按钮右侧
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);
}

void build_tabview(lv_obj_t* parent) {
    constexpr int y = kHeaderH;                     // 90
    constexpr int h = kPanelH - y;                  // 630
    constexpr int kTabBarH = 56;

    lv_obj_t* tv = lv_tabview_create(parent);
    s_ui.tabview = tv;
    lv_obj_set_size(tv, kPanelW, h);
    lv_obj_set_pos(tv, 0, y);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);

    // tabview 自身的 bg 与屏幕一致；标签栏稍微深一点突出按钮。
    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, &font_puhui_20_4, LV_PART_MAIN);
    // tab 按钮：选中态用主色调高亮做下边线
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorBtnActive),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER,
                            LV_PART_ITEMS | LV_STATE_CHECKED);

    // 内容容器是 tabview 内部的横向滚动 snap 区。本屏靠右滑返回主页，
    // 所以把内容区从屏幕级 swipe-back 候选里摘出来（否则横滑切 tab 会
    // 被同时识别成返回）。仍然保留 tab 之间的横向 snap。
    lv_obj_t* content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);

    // 4G 模式下没有 WiFi 列表 / 扫描概念，直接跳过这两个 Tab，只保留「网络
    // 切换」入口。WiFi 模式或未配置时正常展示三个 Tab。
    const bool show_wifi_tabs = !IsCellularMode();

    if (show_wifi_tabs) {
    // Tab 1：附近 WiFi —— 顶部扫描按钮，下面是列表
    lv_obj_t* tab1 = lv_tabview_add_tab(tv, "附近 WiFi");
    s_ui.nearby_tab = tab1;
    lv_obj_set_style_pad_all(tab1, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab1, 10, LV_PART_MAIN);
    lv_obj_remove_flag(tab1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tab1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t* nearby_toolbar = lv_obj_create(tab1);
    screen_strip_obj_chrome(nearby_toolbar);
    lv_obj_set_size(nearby_toolbar, LV_PCT(100), 48);
    lv_obj_set_style_bg_opa(nearby_toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(nearby_toolbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* nearby_hint = lv_label_create(nearby_toolbar);
    lv_label_set_text(nearby_hint, "扫描附近可用网络");
    lv_obj_set_style_text_color(nearby_hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(nearby_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(nearby_hint, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* scan = lv_button_create(nearby_toolbar);
    s_ui.scan_btn = scan;
    lv_obj_set_size(scan, 140, 44);
    lv_obj_align(scan, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(scan, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scan, lv_color_hex(kColorBtnActive), LV_PART_MAIN);
    lv_obj_set_style_bg_color(scan, lv_color_hex(kColorBtn),
                              LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(scan, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scan, on_scan_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(scan, true);
    lv_obj_t* scan_lbl = lv_label_create(scan);
    s_ui.scan_btn_lbl = scan_lbl;
    lv_label_set_text(scan_lbl, "扫描");
    lv_obj_set_style_text_color(scan_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(scan_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(scan_lbl);

    // 扫描 / 连接的状态提示挂在工具栏下方、列表上方，统一显示「准备中…」、
    // 「扫描中…」、「找到 N 个网络」、「连接 xxx 失败」等。
    lv_obj_t* status = lv_label_create(tab1);
    s_ui.status_label = status;
    lv_label_set_text(status, "");
    lv_obj_set_width(status, LV_PCT(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(status, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(status, 4, LV_PART_MAIN);

    lv_obj_t* nearby = lv_obj_create(tab1);
    s_ui.nearby_list = nearby;
    screen_strip_obj_chrome(nearby);
    lv_obj_set_width(nearby, LV_PCT(100));
    lv_obj_set_flex_grow(nearby, 1);
    lv_obj_set_style_bg_color(nearby, lv_color_hex(kColorListBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nearby, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(nearby, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nearby, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(nearby, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(nearby, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nearby, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(nearby, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nearby, LV_SCROLLBAR_MODE_AUTO);

    // 扫描期间的圆环 spinner。LV_OBJ_FLAG_FLOATING 让它不参与 flex 计算，
    // 直接绝对定位到列表正中。默认隐藏，scan_task 通过 post_nearby_spinner
    // 来控制显隐。spinner 自带循环动画，无需我们驱动。
    lv_obj_t* spin = lv_spinner_create(nearby);
    lv_obj_set_size(spin, 96, 96);
    lv_obj_add_flag(spin, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtnActive),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(spin, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 6, LV_PART_MAIN);
    lv_obj_add_flag(spin, LV_OBJ_FLAG_HIDDEN);
    s_ui.nearby_spinner = spin;

    // Tab 2：已保存 WiFi —— 顶部一行操作按钮，下面是列表
    lv_obj_t* tab2 = lv_tabview_add_tab(tv, "已保存 WiFi");
    s_ui.saved_tab = tab2;
    lv_obj_set_style_pad_all(tab2, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab2, 10, LV_PART_MAIN);
    lv_obj_remove_flag(tab2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tab2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t* toolbar = lv_obj_create(tab2);
    screen_strip_obj_chrome(toolbar);
    lv_obj_set_size(toolbar, LV_PCT(100), 48);
    lv_obj_set_style_bg_opa(toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hint = lv_label_create(toolbar);
    lv_label_set_text(hint, "管理已连接过的网络");
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* clear = lv_button_create(toolbar);
    s_ui.clear_btn = clear;
    lv_obj_set_size(clear, 140, 44);
    lv_obj_align(clear, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(clear, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(clear, lv_color_hex(kColorBtnDanger), LV_PART_MAIN);
    lv_obj_set_style_bg_color(clear, lv_color_hex(kColorBtn),
                              LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(clear, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(clear, on_clear_all_saved, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* clear_lbl = lv_label_create(clear);
    lv_label_set_text(clear_lbl, "清空全部");
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(clear_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(clear_lbl);

    // 列表本体
    lv_obj_t* saved = lv_obj_create(tab2);
    s_ui.saved_list = saved;
    screen_strip_obj_chrome(saved);
    lv_obj_set_width(saved, LV_PCT(100));
    lv_obj_set_flex_grow(saved, 1);
    lv_obj_set_style_bg_color(saved, lv_color_hex(kColorListBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(saved, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(saved, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(saved, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(saved, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(saved, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(saved, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(saved, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(saved, LV_SCROLLBAR_MODE_AUTO);
    }  // if (show_wifi_tabs)

    // -----------------------------------------------------------------------
    // 「网络切换」和「SIM 卡切换」两个 Tab 的构造分别封到 lambda 里，让
    // 真正决定显示顺序的逻辑在最后一段集中处理：
    //   - WiFi 模式：只挂「网络切换」
    //   - 4G 模式：先「SIM 卡切换」（最常用的现场操作），再「网络切换」
    // -----------------------------------------------------------------------
    auto build_network_switch_tab = [&]() {
        lv_obj_t* tab3 = lv_tabview_add_tab(tv, "网络切换");
        s_ui.network_tab = tab3;
        lv_obj_set_style_pad_all(tab3, 24, LV_PART_MAIN);
        lv_obj_remove_flag(tab3, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* card = lv_obj_create(tab3);
        screen_strip_obj_chrome(card);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(card, 16, LV_PART_MAIN);

        lv_obj_t* title3 = lv_label_create(card);
        lv_label_set_text(title3, "上网方式");
        lv_obj_set_style_text_color(title3, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(title3, &font_puhui_30_4, LV_PART_MAIN);

        lv_obj_t* hint3 = lv_label_create(card);
        lv_label_set_text(hint3,
                          "选择上网方式。切换后设备将自动重启生效。");
        lv_obj_set_width(hint3, LV_PCT(100));
        lv_label_set_long_mode(hint3, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(hint3, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(hint3, &font_puhui_20_4, LV_PART_MAIN);

        // 两个并排按钮：WiFi / 4G。布局、配色、尺寸、字体全部沿用
        // build_sim_switch_tab 里的 make_sim_btn 配方，保证两类切换在
        // 视觉上完全对仗，用户只要会用 SIM 卡切换就会用这里。
        lv_obj_t* net_row = lv_obj_create(card);
        screen_strip_obj_chrome(net_row);
        lv_obj_set_size(net_row, LV_PCT(100), 96);
        lv_obj_set_style_bg_opa(net_row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(net_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(net_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(net_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        auto make_net_btn = [&](const char* text, lv_event_cb_t cb,
                                lv_obj_t** out_btn, lv_obj_t** out_lbl) {
            lv_obj_t* btn = lv_button_create(net_row);
            lv_obj_set_size(btn, 280, 80);
            lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
            lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtn),
                                      LV_PART_MAIN);
            lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
            screen_swipe_back_ignore(btn, true);
            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, text);
            lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
            lv_obj_center(lbl);
            *out_btn = btn;
            *out_lbl = lbl;
        };

        make_net_btn("WiFi", on_network_wifi_clicked,
                     &s_ui.network_wifi_btn, &s_ui.network_wifi_lbl);
        make_net_btn("4G", on_network_cell_clicked,
                     &s_ui.network_cell_btn, &s_ui.network_cell_lbl);

        // "当前：xxx" 文字由 refresh_network_switch_ui() 同步刷新；这里给
        // 个占位，避免首帧空白。
        lv_obj_t* cur = lv_label_create(card);
        s_ui.network_current_lbl = cur;
        lv_label_set_text(cur, "当前：--");
        lv_obj_set_style_text_color(cur, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(cur, &font_puhui_20_4, LV_PART_MAIN);

        refresh_network_switch_ui();
    };  // build_network_switch_tab

    // SIM 卡切换页（仅 4G 模式构造，WiFi 模式下不挂这个 Tab）
    auto build_sim_switch_tab = [&]() {
        lv_obj_t* tab4 = lv_tabview_add_tab(tv, "SIM 卡切换");
        s_ui.sim_tab = tab4;
        lv_obj_set_style_pad_all(tab4, 24, LV_PART_MAIN);
        lv_obj_remove_flag(tab4, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* sim_card = lv_obj_create(tab4);
        screen_strip_obj_chrome(sim_card);
        lv_obj_set_size(sim_card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(sim_card, lv_color_hex(kColorCard),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sim_card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(sim_card, 18, LV_PART_MAIN);
        lv_obj_set_style_pad_all(sim_card, 24, LV_PART_MAIN);
        lv_obj_remove_flag(sim_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(sim_card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(sim_card, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(sim_card, 16, LV_PART_MAIN);

        lv_obj_t* sim_title = lv_label_create(sim_card);
        lv_label_set_text(sim_title, "SIM 卡选择");
        lv_obj_set_style_text_color(sim_title, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(sim_title, &font_puhui_30_4, LV_PART_MAIN);

        lv_obj_t* sim_hint = lv_label_create(sim_card);
        lv_label_set_text(sim_hint,
                          "选择 4G 模组使用的 SIM 卡。\n切换期间网络会短暂中断。");
        lv_obj_set_width(sim_hint, LV_PCT(100));
        lv_label_set_long_mode(sim_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(sim_hint, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(sim_hint, &font_puhui_20_4, LV_PART_MAIN);

        // 两个并排按钮：外置卡 / 内置卡
        lv_obj_t* sim_row = lv_obj_create(sim_card);
        screen_strip_obj_chrome(sim_row);
        lv_obj_set_size(sim_row, LV_PCT(100), 96);
        lv_obj_set_style_bg_opa(sim_row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(sim_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(sim_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sim_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        auto make_sim_btn = [&](const char* text, lv_event_cb_t cb,
                                lv_obj_t** out_btn, lv_obj_t** out_lbl) {
            lv_obj_t* btn = lv_button_create(sim_row);
            lv_obj_set_size(btn, 280, 80);
            lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
            lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtn),
                                      LV_PART_MAIN);
            lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
            screen_swipe_back_ignore(btn, true);
            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, text);
            lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
            lv_obj_center(lbl);
            *out_btn = btn;
            *out_lbl = lbl;
        };

        make_sim_btn("外置卡", on_sim_external_clicked,
                     &s_ui.sim_external_btn, &s_ui.sim_external_lbl);
        make_sim_btn("内置卡", on_sim_internal_clicked,
                     &s_ui.sim_internal_btn, &s_ui.sim_internal_lbl);

        lv_obj_t* sim_cur = lv_label_create(sim_card);
        s_ui.sim_current_lbl = sim_cur;
        char sim_cur_buf[64];
        snprintf(sim_cur_buf, sizeof(sim_cur_buf), "当前：%s",
                 SimSlotName(GetSavedSimSlot()));
        lv_label_set_text(sim_cur, sim_cur_buf);
        lv_obj_set_style_text_color(sim_cur, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(sim_cur, &font_puhui_20_4, LV_PART_MAIN);

        lv_obj_t* sim_tip = lv_label_create(sim_card);
        lv_label_set_text(sim_tip,
                          "AT 命令序列：\n  AT+CFUN=0\n  AT+ECSIMCFG=SimSlot,X\n  AT+CFUN=1");
        lv_obj_set_width(sim_tip, LV_PCT(100));
        lv_label_set_long_mode(sim_tip, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(sim_tip, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(sim_tip, &font_puhui_20_4, LV_PART_MAIN);

        refresh_sim_slot_ui();
    };  // build_sim_switch_tab

    // 真正决定 Tab 顺序的地方：
    //   - 4G 模式：「SIM 卡切换」放在「网络切换」前面，因为更换 SIM 卡是
    //     4G 用户进入这页最常做的事，放第一个最顺手；「网络切换」是兜底
    //     入口（切回 WiFi）。
    //   - WiFi 模式：只挂「网络切换」，没有 SIM 卡概念。
    if (IsCellularMode()) {
        build_sim_switch_tab();
        build_network_switch_tab();
    } else {
        build_network_switch_tab();
    }
}

}  // namespace

// ===========================================================================
// 公共接口
// ===========================================================================
lv_obj_t* NetworkScreen::Create() {
    s_screen_active = true;
    s_scan_in_progress = false;
    s_connect_in_progress = false;
    s_scan_results.clear();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_tabview(scr);

    // 先填充列表（即使数据空也要画占位提示）。4G 模式下两个 WiFi Tab 不会
    // 创建对应容器，rebuild_*_list_now 内部 nullptr 检查会直接 return。
    rebuild_nearby_list_now();
    rebuild_saved_list_now();

    screen_attach_swipe_back(scr, on_swipe_back);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    return scr;
}

void NetworkScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: network_screen");
        // 4G 模式下「附近 WiFi」「已保存 WiFi」两个 Tab 都被隐藏，没必要
        // 启动本地 STA 栈做扫描——会无谓地占用 wifi 硬件、抢蜂窝模块的
        // 资源、还可能在 SwitchNetworkType 重启前后产生事件回调毛刺。
        if (!IsCellularMode()) {
            // 不再自动扫描——等用户主动点「扫描」。这样不会无谓地停掉外面
            // 跑的 WifiStation 抢硬件，也不会让用户一进来就被 spinner 干扰。
            // 状态栏不再额外提示，列表本体已经有「未发现网络，点右上「扫描」
            // 试试」的占位语，足够指引。
            post_status("", kColorSubtle);
            // 刷新一遍已保存列表（可能用户在外面改过）
            refresh_saved_list();
        }
        refresh_network_switch_ui();
        refresh_sim_slot_ui();
        // 4G 模式下向模组发 AT+ECSIMCFG? 同步真实当前槽位，避免本地 NVS
        // 与模组里实际生效的卡不一致（例如设备外部曾经手动切过卡）。
        if (IsCellularMode()) {
            schedule_sim_slot_query();
        }
    } else {
        ESP_LOGI(TAG, "unload: network_screen");
        s_screen_active = false;
        // 释放我们的 wifi 资源并恢复 WifiStation 接管。函数内部以
        // s_wifi_initialized 短路；4G 模式下未初始化时是 no-op。
        wifi_teardown_for_screen();
    }
}
