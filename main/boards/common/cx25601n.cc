/*
 * CX25601N charger driver — ESP-IDF port (C++).
 * Register map aligned with example/cx2560x.c (MediaTek reference).
 */

#include "cx25601n.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

constexpr const char *TAG = "cx25601n";

/* --- Register map (CX25601N) --- */
constexpr uint8_t REG_ICHG_LO    = 0x02;
constexpr uint8_t REG_ICHG_HI    = 0x03;
constexpr uint8_t REG_VREG_LO    = 0x04;
constexpr uint8_t REG_VREG_HI    = 0x05;
constexpr uint8_t REG_IINDPM_LO  = 0x06;
constexpr uint8_t REG_IINDPM_HI  = 0x07;
constexpr uint8_t REG_IPRECHG_LO = 0x10;
constexpr uint8_t REG_IPRECHG_HI = 0x11;
constexpr uint8_t REG_ITERM_LO   = 0x12;
constexpr uint8_t REG_ITERM_HI   = 0x13;
constexpr uint8_t REG_CHG_CTRL0  = 0x14;
constexpr uint8_t REG_CHG_TMR    = 0x15;
constexpr uint8_t REG_CHG_CTRL1  = 0x16; /* EN_HIZ bit4, EN_CHG bit5, WDT[1:0] */
constexpr uint8_t REG_PART_INFO  = 0x38;
constexpr uint8_t REG_STATUS1    = 0x1E;
constexpr uint8_t REG_UNLOCK     = 0x70;

constexpr int I2C_TIMEOUT_MS = 100;

i2c_master_dev_handle_t s_dev = nullptr;
SemaphoreHandle_t s_lock = nullptr;
bool s_ready = false;

esp_err_t read_byte(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

esp_err_t write_byte(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, I2C_TIMEOUT_MS);
}

esp_err_t update_bits(uint8_t reg, uint8_t mask, uint8_t shift, uint8_t field)
{
    uint8_t cur = 0;
    esp_err_t err = read_byte(reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t m = static_cast<uint8_t>(mask << shift);
    cur = static_cast<uint8_t>((cur & static_cast<uint8_t>(~m)) |
                               ((field << shift) & m));
    return write_byte(reg, cur);
}

esp_err_t read_bits(uint8_t reg, uint8_t mask, uint8_t shift, uint8_t *field)
{
    uint8_t cur = 0;
    esp_err_t err = read_byte(reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    *field = static_cast<uint8_t>((cur >> shift) & mask);
    return ESP_OK;
}

void unlock_private(bool enable)
{
    if (!enable) {
        write_byte(REG_UNLOCK, 0x00);
        return;
    }
    for (int i = 0; i < 10; i++) {
        write_byte(REG_UNLOCK, 0x00);
        write_byte(REG_UNLOCK, 0x50);
        write_byte(REG_UNLOCK, 0x57);
        write_byte(REG_UNLOCK, 0x44);
        uint8_t v = 0;
        if (read_byte(REG_UNLOCK, &v) == ESP_OK && v == 0x03) {
            return;
        }
    }
    ESP_LOGW(TAG, "private register unlock failed");
}

esp_err_t set_dis_dpdm(bool disable)
{
    /* REG0x15 bit6 EN_AUTO_INDET: disable→0, enable→1（对齐 cx25601_set_dis_dpdm） */
    return update_bits(REG_CHG_TMR, 0x01, 6, disable ? 0 : 1);
}

esp_err_t set_iindpm_ma_nolock(uint32_t ma)
{
    if (ma < CX25601N_IINDPM_MIN_MA) {
        ma = CX25601N_IINDPM_MIN_MA;
    }
    if (ma > CX25601N_IINDPM_MAX_MA) {
        ma = CX25601N_IINDPM_MAX_MA;
    }
    uint32_t code = ma / CX25601N_IINDPM_STEP_MA;
    if (code < 5) {
        code = 5;
    }
    if (code > 150) {
        code = 150;
    }

    /* IINDPM[3:0] @ 0x06[7:4], IINDPM[7:4] @ 0x07[3:0] */
    esp_err_t err = update_bits(REG_IINDPM_LO, 0x0F, 4, static_cast<uint8_t>(code & 0x0F));
    if (err == ESP_OK) {
        err = update_bits(REG_IINDPM_HI, 0x0F, 0, static_cast<uint8_t>((code >> 4) & 0x0F));
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "set IINDPM=%lu mA (code=%lu)",
                 static_cast<unsigned long>(code * CX25601N_IINDPM_STEP_MA),
                 static_cast<unsigned long>(code));
    }
    return err;
}

esp_err_t hw_init_defaults(void)
{
    /* 上电关闭 D+/D- 自动识别，避免被判定为 SDP 后限流 500mA */
    ESP_RETURN_ON_ERROR(set_dis_dpdm(true), TAG, "dis_dpdm");

    /* Disable HIZ & watchdog; clear EN_CHG initially (UI will enable). */
    ESP_RETURN_ON_ERROR(update_bits(REG_CHG_CTRL1, 0x01, 4, 0), TAG, "HIZ");
    ESP_RETURN_ON_ERROR(update_bits(REG_CHG_CTRL1, 0x03, 0, 0), TAG, "WDT");

    /* IPRECHG = 12 * 20mA = 240mA */
    ESP_RETURN_ON_ERROR(update_bits(REG_IPRECHG_LO, 0x0F, 4, 0x0C), TAG, "iprechg lo");
    ESP_RETURN_ON_ERROR(update_bits(REG_IPRECHG_HI, 0x01, 0, 0x00), TAG, "iprechg hi");

    /* ITERM = 18 * 10mA = 180mA */
    ESP_RETURN_ON_ERROR(update_bits(REG_ITERM_LO, 0x1F, 3, 0x12), TAG, "iterm lo");
    ESP_RETURN_ON_ERROR(update_bits(REG_ITERM_HI, 0x01, 0, 0x00), TAG, "iterm hi");

    /* VREG default 4200mV */
    ESP_RETURN_ON_ERROR(cx25601n_set_vreg_mv(4200), TAG, "default vreg");

    /* Default ICHG = 1000mA；应用电流时同步写 IINDPM(0x06/0x07) */
    ESP_RETURN_ON_ERROR(cx25601n_set_ichg_ma(1000), TAG, "default ichg");

    /* Vendor private init sequence from reference */
    unlock_private(true);
    write_byte(0x86, 0x06);
    write_byte(0x3A, 0x10);
    write_byte(0x46, 0x20);
    unlock_private(false);

    update_bits(REG_CHG_CTRL0, 0x01, 0, 1);
    update_bits(0x18, 0x01, 2, 0);
    update_bits(0x1A, 0x01, 7, 1);
    update_bits(0x23, 0x07, 2, 0x07);
    update_bits(0x24, 0x01, 3, 1);

    ESP_LOGI(TAG, "hw defaults applied");
    return ESP_OK;
}

}  // namespace

esp_err_t cx25601n_init(i2c_master_bus_handle_t bus)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = CX25601N_I2C_ADDR;
    dev_cfg.scl_speed_hz = 400000;

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t part = 0;
    err = read_byte(REG_PART_INFO, &part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "probe PART_INFO(0x38) failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "PART_INFO=0x%02X @0x%02X", part, CX25601N_I2C_ADDR);

    err = hw_init_defaults();
    if (err != ESP_OK) {
        return err;
    }

    s_ready = true;
    return ESP_OK;
}

bool cx25601n_is_ready(void)
{
    return s_ready;
}

esp_err_t cx25601n_read_reg(uint8_t reg, uint8_t *val)
{
    if (!s_ready || !val) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_byte(reg, val);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t cx25601n_enable_charge(bool enable)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (enable) {
        err = update_bits(REG_CHG_CTRL1, 0x01, 4, 0); /* clear HIZ */
        if (err == ESP_OK) {
            err = update_bits(REG_CHG_CTRL1, 0x01, 5, 1); /* EN_CHG */
        }
    } else {
        err = update_bits(REG_CHG_CTRL1, 0x01, 5, 0);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "charge %s", enable ? "ON" : "OFF");
    return err;
}

esp_err_t cx25601n_is_charge_enabled(bool *enabled)
{
    if (!s_ready || !enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t bit = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_CHG_CTRL1, 0x01, 5, &bit);
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        *enabled = bit != 0;
    }
    return err;
}

esp_err_t cx25601n_set_iindpm_ma(uint32_t ma)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = set_iindpm_ma_nolock(ma);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t cx25601n_get_iindpm_ma(uint32_t *ma)
{
    if (!s_ready || !ma) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t lo = 0, hi = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_IINDPM_LO, 0x0F, 4, &lo);
    if (err == ESP_OK) {
        err = read_bits(REG_IINDPM_HI, 0x0F, 0, &hi);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t code = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 4);
    *ma = code * CX25601N_IINDPM_STEP_MA;
    return ESP_OK;
}

esp_err_t cx25601n_set_ichg_ma(uint32_t ma)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ma < CX25601N_ICHG_MIN_MA) {
        ma = CX25601N_ICHG_MIN_MA;
    }
    if (ma > CX25601N_ICHG_MAX_MA) {
        ma = CX25601N_ICHG_MAX_MA;
    }
    uint32_t code = ma / CX25601N_ICHG_STEP_MA;
    if (code < 1) {
        code = 1;
    }
    if (code > 38) {
        code = 38;
    }
    uint32_t applied_ma = code * CX25601N_ICHG_STEP_MA;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* ICHG[1:0] @ 0x02[7:6], ICHG[5:2] @ 0x03[3:0] */
    esp_err_t err = update_bits(REG_ICHG_LO, 0x03, 6, static_cast<uint8_t>(code & 0x03));
    if (err == ESP_OK) {
        err = update_bits(REG_ICHG_HI, 0x0F, 0, static_cast<uint8_t>((code >> 2) & 0x0F));
    }
    /* 应用电流时同步写输入限流 IINDPM（0x06/0x07），避免被 SDP 等默认限流卡住 */
    if (err == ESP_OK) {
        err = set_iindpm_ma_nolock(applied_ma);
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "set ICHG=%lu mA (code=%lu)", static_cast<unsigned long>(applied_ma),
             static_cast<unsigned long>(code));
    return err;
}

esp_err_t cx25601n_get_ichg_ma(uint32_t *ma)
{
    if (!s_ready || !ma) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t lo = 0, hi = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_ICHG_LO, 0x03, 6, &lo);
    if (err == ESP_OK) {
        err = read_bits(REG_ICHG_HI, 0x0F, 0, &hi);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t code = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 2);
    *ma = code * CX25601N_ICHG_STEP_MA;
    return ESP_OK;
}

esp_err_t cx25601n_set_vreg_mv(uint32_t mv)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mv < CX25601N_VREG_MIN_MV) {
        mv = CX25601N_VREG_MIN_MV;
    }
    if (mv > CX25601N_VREG_MAX_MV) {
        mv = CX25601N_VREG_MAX_MV;
    }
    /* VREG = code * 10 mV, code range 384~480 */
    uint32_t code = mv / CX25601N_VREG_STEP_MV;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* VREG[4:0] @ 0x04[7:3], VREG[8:5] @ 0x05[3:0] */
    esp_err_t err = update_bits(REG_VREG_LO, 0x1F, 3, static_cast<uint8_t>(code & 0x1F));
    if (err == ESP_OK) {
        err = update_bits(REG_VREG_HI, 0x0F, 0, static_cast<uint8_t>((code >> 5) & 0x0F));
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "set VREG=%lu mV (code=%lu)",
             static_cast<unsigned long>(code * CX25601N_VREG_STEP_MV),
             static_cast<unsigned long>(code));
    return err;
}

esp_err_t cx25601n_get_vreg_mv(uint32_t *mv)
{
    if (!s_ready || !mv) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t lo = 0, hi = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_VREG_LO, 0x1F, 3, &lo);
    if (err == ESP_OK) {
        err = read_bits(REG_VREG_HI, 0x0F, 0, &hi);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t code = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 5);
    *mv = code * CX25601N_VREG_STEP_MV;
    return ESP_OK;
}

esp_err_t cx25601n_get_chrg_stat(uint8_t *stat)
{
    if (!s_ready || !stat) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_STATUS1, 0x03, 3, stat);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t cx25601n_get_vbus_stat(uint8_t *stat)
{
    if (!s_ready || !stat) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_bits(REG_STATUS1, 0x07, 0, stat);
    xSemaphoreGive(s_lock);
    return err;
}

const char *cx25601n_chrg_stat_str(uint8_t stat)
{
    switch (stat) {
    case CX25601N_CHG_STAT_NOT:
        return "未充电/已满";
    case CX25601N_CHG_STAT_CC:
        return "涓流/预充/CC";
    case CX25601N_CHG_STAT_CV:
        return "恒压降流";
    case CX25601N_CHG_STAT_TOPOFF:
        return "Top-off";
    default:
        return "未知";
    }
}

const char *cx25601n_vbus_stat_str(uint8_t stat)
{
    switch (stat) {
    case 0:
        return "无输入";
    case 1:
        return "USB SDP";
    case 2:
        return "USB CDP";
    case 3:
        return "USB DCP";
    case 4:
        return "未知适配器";
    case 5:
        return "非标适配器";
    case 7:
        return "OTG";
    default:
        return "适配器";
    }
}
