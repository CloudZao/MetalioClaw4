#include "nu1680_test.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "test_ui_common.h"

extern "C" i2c_master_bus_handle_t metalio_claw_4_get_i2c_bus();

namespace {

constexpr const char* TAG = "Nu1680Test";

// NU1680 无线充电 RX，未放上无线充时 I2C 无应答，属正常现象。
constexpr uint8_t kNu1680Addr      = 0x60;
constexpr int     kProbeTimeoutMs  = 100;

lv_obj_t* s_value_lbl   = nullptr;
lv_obj_t* s_status_icon = nullptr;
bool      s_pass_latched = false;

void SetErrorText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorError),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, false);
}

void SetPassText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, true);
}

bool ProbeDevice() {
    i2c_master_bus_handle_t bus = metalio_claw_4_get_i2c_bus();
    if (bus == nullptr) {
        ESP_LOGE(TAG, "I2C bus not ready");
        return false;
    }
    return i2c_master_probe(bus, kNu1680Addr, kProbeTimeoutMs) == ESP_OK;
}

}  // namespace

namespace Nu1680Test {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "NU1680", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, "检测中...");
}

void OnLoad() {
    s_pass_latched = false;
    Poll();
}

void OnUnload() {
    s_value_lbl = nullptr;
    s_status_icon = nullptr;
    s_pass_latched = false;
}

void Poll() {
    if (s_value_lbl == nullptr || s_pass_latched) {
        return;
    }

    if (!ProbeDevice()) {
        // 未无线充电时地址不可见，持续轮询，放到充上后即锁定通过。
        SetErrorText("未检测到(需无线充)");
        return;
    }

    s_pass_latched = true;
    SetPassText("无线充电成功");
    ESP_LOGI(TAG, "NU1680 probed once @0x%02X, stop further detection",
             kNu1680Addr);
}

}  // namespace Nu1680Test
