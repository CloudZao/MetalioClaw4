#include "bluetooth_screen.h"

#include "IOExpander.hpp"
#include "SimpleUart.hpp"
#include "home_screen/home_screen.h"
#include "screen_util.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "BtScreen";

constexpr int kPanelW = 720;
constexpr int kPanelH = 720;
constexpr int kAddrHexLen = 12;

constexpr uint32_t kColorBg         = 0x0E1116;
constexpr uint32_t kColorCard       = 0x1B2030;
constexpr uint32_t kColorBtn        = 0x2A2F3A;
constexpr uint32_t kColorBtnActive  = 0x3B82F6;
constexpr uint32_t kColorText       = 0xFFFFFF;
constexpr uint32_t kColorSubtle     = 0x9AA3B2;
constexpr uint32_t kColorSuccess    = 0x34C759;
constexpr uint32_t kColorError      = 0xFF3B30;
constexpr uint32_t kColorScanning   = 0xF59E0B;

enum class BtMode : uint8_t {
    kNone = 0,
    kMode1,
    kMode2,
    kMode3,
};

enum class ConnState : uint8_t {
    kIdle,
    kScanning,
    kConnecting,
    kConnected,
};

struct BtDevice {
    char address[kAddrHexLen + 1];
    char name[64];
};

struct UiState {
    lv_obj_t* screen           = nullptr;
    lv_obj_t* status_label     = nullptr;
    lv_obj_t* mode_btns[3]     = {};
    lv_obj_t* mode1_panel      = nullptr;
    lv_obj_t* mode2_panel      = nullptr;
    lv_obj_t* scan_btn         = nullptr;
    lv_obj_t* device_list      = nullptr;
    lv_obj_t* music_btn        = nullptr;
    lv_obj_t* call_btn         = nullptr;
};

UiState               s_ui;
BtMode                s_active_mode = BtMode::kNone;
ConnState             s_conn_state  = ConnState::kIdle;
std::string           s_rx_buffer;
std::vector<BtDevice> s_devices;
bool                  s_screen_active = false;

void update_status_label(const char* text, uint32_t color = kColorText) {
    if (s_ui.status_label == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.status_label, text);
    lv_obj_set_style_text_color(s_ui.status_label, lv_color_hex(color),
                                LV_PART_MAIN);
}

struct AsyncStatusMsg {
    char text[128];
    uint32_t color;
};

void async_update_status(void* user_data) {
    auto* msg = static_cast<AsyncStatusMsg*>(user_data);
    update_status_label(msg->text, msg->color);
    delete msg;
}

void post_status(const char* text, uint32_t color = kColorText) {
    if (!s_screen_active) {
        return;
    }
    auto* msg = new AsyncStatusMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    msg->color = color;
    lv_async_call(async_update_status, msg);
}

void refresh_mode_buttons() {
    for (int i = 0; i < 3; ++i) {
        if (s_ui.mode_btns[i] == nullptr) {
            continue;
        }
        const bool active = (static_cast<int>(s_active_mode) == i + 1);
        lv_obj_set_style_bg_color(
            s_ui.mode_btns[i],
            lv_color_hex(active ? kColorBtnActive : kColorBtn),
            LV_PART_MAIN);
    }
}

void show_mode2_panel(bool show) {
    if (s_ui.mode2_panel == nullptr) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(s_ui.mode2_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.mode2_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void show_mode1_panel(bool show) {
    if (s_ui.mode1_panel == nullptr) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(s_ui.mode1_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.mode1_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void clear_device_list_ui() {
    if (s_ui.device_list == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.device_list);
}

struct AsyncAddDeviceMsg {
    char address[kAddrHexLen + 1];
    char name[64];
};

void async_add_device_item(void* user_data) {
    auto* msg = static_cast<AsyncAddDeviceMsg*>(user_data);
    if (s_ui.device_list == nullptr) {
        delete msg;
        return;
    }

    lv_obj_t* item = lv_button_create(s_ui.device_list);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, 72);
    lv_obj_set_style_radius(item, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(item, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(item, 20, LV_PART_MAIN);

    char* addr_copy = static_cast<char*>(lv_malloc(kAddrHexLen + 1));
    if (addr_copy != nullptr) {
        memcpy(addr_copy, msg->address, kAddrHexLen + 1);
        lv_obj_add_event_cb(
            item,
            [](lv_event_t* e) {
                const char* addr =
                    static_cast<const char*>(lv_event_get_user_data(e));
                if (addr == nullptr) {
                    return;
                }
                char cmd[48];
                snprintf(cmd, sizeof(cmd), "AT+CONNECT=%s\r\n", addr);
                SimpleUart::getInstance().sendString(cmd);
                ESP_LOGI(TAG, "TX: AT+CONNECT=%s", addr);
                s_conn_state = ConnState::kConnecting;
                char status[64];
                snprintf(status, sizeof(status), "连接中: %s...", addr);
                post_status(status, kColorScanning);
            },
            LV_EVENT_CLICKED, addr_copy);
        lv_obj_add_event_cb(
            item,
            [](lv_event_t* e) {
                char* addr = static_cast<char*>(lv_event_get_user_data(e));
                lv_free(addr);
            },
            LV_EVENT_DELETE, addr_copy);
    }

    lv_obj_t* lbl = lv_label_create(item);
    char display[96];
    if (msg->name[0] != '\0') {
        snprintf(display, sizeof(display), "%s\n%s", msg->name, msg->address);
    } else {
        snprintf(display, sizeof(display), "%s", msg->address);
    }
    lv_label_set_text(lbl, display);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    delete msg;
}

void add_device_to_list(const char* address, const char* name) {
    if (!s_screen_active) {
        return;
    }
    auto* msg = new AsyncAddDeviceMsg{};
    snprintf(msg->address, sizeof(msg->address), "%s", address);
    snprintf(msg->name, sizeof(msg->name), "%s", name);
    lv_async_call(async_add_device_item, msg);
}

void async_clear_list(void* /*user_data*/) {
    clear_device_list_ui();
}

void post_clear_list() {
    if (!s_screen_active) {
        return;
    }
    lv_async_call(async_clear_list, nullptr);
}

static void async_on_mode1_set(void* /*user_data*/) {
    refresh_mode_buttons();
    show_mode2_panel(false);
    show_mode1_panel(true);
}

static void async_on_mode2_set(void* /*user_data*/) {
    refresh_mode_buttons();
    show_mode1_panel(false);
    show_mode2_panel(true);
}

static void async_on_mode3_set(void* /*user_data*/) {
    refresh_mode_buttons();
    show_mode1_panel(false);
    show_mode2_panel(false);
}

static bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static void trim_line(std::string& line) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    size_t start = 0;
    while (start < line.size() && line[start] == ' ') {
        ++start;
    }
    if (start > 0) {
        line = line.substr(start);
    }
}

static bool parse_bt_device_line(const std::string& line,
                                 char* address, size_t addr_sz,
                                 char* name, size_t name_sz) {
    constexpr const char* kPrefix = "AT+BT:";
    if (line.rfind(kPrefix, 0) != 0) {
        return false;
    }
    const std::string payload = line.substr(strlen(kPrefix));
    if (payload.size() < static_cast<size_t>(kAddrHexLen)) {
        return false;
    }
    for (int i = 0; i < kAddrHexLen; ++i) {
        if (!is_hex_char(payload[i])) {
            return false;
        }
    }
    snprintf(address, addr_sz, "%.*s", kAddrHexLen, payload.c_str());
    snprintf(name, name_sz, "%s", payload.c_str() + kAddrHexLen);
    return true;
}

static void handle_response_line(const std::string& raw_line) {
    std::string line = raw_line;
    trim_line(line);
    if (line.empty()) {
        return;
    }

    ESP_LOGI(TAG, "RX: %s", line.c_str());

    if (line.find("SET MODE 1") != std::string::npos) {
        s_active_mode = BtMode::kMode1;
        s_conn_state  = ConnState::kIdle;
        post_status("模式1 已设置", kColorSuccess);
        lv_async_call(async_on_mode1_set, nullptr);
        return;
    }
    if (line.find("SET MODE 2") != std::string::npos) {
        s_active_mode = BtMode::kMode2;
        s_conn_state  = ConnState::kIdle;
        post_status("模式2 已设置，可扫描设备", kColorSuccess);
        lv_async_call(async_on_mode2_set, nullptr);
        return;
    }
    if (line.find("SET MODE 3") != std::string::npos) {
        s_active_mode = BtMode::kMode3;
        s_conn_state  = ConnState::kIdle;
        post_status("模式3 已设置", kColorSuccess);
        lv_async_call(async_on_mode3_set, nullptr);
        return;
    }

    if (line.find("RECONNECT") != std::string::npos) {
        post_status(line.c_str(), kColorSubtle);
        return;
    }

    if (line.find("INQUIRING START") != std::string::npos) {
        s_conn_state = ConnState::kScanning;
        s_devices.clear();
        post_clear_list();
        post_status("正在扫描...", kColorScanning);
        return;
    }

    char address[kAddrHexLen + 1];
    char name[64];
    if (parse_bt_device_line(line, address, sizeof(address), name,
                             sizeof(name))) {
        BtDevice dev{};
        snprintf(dev.address, sizeof(dev.address), "%s", address);
        snprintf(dev.name, sizeof(dev.name), "%s", name);
        s_devices.push_back(dev);
        add_device_to_list(address, name);
        char status[96];
        snprintf(status, sizeof(status), "发现设备: %s",
                 name[0] ? name : address);
        post_status(status, kColorSubtle);
        return;
    }

    if (line.find("INQ COMPLETE") != std::string::npos) {
        s_conn_state = ConnState::kIdle;
        char status[64];
        snprintf(status, sizeof(status), "扫描完成，共 %d 个设备",
                 static_cast<int>(s_devices.size()));
        post_status(status, kColorSuccess);
        return;
    }

    if (line.find("CONNECTING") != std::string::npos) {
        s_conn_state = ConnState::kConnecting;
        post_status("正在连接...", kColorScanning);
        return;
    }

    if (line.find("CONNECT SUCCESS") != std::string::npos) {
        s_conn_state = ConnState::kConnected;
        post_status("连接成功", kColorSuccess);
        return;
    }

    if (line.find("CONNECT TIMEOUT") != std::string::npos) {
        s_conn_state = ConnState::kIdle;
        post_status("连接失败 (超时)", kColorError);
        return;
    }

    if (line.find("SETUP SCO") != std::string::npos) {
        post_status("通话模式 (SCO 已建立)", kColorSuccess);
        return;
    }

    if (line.find("DISC SCO") != std::string::npos) {
        post_status("音乐模式 (SCO 已断开)", kColorSuccess);
        return;
    }

    post_status(line.c_str(), kColorSubtle);
}

static void on_uart_data(const std::vector<uint8_t>& data) {
    s_rx_buffer.append(data.begin(), data.end());

    size_t pos = 0;
    while (true) {
        size_t nl = s_rx_buffer.find('\n', pos);
        if (nl == std::string::npos) {
            break;
        }
        std::string line = s_rx_buffer.substr(pos, nl - pos);
        handle_response_line(line);
        pos = nl + 1;
    }
    if (pos > 0) {
        s_rx_buffer.erase(0, pos);
    }

    if (s_rx_buffer.size() > 2048) {
        ESP_LOGW(TAG, "RX buffer overflow, clearing");
        s_rx_buffer.clear();
    }
}

struct ModeCmdArgs {
    BtMode mode;
};

static void mode_cmd_task(void* param) {
    auto* args = static_cast<ModeCmdArgs*>(param);
    SimpleUart& uart = SimpleUart::getInstance();

    switch (args->mode) {
        case BtMode::kMode1:
            post_status("切换模式1...", kColorScanning);
            uart.sendString("AT+RX=2\r\n");
            ESP_LOGI(TAG, "TX: AT+RX=2");
            vTaskDelay(pdMS_TO_TICKS(700));
            uart.sendString("AT+MODE=1\r\n");
            ESP_LOGI(TAG, "TX: AT+MODE=1");
            break;
        case BtMode::kMode2:
            post_status("切换模式2...", kColorScanning);
            uart.sendString("AT+TX=1\r\n");
            ESP_LOGI(TAG, "TX: AT+TX=1");
            vTaskDelay(pdMS_TO_TICKS(700));
            uart.sendString("AT+MODE=2\r\n");
            ESP_LOGI(TAG, "TX: AT+MODE=2");
            break;
        case BtMode::kMode3:
            post_status("切换模式3...", kColorScanning);
            uart.sendString("AT+RX=1\r\n");
            ESP_LOGI(TAG, "TX: AT+RX=1");
            vTaskDelay(pdMS_TO_TICKS(700));
            uart.sendString("AT+MODE=3\r\n");
            ESP_LOGI(TAG, "TX: AT+MODE=3");
            break;
        default:
            break;
    }

    delete args;
    vTaskDelete(nullptr);
}

static void send_mode_command(BtMode mode) {
    if (!SimpleUart::getInstance().isInitialized()) {
        post_status("UART 未初始化", kColorError);
        ESP_LOGE(TAG, "SimpleUart not initialized");
        return;
    }
    auto* args = new ModeCmdArgs{mode};
    xTaskCreate(mode_cmd_task, "bt_mode_cmd", 4096, args, 5, nullptr);
}

static void call_mode_task(void* /*param*/) {
    SimpleUart& uart = SimpleUart::getInstance();
    post_status("切换通话模式...", kColorScanning);
    uart.sendString("AT+PP=1\r\n");
    ESP_LOGI(TAG, "TX: AT+PP=1");
    vTaskDelay(pdMS_TO_TICKS(200));
    uart.sendString("AT+BTSCO=1\r\n");
    ESP_LOGI(TAG, "TX: AT+BTSCO=1");
    vTaskDelete(nullptr);
}

static void music_mode_task(void* /*param*/) {
    SimpleUart& uart = SimpleUart::getInstance();
    post_status("切换音乐模式...", kColorScanning);
    uart.sendString("AT+BTSCO=0\r\n");
    ESP_LOGI(TAG, "TX: AT+BTSCO=0");
    vTaskDelay(pdMS_TO_TICKS(200));
    uart.sendString("AT+PP=1\r\n");
    ESP_LOGI(TAG, "TX: AT+PP=1");
    vTaskDelete(nullptr);
}

void on_mode_btn_clicked(lv_event_t* e) {
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(e)));
    send_mode_command(static_cast<BtMode>(idx + 1));
}

static void async_after_bt_reset(void* /*user_data*/) {
    refresh_mode_buttons();
    show_mode1_panel(false);
    show_mode2_panel(false);
}

static void bt_reset_task(void* /*param*/) {
    post_status("正在复位蓝牙...", kColorScanning);
    auto& io_expander = IOExpander::getInstance();
    io_expander.setLevel(IOExpander::Pin::BT_POWER, false);
    ESP_LOGI(TAG, "BT_POWER off");
    vTaskDelay(pdMS_TO_TICKS(300));
    io_expander.setLevel(IOExpander::Pin::BT_POWER, true);
    ESP_LOGI(TAG, "BT_POWER on");
    s_active_mode = BtMode::kNone;
    s_conn_state  = ConnState::kIdle;
    lv_async_call(async_after_bt_reset, nullptr);
    post_status("蓝牙电源已复位", kColorSuccess);
    vTaskDelete(nullptr);
}

void on_reset_bt_clicked(lv_event_t* /*e*/) {
    xTaskCreate(bt_reset_task, "bt_reset", 4096, nullptr, 5, nullptr);
}

void on_scan_clicked(lv_event_t* /*e*/) {
    if (s_active_mode != BtMode::kMode2) {
        post_status("请先切换到模式2", kColorError);
        return;
    }
    if (!SimpleUart::getInstance().isInitialized()) {
        post_status("UART 未初始化", kColorError);
        return;
    }
    SimpleUart::getInstance().sendString("AT+INQUIRING\r\n");
    ESP_LOGI(TAG, "TX: AT+INQUIRING");
    s_devices.clear();
    post_clear_list();
    post_status("开始扫描...", kColorScanning);
}

void on_call_mode_clicked(lv_event_t* /*e*/) {
    if (s_conn_state != ConnState::kConnected) {
        post_status("请先连接蓝牙设备", kColorError);
        return;
    }
    xTaskCreate(call_mode_task, "bt_call_mode", 4096, nullptr, 5, nullptr);
}

void on_music_mode_clicked(lv_event_t* /*e*/) {
    if (s_conn_state != ConnState::kConnected) {
        post_status("请先连接蓝牙设备", kColorError);
        return;
    }
    xTaskCreate(music_mode_task, "bt_music_mode", 4096, nullptr, 5, nullptr);
}

void OnSwipeBack() {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home    = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void on_back_clicked(lv_event_t* /*e*/) { OnSwipeBack(); }

void on_screen_unloaded(lv_event_t* /*e*/) {
    s_screen_active = false;
    s_ui.screen          = nullptr;
    s_ui.status_label    = nullptr;
    s_ui.mode1_panel     = nullptr;
    s_ui.mode2_panel     = nullptr;
    s_ui.scan_btn        = nullptr;
    s_ui.device_list     = nullptr;
    s_ui.music_btn       = nullptr;
    s_ui.call_btn        = nullptr;
    for (int i = 0; i < 3; ++i) {
        s_ui.mode_btns[i] = nullptr;
    }
    s_rx_buffer.clear();
    s_devices.clear();
    // 注意：不要把 s_active_mode 清回 kNone —— 屏幕卸载并不会改变 BT
    // 硬件的实际模式。下次进入页面时我们要根据它正确还原 UI（开机时已经
    // 由 ApplyDefaultMode() 切到 kMode1，第一次进入应该看到模式1激活）。
    // 只在 bt_reset_task 那种 "硬件真的复位了" 的路径上清。
    s_conn_state  = ConnState::kIdle;
}

lv_obj_t* make_mode_button(lv_obj_t* parent, const char* label, int idx,
                           int x, int y, int w, int h) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_add_event_cb(btn, on_mode_btn_clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(idx)));

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

}  // namespace

lv_obj_t* BluetoothScreen::Create() {
    // s_active_mode 保留先前值（开机 ApplyDefaultMode() 已经把它写成
    // kMode1；重置蓝牙后会被清成 kNone）。这样首次进入页面就能反映出
    // 设备当前实际模式，无需用户手动再点一次。
    s_conn_state    = ConnState::kIdle;
    s_screen_active = true;
    s_rx_buffer.clear();
    s_devices.clear();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen   = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* header = lv_obj_create(scr);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, 90);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // 左上角返回按钮：透明圆形按钮 + "←" 图标，按下时白色半透明叠加
    constexpr int kBackBtnSize = 72;
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
    lv_obj_add_event_cb(back, on_back_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "蓝牙配置");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    lv_obj_t* reset = lv_button_create(header);
    lv_obj_set_size(reset, 160, 60);
    lv_obj_align(reset, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_radius(reset, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(reset, lv_color_hex(0xC4761A), LV_PART_MAIN);
    lv_obj_add_event_cb(reset, on_reset_bt_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* reset_lbl = lv_label_create(reset);
    lv_label_set_text(reset_lbl, "复位蓝牙");
    lv_obj_set_style_text_color(reset_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(reset_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(reset_lbl);

    // 提示：复位蓝牙是烧录蓝牙固件时才用的高危操作，挪到按钮左侧避免误点
    lv_obj_t* reset_hint = lv_label_create(header);
    lv_label_set_text(reset_hint, "烧录蓝牙固件时使用");
    lv_obj_set_style_text_color(reset_hint, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(reset_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align_to(reset_hint, reset, LV_ALIGN_OUT_LEFT_MID, -12, 0);

    constexpr int kModeBtnW = 200;
    constexpr int kModeBtnH = 64;
    constexpr int kModeGap  = 20;
    constexpr int kModeTop  = 110;
    const int kModeTotalW = 3 * kModeBtnW + 2 * kModeGap;
    const int kModeStartX = (kPanelW - kModeTotalW) / 2;

    const char* mode_labels[] = {"模式1", "模式2", "模式3"};
    for (int i = 0; i < 3; ++i) {
        s_ui.mode_btns[i] = make_mode_button(
            scr, mode_labels[i], i,
            kModeStartX + i * (kModeBtnW + kModeGap), kModeTop,
            kModeBtnW, kModeBtnH);
    }

    lv_obj_t* status = lv_label_create(scr);
    s_ui.status_label = status;
    lv_label_set_text(status, "请选择蓝牙模式");
    lv_obj_set_style_text_color(status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_width(status, kPanelW - 60);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 190);

    // ---- 模式1 面板：仅状态提示 ----
    lv_obj_t* m1_panel = lv_obj_create(scr);
    s_ui.mode1_panel = m1_panel;
    screen_strip_obj_chrome(m1_panel);
    lv_obj_set_size(m1_panel, kPanelW - 40, 160);
    lv_obj_set_pos(m1_panel, 20, 250);
    lv_obj_set_style_bg_color(m1_panel, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m1_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(m1_panel, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(m1_panel, 20, LV_PART_MAIN);
    lv_obj_add_flag(m1_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(m1_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* m1_hint = lv_label_create(m1_panel);
    lv_label_set_text(m1_hint, "模式1 已激活\n(AT+RX=2 / AT+MODE=1)");
    lv_obj_set_style_text_color(m1_hint, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(m1_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(m1_hint);

    // ---- 模式2 面板：扫描 + 设备列表 + 音乐/通话模式 ----
    lv_obj_t* panel = lv_obj_create(scr);
    s_ui.mode2_panel = panel;
    screen_strip_obj_chrome(panel);
    lv_obj_set_size(panel, kPanelW - 40, 420);
    lv_obj_set_pos(panel, 20, 250);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 16, LV_PART_MAIN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* scan = lv_button_create(panel);
    s_ui.scan_btn = scan;
    lv_obj_set_size(scan, 220, 56);
    lv_obj_align(scan, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(scan, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scan, lv_color_hex(kColorBtnActive),
                              LV_PART_MAIN);
    lv_obj_add_event_cb(scan, on_scan_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* scan_lbl = lv_label_create(scan);
    lv_label_set_text(scan_lbl, "扫描设备");
    lv_obj_set_style_text_color(scan_lbl, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(scan_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(scan_lbl);

    lv_obj_t* list = lv_obj_create(panel);
    s_ui.device_list = list;
    screen_strip_obj_chrome(list);
    lv_obj_set_size(list, LV_PCT(100), 240);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x12151C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t* music = lv_button_create(panel);
    s_ui.music_btn = music;
    lv_obj_set_size(music, 280, 56);
    lv_obj_align(music, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(music, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(music, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_add_event_cb(music, on_music_mode_clicked, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_t* music_lbl = lv_label_create(music);
    lv_label_set_text(music_lbl, "音乐模式");
    lv_obj_set_style_text_color(music_lbl, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(music_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(music_lbl);

    lv_obj_t* call = lv_button_create(panel);
    s_ui.call_btn = call;
    lv_obj_set_size(call, 280, 56);
    lv_obj_align(call, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(call, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(call, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_add_event_cb(call, on_call_mode_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* call_lbl = lv_label_create(call);
    lv_label_set_text(call_lbl, "通话模式");
    lv_obj_set_style_text_color(call_lbl, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(call_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(call_lbl);

    // 按当前 s_active_mode 还原 UI。开机时 ApplyDefaultMode() 已经把硬件
    // 切到 kMode1 并写好 s_active_mode；用户首次进入页面就能看到 "模式1
    // 已激活" 而不是 "请选择蓝牙模式"。
    refresh_mode_buttons();
    switch (s_active_mode) {
        case BtMode::kMode1:
            show_mode1_panel(true);
            show_mode2_panel(false);
            update_status_label("模式1 已设置", kColorSuccess);
            break;
        case BtMode::kMode2:
            show_mode1_panel(false);
            show_mode2_panel(true);
            update_status_label("模式2 已设置，可扫描设备", kColorSuccess);
            break;
        case BtMode::kMode3:
            show_mode1_panel(false);
            show_mode2_panel(false);
            update_status_label("模式3 已设置", kColorSuccess);
            break;
        default:
            show_mode1_panel(false);
            show_mode2_panel(false);
            break;
    }

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}

void BluetoothScreen::ApplyDefaultMode() {
    // 1. 预先把内部状态置为 kMode1，让用户首次进入页面就能反映出当前
    //    硬件模式（即使模块没回 "SET MODE 1" 也能正确高亮）。
    // 2. 复用现有的 send_mode_command 流程：起一个 4KB 栈的 task 发送
    //    AT+RX=2 / AT+MODE=1（中间 700ms 间隔，与协议匹配）。这条路径
    //    上 post_status() 自带 s_screen_active 守卫，UI 未启动时是
    //    no-op，可以在 InitializeBTAudio() 阶段安全调用。
    s_active_mode = BtMode::kMode1;
    send_mode_command(BtMode::kMode1);
}

void BluetoothScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: bluetooth_screen");
        s_rx_buffer.clear();
        SimpleUart::getInstance().registerCallback(on_uart_data);
    } else {
        ESP_LOGI(TAG, "unload: bluetooth_screen");
        SimpleUart::getInstance().registerCallback(
            std::function<void(const std::vector<uint8_t>&)>());
        s_screen_active = false;
    }
}
