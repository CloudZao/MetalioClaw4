#include "wifi_test.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "screen_util.h"
#include "test_ui_common.h"
#include "wifi_station.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "WifiTest";

constexpr uint32_t kColorBtnIdle = 0x2563EB;
constexpr uint32_t kColorPopupBg = 0x1B2030;
constexpr uint32_t kColorListBg  = 0x12151C;
constexpr uint32_t kColorItemBg  = 0x202736;

constexpr EventBits_t kBitScanDone = BIT0;

struct ApItem {
    std::string ssid;
    int8_t      rssi = -127;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
};

struct ScanDoneMsg {
    bool success;
    int  count;
};

lv_obj_t* s_status_icon   = nullptr;
lv_obj_t* s_value_lbl     = nullptr;
lv_obj_t* s_list_mask     = nullptr;
lv_obj_t* s_list_area     = nullptr;
lv_obj_t* s_list_container = nullptr;
lv_obj_t* s_list_status   = nullptr;
lv_obj_t* s_rescan_btn    = nullptr;
lv_obj_t* s_rescan_lbl    = nullptr;
lv_obj_t* s_list_spinner  = nullptr;

bool                 s_loaded = false;
bool                 s_wifi_initialized = false;
bool                 s_wifi_station_was_active = false;
bool                 s_scan_in_progress = false;

std::vector<ApItem>  s_scan_results;
EventGroupHandle_t   s_evt_group = nullptr;
esp_netif_t*         s_netif = nullptr;
esp_event_handler_instance_t s_wifi_evt_inst = nullptr;
esp_event_handler_instance_t s_ip_evt_inst = nullptr;

const char* AuthLabel(wifi_auth_mode_t mode) {
    return (mode == WIFI_AUTH_OPEN) ? "开放" : "加密";
}

void SetValueText(const char* text, bool error) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, text);
    lv_obj_set_style_text_color(
        s_value_lbl,
        lv_color_hex(error ? kTestColorError : kTestColorTextDim),
        LV_PART_MAIN);
}

void UpdatePassFail(bool pass) {
    if (s_status_icon != nullptr) {
        TestUiUpdateStatus(s_status_icon, pass);
    }
}

void WifiEvtHandler(void* /*arg*/, esp_event_base_t base, int32_t id,
                    void* /*data*/) {
    if (s_evt_group == nullptr) {
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_evt_group, kBitScanDone);
    }
}

bool WifiInitForTest() {
    if (s_wifi_initialized) {
        return true;
    }

    wifi_mode_t mode_before = WIFI_MODE_NULL;
    const esp_err_t mode_err = esp_wifi_get_mode(&mode_before);
    s_wifi_station_was_active =
        (mode_err == ESP_OK && mode_before != WIFI_MODE_NULL);
    if (s_wifi_station_was_active) {
        WifiStation::GetInstance().Stop();
    }

    if (s_evt_group == nullptr) {
        s_evt_group = xEventGroupCreate();
    } else {
        xEventGroupClearBits(s_evt_group, 0xFFFFFF);
    }

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
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEvtHandler, nullptr,
        &s_wifi_evt_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi handler register failed: %d", err);
    }
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEvtHandler, nullptr,
        &s_ip_evt_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ip handler register failed: %d", err);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "wifi stack initialized for test");
    return true;
}

void WifiTeardownForTest() {
    if (!s_wifi_initialized) {
        return;
    }

    esp_wifi_scan_stop();
    esp_wifi_disconnect();

    if (s_wifi_evt_inst != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_evt_inst);
        s_wifi_evt_inst = nullptr;
    }
    if (s_ip_evt_inst != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              s_ip_evt_inst);
        s_ip_evt_inst = nullptr;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_netif != nullptr) {
        esp_netif_destroy(s_netif);
        s_netif = nullptr;
    }
    s_wifi_initialized = false;

    if (s_wifi_station_was_active) {
        WifiStation::GetInstance().Start();
    }
    s_wifi_station_was_active = false;
    ESP_LOGI(TAG, "wifi stack torn down");
}

void SetRescanEnabled(bool enabled) {
    if (s_rescan_btn == nullptr) {
        return;
    }
    if (enabled) {
        lv_obj_add_flag(s_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(s_rescan_btn, lv_color_hex(kColorBtnIdle),
                                  LV_PART_MAIN);
    } else {
        lv_obj_remove_flag(s_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(s_rescan_btn, lv_color_hex(kTestColorMuted),
                                  LV_PART_MAIN);
    }
}

void SetPopupSpinner(bool show) {
    if (s_list_spinner == nullptr) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(s_list_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_list_spinner);
    } else {
        lv_obj_add_flag(s_list_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

void RebuildListPopupNow() {
    if (s_list_container == nullptr) {
        return;
    }

    lv_obj_clean(s_list_container);

    if (s_scan_in_progress) {
        if (s_list_status != nullptr) {
            lv_label_set_text(s_list_status, "正在扫描…");
        }
        return;
    }

    if (s_scan_results.empty()) {
        if (s_list_status != nullptr) {
            lv_label_set_text(s_list_status, "未发现 WiFi 网络");
        }
        lv_obj_t* hint = lv_label_create(s_list_container);
        lv_label_set_text(hint, "请确认路由器已开启，或点击重新扫描");
        lv_obj_set_style_text_color(hint, lv_color_hex(kTestColorTextDim),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_width(hint, LV_PCT(100));
        return;
    }

    char status[48];
    std::snprintf(status, sizeof(status), "共 %d 个网络",
                  static_cast<int>(s_scan_results.size()));
    if (s_list_status != nullptr) {
        lv_label_set_text(s_list_status, status);
    }

    for (const auto& ap : s_scan_results) {
        lv_obj_t* item = lv_obj_create(s_list_container);
        screen_strip_obj_chrome(item);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, 52);
        lv_obj_set_style_bg_color(item, lv_color_hex(kColorItemBg),
                                LV_PART_MAIN);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(item, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(item, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(item, 8, LV_PART_MAIN);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* ssid_lbl = lv_label_create(item);
        lv_label_set_text(ssid_lbl, ap.ssid.c_str());
        lv_obj_set_style_text_color(ssid_lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(ssid_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_flex_grow(ssid_lbl, 1);
        lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);

        char meta[48];
        std::snprintf(meta, sizeof(meta), "%ddBm %s", ap.rssi,
                      AuthLabel(ap.authmode));
        lv_obj_t* meta_lbl = lv_label_create(item);
        lv_label_set_text(meta_lbl, meta);
        lv_obj_set_style_text_color(meta_lbl, lv_color_hex(kTestColorTextDim),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(meta_lbl, &font_puhui_20_4, LV_PART_MAIN);
    }
}

void AsyncRebuildListPopup(void* /*user_data*/) {
    if (!s_loaded) {
        return;
    }
    RebuildListPopupNow();
}

void AsyncScanDone(void* user_data) {
    auto* msg = static_cast<ScanDoneMsg*>(user_data);
    if (!s_loaded) {
        delete msg;
        return;
    }

    if (msg->success && msg->count > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "发现 %d 个", msg->count);
        SetValueText(buf, false);
        UpdatePassFail(true);
    } else if (msg->success) {
        SetValueText("未发现网络", true);
        UpdatePassFail(false);
    } else {
        SetValueText("扫描失败", true);
        UpdatePassFail(false);
    }

    SetRescanEnabled(true);
    SetPopupSpinner(false);
    RebuildListPopupNow();
    delete msg;
}

void ScanTask(void* /*arg*/) {
    s_scan_in_progress = true;
    lv_async_call(AsyncRebuildListPopup, nullptr);

    if (!s_wifi_initialized && !WifiInitForTest()) {
        auto* msg = new ScanDoneMsg{false, 0};
        s_scan_in_progress = false;
        lv_async_call(AsyncScanDone, msg);
        vTaskDelete(nullptr);
        return;
    }

    xEventGroupClearBits(s_evt_group, kBitScanDone);

    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    const esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %d", err);
        auto* msg = new ScanDoneMsg{false, 0};
        s_scan_in_progress = false;
        lv_async_call(AsyncScanDone, msg);
        vTaskDelete(nullptr);
        return;
    }

    const EventBits_t bits =
        xEventGroupWaitBits(s_evt_group, kBitScanDone, pdTRUE, pdTRUE,
                            pdMS_TO_TICKS(15000));
    if (!s_loaded) {
        s_scan_in_progress = false;
        vTaskDelete(nullptr);
        return;
    }

    auto* msg = new ScanDoneMsg{};
    if (!(bits & kBitScanDone)) {
        ESP_LOGW(TAG, "wifi scan timeout");
        msg->success = false;
        msg->count = 0;
        s_scan_in_progress = false;
        lv_async_call(AsyncScanDone, msg);
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

    std::sort(records.begin(), records.end(),
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
                  return a.rssi > b.rssi;
              });

    s_scan_results.clear();
    s_scan_results.reserve(records.size());
    for (auto& r : records) {
        const char* ssid = reinterpret_cast<const char*>(r.ssid);
        if (ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (const auto& existing : s_scan_results) {
            if (existing.ssid == ssid) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        ApItem it;
        it.ssid = ssid;
        it.rssi = r.rssi;
        it.authmode = r.authmode;
        s_scan_results.push_back(std::move(it));
    }

    msg->success = true;
    msg->count = static_cast<int>(s_scan_results.size());
    s_scan_in_progress = false;
    lv_async_call(AsyncScanDone, msg);
    vTaskDelete(nullptr);
}

void ScheduleScan() {
    if (!s_loaded || s_scan_in_progress) {
        return;
    }

    SetValueText("扫描中…", false);
    SetRescanEnabled(false);
    SetPopupSpinner(true);
    if (s_list_mask != nullptr) {
        RebuildListPopupNow();
    }

    if (xTaskCreate(ScanTask, "wifi_test_scan", 4096, nullptr, 5, nullptr) !=
        pdPASS) {
        ESP_LOGE(TAG, "create scan task failed");
        SetValueText("任务创建失败", true);
        UpdatePassFail(false);
        SetRescanEnabled(true);
        SetPopupSpinner(false);
    }
}

void CloseListPopup() {
    if (s_list_mask == nullptr) {
        return;
    }
    lv_obj_delete(s_list_mask);
    s_list_mask = nullptr;
    s_list_area = nullptr;
    s_list_container = nullptr;
    s_list_status = nullptr;
    s_rescan_btn = nullptr;
    s_rescan_lbl = nullptr;
    s_list_spinner = nullptr;
}

void OnClosePopupClicked(lv_event_t* /*e*/) {
    CloseListPopup();
}

void OnRescanClicked(lv_event_t* /*e*/) {
    ScheduleScan();
}

void OpenListPopup() {
    if (s_list_mask != nullptr) {
        return;
    }

    lv_obj_t* parent = TestUiGetScreen();
    if (parent == nullptr) {
        return;
    }

    lv_obj_t* mask = lv_obj_create(parent);
    s_list_mask = mask;
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kTestPanelW, kTestPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 640, 560);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorPopupBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "附近 WiFi");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_width(title, LV_PCT(100));

    s_list_status = lv_label_create(card);
    lv_label_set_text(s_list_status, s_scan_in_progress ? "正在扫描…" : "--");
    lv_obj_set_style_text_color(s_list_status, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_list_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_width(s_list_status, LV_PCT(100));

    s_list_area = lv_obj_create(card);
    screen_strip_obj_chrome(s_list_area);
    lv_obj_set_width(s_list_area, LV_PCT(100));
    lv_obj_set_flex_grow(s_list_area, 1);
    lv_obj_set_style_min_height(s_list_area, 280, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_list_area, lv_color_hex(kColorListBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_list_area, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_list_area, 14, LV_PART_MAIN);
    lv_obj_remove_flag(s_list_area, LV_OBJ_FLAG_SCROLLABLE);

    s_list_container = lv_obj_create(s_list_area);
    screen_strip_obj_chrome(s_list_container);
    lv_obj_set_size(s_list_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_list_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list_container, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list_container, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list_container, LV_SCROLLBAR_MODE_AUTO);

    s_list_spinner = lv_spinner_create(s_list_area);
    lv_obj_add_flag(s_list_spinner, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_list_spinner, 48, 48);
    lv_obj_center(s_list_spinner);
    lv_spinner_set_anim_params(s_list_spinner, 1000, 200);
    lv_obj_set_style_arc_color(s_list_spinner, lv_color_hex(kColorBtnIdle),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_list_spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_list_spinner, lv_color_hex(kTestColorMuted),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_list_spinner, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_list_spinner, 3, LV_PART_MAIN);
    lv_obj_add_flag(s_list_spinner, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* btn_row = lv_obj_create(card);
    screen_strip_obj_chrome(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, 56);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 24, LV_PART_MAIN);

    s_rescan_btn = lv_button_create(btn_row);
    lv_obj_remove_style_all(s_rescan_btn);
    lv_obj_set_size(s_rescan_btn, 180, 52);
    lv_obj_set_style_flex_grow(s_rescan_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_rescan_btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_rescan_btn, lv_color_hex(kColorBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_rescan_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(s_rescan_btn, OnRescanClicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(s_rescan_btn, true);

    s_rescan_lbl = lv_label_create(s_rescan_btn);
    lv_label_set_text(s_rescan_lbl, "重新扫描");
    lv_obj_set_style_text_color(s_rescan_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_rescan_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(s_rescan_lbl);
    lv_obj_remove_flag(s_rescan_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* close_btn = lv_button_create(btn_row);
    lv_obj_remove_style_all(close_btn);
    lv_obj_set_size(close_btn, 140, 52);
    lv_obj_set_style_flex_grow(close_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(close_btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(kTestColorMuted),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(close_btn, OnClosePopupClicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(close_btn, true);

    lv_obj_t* close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "关闭");
    lv_obj_set_style_text_color(close_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(close_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(close_lbl);
    lv_obj_remove_flag(close_lbl, LV_OBJ_FLAG_CLICKABLE);

    SetRescanEnabled(!s_scan_in_progress);
    SetPopupSpinner(s_scan_in_progress);
    RebuildListPopupNow();
}

void OnListBtnClicked(lv_event_t* /*e*/) {
    OpenListPopup();
}

lv_obj_t* CreateListButton(lv_obj_t* parent) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 120, 52);
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtnIdle), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, OnListBtnClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(btn, true);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "列表");
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

}  // namespace

namespace WifiTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "WiFi", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_obj_set_width(s_value_lbl, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_value_lbl, 1);
    lv_label_set_text(s_value_lbl, "扫描中…");
    CreateListButton(ctrl);
}

void OnLoad() {
    s_loaded = true;
    ScheduleScan();
}

void OnUnload() {
    s_loaded = false;
    if (s_scan_in_progress && s_wifi_initialized) {
        esp_wifi_scan_stop();
    }
    CloseListPopup();
    WifiTeardownForTest();
    s_scan_results.clear();
    s_scan_in_progress = false;
    s_status_icon = nullptr;
    s_value_lbl = nullptr;
}

}  // namespace WifiTest
