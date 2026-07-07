#include "sc7a20h_test.h"

#include <cstdio>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "i2c_device.h"
#include "test_ui_common.h"

extern "C" i2c_master_bus_handle_t metalio_claw_4_get_i2c_bus();

namespace {

constexpr const char* TAG = "Sc7a20hTest";

constexpr uint8_t kSc7a20hAddr = 0x19;
constexpr uint8_t kRegWhoAmI   = 0x0F;
constexpr uint8_t kRegCtrlReg1 = 0x20;
constexpr uint8_t kRegCtrlReg4 = 0x23;
constexpr uint8_t kRegOutXL    = 0x28;
constexpr uint8_t kAutoIncMask = 0x80;
constexpr uint8_t kCtrlReg1Val = 0x57;
constexpr uint8_t kCtrlReg4Val = 0x88;
constexpr float   kMgPerLsb    = 1.0f;

class Sc7a20h : public I2cDevice {
public:
    Sc7a20h(i2c_master_bus_handle_t bus, uint8_t addr) : I2cDevice(bus, addr) {}

    void Configure() {
        WriteReg(kRegCtrlReg1, kCtrlReg1Val);
        WriteReg(kRegCtrlReg4, kCtrlReg4Val);
    }

    bool ReadAccelMg(int* ax, int* ay, int* az) {
        uint8_t buf[6] = {0};
        ReadRegs(kRegOutXL | kAutoIncMask, buf, sizeof(buf));
        const int16_t rx =
            static_cast<int16_t>((buf[1] << 8) | buf[0]);
        const int16_t ry =
            static_cast<int16_t>((buf[3] << 8) | buf[2]);
        const int16_t rz =
            static_cast<int16_t>((buf[5] << 8) | buf[4]);
        *ax = static_cast<int>((rx >> 4) * kMgPerLsb);
        *ay = static_cast<int>((ry >> 4) * kMgPerLsb);
        *az = static_cast<int>((rz >> 4) * kMgPerLsb);
        return true;
    }

    uint8_t ReadWhoAmI() { return ReadReg(kRegWhoAmI); }
};

Sc7a20h*   s_sensor      = nullptr;
bool       s_sensor_ok   = false;
lv_obj_t*  s_value_lbl   = nullptr;
lv_obj_t*  s_status_icon = nullptr;

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

void EnsureSensor() {
    if (s_sensor != nullptr && s_sensor_ok) {
        return;
    }

    i2c_master_bus_handle_t bus = metalio_claw_4_get_i2c_bus();
    if (bus == nullptr) {
        ESP_LOGE(TAG, "I2C bus not ready");
        s_sensor_ok = false;
        return;
    }

    if (i2c_master_probe(bus, kSc7a20hAddr, 100) != ESP_OK) {
        ESP_LOGW(TAG, "I2C probe 0x%02X failed", kSc7a20hAddr);
        s_sensor_ok = false;
        return;
    }

    if (s_sensor == nullptr) {
        s_sensor = new Sc7a20h(bus, kSc7a20hAddr);
    }
    if (s_sensor == nullptr) {
        s_sensor_ok = false;
        return;
    }

    const uint8_t who = s_sensor->ReadWhoAmI();
    ESP_LOGI(TAG, "SC7A20H WHO_AM_I=0x%02X", who);
    s_sensor->Configure();
    s_sensor_ok = true;
}

}  // namespace

namespace Sc7a20hTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "SC7A20HTR", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, "初始化中...");
}

void OnLoad() {
    EnsureSensor();
    Poll();
}

void OnUnload() {
    s_value_lbl = nullptr;
    s_status_icon = nullptr;
}

void Poll() {
    if (s_value_lbl == nullptr) {
        return;
    }

    if (!s_sensor_ok || s_sensor == nullptr) {
        SetErrorText("I2C未检测到");
        return;
    }

    int ax = 0;
    int ay = 0;
    int az = 0;
    if (!s_sensor->ReadAccelMg(&ax, &ay, &az)) {
        SetErrorText("I2C读取失败");
        return;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "X:%dmg Y:%dmg Z:%dmg", ax, ay, az);
    SetValueText(buf);
}

}  // namespace Sc7a20hTest
