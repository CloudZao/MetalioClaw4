#include "gps_test.h"

#include <cstdio>

#include "IOExpander.hpp"
#include "esp_log.h"
#include "gps_service.h"
#include "test_ui_common.h"

namespace {

constexpr const char* TAG = "GpsTest";

lv_obj_t* s_status_icon = nullptr;
lv_obj_t* s_value_lbl   = nullptr;
bool      s_gps_powered = false;

void SetPassText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, true);
}

void SetFailText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorError),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, false);
}

}  // namespace

namespace GpsTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "GPS", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, "HDOP --");
}

void OnLoad() {
    auto& io = IOExpander::getInstance();
    io.setLevel(IOExpander::Pin::GPS_POWER, true);
    s_gps_powered = true;

    if (!GpsService::Instance().Start()) {
        ESP_LOGW(TAG, "GpsService start failed");
    }
    Poll();
}

void OnUnload() {
    if (s_gps_powered) {
        IOExpander::getInstance().setLevel(IOExpander::Pin::GPS_POWER, false);
        s_gps_powered = false;
    }
    s_value_lbl = nullptr;
    s_status_icon = nullptr;
}

void Poll() {
    if (s_value_lbl == nullptr) {
        return;
    }

    const GpsService::Snapshot snap = GpsService::Instance().GetSnapshot();

    if (snap.sentence_count == 0) {
        SetFailText("未收到NMEA");
        return;
    }

    if (snap.hdop > 0.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "HDOP %.1f", snap.hdop);
        SetPassText(buf);
    } else {
        SetPassText("HDOP --");
    }
}

}  // namespace GpsTest
