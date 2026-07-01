#pragma once

// ---------------------------------------------------------------------------
// GpsService -- singleton that powers on the on-board GPS module via the
// TCA9555 IO expander, drives UART_NUM_1 at the pins / baud specified by
// the xingzhi-395 hardware, and parses incoming NMEA-0183 sentences.
//
// Pins / baud are hard-coded to the board spec:
//
//     GPS_RX_PIN     = GPIO 34   (ESP32 receives GPS module's TX)
//     GPS_TX_PIN     = GPIO 31   (ESP32 transmits to GPS module's RX)
//     GPS_UART_NUM   = UART_NUM_1
//     GPS_BAUD_RATE  = 9600
//     GPS_POWER     = TCA9555 P0  (driven HIGH to power the module on)
//
// The service starts once Start() is called (idempotent) -- typically the
// first time the user opens the GPS screen -- and runs an RX task that
// continually parses NMEA sentences and updates an internal Snapshot.  UI
// code calls GetSnapshot() once per refresh tick; the call is mutex-guarded
// and returns a stable copy, so the LVGL thread never sees a torn record.
// ---------------------------------------------------------------------------

#include <cstdint>

#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class GpsService {
public:
    // GPS UART configuration. Defined here so any other board code that
    // wants to inspect these values does not have to repeat the literal
    // numbers from the user spec.  Powering the module on / off is the
    // caller's job and is done directly via IOExpander::Pin::GPS_POWER
    // (see GpsScreen's lifecycle callback) -- this header deliberately
    // does NOT alias that pin to keep the dependency one-way.
    static constexpr int         kGpsRxPin    = 37;
    static constexpr int         kGpsTxPin    = 38;
    static constexpr uart_port_t kGpsUartNum  = UART_NUM_0;
    static constexpr int         kGpsBaudRate = 9600;

    // A point-in-time view of the most recent NMEA state.  All numeric
    // fields default to zero / false, so the UI can render a meaningful
    // "no fix yet" placeholder before any sentences have been seen.
    struct Snapshot {
        bool     started          = false;  // Start() succeeded
        bool     fix_valid        = false;  // GGA fix_quality > 0 was seen
        uint8_t  fix_quality      = 0;      // 0=none, 1=GPS, 2=DGPS, ...
        double   latitude_deg     = 0.0;    // +N, -S, decimal degrees
        double   longitude_deg    = 0.0;    // +E, -W, decimal degrees
        double   altitude_m       = 0.0;    // meters above MSL
        uint8_t  satellites_used  = 0;      // from $xxGGA field 7
        uint8_t  satellites_view  = 0;      // sum across constellations (GSV)
        double   hdop             = 0.0;    // horizontal dilution of precision
        double   speed_kmh        = 0.0;    // from $xxRMC, knots -> km/h
        char     utc_time[16]     = "";     // "HH:MM:SS"
        char     utc_date[16]     = "";     // "YYYY-MM-DD" if RMC seen
        uint32_t sentence_count   = 0;      // total NMEA lines parsed
        uint32_t bytes_received   = 0;      // diagnostic
        uint32_t last_sentence_ms = 0;      // FreeRTOS tick (ms)
        uint32_t last_fix_ms      = 0;      // FreeRTOS tick at last fix
    };

    static GpsService& Instance();

    // Installs UART_NUM_1 and spawns the NMEA RX task.  Does NOT touch
    // the GPS power rail -- the screen lifecycle owns that.  Safe to call
    // repeatedly; subsequent calls are no-ops.  Returns false if the
    // hardware setup failed.
    bool Start();

    // Atomic snapshot. Cheap; safe to call from the LVGL thread.
    Snapshot GetSnapshot() const;

    GpsService(const GpsService&)            = delete;
    GpsService& operator=(const GpsService&) = delete;

private:
    GpsService() = default;
    ~GpsService() = default;

    static void RxTaskTrampoline(void* arg);
    void        RxTaskLoop();

    // Sentence-level handlers.  `talker` is the 2-char prefix after `$`
    // (e.g. "GP", "GN", "GL").  `fields` already has the leading $-prefix
    // token stripped of "$" and split by ',' (with the trailing checksum
    // chopped off the last field).
    void HandleSentence(const char* sentence_body, size_t len);
    void ParseGGA(const char* fields[], int n);
    void ParseRMC(const char* fields[], int n);
    void ParseGSV(const char* talker, const char* fields[], int n);

    // Helpers.
    static bool   ChecksumOk(const char* sentence, size_t len);
    static double ParseCoord(const char* raw, char hemi, int deg_digits);
    static int    TalkerIndex(const char* talker);  // -1 if unknown

    // Mutex-protected state.
    mutable SemaphoreHandle_t mutex_   = nullptr;
    Snapshot                  data_    = {};
    bool                      started_ = false;
    TaskHandle_t              rx_task_ = nullptr;

    // Buffered tail of the last UART read, accumulated until we hit a
    // terminator (CR / LF).
    char   line_buf_[128] = {};
    size_t line_len_      = 0;

    // Latest "satellites in view" per constellation talker.  GSV repeats
    // the total-in-view count in every sentence of its group, so we just
    // store the latest value and sum across talker buckets when assembling
    // the snapshot.
    enum { kTalkerGP = 0, kTalkerGL, kTalkerGA, kTalkerBD, kTalkerGN, kTalkerCount };
    uint8_t sats_view_per_talker_[kTalkerCount] = {};
};
