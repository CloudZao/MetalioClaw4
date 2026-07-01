#include "level_screen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/i2c_master.h>

#include "home_screen/home_screen.h"
#include "i2c_device.h"
#include "screen_util.h"
#include "settings.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

extern "C" i2c_master_bus_handle_t xingzhi_395_get_i2c_bus();

namespace {

constexpr const char* TAG = "LevelScreen";

// ---------------------------------------------------------------------------
// SC7A20H accelerometer (I2C 0x19) -- LIS2DH12-compatible register map.
//
// 上电后默认是 power-down，必须显式写 CTRL_REG1 才会出数据。我们用 ±2g
// 高分辨率模式（HR=1，BDU=1），输出 12-bit 左对齐到 16-bit 寄存器，灵敏度
// 1 mg/LSB。读 6 字节连续输出寄存器时必须把寄存器地址的 MSB 置 1 触发
// 自增，否则只会反复读到 OUT_X_L。
// ---------------------------------------------------------------------------
constexpr uint8_t kSc7a20hAddr  = 0x19;
constexpr uint8_t kRegWhoAmI    = 0x0F;
constexpr uint8_t kRegCtrlReg1  = 0x20;
constexpr uint8_t kRegCtrlReg4  = 0x23;
constexpr uint8_t kRegOutXL     = 0x28;
constexpr uint8_t kAutoIncMask  = 0x80;

// CTRL_REG1 = 0x57 -> ODR=100Hz, LPen=0 (HR-capable), Z/Y/X enable.
constexpr uint8_t kCtrlReg1Val  = 0x57;
// CTRL_REG4 = 0x88 -> BDU=1, BLE=0, FS=00 (±2g), HR=1, ST=00, SIM=0.
constexpr uint8_t kCtrlReg4Val  = 0x88;
// 在 ±2g + HR(12-bit) 模式下，1 LSB ≈ 1 mg。
constexpr float   kMgPerLsb     = 1.0f;
constexpr int     kGravityMg    = 1000;

// SC7A20H 跟 ST 家系列共用 0x33 / 0x32 / 0x11 等 WHO_AM_I；不强校验，只要能
// 读得到东西就当成在线。把已知值列出来仅用于日志，便于排查。
constexpr uint8_t kWhoAmIExpected[] = {0x11, 0x33, 0x32, 0x44};

// ---------------------------------------------------------------------------
// 几何 / 视觉常量（720x720 面板）
// ---------------------------------------------------------------------------
constexpr int kPanelW          = 720;
constexpr int kPanelH          = 720;
constexpr int kHeaderH         = 90;
constexpr int kFooterTopY      = 540;
constexpr int kFooterH         = kPanelH - kFooterTopY;

constexpr int kCenterX         = kPanelW / 2;     // 360
constexpr int kCenterY         = (kHeaderH + kFooterTopY) / 2;  // 315
constexpr int kOuterRadius     = 210;             // 外参考圈
constexpr int kMidRadius       = 110;             // 第二圈刻度
constexpr int kTargetRadius    = 30;              // 中心目标小圈
constexpr int kBubbleRadius    = 26;              // 气泡半径
constexpr int kBubbleMaxOffset = kOuterRadius - kBubbleRadius - 6;

// 判定 “水平” 的阈值，单位：度。两轴 |角度| 都小于这个值则视为水平。
constexpr float kLevelTolDeg   = 0.5f;

// 采样间隔：50ms = 20Hz UI 更新，肉眼足够顺滑且不会压垮 LVGL 主线程。
constexpr uint32_t kSamplePeriodMs = 50;
// 气泡跟手平滑系数（一阶低通）。0.0..1.0；越小越平滑越拖滞。
constexpr float kBubbleSmoothing = 0.25f;

// ---------------------------------------------------------------------------
// SC7A20H driver
//
// I2cDevice 的成员函数是 protected，外部用不到；这里 wrap 一层公有接口。
// 只在第一次 Create() / LifecycleCallback(LOAD) 时懒构造一次，之后页面来回
// 切换都共享同一份实例。
// ---------------------------------------------------------------------------
class Sc7a20h : public I2cDevice {
public:
    Sc7a20h(i2c_master_bus_handle_t bus, uint8_t addr) : I2cDevice(bus, addr) {}

    bool Probe() {
        uint8_t who = ReadReg(kRegWhoAmI);
        last_who_am_i_ = who;
        bool match = false;
        for (uint8_t v : kWhoAmIExpected) {
            if (who == v) { match = true; break; }
        }
        if (!match) {
            ESP_LOGW(TAG, "WHO_AM_I=0x%02X 不在已知列表里，按 best-effort 继续", who);
        } else {
            ESP_LOGI(TAG, "SC7A20H online, WHO_AM_I=0x%02X", who);
        }
        return true;
    }

    void Configure() {
        WriteReg(kRegCtrlReg1, kCtrlReg1Val);
        WriteReg(kRegCtrlReg4, kCtrlReg4Val);
    }

    // 读一组 (X,Y,Z) 加速度，单位 mg。返回 false 表示总线读失败（罕见
    // 情况下 i2c_master_transmit_receive 会卡 ESP_ERROR_CHECK 直接重启，
    // 这里返回 bool 主要是给后续替换更宽容的 I2C wrapper 留口子）。
    bool ReadAccelMg(int* ax, int* ay, int* az) {
        uint8_t buf[6] = {0};
        ReadRegs(kRegOutXL | kAutoIncMask, buf, sizeof(buf));
        // 12-bit 左对齐到 16-bit：先按 int16_t 拼出来再算术右移 4。
        int16_t rx = static_cast<int16_t>((buf[1] << 8) | buf[0]);
        int16_t ry = static_cast<int16_t>((buf[3] << 8) | buf[2]);
        int16_t rz = static_cast<int16_t>((buf[5] << 8) | buf[4]);
        *ax = static_cast<int>((rx >> 4) * kMgPerLsb);
        *ay = static_cast<int>((ry >> 4) * kMgPerLsb);
        *az = static_cast<int>((rz >> 4) * kMgPerLsb);
        return true;
    }

    uint8_t LastWhoAmI() const { return last_who_am_i_; }

private:
    uint8_t last_who_am_i_ = 0;
};

// ---------------------------------------------------------------------------
// 模块全局状态：传感器实例 + 校准偏移
// ---------------------------------------------------------------------------
struct CalOffsets {
    int ox = 0;     // ax 轴在水平静止时的零点偏移 (mg)
    int oy = 0;
    int oz = 0;     // az 轴减去 1g(1000mg) 之后的偏移
};

Sc7a20h*   s_sensor      = nullptr;
bool       s_sensor_init = false;     // probe + configure 是否成功
CalOffsets s_cal;
bool       s_cal_loaded  = false;

void LoadCalOffsetsOnce() {
    if (s_cal_loaded) return;
    Settings settings("level", false);
    s_cal.ox = settings.GetInt("ox", 0);
    s_cal.oy = settings.GetInt("oy", 0);
    s_cal.oz = settings.GetInt("oz", 0);
    s_cal_loaded = true;
    ESP_LOGI(TAG, "load cal offsets: ox=%d oy=%d oz=%d", s_cal.ox, s_cal.oy, s_cal.oz);
}

void SaveCalOffsets(const CalOffsets& c) {
    Settings settings("level", true);
    settings.SetInt("ox", c.ox);
    settings.SetInt("oy", c.oy);
    settings.SetInt("oz", c.oz);
    s_cal = c;
    s_cal_loaded = true;
    ESP_LOGI(TAG, "save cal offsets: ox=%d oy=%d oz=%d", c.ox, c.oy, c.oz);
}

void ResetCalOffsets() {
    SaveCalOffsets({0, 0, 0});
}

// 懒初始化 SC7A20H：第一次进入屏幕或 lifecycle LOAD 时调用。失败也不抛，
// 只把 s_sensor_init 置 false，UI 上会显示 “未检测到传感器”。
void EnsureSensorInited() {
    if (s_sensor != nullptr && s_sensor_init) return;
    if (s_sensor == nullptr) {
        i2c_master_bus_handle_t bus = xingzhi_395_get_i2c_bus();
        if (bus == nullptr) {
            ESP_LOGE(TAG, "I2C bus 未就绪，跳过 SC7A20H 初始化");
            s_sensor_init = false;
            return;
        }
        // 先用 i2c_master_probe 确认地址有响应；否则 add_device 仍会成功
        // 但后面每次读都会 ESP_ERROR_CHECK 直接 reboot。
        if (i2c_master_probe(bus, kSc7a20hAddr, 100) != ESP_OK) {
            ESP_LOGW(TAG, "I2C probe 0x%02X 失败：未检测到 SC7A20H", kSc7a20hAddr);
            s_sensor_init = false;
            return;
        }
        s_sensor = new Sc7a20h(bus, kSc7a20hAddr);
    }
    if (s_sensor == nullptr) {
        s_sensor_init = false;
        return;
    }
    s_sensor->Probe();
    s_sensor->Configure();
    s_sensor_init = true;
}

// ---------------------------------------------------------------------------
// UI 状态
// ---------------------------------------------------------------------------
struct UiState {
    lv_obj_t* screen        = nullptr;
    lv_obj_t* outer_circle  = nullptr;
    lv_obj_t* mid_circle    = nullptr;
    lv_obj_t* target_ring   = nullptr;
    lv_obj_t* hline         = nullptr;
    lv_obj_t* vline         = nullptr;
    lv_obj_t* bubble        = nullptr;
    lv_obj_t* status_lbl    = nullptr;   // “水平 / 偏 X°” 大字状态
    lv_obj_t* xyz_lbl       = nullptr;   // ax/ay/az(mg)
    lv_obj_t* angle_lbl     = nullptr;   // pitch / roll
    lv_obj_t* sensor_lbl    = nullptr;   // 顶栏右侧 SC7A20H 状态
    lv_obj_t* cal_btn       = nullptr;
    lv_obj_t* reset_btn     = nullptr;

    // 校准对话框相关
    lv_obj_t* dialog_mask   = nullptr;
    lv_obj_t* dialog_card   = nullptr;
    lv_obj_t* dialog_title  = nullptr;
    lv_obj_t* dialog_text   = nullptr;
    lv_obj_t* dialog_ok_btn = nullptr;
    lv_obj_t* dialog_cancel = nullptr;

    lv_timer_t* sample_timer = nullptr;
    lv_timer_t* dialog_timer = nullptr;

    // 平滑过的气泡像素位置（浮点便于做低通）
    float bubble_x_px = 0.0f;
    float bubble_y_px = 0.0f;
};

UiState s_ui;

// 校准对话框的状态机：IDLE -> CONFIRM(等用户点确认) -> SAMPLING(取样中) -> DONE
enum class CalState : uint8_t {
    kIdle      = 0,
    kConfirm   = 1,   // 已弹出，等待用户点击“开始校准”
    kSampling  = 2,   // 正在取样累加
    kDone      = 3,   // 取样完成，显示 “校准完成” 一会儿后关闭
};

CalState s_cal_state = CalState::kIdle;
int      s_cal_count = 0;     // 累计采样次数
int64_t  s_cal_sum_x = 0;     // 累加和（整数 mg），32 位足够
int64_t  s_cal_sum_y = 0;
int64_t  s_cal_sum_z = 0;
constexpr int kCalSamples = 32;

// ---------------------------------------------------------------------------
// 工具：取标度后的传感器读数（带偏移修正）
//
// 返回一次 “世界坐标” 加速度（单位 mg）。如果传感器未在线，把 az 设为
// 1000、ax/ay 设为 0，让 UI 维持在“水平”态而不是乱跳。
//
// raw_* 输出原始未校准的 mg 值，校准流程 (OnSampleTick / kSampling) 需要
// 它来累加；UI 渲染只关心已校准的 ax/ay/az。
// ---------------------------------------------------------------------------
bool ReadCorrectedAccel(int* ax, int* ay, int* az,
                        int* raw_x = nullptr, int* raw_y = nullptr, int* raw_z = nullptr) {
    if (!s_sensor_init || s_sensor == nullptr) {
        *ax = 0;
        *ay = 0;
        *az = kGravityMg;
        if (raw_x != nullptr) *raw_x = 0;
        if (raw_y != nullptr) *raw_y = 0;
        if (raw_z != nullptr) *raw_z = kGravityMg;
        return false;
    }
    int rx = 0, ry = 0, rz = 0;
    s_sensor->ReadAccelMg(&rx, &ry, &rz);
    *ax = rx - s_cal.ox;
    *ay = ry - s_cal.oy;
    *az = rz - s_cal.oz;
    if (raw_x != nullptr) *raw_x = rx;
    if (raw_y != nullptr) *raw_y = ry;
    if (raw_z != nullptr) *raw_z = rz;
    return true;
}

// ---------------------------------------------------------------------------
// UI 同步：把一次 (ax, ay, az) 反映到气泡位置 + 文字
// ---------------------------------------------------------------------------
void UpdateUiFromAccel(int ax, int ay, int az) {
    // 防止 az 太小 / 接近 0（设备直立时）导致 atan2 输出失真。我们仍然
    // 计算一次，但气泡位置会基于 ax/ay 占 1g 的比例去截断。
    float ax_g = static_cast<float>(ax) / kGravityMg;
    float ay_g = static_cast<float>(ay) / kGravityMg;
    float az_g = static_cast<float>(az) / kGravityMg;

    // 真实姿态角（pitch=绕Y轴, roll=绕X轴）。az 接近 0 时 atan2 仍可用，
    // 但视觉上气泡早就贴边了，文字数值可以保留作为参考。
    float pitch_deg = std::atan2(ax_g, std::sqrt(ay_g * ay_g + az_g * az_g)) * 180.0f /
                      static_cast<float>(M_PI);
    float roll_deg  = std::atan2(ay_g, std::sqrt(ax_g * ax_g + az_g * az_g)) * 180.0f /
                      static_cast<float>(M_PI);

    // 气泡位置：用 ax/ay 占 1g 的比例直接映射到像素，截断在最大半径。
    // 选择 “气泡随着设备最高的一侧漂动” 的视觉，因此 x 取负、y 取正：
    //   - ax > 0 (设备右侧偏低) -> 气泡向左漂
    //   - ay > 0 (设备前/下侧偏低) -> 气泡向下漂
    // 这样把屏幕当成一个真实的水平盘看时，气泡会浮向地势更高的一边。
    float target_x = -ax_g * kBubbleMaxOffset;
    float target_y =  ay_g * kBubbleMaxOffset;

    // 圆形限位：保持气泡始终在外圈以内
    float r = std::sqrt(target_x * target_x + target_y * target_y);
    if (r > kBubbleMaxOffset) {
        float k = kBubbleMaxOffset / r;
        target_x *= k;
        target_y *= k;
    }

    // 一阶低通平滑，避免抖动
    s_ui.bubble_x_px += (target_x - s_ui.bubble_x_px) * kBubbleSmoothing;
    s_ui.bubble_y_px += (target_y - s_ui.bubble_y_px) * kBubbleSmoothing;

    if (s_ui.bubble != nullptr) {
        // 圆心在 (kCenterX, kCenterY)；LV_ALIGN_CENTER 是相对屏幕中心，
        // 所以这里加上圆心相对屏幕中心的偏移量。
        constexpr int kCircleOffsetX = kCenterX - kPanelW / 2;
        constexpr int kCircleOffsetY = kCenterY - kPanelH / 2;
        lv_obj_align(s_ui.bubble, LV_ALIGN_CENTER,
                     static_cast<int32_t>(s_ui.bubble_x_px) + kCircleOffsetX,
                     static_cast<int32_t>(s_ui.bubble_y_px) + kCircleOffsetY);

        bool level = (std::fabs(pitch_deg) < kLevelTolDeg) &&
                     (std::fabs(roll_deg)  < kLevelTolDeg) && s_sensor_init;
        uint32_t color = level ? 0x22C55E : 0x3DDCFF;
        lv_obj_set_style_bg_color(s_ui.bubble, lv_color_hex(color), LV_PART_MAIN);
    }

    if (s_ui.status_lbl != nullptr) {
        if (!s_sensor_init) {
            lv_label_set_text(s_ui.status_lbl, "未检测到传感器");
            lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(0xF87171), LV_PART_MAIN);
        } else if (std::fabs(pitch_deg) < kLevelTolDeg && std::fabs(roll_deg) < kLevelTolDeg) {
            lv_label_set_text(s_ui.status_lbl, "水平");
            lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(0x22C55E), LV_PART_MAIN);
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "未水平  %.1f°",
                          std::sqrt(pitch_deg * pitch_deg + roll_deg * roll_deg));
            lv_label_set_text(s_ui.status_lbl, buf);
            lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(0xFBBF24), LV_PART_MAIN);
        }
    }

    if (s_ui.xyz_lbl != nullptr) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "X: %5d  Y: %5d  Z: %5d  (mg)", ax, ay, az);
        lv_label_set_text(s_ui.xyz_lbl, buf);
    }

    if (s_ui.angle_lbl != nullptr) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "Pitch: %+5.1f°   Roll: %+5.1f°", pitch_deg, roll_deg);
        lv_label_set_text(s_ui.angle_lbl, buf);
    }
}

// ---------------------------------------------------------------------------
// 主采样 timer 回调：每 50ms 跑一次。
//
// 根据当前校准状态机：
//   - IDLE / CONFIRM / DONE：仅刷新 UI
//   - SAMPLING：累加 raw 值；够 kCalSamples 个就求平均、保存、切到 DONE
// ---------------------------------------------------------------------------
void OnDialogAutoDismiss(lv_timer_t* t);

void OnSampleTick(lv_timer_t* /*t*/) {
    int ax = 0, ay = 0, az = kGravityMg;
    int rx = 0, ry = 0, rz = kGravityMg;
    bool ok = ReadCorrectedAccel(&ax, &ay, &az, &rx, &ry, &rz);

    if (s_cal_state == CalState::kSampling && ok) {
        s_cal_sum_x += rx;
        s_cal_sum_y += ry;
        s_cal_sum_z += rz;
        s_cal_count++;
        if (s_ui.dialog_text != nullptr) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "校准中... %d / %d", s_cal_count, kCalSamples);
            lv_label_set_text(s_ui.dialog_text, buf);
        }
        if (s_cal_count >= kCalSamples) {
            CalOffsets c;
            c.ox = static_cast<int>(s_cal_sum_x / s_cal_count);
            c.oy = static_cast<int>(s_cal_sum_y / s_cal_count);
            // az 校准目标：水平时应该读 1g(1000mg)，所以偏移 = 平均 - 1000
            c.oz = static_cast<int>(s_cal_sum_z / s_cal_count) - kGravityMg;
            SaveCalOffsets(c);
            s_cal_state = CalState::kDone;
            if (s_ui.dialog_text != nullptr) {
                char buf[80];
                std::snprintf(buf, sizeof(buf), "校准完成\nox=%d  oy=%d  oz=%d", c.ox, c.oy, c.oz);
                lv_label_set_text(s_ui.dialog_text, buf);
            }
            if (s_ui.dialog_title != nullptr) {
                lv_label_set_text(s_ui.dialog_title, "完成");
            }
            // 1.2s 后自动关闭对话框
            if (s_ui.dialog_timer == nullptr) {
                s_ui.dialog_timer = lv_timer_create(OnDialogAutoDismiss, 1200, nullptr);
                lv_timer_set_repeat_count(s_ui.dialog_timer, 1);
            }
        }
    }

    UpdateUiFromAccel(ax, ay, az);
}

// ---------------------------------------------------------------------------
// 校准对话框：覆盖在屏幕上的半透明遮罩 + 居中卡片。
//
// 流程：
//   1. 用户点 “校准” 按钮 -> ShowCalibrationDialog()，状态 = kConfirm
//   2. 用户点 “开始校准” -> 切到 kSampling，sample timer 累加 raw 值
//   3. 累加 32 个之后求平均、保存到 NVS、切到 kDone，文本变 “校准完成”
//   4. 1.2s 后自动 dismiss；用户也可以随时点 “取消” / “关闭”
// ---------------------------------------------------------------------------
void DismissCalibrationDialog();

void OnDialogAutoDismiss(lv_timer_t* /*t*/) {
    // repeat_count=1 的 lv_timer 触发后 LVGL 会自动 delete，所以这里要把
    // 句柄置空，避免 DismissCalibrationDialog 再 lv_timer_delete 一个野指针。
    s_ui.dialog_timer = nullptr;
    DismissCalibrationDialog();
}

void DismissCalibrationDialog() {
    if (s_ui.dialog_timer != nullptr) {
        lv_timer_delete(s_ui.dialog_timer);
        s_ui.dialog_timer = nullptr;
    }
    if (s_ui.dialog_mask != nullptr) {
        lv_obj_delete(s_ui.dialog_mask);
        s_ui.dialog_mask   = nullptr;
        s_ui.dialog_card   = nullptr;
        s_ui.dialog_title  = nullptr;
        s_ui.dialog_text   = nullptr;
        s_ui.dialog_ok_btn = nullptr;
        s_ui.dialog_cancel = nullptr;
    }
    s_cal_state = CalState::kIdle;
    s_cal_count = 0;
    s_cal_sum_x = s_cal_sum_y = s_cal_sum_z = 0;
}

void OnDialogOkClicked(lv_event_t* /*e*/) {
    if (s_cal_state == CalState::kConfirm) {
        if (!s_sensor_init) {
            // 没有传感器时直接关闭对话框、提示一下
            if (s_ui.dialog_text != nullptr) {
                lv_label_set_text(s_ui.dialog_text, "未检测到传感器，无法校准");
            }
            s_ui.dialog_timer = lv_timer_create(OnDialogAutoDismiss, 1200, nullptr);
            lv_timer_set_repeat_count(s_ui.dialog_timer, 1);
            return;
        }
        s_cal_state = CalState::kSampling;
        s_cal_count = 0;
        s_cal_sum_x = s_cal_sum_y = s_cal_sum_z = 0;
        if (s_ui.dialog_title != nullptr) lv_label_set_text(s_ui.dialog_title, "校准中");
        if (s_ui.dialog_text  != nullptr) lv_label_set_text(s_ui.dialog_text,  "校准中... 0 / 32");
        if (s_ui.dialog_ok_btn != nullptr) {
            // 取样阶段不允许再点 OK，把按钮置灰
            lv_obj_add_flag(s_ui.dialog_ok_btn, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_cal_state == CalState::kDone) {
        DismissCalibrationDialog();
    }
}

void OnDialogCancelClicked(lv_event_t* /*e*/) {
    // 取消逻辑：无论在哪个状态都直接关闭。kSampling 中途取消等于丢弃本次。
    DismissCalibrationDialog();
}

void ShowCalibrationDialog() {
    if (s_ui.dialog_mask != nullptr) return;  // 已经开着
    s_cal_state = CalState::kConfirm;
    s_cal_count = 0;
    s_cal_sum_x = s_cal_sum_y = s_cal_sum_z = 0;

    // ---- 半透明遮罩，吃掉所有点击 ----
    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    s_ui.dialog_mask = mask;
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    // 让 mask 自己 CLICKABLE 拦截 swipe-back 之类的手势
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);

    // ---- 中央卡片 ----
    constexpr int kCardW = 580;
    constexpr int kCardH = 320;
    lv_obj_t* card = lv_obj_create(mask);
    s_ui.dialog_card = card;
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 32, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 32, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(card);
    s_ui.dialog_title = title;
    lv_label_set_text(title, "校准水平仪");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t* text = lv_label_create(card);
    s_ui.dialog_text = text;
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text, kCardW - 80);
    lv_label_set_text(text, "请将设备水平放置在桌面上保持静止，\n点击 “开始校准” 后等待 1~2 秒");
    lv_obj_set_style_text_color(text, lv_color_hex(0xC7CDD9), LV_PART_MAIN);
    lv_obj_set_style_text_font(text, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(text, LV_ALIGN_CENTER, 0, 0);

    // ---- 取消按钮 ----
    lv_obj_t* cancel = lv_button_create(card);
    s_ui.dialog_cancel = cancel;
    lv_obj_set_size(cancel, 200, 70);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 30, -24);
    lv_obj_set_style_radius(cancel, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_border_width(cancel, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, OnDialogCancelClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "取消");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(cancel_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);

    // ---- 确认/开始 按钮 ----
    lv_obj_t* ok = lv_button_create(card);
    s_ui.dialog_ok_btn = ok;
    lv_obj_set_size(ok, 260, 70);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, -30, -24);
    lv_obj_set_style_radius(ok, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_border_width(ok, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(ok, OnDialogOkClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* ok_lbl = lv_label_create(ok);
    lv_label_set_text(ok_lbl, "开始校准");
    lv_obj_set_style_text_color(ok_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(ok_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(ok_lbl);
}

// ---------------------------------------------------------------------------
// 顶层按钮回调
// ---------------------------------------------------------------------------
void OnCalibrateClicked(lv_event_t* /*e*/) {
    ShowCalibrationDialog();
}

void OnResetCalClicked(lv_event_t* /*e*/) {
    ResetCalOffsets();
    if (s_ui.status_lbl != nullptr) {
        lv_label_set_text(s_ui.status_lbl, "已重置校准");
        lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    }
}

// ---------------------------------------------------------------------------
// 屏幕导航
// ---------------------------------------------------------------------------
void OnSwipeBack() {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home    = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnBackBtnClicked(lv_event_t* /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_ui.sample_timer != nullptr) {
        lv_timer_delete(s_ui.sample_timer);
        s_ui.sample_timer = nullptr;
    }
    if (s_ui.dialog_timer != nullptr) {
        lv_timer_delete(s_ui.dialog_timer);
        s_ui.dialog_timer = nullptr;
    }
    // 清掉所有指针引用 —— 屏幕对象本身会由 lv_obj_delete_async 释放。
    s_ui = UiState{};
    s_cal_state = CalState::kIdle;
}

// ---------------------------------------------------------------------------
// UI 构建子函数
// ---------------------------------------------------------------------------
void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // 左上角返回按钮：透明圆形按钮 + "←" 图标，按下时白色半透明叠加
    constexpr int kBackBtnSize = 72;
    lv_obj_t* back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackBtnClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "水平仪");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    lv_obj_t* sensor_lbl = lv_label_create(header);
    s_ui.sensor_lbl = sensor_lbl;
    char buf[32];
    if (s_sensor_init && s_sensor != nullptr) {
        std::snprintf(buf, sizeof(buf), "SC7A20H  ID 0x%02X", s_sensor->LastWhoAmI());
    } else {
        std::snprintf(buf, sizeof(buf), "SC7A20H  --");
    }
    lv_label_set_text(sensor_lbl, buf);
    lv_obj_set_style_text_color(sensor_lbl,
                                s_sensor_init ? lv_color_hex(0x9AA3B2) : lv_color_hex(0xF87171),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(sensor_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(sensor_lbl, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16 + 130, 0);
}

void BuildLevelGraphic(lv_obj_t* parent) {
    // 圆心相对屏幕中心的偏移
    constexpr int kCircleOffsetX = kCenterX - kPanelW / 2;
    constexpr int kCircleOffsetY = kCenterY - kPanelH / 2;

    // 外参考圈：浅色细描边，背景半透明
    lv_obj_t* outer = lv_obj_create(parent);
    s_ui.outer_circle = outer;
    lv_obj_remove_style_all(outer);
    lv_obj_set_size(outer, kOuterRadius * 2, kOuterRadius * 2);
    lv_obj_align(outer, LV_ALIGN_CENTER, kCircleOffsetX, kCircleOffsetY);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(outer, lv_color_hex(0x101723), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(outer, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(outer, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(outer, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_border_opa(outer, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(outer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(outer, LV_OBJ_FLAG_CLICKABLE);

    // 中间过渡圈：更细更暗，纯装饰
    lv_obj_t* mid = lv_obj_create(parent);
    s_ui.mid_circle = mid;
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, kMidRadius * 2, kMidRadius * 2);
    lv_obj_align(mid, LV_ALIGN_CENTER, kCircleOffsetX, kCircleOffsetY);
    lv_obj_set_style_radius(mid, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(mid, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(mid, lv_color_hex(0x60A5FA), LV_PART_MAIN);
    lv_obj_set_style_border_opa(mid, LV_OPA_50, LV_PART_MAIN);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_CLICKABLE);

    // 十字基准线（横 / 竖各一根细条）
    lv_obj_t* hl = lv_obj_create(parent);
    s_ui.hline = hl;
    lv_obj_remove_style_all(hl);
    lv_obj_set_size(hl, kOuterRadius * 2 - 20, 2);
    lv_obj_align(hl, LV_ALIGN_CENTER, kCircleOffsetX, kCircleOffsetY);
    lv_obj_set_style_bg_color(hl, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hl, LV_OPA_30, LV_PART_MAIN);
    lv_obj_remove_flag(hl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* vl = lv_obj_create(parent);
    s_ui.vline = vl;
    lv_obj_remove_style_all(vl);
    lv_obj_set_size(vl, 2, kOuterRadius * 2 - 20);
    lv_obj_align(vl, LV_ALIGN_CENTER, kCircleOffsetX, kCircleOffsetY);
    lv_obj_set_style_bg_color(vl, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vl, LV_OPA_30, LV_PART_MAIN);
    lv_obj_remove_flag(vl, LV_OBJ_FLAG_CLICKABLE);

    // 中心目标圈 —— 气泡进入这里就算 “水平”，醒目的红色细描边
    lv_obj_t* target = lv_obj_create(parent);
    s_ui.target_ring = target;
    lv_obj_remove_style_all(target);
    lv_obj_set_size(target, kTargetRadius * 2, kTargetRadius * 2);
    lv_obj_align(target, LV_ALIGN_CENTER, kCircleOffsetX, kCircleOffsetY);
    lv_obj_set_style_radius(target, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(target, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(target, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(target, lv_color_hex(0xF87171), LV_PART_MAIN);
    lv_obj_remove_flag(target, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(target, LV_OBJ_FLAG_CLICKABLE);

    // 气泡：固定大小填充圆，会按加速度移动
    lv_obj_t* bubble = lv_obj_create(parent);
    s_ui.bubble = bubble;
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, kBubbleRadius * 2, kBubbleRadius * 2);
    lv_obj_align(bubble, LV_ALIGN_CENTER, kCircleOffsetX, kCircleOffsetY);
    lv_obj_set_style_radius(bubble, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0x3DDCFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 24, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(bubble, lv_color_hex(0x3DDCFF), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(bubble, LV_OPA_50, LV_PART_MAIN);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
}

void BuildFooter(lv_obj_t* parent) {
    // 状态大字（“水平” / “未水平 X°”）
    lv_obj_t* status = lv_label_create(parent);
    s_ui.status_lbl = status;
    lv_label_set_text(status, "等待数据...");
    lv_obj_set_style_text_color(status, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_pos(status, 0, kFooterTopY - 2);
    lv_obj_set_width(status, kPanelW);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // pitch / roll 中字
    lv_obj_t* angle = lv_label_create(parent);
    s_ui.angle_lbl = angle;
    lv_label_set_text(angle, "Pitch: --   Roll: --");
    lv_obj_set_style_text_color(angle, lv_color_hex(0xC7CDD9), LV_PART_MAIN);
    lv_obj_set_style_text_font(angle, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(angle, 0, kFooterTopY + 44);
    lv_obj_set_width(angle, kPanelW);
    lv_obj_set_style_text_align(angle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // raw mg 三轴
    lv_obj_t* xyz = lv_label_create(parent);
    s_ui.xyz_lbl = xyz;
    lv_label_set_text(xyz, "X: --   Y: --   Z: --   (mg)");
    lv_obj_set_style_text_color(xyz, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(xyz, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(xyz, 0, kFooterTopY + 70);
    lv_obj_set_width(xyz, kPanelW);
    lv_obj_set_style_text_align(xyz, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // ---- 底部两个操作按钮 ----
    constexpr int kBtnW   = 280;
    constexpr int kBtnH   = 70;
    constexpr int kBtnY   = kPanelH - kBtnH - 12;
    constexpr int kBtnGap = 24;
    constexpr int kBtnTotalW = kBtnW * 2 + kBtnGap;
    constexpr int kBtnStartX = (kPanelW - kBtnTotalW) / 2;

    lv_obj_t* cal = lv_button_create(parent);
    s_ui.cal_btn = cal;
    lv_obj_set_size(cal, kBtnW, kBtnH);
    lv_obj_set_pos(cal, kBtnStartX, kBtnY);
    lv_obj_set_style_radius(cal, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cal, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_border_width(cal, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cal, OnCalibrateClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cal_lbl = lv_label_create(cal);
    lv_label_set_text(cal_lbl, "校准");
    lv_obj_set_style_text_color(cal_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(cal_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(cal_lbl);

    lv_obj_t* reset = lv_button_create(parent);
    s_ui.reset_btn = reset;
    lv_obj_set_size(reset, kBtnW, kBtnH);
    lv_obj_set_pos(reset, kBtnStartX + kBtnW + kBtnGap, kBtnY);
    lv_obj_set_style_radius(reset, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(reset, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_border_width(reset, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(reset, lv_color_hex(0x3A4050), LV_PART_MAIN);
    lv_obj_add_event_cb(reset, OnResetCalClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* reset_lbl = lv_label_create(reset);
    lv_label_set_text(reset_lbl, "重置校准");
    lv_obj_set_style_text_color(reset_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(reset_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(reset_lbl);
}

}  // namespace

// ===========================================================================
// 公共接口
// ===========================================================================
lv_obj_t* LevelScreen::Create() {
    EnsureSensorInited();
    LoadCalOffsetsOnce();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui = UiState{};
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildLevelGraphic(scr);
    BuildFooter(scr);

    s_ui.bubble_x_px = 0.0f;
    s_ui.bubble_y_px = 0.0f;

    s_ui.sample_timer = lv_timer_create(OnSampleTick, kSamplePeriodMs, nullptr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    ESP_LOGI(TAG, "level screen ready (sensor=%s)", s_sensor_init ? "ok" : "absent");
    return scr;
}

void LevelScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: level_screen");
        EnsureSensorInited();
        LoadCalOffsetsOnce();
    } else {
        ESP_LOGI(TAG, "unload: level_screen");
        // 屏幕卸载时 sample timer 已经在 OnScreenUnloaded 里关掉；这里
        // 不动 SC7A20H 配置，下一次进入页面再用。
    }
}






