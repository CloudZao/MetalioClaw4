/*
 * CX25601N charger (I2C) — ESP-IDF port from MTK/Linux reference drivers.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CX25601N_I2C_ADDR           0x6B

#define CX25601N_ICHG_MIN_MA        80
#define CX25601N_ICHG_MAX_MA        3040
#define CX25601N_ICHG_STEP_MA       80

/* IINDPM = code * 20 mA, code 5..150 → 100..3000 mA */
#define CX25601N_IINDPM_MIN_MA      100
#define CX25601N_IINDPM_MAX_MA      3000
#define CX25601N_IINDPM_STEP_MA     20

#define CX25601N_VREG_MIN_MV        3840
#define CX25601N_VREG_MAX_MV        4800
#define CX25601N_VREG_STEP_MV       10

/* REG0x1E CHG_STAT[4:3] (datasheet V2.2.2) */
#define CX25601N_CHG_STAT_NOT       0  /* 未充电/终止 */
#define CX25601N_CHG_STAT_CC        1  /* 涓流/预充/CC */
#define CX25601N_CHG_STAT_CV        2  /* 恒压降流 */
#define CX25601N_CHG_STAT_TOPOFF    3  /* Top-off */

esp_err_t cx25601n_init(i2c_master_bus_handle_t bus);
bool cx25601n_is_ready(void);

esp_err_t cx25601n_enable_charge(bool enable);
esp_err_t cx25601n_is_charge_enabled(bool *enabled);

esp_err_t cx25601n_set_ichg_ma(uint32_t ma);
esp_err_t cx25601n_get_ichg_ma(uint32_t *ma);

esp_err_t cx25601n_set_iindpm_ma(uint32_t ma);
esp_err_t cx25601n_get_iindpm_ma(uint32_t *ma);

esp_err_t cx25601n_set_vreg_mv(uint32_t mv);
esp_err_t cx25601n_get_vreg_mv(uint32_t *mv);

esp_err_t cx25601n_get_chrg_stat(uint8_t *stat);
esp_err_t cx25601n_get_vbus_stat(uint8_t *stat);
esp_err_t cx25601n_read_reg(uint8_t reg, uint8_t *val);

const char *cx25601n_chrg_stat_str(uint8_t stat);
const char *cx25601n_vbus_stat_str(uint8_t stat);

#ifdef __cplusplus
}
#endif
