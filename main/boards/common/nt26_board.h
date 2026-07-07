#ifndef NT26_BOARD_H
#define NT26_BOARD_H

#include <memory>
#include <uart_eth_modem.h>
#include <esp_network.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include "freertos/event_groups.h"
#include "board.h"

struct Nt26CeregState {
    int stat = 0;
    std::string tac;
    std::string ci;
    int AcT = -1;

    std::string ToString() const {
        std::string json = "{";
        json += "\"stat\":" + std::to_string(stat);
        if (!tac.empty()) json += ",\"tac\":\"" + tac + "\"";
        if (!ci.empty()) json += ",\"ci\":\"" + ci + "\"";
        if (AcT >= 0) json += ",\"AcT\":" + std::to_string(AcT);
        json += "}";
        return json;
    }
};

class Nt26Board : public Board {
protected:
    std::unique_ptr<UartEthModem> modem_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    gpio_num_t dtr_pin_; // mrdy_pin
    gpio_num_t ri_pin_;  // srdy_pin
    gpio_num_t reset_pin_;
    
    NetworkEventCallback network_event_callback_;
    esp_pm_lock_handle_t pm_lock_cpu_max_ = nullptr;
    PowerSaveLevel current_power_level_ = PowerSaveLevel::LOW_POWER;
    esp_timer_handle_t network_ready_timer_ = nullptr;
    EventGroupHandle_t network_wait_event_ = nullptr;

    virtual std::string GetBoardJson() override;
    
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");
    static void OnNetworkReadyTimeout(void* arg);
    void ScheduleAsyncStop();

public:
    Nt26Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin, gpio_num_t reset_pin = GPIO_NUM_NC);
    virtual ~Nt26Board();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual const char* GetNetworkStateIcon() override;
    // virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    Nt26CeregState GetRegistrationState();

    // 转发到 UartEthModem::SendAt。线程安全（modem 内部用 mutex 串行化）。
    // 在 modem 还没初始化、或 ML307/4G 网络未就绪时返回 ESP_ERR_INVALID_STATE。
    // 给 UI 屏幕（如 CallScreen）拨号 / 挂断用：
    //   SendAtCommand("ATD17880684667", resp);  -> 返回 ESP_OK 且 resp 含 "OK"
    //   SendAtCommand("ATH", resp);
    //
    // bypass_init_check=true: 跳过本地的 IsInitialized() 检查，只要 modem
    // 实例存在就直接转发到底层 SendAt（UartEthModem 内部仍然有它自己的
    // handshake / stop_flag 保护）。专门给 SIM 卡切换之类的场景用：外置卡
    // 没插的时候 modem 不会进入 “initialized” 状态，但 AT 通道本身是通的，
    // 拒绝下发会让用户永远卡死在没卡的槽位上。
    esp_err_t SendAtCommand(const std::string& cmd, std::string& response,
                            uint32_t timeout_ms = 5000,
                            bool bypass_init_check = false);

    // 发送 AT 并持续收集 URC，直到出现 done_marker（如 "+ECPING: DONE"）。
    esp_err_t SendAtCommandCollectUntil(const std::string& cmd,
                                        std::string& response,
                                        uint32_t timeout_ms,
                                        const char* done_marker,
                                        bool bypass_init_check = false);
};

#endif // NT26_BOARD_H
