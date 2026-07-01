#ifndef IO_EXPANDER_HPP
#define IO_EXPANDER_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <vector>

#include <driver/i2c_master.h>
#include <esp_io_expander.h>
#include <esp_io_expander_tca95xx_16bit.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
// IOExpander
//
// Thin singleton wrapper around a TCA9555 16-bit I/O expander.  Each board
// signal is declared as a strongly-typed `Pin` enum and bound to one of the
// chip's 16 lines plus a `Direction` (input / output) via the pin map.
//
// Typical use (xingzhi-395 board -- default pin map is built in):
//
//     auto& io = IOExpander::getInstance();
//     io.begin(i2c_bus_);
//
//     // outputs: drive the line
//     io.setLevel(IOExpander::Pin::PA, true);
//
//     // inputs: read the line
//     if (io.readLevel(IOExpander::Pin::PWR_KEY)) { ... }
//
//     // click: fire on a short press-and-release
//     io.onClick(IOExpander::Pin::PWR_KEY, []() {
//         ESP_LOGI("...", "PWR_KEY 单击");
//     });
//
//     // long-press: fire a callback when an input is held for >= 2 s
//     io.onLongPress(IOExpander::Pin::PWR_KEY, 2000, []() {
//         ESP_LOGW("...", "PWR_KEY 长按 2 秒");
//     });
//
// A different board can override the map before begin() via setPinMap().
// Each entry in the map can override `direction`; it defaults to
// `Direction::kOutput`, so existing `{pin, io_index}` initializers keep
// compiling without change -- only signals that are physically driven
// inwards (interrupts, button states, ...) need to opt into kInput.
// ---------------------------------------------------------------------------
class IOExpander {
public:
    // ----------------------------------------------------------------------
    // Logical pins.
    //
    // Underlying values are arbitrary -- they are only used as indices into
    // an internal `pin -> slot` table, NOT as the TCA9555 P0..P15 numbers.
    // The hardware mapping (which logical pin lives on which TCA9555 line,
    // and whether it's an input or output) is built into kDefaultPinMap;
    // other boards may call setPinMap() before begin() to override.
    // ----------------------------------------------------------------------
    enum class Pin : uint8_t {
        GPS_POWER = 0,    // GPS module power
        PA_SWITCH,        // PA source select (0=4G, 1=WIFI)
        CAM_PWDN,         // camera power-down
        SD,               // SD card power / detect (active-low)
        PWR_KEY_PULSE,    // power-key pulse output (we drive a press)
        PWR_KEY,          // power-key state input (user button)
        BT_POWER,         // Bluetooth chip power
        RST_4G,           // 4G module reset
        PA,               // audio PA enable
        ACCEL_INT,        // accelerometer interrupt (active-low input)
        USB_INSERT_DET,   // USB 插入检测（P1-2，输入；电平含义见原理图）
        WIRELESS_CHARGE_DET,  // 无线充电检测（P1-3，输入；电平含义见原理图）
        kPinCount,        // sentinel; keep last
    };

    enum class Direction : uint8_t {
        kOutput = 0,  // we drive the line (default for almost all rails)
        kInput  = 1,  // external signal -- only readLevel() is meaningful
    };

    // Pin map entry.  `direction` defaults to kOutput so existing
    // `{Pin::X, io_index}` initializers continue to compile and behave
    // exactly as before (mapped as output).
    struct PinMapEntry {
        Pin       pin;
        uint8_t   io_index;
        Direction direction = Direction::kOutput;
    };

    // xingzhi-395 TCA9555 wiring.
    //
    // 这里 kDefaultPinMap 被声明成 `static constexpr` 数组放在类体内，
    // 同一个类还在解析中 ── gcc 不允许这种位置使用 PinMapEntry 的默认
    // 成员初始化器（会报 "default member initializer required before the
    // end of its enclosing class"）。所以这张表里每一项都把三个字段写
    // 全，保持显式而且没有歧义。下游用户通过 setPinMap() 自定义时仍
    // 然可以省略 `direction`，那种调用站点 PinMapEntry 已经完整了。
    static constexpr PinMapEntry kDefaultPinMap[] = {
        {Pin::GPS_POWER,           0,  Direction::kOutput},  // GPS 电源
        {Pin::PA_SWITCH,           1,  Direction::kOutput},  // 音频功放切换 0=4G 1=WIFI
        {Pin::CAM_PWDN,            2,  Direction::kOutput},  // 摄像头 低电平通电
        {Pin::SD,                  3,  Direction::kOutput},  // SD 卡 低电平通电
        {Pin::PWR_KEY_PULSE,       4,  Direction::kOutput},  // 开关机脉冲
        {Pin::PWR_KEY,             5,  Direction::kInput },  // 开机按键状态（外部输入）
        {Pin::BT_POWER,            6,  Direction::kOutput},  // 蓝牙芯片电源
        {Pin::RST_4G,              7,  Direction::kOutput},  // 4G RST
        {Pin::PA,                  8,  Direction::kOutput},  // 音频功放
        {Pin::ACCEL_INT,           9,  Direction::kInput },  // 加速度传感器中断（外部输入）
        {Pin::USB_INSERT_DET,      10, Direction::kInput },  // USB 插入检测 P1-2（外部输入）
        {Pin::WIRELESS_CHARGE_DET, 11, Direction::kInput },  // 无线充电检测 P1-3（外部输入）
    };

    static IOExpander& getInstance() {
        static IOExpander instance;
        return instance;
    }

    IOExpander(const IOExpander&)            = delete;
    IOExpander& operator=(const IOExpander&) = delete;

    // Stable, human-readable name for `pin`.  Unknown values produce "?".
    static const char* PinName(Pin pin) {
        switch (pin) {
            case Pin::GPS_POWER:           return "GPS_POWER";
            case Pin::PA_SWITCH:           return "PA_SWITCH";
            case Pin::CAM_PWDN:            return "CAM_PWDN";
            case Pin::SD:                  return "SD";
            case Pin::PWR_KEY_PULSE:       return "PWR_KEY_PULSE";
            case Pin::PWR_KEY:             return "PWR_KEY";
            case Pin::BT_POWER:            return "BT_POWER";
            case Pin::RST_4G:              return "4G_RST";
            case Pin::PA:                  return "PA";
            case Pin::ACCEL_INT:           return "ACCEL_INT";
            case Pin::USB_INSERT_DET:      return "USB_INSERT_DET";
            case Pin::WIRELESS_CHARGE_DET: return "WIRELESS_CHARGE_DET";
            default:                       return "?";
        }
    }

    static const char* DirectionName(Direction d) {
        return d == Direction::kInput ? "IN" : "OUT";
    }

    esp_err_t setPinMap(std::initializer_list<PinMapEntry> map) {
        if (map.size() == 0) {
            ESP_LOGE(TAG, "Invalid pin map");
            return ESP_ERR_INVALID_ARG;
        }
        clearPinMap();
        for (const auto& entry : map) {
            assignPin(entry);
        }
        return ESP_OK;
    }

    esp_err_t setPinMap(const std::vector<PinMapEntry>& map) {
        if (map.empty()) {
            ESP_LOGE(TAG, "Invalid pin map");
            return ESP_ERR_INVALID_ARG;
        }
        clearPinMap();
        for (const auto& entry : map) {
            assignPin(entry);
        }
        return ESP_OK;
    }

    esp_err_t begin(i2c_master_bus_handle_t i2c_bus,
                    uint32_t dev_addr = ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000) {
        if (!hasAnyPin()) {
            applyDefaultPinMap();
        }
        return beginImpl(i2c_bus, dev_addr);
    }

    esp_err_t begin(i2c_master_bus_handle_t i2c_bus,
                    std::initializer_list<PinMapEntry> map,
                    uint32_t dev_addr = ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000) {
        esp_err_t ret = setPinMap(map);
        if (ret != ESP_OK) {
            return ret;
        }
        return begin(i2c_bus, dev_addr);
    }

    esp_err_t begin(i2c_master_bus_handle_t i2c_bus,
                    const std::vector<PinMapEntry>& map,
                    uint32_t dev_addr = ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000) {
        esp_err_t ret = setPinMap(map);
        if (ret != ESP_OK) {
            return ret;
        }
        return begin(i2c_bus, dev_addr);
    }

    // ----------------------------------------------------------------------
    // Output API: setLevel
    //
    // setLevel on an INPUT pin is a programmer error -- we don't drive
    // the line, so writing to it has no electrical effect.  Reject loudly
    // (warn + ESP_ERR_INVALID_STATE) so the bug shows up in logs instead
    // of becoming a silent no-op that obscures pin-map mistakes.
    // ----------------------------------------------------------------------
    esp_err_t setLevel(Pin pin, uint8_t level) {
        if (!initialized_ || handle_ == nullptr) {
            ESP_LOGE(TAG, "Not initialized");
            return ESP_ERR_INVALID_STATE;
        }
        const PinSlot* slot = lookupSlot(pin);
        if (slot == nullptr) {
            logUnknownPin(pin);
            return ESP_ERR_NOT_FOUND;
        }
        if (slot->direction != Direction::kOutput) {
            ESP_LOGW(TAG,
                     "setLevel(%s) ignored: pin is configured as input",
                     PinName(pin));
            return ESP_ERR_INVALID_STATE;
        }
        const uint32_t mask = 1U << static_cast<uint32_t>(slot->io_index);
        return esp_io_expander_set_level(handle_, mask, level ? 1 : 0);
    }

    esp_err_t setLevel(Pin pin, bool high) {
        return setLevel(pin, static_cast<uint8_t>(high ? 1 : 0));
    }

    // ----------------------------------------------------------------------
    // Read API: getLevel / readLevel
    //
    // Both work on input AND output pins.  On outputs they read back the
    // value latched in the TCA9555 -- handy for sanity checks ("did the
    // last setLevel actually take effect?").
    //
    //   getLevel(Pin, uint8_t*) -- esp-idf style, returns esp_err_t
    //   readLevel(Pin, bool*)   -- same, but bool is friendlier than 0/1
    //   readLevel(Pin)          -- convenience for `if (io.readLevel(KEY))`
    //                              callers who don't care about the I2C
    //                              error path; logs and returns false on
    //                              failure.
    // ----------------------------------------------------------------------
    esp_err_t getLevel(Pin pin, uint8_t* level) const {
        if (!initialized_ || handle_ == nullptr) {
            ESP_LOGE(TAG, "Not initialized");
            return ESP_ERR_INVALID_STATE;
        }
        if (level == nullptr) {
            ESP_LOGE(TAG, "Level output pointer is null");
            return ESP_ERR_INVALID_ARG;
        }
        const PinSlot* slot = lookupSlot(pin);
        if (slot == nullptr) {
            logUnknownPin(pin);
            return ESP_ERR_NOT_FOUND;
        }
        const uint32_t mask = 1U << static_cast<uint32_t>(slot->io_index);
        uint32_t value = 0;
        const esp_err_t ret =
            esp_io_expander_get_level(handle_, mask, &value);
        if (ret == ESP_OK) {
            *level = (value & mask) ? 1 : 0;
        }
        return ret;
    }

    esp_err_t readLevel(Pin pin, bool* high) const {
        if (high == nullptr) {
            ESP_LOGE(TAG, "Level output pointer is null");
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t v = 0;
        const esp_err_t ret = getLevel(pin, &v);
        if (ret == ESP_OK) {
            *high = (v != 0);
        }
        return ret;
    }

    // 便利版本：失败 / 未映射时返回 false 并打日志。需要区分「读到低电平」
    // 与「读取出错」的调用方应当用上面带 bool* 的重载。
    bool readLevel(Pin pin) const {
        bool high = false;
        return readLevel(pin, &high) == ESP_OK && high;
    }

    // ----------------------------------------------------------------------
    // Long-press detection
    //
    // Register a callback to fire when an input pin reads `pressed_level`
    // continuously for at least `duration_ms`.  Designed for buttons:
    //
    //     io.onLongPress(Pin::PWR_KEY, 2000, []() {
    //         ESP_LOGW(TAG, "PWR_KEY 长按 2 秒");
    //     });
    //
    // Semantics:
    //   - Each press fires the callback at most once -- once threshold is
    //     crossed, the handler is "armed-down" until the pin returns to the
    //     released state (rising edge), at which point the next press starts
    //     fresh.
    //   - Multiple registrations on the same pin are allowed; for example
    //     register one at 2 s and another at 5 s and they fire independently
    //     during a single long hold.
    //   - The callback runs in the dedicated monitor task (priority 4,
    //     core 0).  Keep it short -- if you need heavy work, post to the
    //     UI queue / lv_async_call rather than blocking the polling loop.
    //
    // Parameters:
    //   pin            - must be present in the pin map AND configured as
    //                    Direction::kInput.
    //   duration_ms    - hold time required to trigger.  ~50 ms granularity
    //                    (the polling period); use multiples of 50 if you
    //                    care about precision.
    //   callback       - any callable; capture pin / duration / context as
    //                    needed.  Empty std::function is rejected.
    //   pressed_level  - the line level (0 or 1) the pin reads while the
    //                    button is being pressed.  Defaults to false
    //                    (active-low: button shorts to GND, internal/external
    //                    pull-up keeps the line high otherwise) which is the
    //                    standard wiring.  Pass `true` for active-high.
    //
    // Returns:
    //   ESP_OK                  - registered.  The polling task is lazily
    //                             created on the first successful call.
    //   ESP_ERR_INVALID_ARG     - callback is empty.
    //   ESP_ERR_NOT_FOUND       - pin is not in the pin map.
    //   ESP_ERR_INVALID_STATE   - pin is mapped but as output, not input.
    // ----------------------------------------------------------------------
    // Click detection
    //
    // Register a callback to fire when an input pin completes a short
    // press-and-release cycle.  Designed for buttons:
    //
    //     io.onClick(Pin::PWR_KEY, []() {
    //         ESP_LOGI(TAG, "PWR_KEY 单击");
    //     });
    //
    // Semantics:
    //   - Fires on the rising edge (release) if the preceding hold was at
    //     most `max_duration_ms`.  Holds longer than that are treated as
    //     long-press candidates and do not generate a click.
    //   - Multiple registrations on the same pin are allowed; each row
    //     tracks its own edge state independently.
    //   - The callback runs in the dedicated monitor task (same as
    //     onLongPress).  Keep it short -- post to the UI queue if needed.
    //
    // Parameters:
    //   pin              - must be mapped as Direction::kInput.
    //   callback         - any callable; empty std::function is rejected.
    //   pressed_level    - line level while pressed (default false =
    //                      active-low).
    //   max_duration_ms  - maximum hold time that still counts as a click.
    //                      Defaults to 500 ms so it does not collide with
    //                      typical long-press thresholds (>= 1.5 s).
    //
    // Returns: same error codes as onLongPress.
    // ----------------------------------------------------------------------
    using ClickCallback     = std::function<void()>;
    using LongPressCallback = std::function<void()>;

    esp_err_t onClick(Pin pin,
                      ClickCallback callback,
                      bool pressed_level = false,
                      uint32_t max_duration_ms = 500) {
        if (!callback) {
            ESP_LOGE(TAG, "onClick: callback is empty");
            return ESP_ERR_INVALID_ARG;
        }
        const PinSlot* slot = lookupSlot(pin);
        if (slot == nullptr) {
            ESP_LOGE(TAG, "onClick: pin '%s' is not in pin map", PinName(pin));
            return ESP_ERR_NOT_FOUND;
        }
        if (slot->direction != Direction::kInput) {
            ESP_LOGE(TAG,
                     "onClick: pin '%s' is not configured as input "
                     "(reading an output reads back the latched value, "
                     "not a real button state)",
                     PinName(pin));
            return ESP_ERR_INVALID_STATE;
        }

        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            click_handlers_.push_back(ClickHandler{
                pin,
                pressed_level,
                max_duration_ms,
                std::move(callback),
                /*was_pressed*/false,
                /*press_start_us*/0,
            });
            ensureMonitorTaskStartedLocked();
        }

        ESP_LOGI(TAG,
                 "onClick: armed pin '%s' max=%u ms (active=%s)",
                 PinName(pin), (unsigned)max_duration_ms,
                 pressed_level ? "high" : "low");
        return ESP_OK;
    }

    // Remove all click handlers registered for `pin`.  Use when a screen
    // unloads and no longer wants short-press handling on that input.
    esp_err_t offClick(Pin pin) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        const auto before = click_handlers_.size();
        click_handlers_.erase(
            std::remove_if(click_handlers_.begin(), click_handlers_.end(),
                           [pin](const ClickHandler& h) { return h.pin == pin; }),
            click_handlers_.end());
        if (click_handlers_.size() < before) {
            ESP_LOGI(TAG, "offClick: removed handlers for pin '%s'",
                     PinName(pin));
        }
        return ESP_OK;
    }

    esp_err_t onLongPress(Pin pin,
                          uint32_t duration_ms,
                          LongPressCallback callback,
                          bool pressed_level = false) {
        if (!callback) {
            ESP_LOGE(TAG, "onLongPress: callback is empty");
            return ESP_ERR_INVALID_ARG;
        }
        const PinSlot* slot = lookupSlot(pin);
        if (slot == nullptr) {
            ESP_LOGE(TAG, "onLongPress: pin '%s' is not in pin map",
                     PinName(pin));
            return ESP_ERR_NOT_FOUND;
        }
        if (slot->direction != Direction::kInput) {
            ESP_LOGE(TAG,
                     "onLongPress: pin '%s' is not configured as input "
                     "(reading an output reads back the latched value, "
                     "not a real button state)",
                     PinName(pin));
            return ESP_ERR_INVALID_STATE;
        }

        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            handlers_.push_back(LongPressHandler{
                pin,
                pressed_level,
                duration_ms,
                std::move(callback),
                /*was_pressed*/false,
                /*press_start_us*/0,
                /*fired*/false,
            });
            ensureMonitorTaskStartedLocked();
        }

        ESP_LOGI(TAG,
                 "onLongPress: armed pin '%s' threshold=%u ms (active=%s)",
                 PinName(pin), (unsigned)duration_ms,
                 pressed_level ? "high" : "low");
        return ESP_OK;
    }

    // ----------------------------------------------------------------------
    // Direction / mapping introspection.
    // ----------------------------------------------------------------------
    bool isMapped(Pin pin) const { return lookupSlot(pin) != nullptr; }

    Direction getDirection(Pin pin) const {
        const PinSlot* slot = lookupSlot(pin);
        return slot ? slot->direction : Direction::kOutput;
    }

    bool isInput(Pin pin) const {
        return getDirection(pin) == Direction::kInput;
    }

    bool isInitialized() const { return initialized_; }

    esp_io_expander_handle_t handle() const { return handle_; }

private:
    static constexpr const char* TAG       = "IOExpander";
    static constexpr uint8_t     kUnmapped = 0xFF;
    static constexpr size_t      kPinCountValue =
        static_cast<size_t>(Pin::kPinCount);

    // Per-pin row in the lookup table.  io_index == kUnmapped means the
    // logical Pin has not been bound to any TCA9555 line.
    struct PinSlot {
        uint8_t   io_index  = kUnmapped;
        Direction direction = Direction::kOutput;
    };

    IOExpander() { clearPinMap(); }

    void applyDefaultPinMap() {
        clearPinMap();
        for (const auto& entry : kDefaultPinMap) {
            assignPin(entry);
        }
    }

    void clearPinMap() {
        for (auto& slot : pin_to_slot_) {
            slot = PinSlot{};
        }
    }

    void assignPin(const PinMapEntry& entry) {
        const size_t idx = static_cast<size_t>(entry.pin);
        if (idx >= kPinCountValue) {
            ESP_LOGE(TAG, "Pin enum value %u out of range", (unsigned)idx);
            return;
        }
        if (entry.io_index >= 16) {
            ESP_LOGE(TAG, "IO index %u out of range for pin %s",
                     entry.io_index, PinName(entry.pin));
            return;
        }
        pin_to_slot_[idx] = PinSlot{entry.io_index, entry.direction};
    }

    bool hasAnyPin() const {
        for (const auto& slot : pin_to_slot_) {
            if (slot.io_index != kUnmapped) return true;
        }
        return false;
    }

    const PinSlot* lookupSlot(Pin pin) const {
        const size_t idx = static_cast<size_t>(pin);
        if (idx >= kPinCountValue) return nullptr;
        const PinSlot& slot = pin_to_slot_[idx];
        return slot.io_index == kUnmapped ? nullptr : &slot;
    }

    void logUnknownPin(Pin pin) const {
        ESP_LOGE(TAG, "Pin '%s' not present in IO expander map",
                 PinName(pin));
        if (!hasAnyPin()) {
            ESP_LOGE(TAG, "Pin map is empty, call setPinMap() first");
            return;
        }
        ESP_LOGW(TAG, "Available pins:");
        for (size_t i = 0; i < kPinCountValue; ++i) {
            const PinSlot& slot = pin_to_slot_[i];
            if (slot.io_index != kUnmapped) {
                ESP_LOGW(TAG, "  %s -> IO%u (%s)",
                         PinName(static_cast<Pin>(i)), slot.io_index,
                         DirectionName(slot.direction));
            }
        }
    }

    // Walk the pin map and split mapped lines into output / input
    // bitmasks.  begin() programs the TCA9555 direction register from
    // these so input pins are tri-stated and outputs are driven from
    // the moment the chip comes up.
    void buildDirectionMasks(uint32_t* output_mask,
                             uint32_t* input_mask) const {
        uint32_t out = 0;
        uint32_t in  = 0;
        for (size_t i = 0; i < kPinCountValue; ++i) {
            const PinSlot& slot = pin_to_slot_[i];
            if (slot.io_index == kUnmapped) continue;
            const uint32_t bit = 1U << slot.io_index;
            if (slot.direction == Direction::kInput) {
                in |= bit;
            } else {
                out |= bit;
            }
        }
        *output_mask = out;
        *input_mask  = in;
    }

    esp_err_t beginImpl(i2c_master_bus_handle_t i2c_bus, uint32_t dev_addr) {
        if (i2c_bus == nullptr) {
            ESP_LOGE(TAG, "I2C bus handle is null");
            return ESP_ERR_INVALID_ARG;
        }
        if (handle_ != nullptr) {
            ESP_LOGW(TAG, "TCA9555 already initialized");
            return ESP_OK;
        }

        esp_err_t ret =
            esp_io_expander_new_i2c_tca95xx_16bit(i2c_bus, dev_addr, &handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TCA9555 init failed: %s", esp_err_to_name(ret));
            handle_ = nullptr;
            return ret;
        }

        uint32_t output_mask = 0;
        uint32_t input_mask  = 0;
        buildDirectionMasks(&output_mask, &input_mask);

        // Outputs first: program direction, then drive low so peripherals
        // stay off until the board explicitly powers them.
        if (output_mask != 0) {
            ret = esp_io_expander_set_dir(handle_, output_mask,
                                          IO_EXPANDER_OUTPUT);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Set output dir failed: %s",
                         esp_err_to_name(ret));
                return ret;
            }
            ret = esp_io_expander_set_level(handle_, output_mask, 0);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Set default level failed: %s",
                         esp_err_to_name(ret));
                return ret;
            }
        }

        // Inputs: only configure direction; we never drive these.
        if (input_mask != 0) {
            ret = esp_io_expander_set_dir(handle_, input_mask,
                                          IO_EXPANDER_INPUT);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Set input dir failed: %s",
                         esp_err_to_name(ret));
                return ret;
            }
        }

        initialized_ = true;
        ESP_LOGI(TAG,
                 "TCA9555 initialized, addr=0x%02lx (out=0x%04lx in=0x%04lx)",
                 (unsigned long)dev_addr,
                 (unsigned long)output_mask,
                 (unsigned long)input_mask);
        return ESP_OK;
    }

    // ----------------------------------------------------------------------
    // Input-event machinery (click + long-press)
    //
    // Each registration is one row in click_handlers_ / handlers_; the
    // monitor task walks both vectors every kMonitorPollMs ms and runs an
    // independent edge-triggered state machine per row.  Callbacks are
    // deferred until after the mutex is released (so a callback can safely
    // call back into onClick / onLongPress / setLevel without deadlocking).
    // ----------------------------------------------------------------------
    static constexpr uint32_t kMonitorPollMs   = 50;     // 20 Hz polling
    static constexpr uint32_t kMonitorStackSz  = 3 * 1024;
    static constexpr UBaseType_t kMonitorPrio  = 4;
    static constexpr BaseType_t  kMonitorCore  = 0;

    struct ClickHandler {
        Pin            pin;
        bool           pressed_level;
        uint32_t       max_duration_ms;
        ClickCallback  callback;
        bool           was_pressed;
        int64_t        press_start_us;
    };

    struct LongPressHandler {
        Pin               pin;
        bool              pressed_level;
        uint32_t          duration_ms;
        LongPressCallback callback;
        // mutable per-handler state, owned by the monitor task
        bool              was_pressed;
        int64_t           press_start_us;
        bool              fired;
    };

    // Caller must hold handlers_mutex_.
    void ensureMonitorTaskStartedLocked() {
        if (monitor_task_handle_ != nullptr) return;
        BaseType_t r = xTaskCreatePinnedToCore(
            &IOExpander::monitorTaskTrampoline, "io_expander_mon",
            kMonitorStackSz, this, kMonitorPrio,
            &monitor_task_handle_, kMonitorCore);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "Failed to create io_expander_mon task");
            monitor_task_handle_ = nullptr;
        }
    }

    static void monitorTaskTrampoline(void* arg) {
        static_cast<IOExpander*>(arg)->monitorTaskLoop();
    }

    void monitorTaskLoop() {
        // Local list reused across iterations to amortize allocation.
        // Callbacks captured here run AFTER we release the mutex, so a
        // callback can re-enter onClick / onLongPress / setLevel without
        // deadlocking.
        std::vector<std::function<void()>> pending;

        while (true) {
            pending.clear();
            const int64_t now_us = esp_timer_get_time();

            {
                std::lock_guard<std::mutex> lock(handlers_mutex_);
                for (auto& h : click_handlers_) {
                    bool level = false;
                    if (readLevel(h.pin, &level) != ESP_OK) {
                        continue;
                    }
                    const bool is_pressed = (level == h.pressed_level);

                    if (is_pressed && !h.was_pressed) {
                        h.press_start_us = now_us;
                    } else if (!is_pressed && h.was_pressed) {
                        const int64_t held_ms =
                            (now_us - h.press_start_us) / 1000;
                        if (held_ms <= static_cast<int64_t>(h.max_duration_ms)) {
                            pending.push_back(h.callback);
                        }
                    }
                    h.was_pressed = is_pressed;
                }
                for (auto& h : handlers_) {
                    bool level = false;
                    if (readLevel(h.pin, &level) != ESP_OK) {
                        // I2C transient or pin no longer mapped/input;
                        // skip this tick without disturbing state.
                        continue;
                    }
                    const bool is_pressed = (level == h.pressed_level);

                    if (is_pressed && !h.was_pressed) {
                        // Falling edge into pressed: arm timer.
                        h.press_start_us = now_us;
                        h.fired          = false;
                    } else if (is_pressed && !h.fired) {
                        const int64_t held_ms =
                            (now_us - h.press_start_us) / 1000;
                        if (held_ms >= static_cast<int64_t>(h.duration_ms)) {
                            pending.push_back(h.callback);
                            h.fired = true;
                        }
                    }
                    // On release (was_pressed=true -> is_pressed=false) we
                    // simply update was_pressed below; the next falling edge
                    // re-arms the timer naturally.
                    h.was_pressed = is_pressed;
                }
            }

            for (auto& cb : pending) {
                cb();
            }

            vTaskDelay(pdMS_TO_TICKS(kMonitorPollMs));
        }
    }

    PinSlot                       pin_to_slot_[kPinCountValue];
    esp_io_expander_handle_t      handle_              = nullptr;
    bool                          initialized_         = false;

    std::mutex                    handlers_mutex_;
    std::vector<ClickHandler>     click_handlers_;
    std::vector<LongPressHandler> handlers_;
    TaskHandle_t                  monitor_task_handle_ = nullptr;
};

#endif  // IO_EXPANDER_HPP
