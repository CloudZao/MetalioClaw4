#include "qmc6309_test.h"

#include <cstdio>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_device.h"
#include "test_ui_common.h"

extern "C" i2c_master_bus_handle_t metalio_claw_4_get_i2c_bus();

namespace {

constexpr const char* TAG = "Qmc6309Test";

constexpr uint8_t kQmcAddr      = 0x7C;
constexpr uint8_t kQmcRegChipId = 0x00;
constexpr uint8_t kQmcRegXOutL  = 0x01;
constexpr uint8_t kQmcRegCr1    = 0x0A;
constexpr uint8_t kQmcRegCr2    = 0x0B;
constexpr uint8_t kQmcChipId    = 0x90;
constexpr uint8_t kQmcOsr11     = 0xC0;
constexpr uint8_t kQmcRng01      = 0x10;
constexpr uint8_t kQmcModeSuspend = 0x00;
constexpr uint8_t kQmcModeNormal  = 0x01;
constexpr uint8_t kQmcModeContin  = 0x03;
constexpr uint8_t kQmcCr1Base     = kQmcOsr11 | kQmcRng01;
constexpr uint8_t kQmcCr2Val      = 0x03;
constexpr uint8_t kQmcCr2SoftRst  = 0x80;

class Qmc6309 : public I2cDevice {
public:
    Qmc6309(i2c_master_bus_handle_t bus, uint8_t addr) : I2cDevice(bus, addr) {}

    bool Probe() {
        chip_id_ = ReadReg(kQmcRegChipId);
        if (chip_id_ != kQmcChipId) {
            ESP_LOGW(TAG, "CHIP_ID=0x%02X expect 0x90", chip_id_);
            return false;
        }
        return true;
    }

    void Configure() {
        WriteReg(kQmcRegCr2, kQmcCr2SoftRst);
        vTaskDelay(pdMS_TO_TICKS(25));
        WriteReg(kQmcRegCr2, 0x00);
        vTaskDelay(pdMS_TO_TICKS(25));

        SetMode(kQmcModeSuspend);
        SetMode(kQmcModeNormal);
        SetMode(kQmcModeSuspend);
        SetMode(kQmcModeContin);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void ReadMagRaw(int16_t* mx, int16_t* my, int16_t* mz) {
        uint8_t buf[6] = {0};
        ReadRegs(kQmcRegXOutL, buf, sizeof(buf));
        *mx = static_cast<int16_t>((buf[1] << 8) | buf[0]);
        *my = static_cast<int16_t>((buf[3] << 8) | buf[2]);
        *mz = static_cast<int16_t>((buf[5] << 8) | buf[4]);
    }

    uint8_t LastChipId() const { return chip_id_; }

private:
    void SetMode(uint8_t mode) {
        const uint8_t cr1 = kQmcCr1Base | (mode & 0x03);
        WriteReg(kQmcRegCr1, cr1);
        WriteReg(kQmcRegCr2, kQmcCr2Val);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint8_t chip_id_ = 0;
};

Qmc6309*  s_mag       = nullptr;
bool      s_mag_ok    = false;
lv_obj_t* s_value_lbl = nullptr;
lv_obj_t* s_status_icon = nullptr;

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
    if (s_mag != nullptr && s_mag_ok) {
        return;
    }

    i2c_master_bus_handle_t bus = metalio_claw_4_get_i2c_bus();
    if (bus == nullptr) {
        ESP_LOGE(TAG, "I2C bus not ready");
        s_mag_ok = false;
        return;
    }

    if (i2c_master_probe(bus, kQmcAddr, 100) != ESP_OK) {
        ESP_LOGW(TAG, "I2C probe 0x%02X failed", kQmcAddr);
        s_mag_ok = false;
        return;
    }

    if (s_mag == nullptr) {
        s_mag = new Qmc6309(bus, kQmcAddr);
    }
    if (s_mag == nullptr) {
        s_mag_ok = false;
        return;
    }

    if (!s_mag->Probe()) {
        s_mag_ok = false;
        return;
    }

    s_mag->Configure();
    s_mag_ok = true;
    ESP_LOGI(TAG, "QMC6309 online, ID=0x%02X", s_mag->LastChipId());
}

}  // namespace

namespace Qmc6309Test {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "QMC6309", &s_status_icon, &ctrl);
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

    if (!s_mag_ok || s_mag == nullptr) {
        SetErrorText("I2C未检测到");
        return;
    }

    int16_t mx = 0;
    int16_t my = 0;
    int16_t mz = 0;
    s_mag->ReadMagRaw(&mx, &my, &mz);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "X:%d Y:%d Z:%d", mx, my, mz);
    SetValueText(buf);
}

}  // namespace Qmc6309Test
