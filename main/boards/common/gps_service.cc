#include "gps_service.h"

#include <esp_log.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// NMEA-0183 reader for the xingzhi-395's on-board GPS module.
//
// Wiring:
//   - GPS power is gated by TCA9555 P0 ("GPS_POWER").  This service does
//     NOT touch that pin -- the GPS screen's lifecycle callback drives it
//     HIGH on screen LOAD and LOW on UNLOAD, so the module is only powered
//     while the user is looking at the page.
//   - The module's TX feeds GPIO 34 (ESP_RX); ESP_TX (GPIO 31) feeds the
//     module's RX -- we currently never transmit, but the UART driver
//     requires a valid TX pin, so we still configure it.
//   - 9600 bps, 8N1, no flow control.
//
// We accept GGA / RMC / GSV from any talker (GP / GN / GL / GA / BD), so the
// service works equally well with single-GPS or multi-GNSS modules.
// ---------------------------------------------------------------------------

namespace {

constexpr const char* TAG = "GpsService";

// ESP-IDF requires "RX buffer > UART_HW_FIFO_LEN (128)".  At 9600 bps a
// single second of traffic is ~1.2 KB, so 2 KB gives us comfortable headroom
// for any small parsing delays.
constexpr size_t kUartRxBufferBytes = 2048;
// We do not transmit, so the TX buffer can stay at the minimum (0 == driver
// will block until each byte is sent; we never write anyway).
constexpr size_t kUartTxBufferBytes = 0;

constexpr TickType_t kReadTimeoutTicks = pdMS_TO_TICKS(100);

uint32_t now_ms() {
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

}  // namespace

GpsService& GpsService::Instance() {
    static GpsService instance;
    return instance;
}

bool GpsService::Start() {
    if (started_) {
        return true;
    }

    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
        if (mutex_ == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate GPS mutex");
            return false;
        }
    }

    // -----------------------------------------------------------------
    // Configure UART_NUM_1 @ 9600 8N1, no flow control, RX on GPIO 34 and
    // TX on GPIO 31.  We allocate a 2 KB RX buffer; no TX buffer because
    // the application never writes.  Powering the module on / off is the
    // caller's responsibility -- see GpsScreen's lifecycle callback.
    // -----------------------------------------------------------------
    uart_config_t uart_cfg = {
        .baud_rate           = kGpsBaudRate,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk          = UART_SCLK_DEFAULT,
        .flags               = 0,
    };

    esp_err_t err = uart_driver_install(kGpsUartNum, kUartRxBufferBytes,
                                        kUartTxBufferBytes, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    err = uart_param_config(kGpsUartNum, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(kGpsUartNum);
        return false;
    }

    err = uart_set_pin(kGpsUartNum, kGpsTxPin, kGpsRxPin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(kGpsUartNum);
        return false;
    }

    // -----------------------------------------------------------------
    // Spawn the RX task.  4 KB of stack is comfortable for the NMEA
    // string handling (no heavy floats, no recursion).  Priority 4
    // matches the weather fetcher -- well below the UI thread but high
    // enough to drain the UART quickly.
    // -----------------------------------------------------------------
    BaseType_t task_ok = xTaskCreate(&GpsService::RxTaskTrampoline,
                                     "gps_rx", 4 * 1024, this, 4,
                                     &rx_task_);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn GPS RX task");
        uart_driver_delete(kGpsUartNum);
        return false;
    }

    if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
        data_.started = true;
        xSemaphoreGive(mutex_);
    }
    started_ = true;
    ESP_LOGI(TAG, "GPS service started (UART%d, %d bps, RX=%d, TX=%d)",
             (int)kGpsUartNum, kGpsBaudRate, kGpsRxPin, kGpsTxPin);
    return true;
}

GpsService::Snapshot GpsService::GetSnapshot() const {
    Snapshot copy;
    if (mutex_ == nullptr) {
        return copy;
    }
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        copy = data_;
        xSemaphoreGive(mutex_);
    }
    return copy;
}

// ---------------------------------------------------------------------------
// RX task
// ---------------------------------------------------------------------------

void GpsService::RxTaskTrampoline(void* arg) {
    static_cast<GpsService*>(arg)->RxTaskLoop();
}

void GpsService::RxTaskLoop() {
    uint8_t chunk[256];

    while (true) {
        int n = uart_read_bytes(kGpsUartNum, chunk, sizeof(chunk),
                                kReadTimeoutTicks);
        if (n <= 0) {
            continue;
        }

        if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        data_.bytes_received += static_cast<uint32_t>(n);
        xSemaphoreGive(mutex_);

        // Slot each byte into our line buffer; on terminator, parse.
        for (int i = 0; i < n; ++i) {
            const char c = static_cast<char>(chunk[i]);
            if (c == '\r' || c == '\n') {
                if (line_len_ > 0) {
                    line_buf_[line_len_] = '\0';
                    HandleSentence(line_buf_, line_len_);
                    line_len_ = 0;
                }
                continue;
            }
            if (line_len_ + 1 >= sizeof(line_buf_)) {
                // Garbage / overflow -- drop the buffered line and resync.
                line_len_ = 0;
                continue;
            }
            line_buf_[line_len_++] = c;
        }
    }
}

// ---------------------------------------------------------------------------
// Sentence dispatch
// ---------------------------------------------------------------------------

void GpsService::HandleSentence(const char* sentence, size_t len) {
    // Must start with '$' and be long enough to hold at least "$xxYYY".
    if (len < 6 || sentence[0] != '$') {
        return;
    }
    if (!ChecksumOk(sentence, len)) {
        // Bad checksums happen during boot / brown-outs; quietly skip them.
        return;
    }

    // Copy into a mutable buffer so we can split in-place by ','.
    char buf[160];
    if (len >= sizeof(buf)) {
        return;
    }
    std::memcpy(buf, sentence, len);
    buf[len] = '\0';

    // Strip checksum suffix (`*XX`) from the last field by terminating at '*'.
    char* star = std::strchr(buf, '*');
    if (star != nullptr) {
        *star = '\0';
    }

    // Tokenize.  We point fields[0] at the leading "$xxYYY" token so the
    // talker / sentence type can be extracted from it later.
    constexpr int kMaxFields = 24;
    const char* fields[kMaxFields] = {};
    int field_count = 0;

    char* cursor = buf;
    fields[field_count++] = cursor;
    while (*cursor != '\0' && field_count < kMaxFields) {
        if (*cursor == ',') {
            *cursor = '\0';
            fields[field_count++] = cursor + 1;
        }
        ++cursor;
    }

    // fields[0] looks like "$GPGGA", "$GNRMC", "$GLGSV", ...
    const char* head = fields[0];
    if (std::strlen(head) < 6) {
        return;
    }
    const char talker[3] = { head[1], head[2], '\0' };
    const char* sentence_type = head + 3;  // "GGA" / "RMC" / "GSV" / ...

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }
    data_.sentence_count += 1;
    data_.last_sentence_ms = now_ms();
    xSemaphoreGive(mutex_);

    if (std::strcmp(sentence_type, "GGA") == 0) {
        ParseGGA(fields, field_count);
    } else if (std::strcmp(sentence_type, "RMC") == 0) {
        ParseRMC(fields, field_count);
    } else if (std::strcmp(sentence_type, "GSV") == 0) {
        ParseGSV(talker, fields, field_count);
    }
    // Other sentence types (VTG, GSA, ...) are ignored for now.
}

// ---------------------------------------------------------------------------
// NMEA parsers
//
// GGA layout (fields after the talker):
//   1: HHMMSS.sss
//   2: latitude  (DDMM.mmmm)
//   3: N/S
//   4: longitude (DDDMM.mmmm)
//   5: E/W
//   6: fix quality (0..)
//   7: satellites in use
//   8: HDOP
//   9: altitude
//
// RMC layout:
//   1: HHMMSS.sss
//   2: status A/V
//   3..6: lat/N/S/lon/E/W
//   7: speed in knots
//   8: track in degrees
//   9: DDMMYY
//
// GSV layout:
//   1: total messages in group
//   2: message number
//   3: satellites in view (for this constellation)
//   4..: per-satellite tuples
// ---------------------------------------------------------------------------

void GpsService::ParseGGA(const char* fields[], int n) {
    if (n < 10) {
        return;
    }
    const char* utc        = fields[1];
    const char* lat_raw    = fields[2];
    const char* ns         = fields[3];
    const char* lon_raw    = fields[4];
    const char* ew         = fields[5];
    const char* fix_q_raw  = fields[6];
    const char* sats_raw   = fields[7];
    const char* hdop_raw   = fields[8];
    const char* alt_raw    = fields[9];

    const int fix_quality = (fix_q_raw && *fix_q_raw) ? std::atoi(fix_q_raw) : 0;
    const double hdop     = (hdop_raw && *hdop_raw) ? std::atof(hdop_raw) : 0.0;
    const double altitude = (alt_raw  && *alt_raw)  ? std::atof(alt_raw)  : 0.0;
    const int sats_used   = (sats_raw && *sats_raw) ? std::atoi(sats_raw) : 0;

    const double lat = ParseCoord(lat_raw, (ns && *ns) ? *ns : '\0', 2);
    const double lon = ParseCoord(lon_raw, (ew && *ew) ? *ew : '\0', 3);

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }

    data_.fix_quality     = static_cast<uint8_t>(fix_quality);
    data_.satellites_used = static_cast<uint8_t>(sats_used);
    data_.hdop            = hdop;

    if (fix_quality > 0) {
        data_.fix_valid     = true;
        data_.latitude_deg  = lat;
        data_.longitude_deg = lon;
        data_.altitude_m    = altitude;
        data_.last_fix_ms   = now_ms();
    }

    // HH:MM:SS from "HHMMSS.sss".
    if (utc != nullptr && std::strlen(utc) >= 6) {
        std::snprintf(data_.utc_time, sizeof(data_.utc_time),
                      "%c%c:%c%c:%c%c",
                      utc[0], utc[1], utc[2], utc[3], utc[4], utc[5]);
    }

    xSemaphoreGive(mutex_);
}

void GpsService::ParseRMC(const char* fields[], int n) {
    if (n < 10) {
        return;
    }
    const char* utc      = fields[1];
    const char* status   = fields[2];
    const char* lat_raw  = fields[3];
    const char* ns       = fields[4];
    const char* lon_raw  = fields[5];
    const char* ew       = fields[6];
    const char* spd_raw  = fields[7];
    const char* date_raw = fields[9];

    const bool   active = (status && status[0] == 'A');
    const double knots  = (spd_raw && *spd_raw) ? std::atof(spd_raw) : 0.0;
    const double lat = ParseCoord(lat_raw, (ns && *ns) ? *ns : '\0', 2);
    const double lon = ParseCoord(lon_raw, (ew && *ew) ? *ew : '\0', 3);

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }

    // Knots -> km/h.  1 knot = 1.852 km/h.
    data_.speed_kmh = knots * 1.852;

    if (active) {
        data_.fix_valid     = true;
        data_.latitude_deg  = lat;
        data_.longitude_deg = lon;
        data_.last_fix_ms   = now_ms();
    }

    if (utc != nullptr && std::strlen(utc) >= 6) {
        std::snprintf(data_.utc_time, sizeof(data_.utc_time),
                      "%c%c:%c%c:%c%c",
                      utc[0], utc[1], utc[2], utc[3], utc[4], utc[5]);
    }

    // DDMMYY -> "20YY-MM-DD".  RMC dates are always 21st century for the
    // foreseeable future, so the hard-coded "20" prefix is safe.
    if (date_raw != nullptr && std::strlen(date_raw) >= 6) {
        std::snprintf(data_.utc_date, sizeof(data_.utc_date),
                      "20%c%c-%c%c-%c%c",
                      date_raw[4], date_raw[5],
                      date_raw[2], date_raw[3],
                      date_raw[0], date_raw[1]);
    }

    xSemaphoreGive(mutex_);
}

void GpsService::ParseGSV(const char* talker, const char* fields[], int n) {
    if (n < 4) {
        return;
    }
    const char* sats_view_raw = fields[3];
    if (sats_view_raw == nullptr || *sats_view_raw == '\0') {
        return;
    }
    const int sats_view = std::atoi(sats_view_raw);
    if (sats_view < 0 || sats_view > 64) {
        return;
    }

    const int slot = TalkerIndex(talker);
    if (slot < 0) {
        return;
    }
    sats_view_per_talker_[slot] = static_cast<uint8_t>(sats_view);

    uint16_t total = 0;
    for (int i = 0; i < kTalkerCount; ++i) {
        total += sats_view_per_talker_[i];
    }
    if (total > 255) {
        total = 255;
    }

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }
    data_.satellites_view = static_cast<uint8_t>(total);
    xSemaphoreGive(mutex_);
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

bool GpsService::ChecksumOk(const char* sentence, size_t len) {
    // NMEA sentence is "$<payload>*<HH>".  Validate that '*' exists and is
    // followed by exactly two hex digits, then XOR all chars between '$' and
    // '*' and compare.
    const char* star = nullptr;
    for (size_t i = 0; i < len; ++i) {
        if (sentence[i] == '*') {
            star = &sentence[i];
            break;
        }
    }
    if (star == nullptr || (star - sentence) < 1) {
        return false;
    }
    const size_t hex_off = static_cast<size_t>(star - sentence) + 1;
    if (hex_off + 2 > len) {
        return false;
    }

    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return -1;
    };
    const int hi = hexval(sentence[hex_off]);
    const int lo = hexval(sentence[hex_off + 1]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    const uint8_t expected = static_cast<uint8_t>((hi << 4) | lo);

    uint8_t computed = 0;
    for (size_t i = 1; i < static_cast<size_t>(star - sentence); ++i) {
        computed ^= static_cast<uint8_t>(sentence[i]);
    }
    return computed == expected;
}

double GpsService::ParseCoord(const char* raw, char hemi, int deg_digits) {
    if (raw == nullptr || *raw == '\0') {
        return 0.0;
    }
    const size_t raw_len = std::strlen(raw);
    if (static_cast<int>(raw_len) <= deg_digits) {
        return 0.0;
    }

    char deg_buf[4] = {};
    if (deg_digits >= static_cast<int>(sizeof(deg_buf))) {
        return 0.0;
    }
    std::memcpy(deg_buf, raw, deg_digits);
    deg_buf[deg_digits] = '\0';

    const int    degrees = std::atoi(deg_buf);
    const double minutes = std::atof(raw + deg_digits);
    double value = static_cast<double>(degrees) + (minutes / 60.0);

    if (hemi == 'S' || hemi == 'W') {
        value = -value;
    }
    if (std::isnan(value) || std::isinf(value)) {
        return 0.0;
    }
    return value;
}

int GpsService::TalkerIndex(const char* talker) {
    if (talker == nullptr) {
        return -1;
    }
    if (std::strcmp(talker, "GP") == 0) return kTalkerGP;
    if (std::strcmp(talker, "GL") == 0) return kTalkerGL;
    if (std::strcmp(talker, "GA") == 0) return kTalkerGA;
    if (std::strcmp(talker, "BD") == 0) return kTalkerBD;
    if (std::strcmp(talker, "GN") == 0) return kTalkerGN;
    return -1;
}
