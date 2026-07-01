#ifndef SIMPLE_UART_HPP
#define SIMPLE_UART_HPP

#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <vector>
#include <cstring>
#include <string>

class SimpleUart {
public:
    // 获取单例实例
    static SimpleUart& getInstance() {
        static SimpleUart instance;
        return instance;
    }

    // 删除拷贝构造函数和赋值运算符
    SimpleUart(const SimpleUart&) = delete;
    SimpleUart& operator=(const SimpleUart&) = delete;

    // 配置UART参数
    bool begin(int txPin, int rxPin, int baudRate, uart_port_t uartNum = UART_NUM_1) {
        if (m_initialized) {
            return false;
        }

        m_uartNum = uartNum;
        m_txPin = txPin;
        m_rxPin = rxPin;
        m_baudRate = baudRate;

        // UART配置 - 完整初始化所有成员
        uart_config_t uartConfig = {
            .baud_rate = m_baudRate,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 0,
            .source_clk = UART_SCLK_DEFAULT,
            .flags = 0
        };

        // 安装UART驱动
        esp_err_t err = uart_driver_install(m_uartNum, RX_BUFFER_SIZE, TX_BUFFER_SIZE, 10, &m_uartQueue, 0);
        if (err != ESP_OK) {
            return false;
        }

        err = uart_param_config(m_uartNum, &uartConfig);
        if (err != ESP_OK) {
            return false;
        }

        err = uart_set_pin(m_uartNum, m_txPin, m_rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            return false;
        }

        m_initialized = true;
        
        // 创建接收任务
        xTaskCreate(uartReceiveTask, "uart_rx_task", 4096, this, 10, &m_rxTaskHandle);
        
        return true;
    }

    // 注册接收回调函数
    void registerCallback(std::function<void(const std::vector<uint8_t>&)> callback) {
        m_callback = callback;
    }

    // 发送数据
    bool sendData(const uint8_t* data, size_t length) {
        if (!m_initialized) {
            return false;
        }
        
        int bytesSent = uart_write_bytes(m_uartNum, (const char*)data, length);
        return bytesSent == length;
    }

    // 发送字符串
    bool sendString(const std::string& data) {
        return sendData(reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
    }

    // 发送字符串（C风格）
    bool sendString(const char* data) {
        if (data == nullptr) return false;
        return sendData(reinterpret_cast<const uint8_t*>(data), strlen(data));
    }

    // 发送单个字节
    bool sendByte(uint8_t byte) {
        return sendData(&byte, 1);
    }

    // 结束UART
    void end() {
        if (m_initialized) {
            if (m_rxTaskHandle != nullptr) {
                vTaskDelete(m_rxTaskHandle);
                m_rxTaskHandle = nullptr;
            }
            
            if (m_uartQueue != nullptr) {
                uart_driver_delete(m_uartNum);
                m_uartQueue = nullptr;
            }
            
            m_initialized = false;
        }
    }

    // 检查是否已初始化
    bool isInitialized() const {
        return m_initialized;
    }

private:
    // 私有构造函数
    SimpleUart() 
        : m_uartNum(UART_NUM_1)
        , m_txPin(1)
        , m_rxPin(3)
        , m_baudRate(115200)
        , m_initialized(false)
        , m_rxTaskHandle(nullptr)
        , m_uartQueue(nullptr)
        , m_callback(nullptr) {}

    // 析构函数
    ~SimpleUart() {
        end();
    }

    // UART接收任务
    static void uartReceiveTask(void* arg) {
        SimpleUart* uart = static_cast<SimpleUart*>(arg);
        uint8_t buffer[RX_BUFFER_SIZE];
        
        while (true) {
            int length = uart_read_bytes(uart->m_uartNum, buffer, RX_BUFFER_SIZE - 1, pdMS_TO_TICKS(100));
            
            if (length > 0 && uart->m_callback) {
                std::vector<uint8_t> data(buffer, buffer + length);
                uart->m_callback(data);
            }
        }
    }

    // 常量定义
    static constexpr size_t RX_BUFFER_SIZE = 1024;
    static constexpr size_t TX_BUFFER_SIZE = 1024;

    // UART配置参数
    uart_port_t m_uartNum;
    int m_txPin;
    int m_rxPin;
    int m_baudRate;
    bool m_initialized;
    
    // 任务句柄
    TaskHandle_t m_rxTaskHandle;
    QueueHandle_t m_uartQueue;
    
    // 回调函数
    std::function<void(const std::vector<uint8_t>&)> m_callback;
};

#endif // SIMPLE_UART_HPP