#ifndef BQ27220_GAUGE_H
#define BQ27220_GAUGE_H

#include <cstdint>
#include <driver/i2c_master.h>

// ---------------------------------------------------------------------------
// Bq27220Gauge
//
// TI BQ27220 单节锂电电量计的瘦封装。设计成单例：板级在 InitializeI2C() 之后
// 调用 Begin(bus)，其它子系统（UI / 调试 task / 应用层）只要 include 这一个
// 头文件、调 Bq27220Gauge::GetInstance().XXX() 就能拿到电压 / 电流 / 电量，
// 不用再 extern 或者透出板子私有类。
//
// 行为要点（沿用从 xingzhi-395 板抽出的实现，保持兼容）：
//   - Begin() 用 i2c_master_probe 做地址级 ACK 检测，BQ27220 没接 / 没上电时
//     不会 ESP_ERROR_CHECK 崩溃，只是 IsReady() 返回 false。
//   - GetBatteryLevel() 走 "电压 -> SOC 线性内插 + 60 点滑动平均" 路径，
//     不依赖芯片未标定的 SOC 寄存器；同时会自动节流重试 probe（10s/次），
//     后插电池场景能自愈。
//   - 充电方向用电流寄存器 (0x0C, int16, mA) 的符号判定，留 5mA 死区避免
//     空载抖动。
//   - 读寄存器失败按 10 次连错才打一条警告，不会刷屏。
//
// Typical use:
//
//     auto& gauge = Bq27220Gauge::GetInstance();
//     gauge.Begin(i2c_bus_);                 // 板级 init 阶段调一次
//
//     int level; bool charging, discharging;
//     if (gauge.GetBatteryLevel(level, charging, discharging)) {
//         printf("battery %d%% charging=%d\n", level, charging);
//     }
//     uint16_t mv;
//     if (gauge.GetVoltageMv(mv)) {
//         printf("voltage %u mV\n", mv);
//     }
// ---------------------------------------------------------------------------
class Bq27220Gauge {
public:
    static constexpr uint8_t kDefaultAddr = 0x55;

    static Bq27220Gauge& GetInstance() {
        static Bq27220Gauge instance;
        return instance;
    }

    // 在已有的 v2 master bus 上挂 BQ27220。重复调用安全，已挂上时直接返回 true。
    // probe NACK 时返回 false，调用方不需要做任何事 —— 后续 GetBatteryLevel
    // 内部会节流自愈，10s 重试一次。
    bool Begin(i2c_master_bus_handle_t bus, uint8_t addr = kDefaultAddr);

    // 当前是否已经挂上了 BQ27220 device handle。
    bool IsReady() const { return dev_ != nullptr; }

    // ---- 低层寄存器读 ----
    // 返回 false 表示总线错误 / 设备没挂上；调用方可以兜底显示 "--"。
    bool ReadVoltageMv(uint16_t& mv);
    bool ReadCurrentMa(int16_t& current_ma);

    // 别名：和板子原本的命名兼容。
    bool GetVoltageMv(uint16_t& mv) { return ReadVoltageMv(mv); }

    // ---- 高层接口 ----
    // 完整电量 / 充放电状态。语义和 Board::GetBatteryLevel 对齐：
    //   level       : 0..100，电压 → SOC 线性内插 + 60 点滑动平均
    //   charging    : 电流 > +5mA
    //   discharging : 电流 < -5mA
    // 返回 false 表示 device 没挂上、或这一帧总线读失败；调用方应保留上次值
    // 或显示占位。
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging);

    // 重置内部滑动平均缓存。换电池 / 长时间断开总线后可以调一次，避免老数据
    // 把新电压拉低。一般不需要主动调。
    void ResetFilter();

private:
    Bq27220Gauge() = default;
    Bq27220Gauge(const Bq27220Gauge&) = delete;
    Bq27220Gauge& operator=(const Bq27220Gauge&) = delete;

    // 读 BQ27220 一个 uint16 寄存器（标准命令均按 little-endian 解析）。
    bool ReadU16(uint8_t reg, uint16_t* out);

    // 60 点滑动平均；首次填满整窗口避免开机一两秒内 SOC 大跳。
    float FilterPush(float sample);

    static constexpr int kFilterSize = 60;

    i2c_master_bus_handle_t bus_   = nullptr;
    i2c_master_dev_handle_t dev_   = nullptr;
    uint8_t                 addr_  = kDefaultAddr;

    int  consecutive_err_  = 0;   // 连续读失败计数，用于警告去抖
    int  retry_counter_    = 0;   // GetBatteryLevel 中的重挂节流

    float filter_buf_[kFilterSize] = {0};
    int   filter_idx_      = 0;
    int   filter_count_    = 0;
    float filter_sum_      = 0.0f;
    bool  filter_primed_   = false;
};

#endif  // BQ27220_GAUGE_H
