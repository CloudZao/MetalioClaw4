#include "bq27220_test.h"
#include "i18n.h"

#include <cstdio>

#include "bq27220_gauge.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "test_ui_common.h"

extern "C" i2c_master_bus_handle_t metalio_claw_4_get_i2c_bus();

namespace {

constexpr const char* TAG = "Bq27220Test";

lv_obj_t* s_value_lbl   = nullptr;
lv_obj_t* s_status_icon = nullptr;
bool      s_comm_ok     = false;

void SetErrorText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorError),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, false);
}

void SetValueText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, true);
}

const char* ChargeLabel(bool charging, bool discharging) {
    if (charging) {
        return I18n::T("充电中");
    }
    if (discharging) {
        return I18n::T("放电中");
    }
    return I18n::T("空闲");
}

bool EnsureGauge() {
    auto& gauge = Bq27220Gauge::GetInstance();
    if (gauge.IsReady()) {
        s_comm_ok = true;
        return true;
    }

    i2c_master_bus_handle_t bus = metalio_claw_4_get_i2c_bus();
    if (bus == nullptr) {
        ESP_LOGE(TAG, "I2C bus not ready");
        s_comm_ok = false;
        return false;
    }

    s_comm_ok = gauge.Begin(bus, Bq27220Gauge::kDefaultAddr);
    if (!s_comm_ok) {
        ESP_LOGW(TAG, "BQ27220 @0x%02X not found", Bq27220Gauge::kDefaultAddr);
    }
    return s_comm_ok;
}

}  // namespace

namespace Bq27220Test {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "BQ27220", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, I18n::T("检测中..."));
}

void OnLoad() {
    s_comm_ok = false;
    EnsureGauge();
    Poll();
}

void OnUnload() {
    s_value_lbl = nullptr;
    s_status_icon = nullptr;
    s_comm_ok = false;
}

void Poll() {
    if (s_value_lbl == nullptr) {
        return;
    }

    if (!EnsureGauge()) {
        SetErrorText(I18n::T("I2C未检测到"));
        return;
    }

    auto& gauge = Bq27220Gauge::GetInstance();
    uint16_t mv = 0;
    int level = 0;
    bool charging = false;
    bool discharging = false;

    if (!gauge.GetVoltageMv(mv) ||
        !gauge.GetBatteryLevel(level, charging, discharging)) {
        s_comm_ok = false;
        SetErrorText(I18n::T("读取失败"));
        return;
    }

    char buf[48];
    std::snprintf(buf, sizeof(buf), "%umV %s", mv,
                  ChargeLabel(charging, discharging));
    SetValueText(buf);
}

}  // namespace Bq27220Test
