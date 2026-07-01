// Copyright 2025 Terrence
// SPDX-License-Identifier: Apache-2.0

#include "uart_eth_modem.h"

#include <cstdio>
#include <cstring>

#include <esp_check.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <hal/gpio_ll.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>


// Static member definitions
constexpr uint8_t UartEthModem::kHandshakeRequest[];
constexpr uint8_t UartEthModem::kHandshakeAck[];

UartEthModem::UartEthModem(const Config& config) : config_(config) {
    // Generate MAC address
    esp_read_mac(mac_addr_, ESP_MAC_ETH);
    mac_addr_[5] ^= 0x01;  // Make unique

    // Create event group
    event_group_ = xEventGroupCreate();
    if (!event_group_) {
        ESP_LOGE(kTag, "Failed to create event group");
        abort();  // Constructor cannot fail gracefully
    }

    // Initialize driver structure
    driver_.name = "uart_eth";
    driver_.init = [](iot_eth_driver_t* driver) -> esp_err_t {
        // Already initialized
        return ESP_OK;
    };
    driver_.deinit = [](iot_eth_driver_t* driver) -> esp_err_t {
        return ESP_OK;
    };
    driver_.start = [](iot_eth_driver_t* driver) -> esp_err_t {
        return ESP_OK;
    };
    driver_.stop = [](iot_eth_driver_t* driver) -> esp_err_t {
        return ESP_OK;
    };
    driver_.transmit = [](iot_eth_driver_t* driver, uint8_t* buf, size_t len) -> esp_err_t {
        // Get UartEthModem instance using container_of pattern
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
        auto* self = reinterpret_cast<UartEthModem*>(reinterpret_cast<char*>(driver) - offsetof(UartEthModem, driver_));
#pragma GCC diagnostic pop
        if (!self->handshake_done_.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        
        // Non-blocking: enqueue frame for TX task to send
        return self->EnqueueTxFrame(buf, len);
    };
    driver_.get_addr = [](iot_eth_driver_t* driver, uint8_t* mac) -> esp_err_t {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
        auto* self = reinterpret_cast<UartEthModem*>(reinterpret_cast<char*>(driver) - offsetof(UartEthModem, driver_));
#pragma GCC diagnostic pop
        memcpy(mac, self->mac_addr_, 6);
        return ESP_OK;
    };
    driver_.set_mediator = [](iot_eth_driver_t* driver, iot_eth_mediator_t* mediator) -> esp_err_t {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
        auto* self = reinterpret_cast<UartEthModem*>(reinterpret_cast<char*>(driver) - offsetof(UartEthModem, driver_));
#pragma GCC diagnostic pop
        self->mediator_ = mediator;
        return ESP_OK;
    };
}

UartEthModem::~UartEthModem() {
    Stop();

    // Cleanup resources created in constructor
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

esp_err_t UartEthModem::Start(bool flight_mode) {
    ESP_LOGI(kTag, "Starting UartEthModem%s...", flight_mode ? " (flight mode)" : "");

    if (initialized_.load()) {
        ESP_LOGW(kTag, "Already started");
        return ESP_ERR_INVALID_STATE;
    }

    flight_mode_ = flight_mode;
    stop_flag_ = false;
    handshake_done_ = false;
    initializing_ = true;

    // Create event queue FIRST (before GPIO init, since ISR uses it)
    event_queue_ = xQueueCreate(32, sizeof(Event));
    if (!event_queue_) {
        ESP_LOGE(kTag, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    // Create TX queue for non-blocking transmit from LWIP
    tx_queue_ = xQueueCreate(kTxQueueDepth, sizeof(TxFrame));
    if (!tx_queue_) {
        ESP_LOGE(kTag, "Failed to create TX queue");
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    // Initialize UART
    esp_err_t ret = InitUart();
    if (ret != ESP_OK) {
        vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
        return ret;
    }

    // Initialize GPIO
    ret = InitGpio();
    if (ret != ESP_OK) {
        DeinitUart();
        vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
        return ret;
    }

    // Allocate frame reassembly buffer
    reassembly_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(kMaxFrameSize, MALLOC_CAP_INTERNAL));
    if (!reassembly_buffer_) {
        ESP_LOGE(kTag, "Failed to allocate reassembly buffer");
        vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
        vQueueDelete(event_queue_);
        DeinitGpio();
        DeinitUart();
        return ESP_ERR_NO_MEM;
    }
    reassembly_size_ = 0;
    reassembly_expected_ = 0;

    // Initialize UART UHCI DMA controller with buffer pool
    UartUhci::Config uhci_cfg = {
        .uart_port = config_.uart_num,
        .dma_burst_size = 32,
        .rx_pool = {
            .buffer_count = config_.rx_buffer_count,
            .buffer_size = config_.rx_buffer_size,
        },
    };

    ret = uart_uhci_.Init(uhci_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to init UHCI: %s", esp_err_to_name(ret));
        free(reassembly_buffer_);
        reassembly_buffer_ = nullptr;
        vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
        vQueueDelete(event_queue_);
        DeinitGpio();
        DeinitUart();
        return ret;
    }

    // Register UHCI callbacks
    uart_uhci_.SetRxCallback(UhciRxCallbackStatic, this);

    // Note: Don't start DMA receive here, will be started when entering Active state

    // Create main task (handles all events, no blocking operations)
    xTaskCreate([](void* arg) {
        static_cast<UartEthModem*>(arg)->MainTaskRun();
        vTaskDelete(nullptr);
    }, "uart_eth_main", 4096, this, 10, &main_task_);
    
    // Create init task
    xTaskCreate([](void* arg) {
        static_cast<UartEthModem*>(arg)->InitTaskRun();
        vTaskDelete(nullptr);
    }, "uart_eth_init", 4096, this, 4, &init_task_);

    // Create TX task (dedicated for non-blocking transmit from LWIP)
    xTaskCreate([](void* arg) {
        static_cast<UartEthModem*>(arg)->TxTaskRun();
        vTaskDelete(nullptr);
    }, "uart_eth_tx", 3072, this, 9, &tx_task_);  // Priority slightly lower than main

    if (!main_task_ || !init_task_ || !tx_task_) {
        ESP_LOGE(kTag, "Failed to create tasks");
        stop_flag_ = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        uart_uhci_.Deinit();
        free(reassembly_buffer_);
        reassembly_buffer_ = nullptr;
        vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
        vQueueDelete(event_queue_);
        DeinitGpio();
        DeinitUart();
        return ESP_ERR_NO_MEM;
    }

    // Signal start (initialization continues asynchronously in InitTaskRun)
    // Failure will be notified via event callback (ErrorInitFailed, ErrorNoSim, etc.)
    // Caller should call Stop() after receiving failure event to cleanup resources
    xEventGroupSetBits(event_group_, kEventStart);

    ESP_LOGI(kTag, "UartEthModem starting asynchronously...");
    return ESP_OK;
}

esp_err_t UartEthModem::Stop() {
    // Check if there's anything to stop: event_queue_ is created in Start()
    // and destroyed in CleanupResources(). If it exists, tasks may be running.
    if (!event_queue_) {
        return ESP_OK;
    }

    ESP_LOGI(kTag, "Stopping UartEthModem...");

    stop_flag_ = true;
    initializing_ = false;
    if (event_group_) {
        xEventGroupSetBits(event_group_, kEventStop);
    }

    // Send Stop event to main task queue to wake it up from xQueueReceive
    // (MainTask may be blocked on portMAX_DELAY in Idle state)
    if (event_queue_) {
        Event event = {.type = EventType::Stop, .rx_buffer = nullptr};
        xQueueSend(event_queue_, &event, 0);
    }

    // Send dummy frame to tx_queue to wake up TxTask from xQueueReceive
    // (TxTask may be blocked on portMAX_DELAY waiting for frame)
    if (tx_queue_) {
        TxFrame dummy_frame = {};
        xQueueSend(tx_queue_, &dummy_frame, 0);
    }

    // Wait for tasks to finish
    if (event_group_) {
        auto bits = xEventGroupWaitBits(event_group_, kEventAllTasksDone, pdTRUE, pdTRUE, pdMS_TO_TICKS(10000));
        if (!(bits & kEventAllTasksDone)) {
            ESP_LOGE(kTag, "Timeout to wait for tasks to finish");
        }
    }

    // Cleanup all resources including iot_eth
    CleanupResources(true);

    initialized_ = false;

    ESP_LOGI(kTag, "UartEthModem stopped");
    return ESP_OK;
}

esp_err_t UartEthModem::SendAt(const std::string& cmd, std::string& response, uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(at_mutex_);

    // Check again after acquiring mutex in case Stop() was called while waiting
    if (stop_flag_.load()) {
        return ESP_ERR_INVALID_STATE;
    }

    // Allow AT commands during initialization even if not connected
    if (!handshake_done_.load() && !initializing_.load() && !initialized_.load()) {
        ESP_LOGE(kTag, "Failed to send AT command: not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    at_command_response_.clear();
    waiting_for_at_response_ = true;
    xEventGroupClearBits(event_group_, kEventAtResponse);  // Clear any pending

    // Add \r if not present
    std::string cmd_with_cr = cmd;
    if (cmd_with_cr.empty() || cmd_with_cr.back() != '\r') {
        cmd_with_cr += '\r';
    }

    if (debug_enabled_.load()) {
        ESP_LOGI(kTag, "AT>>> %s", cmd.c_str());
    }

    // Send AT command frame
    esp_err_t ret = SendFrame(reinterpret_cast<const uint8_t*>(cmd_with_cr.c_str()), cmd_with_cr.size(), FrameType::kAtCommand);
    if (ret != ESP_OK) {
        waiting_for_at_response_ = false;
        return ret;
    }

    // Wait for response
    EventBits_t bits = xEventGroupWaitBits(
        event_group_,
        kEventAtResponse | kEventStop,
        pdTRUE,   // Clear on exit
        pdFALSE,  // Wait for any bit
        pdMS_TO_TICKS(timeout_ms)
    );
    if (bits & kEventStop) {
        waiting_for_at_response_ = false;
        return ESP_ERR_INVALID_STATE;
    }
    if (!(bits & kEventAtResponse)) {
        ESP_LOGW(kTag, "AT timeout: %s", cmd.c_str());
        waiting_for_at_response_ = false;
        return ESP_ERR_TIMEOUT;
    }

    waiting_for_at_response_ = false;
    response = at_command_response_;

    // Check for OK/ERROR
    if (response.find("OK") != std::string::npos) {
        return ESP_OK;
    } else if (response.find("ERROR") != std::string::npos) {
        return ESP_FAIL;
    }

    // Response contains neither OK nor ERROR (unexpected)
    return ESP_FAIL;
}

void UartEthModem::SetNetworkEventCallback(UartEthModemEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void UartEthModem::SetDebug(bool enabled) {
    debug_enabled_.store(enabled);
}

std::string UartEthModem::GetImei() {
    if (imei_.empty()) {
        std::string resp;
        if (SendAt("AT+CGSN=1", resp) == ESP_OK) {
            // Parse +CGSN: "IMEI" using sscanf, consistent with C implementation
            char imei[16] = {0};
            if (sscanf(resp.c_str(), "\r\n+CGSN: \"%15s", imei) == 1) {
                imei_ = imei;
            }
        }
    }
    return imei_;
}

std::string UartEthModem::GetImsi() {
    if (imsi_.empty()) {
        std::string resp;
        if (SendAt("AT+CIMI", resp) == ESP_OK) {
            // Parse response using sscanf, consistent with C implementation
            char imsi[16] = {0};
            if (sscanf(resp.c_str(), "\r\n%15s", imsi) == 1) {
                imsi_ = imsi;
            }
        }
    }
    return imsi_;
}

std::string UartEthModem::GetIccid() {
    if (iccid_.empty()) {
        std::string resp;
        if (SendAt("AT+ECICCID", resp) == ESP_OK) {
            // Parse +ECICCID: ICCID using sscanf, consistent with C implementation
            char iccid[21] = {0};
            if (sscanf(resp.c_str(), "\r\n+ECICCID: %20s", iccid) == 1) {
                iccid_ = iccid;
            }
        }
    }
    return iccid_;
}

std::string UartEthModem::GetCarrierName() {
    std::string resp;
    if (SendAt("AT+COPS?", resp) == ESP_OK) {
        // Parse +COPS: mode,format,"operator",act using sscanf
        int mode = 0, format = 0, act = 0;
        char operator_name[64] = {0};
        if (sscanf(resp.c_str(), "\r\n+COPS: %d,%d,\"%63[^\"]\",%d", &mode, &format, operator_name, &act) >= 3) {
            carrier_name_ = operator_name;
        }
    }
    return carrier_name_;
}

std::string UartEthModem::GetModuleRevision() {
    if (module_revision_.empty()) {
        std::string resp;
        if (SendAt("AT+CGMR", resp) == ESP_OK) {
            // Parse response using sscanf, consistent with C implementation
            char revision[128] = {0};
            if (resp.find("+CGMR:") != std::string::npos) {
                // Format: "\r\n+CGMR: \r\n<version>\r\n"
                if (sscanf(resp.c_str(), "\r\n+CGMR: \r\n%127[^\r\n]", revision) == 1) {
                    module_revision_ = revision;
                }
            } else {
                // Format: "\r\n<version>\r\n"
                if (sscanf(resp.c_str(), "\r\n%127[^\r\n]", revision) == 1) {
                    module_revision_ = revision;
                }
            }
        }
    }
    return module_revision_;
}

int UartEthModem::GetSignalStrength() {
    if (!initialized_.load()) {
        return 99;
    }
    if (cell_info_.stat == 2) {
        return 99;
    }

    std::string resp;
    if (SendAt("AT+CSQ", resp, 500) == ESP_OK) {
        // Parse +CSQ: rssi,ber
        auto pos = resp.find("+CSQ:");
        if (pos != std::string::npos) {
            int rssi = 99;
            sscanf(resp.c_str() + pos, "+CSQ: %d", &rssi);
            signal_strength_ = rssi;
        }
    }
    return signal_strength_;
}

UartEthModem::CellInfo UartEthModem::GetCellInfo() {
    std::string resp;
    if (SendAt("AT+CEREG?", resp) == ESP_OK) {
        ParseAtResponse(resp);
    }
    return cell_info_;
}

const char* UartEthModem::GetNetworkEventName(UartEthModemEvent event) {
    switch (event) {
        case UartEthModemEvent::Connecting: return "Connecting";
        case UartEthModemEvent::Connected: return "Connected";
        case UartEthModemEvent::Disconnected: return "Disconnected";
        case UartEthModemEvent::InFlightMode: return "InFlightMode";
        case UartEthModemEvent::ErrorNoSim: return "ErrorNoSim";
        case UartEthModemEvent::ErrorRegistrationDenied: return "ErrorRegistrationDenied";
        case UartEthModemEvent::ErrorInitFailed: return "ErrorInitFailed";
        case UartEthModemEvent::ErrorNoCarrier: return "ErrorNoCarrier";
        case UartEthModemEvent::RequestingPdpContext: return "RequestingPdpContext";
        default: return "Unknown";
    }
}

// Private methods

esp_err_t UartEthModem::InitUart() {
    // UHCI DMA mode: only configure UART params, don't install driver (UHCI takes over)
    uart_config_t uart_config = {
        .baud_rate = config_.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {.allow_pd = 0, .backup_before_sleep = 0},
    };
    esp_err_t ret = uart_param_config(config_.uart_num, &uart_config);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to configure UART");

    ret = uart_set_pin(config_.uart_num, config_.tx_pin, config_.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to set UART pins");

    gpio_set_pull_mode(config_.rx_pin, GPIO_PULLUP_ONLY);

    return ESP_OK;
}

esp_err_t UartEthModem::InitGpio() {
    // MRDY pin (output) - Master Ready/Busy
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << config_.mrdy_pin);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    esp_err_t ret = gpio_config(&io_conf);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to configure MRDY pin");

    // Set MRDY high initially (idle)
    gpio_set_level(config_.mrdy_pin, 1);
    gpio_sleep_sel_dis(config_.mrdy_pin);

    // SRDY pin (input with interrupt) - Slave Ready/Busy
    io_conf.pin_bit_mask = (1ULL << config_.srdy_pin);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ret = gpio_config(&io_conf);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to configure SRDY pin");

    // Enable GPIO wakeup on low level (for light sleep)
    ret = gpio_wakeup_enable(config_.srdy_pin, GPIO_INTR_LOW_LEVEL);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to enable GPIO wakeup");

    // Install ISR handler
    ret = gpio_isr_handler_add(config_.srdy_pin, SrdyIsrHandler, this);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to add ISR handler");

    // Initially configure for wakeup (low level trigger)
    ConfigureSrdyInterrupt(kSrdyInterruptForWakeup);

    return ESP_OK;
}

esp_err_t UartEthModem::InitIotEth() {
    // Install iot_eth driver
    iot_eth_config_t eth_cfg = {
        .driver = &driver_,
        .stack_input = nullptr,
        .user_data = this,
    };

    esp_err_t ret = iot_eth_install(&eth_cfg, &eth_handle_);
    ESP_RETURN_ON_ERROR(ret, kTag, "Failed to install iot_eth driver");

    // Create netif with GARP disabled
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    // Copy base config to modify flags (original is const)
    esp_netif_inherent_config_t base_cfg = *netif_cfg.base;
    base_cfg.flags = static_cast<esp_netif_flags_t>(base_cfg.flags & ~ESP_NETIF_FLAG_GARP);
    netif_cfg.base = &base_cfg;
    eth_netif_ = esp_netif_new(&netif_cfg);
    if (!eth_netif_) {
        ESP_LOGE(kTag, "Failed to create netif");
        iot_eth_uninstall(eth_handle_);
        eth_handle_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    // Create glue and attach
    glue_ = iot_eth_new_netif_glue(eth_handle_);
    if (!glue_) {
        ESP_LOGE(kTag, "Failed to create netif glue");
        esp_netif_destroy(eth_netif_);
        eth_netif_ = nullptr;
        iot_eth_uninstall(eth_handle_);
        eth_handle_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    esp_netif_attach(eth_netif_, glue_);

    // Start iot_eth
    ret = iot_eth_start(eth_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start iot_eth");
        iot_eth_del_netif_glue(glue_);
        glue_ = nullptr;
        esp_netif_destroy(eth_netif_);
        eth_netif_ = nullptr;
        iot_eth_uninstall(eth_handle_);
        eth_handle_ = nullptr;
        return ret;
    }

    // Register IP event handler to detect when we get an IP address
    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                              &IpEventHandler, this,
                                              &ip_event_handler_instance_);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register IP event handler");
        iot_eth_stop(eth_handle_);
        iot_eth_del_netif_glue(glue_);
        glue_ = nullptr;
        esp_netif_destroy(eth_netif_);
        eth_netif_ = nullptr;
        iot_eth_uninstall(eth_handle_);
        eth_handle_ = nullptr;
        return ret;
    }

    // Notify iot_eth of link state changes (critical for netif to work)
    // This triggers IOT_ETH_EVENT_CONNECTED which starts DHCP
    if (mediator_) {
        // Notify MAC address available
        mediator_->on_stage_changed(mediator_, IOT_ETH_STAGE_GET_MAC, nullptr);
        // Notify link is up (IOT_ETH_LINK_UP = 0)
        int link_status = 0;  // IOT_ETH_LINK_UP
        mediator_->on_stage_changed(mediator_, IOT_ETH_STAGE_LINK, &link_status);
    }

    // Clear DNS cache
    tcpip_callback([](void* arg) -> void {
        dns_clear_cache();
    }, nullptr);

    return ESP_OK;
}

void UartEthModem::DeinitUart() {
    // UHCI mode: UART driver is not installed, just disconnect pins and reset GPIO
    // Disconnect UART pins (set to UART_PIN_NO_CHANGE disconnects the signal)
    uart_set_pin(config_.uart_num, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // Reset GPIO pins to default state (input, no pull) to save power
    gpio_reset_pin(config_.tx_pin);
    gpio_reset_pin(config_.rx_pin);
}

void UartEthModem::DeinitGpio() {
    gpio_wakeup_disable(config_.srdy_pin);
    gpio_isr_handler_remove(config_.srdy_pin);
    // Re-enable sleep select (was disabled in InitGpio for MRDY)
    gpio_sleep_sel_en(config_.mrdy_pin);
    // Reset GPIO pins to default state (input, no pull) to save power
    gpio_reset_pin(config_.mrdy_pin);
    gpio_reset_pin(config_.srdy_pin);
}

void UartEthModem::DeinitIotEth() {
    // Unregister IP event handler first
    if (ip_event_handler_instance_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                              ip_event_handler_instance_);
        ip_event_handler_instance_ = nullptr;
    }
    if (eth_handle_) {
        iot_eth_stop(eth_handle_);
    }
    if (glue_) {
        iot_eth_del_netif_glue(glue_);
        glue_ = nullptr;
    }
    if (eth_netif_) {
        // Stop DHCP client first to prevent use-after-free in dhcp_fine_tmr
        // The DHCP timer runs periodically and accesses netif's DHCP data;
        // destroying netif without stopping DHCP causes crash.
        esp_netif_dhcpc_stop(eth_netif_);
        esp_netif_destroy(eth_netif_);
        eth_netif_ = nullptr;
    }
    if (eth_handle_) {
        iot_eth_uninstall(eth_handle_);
        eth_handle_ = nullptr;
    }
}

// TX task: unified task for all frame transmission
// All frames (AT commands, handshake, Ethernet) go through this single path,
// eliminating resource contention between SendFrame and direct UHCI access.
void UartEthModem::TxTaskRun() {
    ESP_LOGD(kTag, "TX task started");

    while (!stop_flag_.load()) {
        TxFrame frame;
        // Wait for frame in queue (blocks here, not in LWIP context)
        if (xQueueReceive(tx_queue_, &frame, portMAX_DELAY) == pdTRUE) {
            if (stop_flag_.load()) {
                // Cleanup frame if stopping
                if (frame.data) {
                    free(frame.data);
                }
                // Notify waiter if any
                if (frame.done_sem) {
                    if (frame.result) {
                        *frame.result = ESP_ERR_INVALID_STATE;
                    }
                    xSemaphoreGive(frame.done_sem);
                }
                break;
            }

            esp_err_t ret = ESP_OK;

            // Enter active state if needed (via MainTask)
            if (working_state_.load() != WorkingState::Active) {
                Event event = {.type = EventType::TxRequest, .rx_buffer = nullptr};
                xQueueSend(event_queue_, &event, pdMS_TO_TICKS(10));
                
                // Wait for active state
                EventBits_t bits = xEventGroupWaitBits(
                    event_group_,
                    kEventActiveState | kEventStop,
                    pdFALSE,
                    pdFALSE,
                    pdMS_TO_TICKS(200) // Increased from 50ms for slow wakeup
                );

                if (bits & kEventStop) {
                    ret = ESP_ERR_INVALID_STATE;
                    goto done;
                }

                if (!(bits & kEventActiveState)) {
                    ESP_LOGE(kTag, "TX task: timeout waiting for active state");
                    ret = ESP_ERR_TIMEOUT;
                    goto done;
                }
            }

            // Update activity time
            last_activity_time_us_ = esp_timer_get_time();
            
            // Clear event bits before sending
            //ConfigureSrdyInterrupt(kSrdyInterruptForAck);
            gpio_set_intr_type(config_.srdy_pin, GPIO_INTR_NEGEDGE);
            gpio_intr_enable(config_.srdy_pin);
            xEventGroupClearBits(event_group_, kEventSrdyHigh);

            // Transmit via UHCI (Synchronous FIFO mode)
            ret = uart_uhci_.Transmit(frame.data, frame.length);
            if (ret != ESP_OK) {
                ESP_LOGE(kTag, "TX task: UHCI transmit failed: %s", esp_err_to_name(ret));
                goto done;
            }

            // Wait for ACK (SRDY high)
            {
                EventBits_t bits = xEventGroupWaitBits(
                    event_group_,
                    kEventSrdyHigh | kEventStop,
                    pdTRUE,   // clear on exit
                    pdFALSE,  // wait for any bit
                    pdMS_TO_TICKS(kAckTimeoutMs)
                );

                if (bits & kEventStop) {
                    ret = ESP_ERR_INVALID_STATE;
                    goto done;
                }

                if (!(bits & kEventSrdyHigh)) {
                    // ACK timeout - assume data was received
                    ESP_LOGW(kTag, "TX task: ACK timeout in %ld us", (long)(esp_timer_get_time() - last_activity_time_us_));
                } else if (debug_enabled_.load()) {
                    ESP_LOGI(kTag, "TX task: frame sent, %d bytes, acked in %ld us", 
                             frame.length, (long)(esp_timer_get_time() - last_activity_time_us_));
                }
            }

done:
            ConfigureSrdyInterrupt(kSrdyInterruptForAck);
            // Cleanup and notify
            free(frame.data);
            if (frame.done_sem) {
                if (frame.result) {
                    *frame.result = ret;
                }
                xSemaphoreGive(frame.done_sem);
            }
        }
    }

    ESP_LOGD(kTag, "TX task exiting");
    xEventGroupSetBits(event_group_, kEventTxTaskDone);
}

// Main task: handles all events (using UHCI DMA, no blocking I/O)
void UartEthModem::MainTaskRun() {
    ESP_LOGD(kTag, "Main task started (UHCI DMA mode)");

    // Initialize MRDY to high (not busy, allow slave to sleep)
    SetMrdy(MrdyLevel::High);
    working_state_.store(WorkingState::Idle);

    while (!stop_flag_.load()) {
        // Calculate next timeout based on current state
        TickType_t wait_ticks = CalculateNextTimeout();

        Event event;
        if (xQueueReceive(event_queue_, &event, wait_ticks) == pdTRUE) {
            HandleEvent(event);
        } else {
            // Timeout occurred
            HandleIdleTimeout();
        }
    }

    // Ensure we're in idle state before exiting
    if (working_state_.load() != WorkingState::Idle) {
        EnterIdleState();
    }

    ESP_LOGD(kTag, "Main task exiting");
    xEventGroupSetBits(event_group_, kEventMainTaskDone);
}

// UHCI RX callback static wrapper (called from ISR context)
bool IRAM_ATTR UartEthModem::UhciRxCallbackStatic(const UartUhci::RxEventData& data, void* user_data) {
    auto* self = static_cast<UartEthModem*>(user_data);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Send RxData event to main task with buffer pointer
    Event event = {
        .type = EventType::RxData,
        .rx_buffer = data.buffer,
    };

    xQueueSendFromISR(self->event_queue_, &event, &xHigherPriorityTaskWoken);

    return xHigherPriorityTaskWoken == pdTRUE;
}

// Start continuous DMA receive using buffer pool
void UartEthModem::StartDmaReceive() {
    // Only start DMA receive if in Active or PendingIdle state
    // In Idle state, don't start receive to release UHCI's NO_LIGHT_SLEEP lock
    WorkingState state = working_state_.load();
    if (state == WorkingState::Idle) {
        ESP_LOGD(kTag, "Skipping DMA receive in idle state");
        return;
    }

    // Start continuous receive with buffer pool
    esp_err_t ret = uart_uhci_.StartReceive();
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start DMA receive: %s", esp_err_to_name(ret));
    }
}

// Calculate next timeout based on current working state
TickType_t UartEthModem::CalculateNextTimeout() {
    WorkingState state = working_state_.load();

    if (state == WorkingState::Idle) {
        // In idle state, wait indefinitely for events
        return portMAX_DELAY;
    }

    if (state == WorkingState::PendingActive) {
        // In pending active state, wait for SRDY low with timeout
        return pdMS_TO_TICKS(100);  // Increased from 50ms for slow slave wakeup
    }

    if (state == WorkingState::Active) {
        // In active state, calculate remaining time until idle timeout
        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_us = now_us - last_activity_time_us_;
        int64_t elapsed_ms = elapsed_us / 1000;
        int64_t remaining_ms = kIdleTimeoutMs - elapsed_ms;

        if (remaining_ms <= 0) {
            return 0;  // Already timed out
        }
        return pdMS_TO_TICKS(remaining_ms);
    }

    if (state == WorkingState::PendingIdle) {
        // In pending idle state, check SRDY periodically
        return pdMS_TO_TICKS(10);  // Check every 10ms
    }

    return portMAX_DELAY;
}

// Handle incoming event
void UartEthModem::HandleEvent(const Event& event) {
    switch (event.type) {
        case EventType::TxRequest: {
            // Master wants to send data
            WorkingState state = working_state_.load();
            if (state == WorkingState::Idle || state == WorkingState::PendingIdle) {
                // Need to wake up slave first (or ensure slave stays awake)
                // Always go through PendingActive to avoid race condition:
                // In PendingIdle, MRDY is high. Even if SRDY is low now,
                // slave could see MRDY high and start sleeping before we
                // call EnterActiveState(). So always set MRDY low first,
                // then wait for SRDY low confirmation.
                EnterPendingActiveState();
            }
            // If already Active or PendingActive, nothing to do
            break;
        }

        case EventType::SrdyLow:
            HandleSrdyLow();
            break;

        case EventType::SrdyHigh:
            HandleSrdyHigh();
            break;

        case EventType::RxData:
            HandleRxData(event.rx_buffer);
            break;

        case EventType::Stop:
            stop_flag_.store(true);
            break;

        default:
            break;
    }
}

// Handle data received via DMA buffer pool
void UartEthModem::HandleRxData(UartUhci::RxBuffer* buffer) {
    if (!buffer || buffer->size == 0) {
        if (buffer) {
            uart_uhci_.ReturnBuffer(buffer);
        }
        return;
    }

    // Ensure we're in active state
    if (working_state_.load() != WorkingState::Active) {
        EnterActiveState();
    }

    // Update activity time
    last_activity_time_us_ = esp_timer_get_time();

    uint8_t* data = buffer->data;
    size_t size = buffer->size;
    size_t offset = 0;

    // Process data, potentially multiple frames or partial frames
    while (offset < size) {
        if (reassembly_size_ == 0) {
            // Not currently reassembling, look for new frame header
            size_t remaining = size - offset;
            
            if (remaining < sizeof(FrameHeader)) {
                // Not enough data for header, copy to reassembly buffer
                memcpy(reassembly_buffer_, data + offset, remaining);
                reassembly_size_ = remaining;
                reassembly_expected_ = 0;  // Don't know expected size yet
                break;
            }

            // Parse header
            FrameHeader* header = reinterpret_cast<FrameHeader*>(data + offset);
            if (!header->ValidateChecksum()) {
                // Invalid header - likely end of valid data or corruption
                // Don't scan further, just discard remaining data
                if (debug_enabled_.load()) {
                    ESP_LOGI(kTag, "Invalid checksum at offset %d, discarding %d remaining bytes",
                             offset, size - offset);
                }
                break;
            }

            uint16_t payload_len = header->GetPayloadLength();
            size_t frame_size = sizeof(FrameHeader) + payload_len;

            if (frame_size > kMaxFrameSize) {
                ESP_LOGW(kTag, "Frame too large: %d bytes", frame_size);
                offset++;
                continue;
            }

            if (remaining >= frame_size) {
                // Complete frame available, process directly
                ProcessReceivedFrame(data + offset, frame_size);
                offset += frame_size;
                
                // Send ACK after processing complete frame
                SendAckPulse();
            } else {
                // Partial frame, copy to reassembly buffer
                memcpy(reassembly_buffer_, data + offset, remaining);
                reassembly_size_ = remaining;
                reassembly_expected_ = frame_size;
                break;
            }
        } else {
            // Currently reassembling a frame
            size_t remaining = size - offset;
            
            if (reassembly_expected_ == 0) {
                // Still need to determine frame size (was missing header bytes)
                size_t need_for_header = sizeof(FrameHeader) - reassembly_size_;
                size_t copy_size = std::min(remaining, need_for_header);
                memcpy(reassembly_buffer_ + reassembly_size_, data + offset, copy_size);
                reassembly_size_ += copy_size;
                offset += copy_size;

                if (reassembly_size_ >= sizeof(FrameHeader)) {
                    // Now we have the header
                    FrameHeader* header = reinterpret_cast<FrameHeader*>(reassembly_buffer_);
                    if (!header->ValidateChecksum()) {
                        if (debug_enabled_.load()) {
                            ESP_LOGI(kTag, "Invalid reassembled header checksum, discarding");
                        }
                        reassembly_size_ = 0;
                        reassembly_expected_ = 0;
                        break;  // Stop processing this buffer
                    }
                    uint16_t payload_len = header->GetPayloadLength();
                    reassembly_expected_ = sizeof(FrameHeader) + payload_len;

                    if (reassembly_expected_ > kMaxFrameSize) {
                        ESP_LOGW(kTag, "Reassembled frame too large: %d bytes", reassembly_expected_);
                        reassembly_size_ = 0;
                        reassembly_expected_ = 0;
                        continue;
                    }
                }
                continue;
            }

            // We know the expected size, continue collecting data
            size_t need = reassembly_expected_ - reassembly_size_;
            size_t copy_size = std::min(remaining, need);
            
            if (reassembly_size_ + copy_size > kMaxFrameSize) {
                ESP_LOGW(kTag, "Reassembly buffer overflow");
                reassembly_size_ = 0;
                reassembly_expected_ = 0;
                break;
            }

            memcpy(reassembly_buffer_ + reassembly_size_, data + offset, copy_size);
            reassembly_size_ += copy_size;
            offset += copy_size;

            if (reassembly_size_ >= reassembly_expected_) {
                // Frame complete, process it
                ProcessReceivedFrame(reassembly_buffer_, reassembly_expected_);
                reassembly_size_ = 0;
                reassembly_expected_ = 0;
                
                // Send ACK after processing complete frame
                SendAckPulse();
            }
        }
    }

    // Return buffer to pool
    uart_uhci_.ReturnBuffer(buffer);
}

// Handle SRDY low event: Slave wants to send data or is ready to receive
void UartEthModem::HandleSrdyLow() {
    WorkingState state = working_state_.load();

    if (state == WorkingState::PendingActive) {
        // Slave acknowledged our wakeup, now enter active state
        if (debug_enabled_.load()) {
            ESP_LOGI(kTag, "Slave ready, entering active state");
        }
        EnterActiveState();
    } else if (state == WorkingState::Idle || state == WorkingState::PendingIdle) {
        // Slave is waking us up (slave-initiated), enter active state directly
        if (debug_enabled_.load()) {
            ESP_LOGI(kTag, "Slave wakeup detected, entering active state");
        }
        EnterActiveState();
    }
}

// Handle SRDY high event: Slave ACK or entering sleep
void UartEthModem::HandleSrdyHigh() {
    WorkingState state = working_state_.load();

    if (state == WorkingState::PendingIdle) {
        // Both sides are now idle, enter idle state
        if (debug_enabled_.load()) {
            ESP_LOGI(kTag, "Slave also idle, entering idle state");
        }
        EnterIdleState();  // This will configure interrupt for wakeup
    }
}

// Handle idle timeout
void UartEthModem::HandleIdleTimeout() {
    WorkingState state = working_state_.load();

    if (state == WorkingState::PendingActive) {
        // Timeout waiting for slave to wake up
        // Check if SRDY is already low (we might have missed the interrupt)
        if (IsSrdyLow()) {
            if (debug_enabled_.load()) {
                ESP_LOGI(kTag, "Slave ready (polled), entering active state");
            }
            EnterActiveState();
        } else {
            // Slave not responding, signal timeout but enter active state anyway
            // (the TX will likely fail, but that's handled at higher level)
            ESP_LOGW(kTag, "Slave not responding (SRDY still high), forcing active state");
            EnterActiveState();
        }
    } else if (state == WorkingState::Active) {
        // Timeout in active state, enter pending idle
        if (debug_enabled_.load()) {
            ESP_LOGI(kTag, "Idle timeout, entering pending idle state");
        }
        EnterPendingIdleState();
    } else if (state == WorkingState::PendingIdle) {
        // Check if SRDY is high
        if (!IsSrdyLow()) {
            if (debug_enabled_.load()) {
                ESP_LOGI(kTag, "Slave is idle, entering idle state");
            }
            EnterIdleState();
        }
        // If SRDY is still low, slave has data to send
        // Stay in pending idle and wait for SRDY high event
    }
}

// Enter pending active state: Master initiates wakeup, wait for slave
void UartEthModem::EnterPendingActiveState() {
    WorkingState prev_state = working_state_.load();
    if (prev_state != WorkingState::Idle && prev_state != WorkingState::PendingIdle) {
        return;  // Only valid from Idle or PendingIdle state
    }

    // Set MRDY low first to prevent slave from sleeping
    SetMrdy(MrdyLevel::Low);

    // Check if SRDY is already low (slave still active or already responded)
    // This avoids waiting for an interrupt that will never come
    if (IsSrdyLow()) {
        if (debug_enabled_.load()) {
            ESP_LOGI(kTag, "Slave already ready, entering active state directly");
        }
        // Slave is already ready, go directly to active state
        EnterActiveState();
        return;
    }

    if (debug_enabled_.load()) {
        ESP_LOGI(kTag, "Entering pending active state (waking up slave)");
    }

    // Update state
    working_state_.store(WorkingState::PendingActive);

    // Configure SRDY interrupt to detect slave wakeup (falling edge -> low)
    ConfigureSrdyInterrupt(kSrdyInterruptForAck);
}

// Enter active working state
void UartEthModem::EnterActiveState() {
    WorkingState prev_state = working_state_.load();
    if (prev_state == WorkingState::Active) {
        // Already active, just signal in case someone is waiting
        xEventGroupSetBits(event_group_, kEventActiveState);
        return;
    }

    ESP_LOGD(kTag, "Entering active state");

    // Start DMA receive if not already running
    // (DMA keeps running during PendingIdle, so check before starting)
    if (!uart_uhci_.IsReceiving()) {
        esp_err_t ret = uart_uhci_.StartReceive();
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to start UHCI receive: %s", esp_err_to_name(ret));
        }
    }

    // Set MRDY low (busy) - may already be low from PendingActive
    SetMrdy(MrdyLevel::Low);

    // Update state
    working_state_.store(WorkingState::Active);
    last_activity_time_us_ = esp_timer_get_time();

    // Signal that active state is ready (DMA receive started)
    xEventGroupSetBits(event_group_, kEventActiveState);
}

// Enter pending idle state
void UartEthModem::EnterPendingIdleState() {
    if (working_state_.load() != WorkingState::Active) {
        return;  // Not in active state
    }

    ESP_LOGD(kTag, "Entering pending idle state");

    // Set MRDY high (not busy, allow slave to sleep)
    SetMrdy(MrdyLevel::High);

    // Update state
    working_state_.store(WorkingState::PendingIdle);
}

// Enter idle/sleep state
void UartEthModem::EnterIdleState() {
    ESP_LOGD(kTag, "Entering idle state");

    // Clear active state bit (DMA will be stopped)
    xEventGroupClearBits(event_group_, kEventActiveState);

    // Ensure MRDY is high
    SetMrdy(MrdyLevel::High);

    // Update state
    working_state_.store(WorkingState::Idle);

    // Stop DMA receive to release UHCI's NO_LIGHT_SLEEP lock
    uart_uhci_.StopReceive();

    // Configure SRDY interrupt for wakeup (low level trigger)
    ConfigureSrdyInterrupt(kSrdyInterruptForWakeup);
}

void UartEthModem::InitTaskRun() {
    ESP_LOGD(kTag, "Init task started");

    // Wait for start signal
    xEventGroupWaitBits(event_group_, kEventStart, pdTRUE, pdTRUE, portMAX_DELAY);

    if (stop_flag_.load()) {
        goto exit;
    }

    // Run initialization sequence based on mode
    {
        esp_err_t init_ret = flight_mode_ ? RunFlightModeInitSequence() : RunNormalModeInitSequence();
        if (init_ret != ESP_OK) {
            ESP_LOGE(kTag, "Initialization sequence failed");
            stop_flag_ = true;
            initializing_ = false;
            xEventGroupSetBits(event_group_, kEventStop);
            goto exit;
        }

        // Initialize iot_eth (skip in flight mode - no network needed)
        if (!flight_mode_) {
            if (InitIotEth() != ESP_OK) {
                ESP_LOGE(kTag, "Failed to initialize iot_eth");
                stop_flag_ = true;
                initializing_ = false;
                xEventGroupSetBits(event_group_, kEventStop);
                goto exit;
            }
        }

        ESP_LOGD(kTag, "Initialization complete");
        initializing_ = false;
        xEventGroupSetBits(event_group_, kEventInitDone);
    }

exit:
    ESP_LOGD(kTag, "Init task exiting");
    xEventGroupSetBits(event_group_, kEventInitTaskDone);
}

// Enqueue TX frame for non-blocking transmission
esp_err_t UartEthModem::EnqueueTxFrame(const uint8_t* buf, size_t len) {
    // Allocate frame with header (DMA compatible memory)
    size_t total_len = sizeof(FrameHeader) + len;
    uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(total_len, MALLOC_CAP_DMA));
    if (!buffer) {
        ESP_LOGE(kTag, "Failed to allocate TX buffer for queue");
        return ESP_ERR_NO_MEM;
    }

    // Build header
    FrameHeader* header = reinterpret_cast<FrameHeader*>(buffer);
    *reinterpret_cast<uint32_t*>(header->raw) = 0;
    header->SetPayloadLength(len);
    header->SetSequence(seq_no_++);
    header->SetFlowControl(false);  // XON = 0 (permit to send)
    header->SetType(FrameType::kEthernet);
    header->UpdateChecksum();

    // Copy payload
    memcpy(buffer + sizeof(FrameHeader), buf, len);

    // Enqueue frame (non-blocking, no completion notification)
    TxFrame frame = {
        .data = buffer, 
        .length = total_len,
        .done_sem = nullptr,
        .result = nullptr
    };
    if (xQueueSend(tx_queue_, &frame, 0) != pdTRUE) {
        ESP_LOGW(kTag, "TX queue full, dropping frame");
        free(buffer);
        return ESP_ERR_NO_MEM;  // Queue full
    }

    return ESP_OK;
}

// Send frame (public interface): enqueue and wait for completion
// All transmission goes through TxTaskRun to avoid resource contention.
esp_err_t UartEthModem::SendFrame(const uint8_t* data, size_t length, FrameType type) {
    // Allocate frame with header (DMA compatible memory)
    size_t total_len = sizeof(FrameHeader) + length;
    uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(total_len, MALLOC_CAP_DMA));
    if (!buffer) {
        ESP_LOGE(kTag, "Failed to allocate TX buffer");
        return ESP_ERR_NO_MEM;
    }

    // Build header
    FrameHeader* header = reinterpret_cast<FrameHeader*>(buffer);
    *reinterpret_cast<uint32_t*>(header->raw) = 0;
    header->SetPayloadLength(length);
    header->SetSequence(seq_no_++);
    header->SetFlowControl(false);  // XON = 0 (permit to send), XOFF = 1 (shall not send)
    header->SetType(type);
    header->UpdateChecksum();

    // Copy payload
    memcpy(buffer + sizeof(FrameHeader), data, length);

    // Create binary semaphore for synchronous wait
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) {
        ESP_LOGE(kTag, "Failed to create semaphore");
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    // Prepare frame with completion notification
    esp_err_t result = ESP_OK;
    TxFrame frame = {
        .data = buffer,
        .length = total_len,
        .done_sem = done_sem,
        .result = &result
    };

    // Enqueue frame (block for a short time if queue is full)
    if (xQueueSend(tx_queue_, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(kTag, "TX queue full, cannot send frame");
        vSemaphoreDelete(done_sem);
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    // Wait for transmission to complete (with timeout)
    // We wait long enough for TxTaskRun to finish its own internal timeouts (up to 1s for TX, 200ms for active state)
    if (xSemaphoreTake(done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(kTag, "SendFrame timeout waiting for completion");
        // WARNING: If we delete the semaphore here, TxTaskRun might still try to use it later,
        // causing a crash. However, after 2 seconds, it's very likely TxTaskRun has already
        // finished or timed out itself.
        vSemaphoreDelete(done_sem);
        // Note: buffer is freed by TxTaskRun, don't free here
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(done_sem);
    return result;
}

// Process frame data received via DMA
void UartEthModem::ProcessReceivedFrame(uint8_t* data, size_t size) {
    if (size < sizeof(FrameHeader)) {
        if (debug_enabled_.load() && size > 0) {
            ESP_LOGI(kTag, "Not enough data for header, size: %d", size);
        }
        return;
    }

    // Parse header from DMA buffer
    FrameHeader* header = reinterpret_cast<FrameHeader*>(data);

    // Validate checksum
    if (!header->ValidateChecksum()) {
        ESP_LOGW(kTag, "Invalid checksum, raw: %02x %02x %02x %02x", 
                 header->raw[0], header->raw[1], header->raw[2], header->raw[3]);
        return;
    }

    uint16_t payload_len = header->GetPayloadLength();
    if (sizeof(FrameHeader) + payload_len > size) {
        ESP_LOGW(kTag, "Incomplete frame: expected %d, got %d", 
                 sizeof(FrameHeader) + payload_len, size);
        return;
    }

    if (debug_enabled_.load()) {
        ESP_LOGI(kTag, "RX frame: type=%d, len=%d, seq=%d", 
                 static_cast<int>(header->GetType()), payload_len, header->GetSequence());
    }

    // Get payload pointer (data is in DMA buffer, need to copy for ownership)
    uint8_t* payload = data + sizeof(FrameHeader);

    // Handle frame based on type
    if (header->GetType() == FrameType::kEthernet) {
        // For Ethernet frames, we need to copy data since HandleEthFrame takes ownership
        uint8_t* payload_copy = static_cast<uint8_t*>(malloc(payload_len));
        if (payload_copy) {
            memcpy(payload_copy, payload, payload_len);
            HandleEthFrame(payload_copy, payload_len);
        } else {
            ESP_LOGE(kTag, "Failed to allocate RX buffer");
        }
    } else if (header->GetType() == FrameType::kAtCommand) {
        // For AT responses, we can use the data in place (it will be copied in HandleAtResponse)
        HandleAtResponse(reinterpret_cast<char*>(payload), payload_len);
    }
}

void UartEthModem::HandleEthFrame(uint8_t* data, size_t length) {
    if (!handshake_done_.load()) {
        // Check for handshake ACK
        if (length >= sizeof(kHandshakeAck) && memcmp(data, kHandshakeAck, sizeof(kHandshakeAck)) == 0) {
            ESP_LOGD(kTag, "Handshake ACK received");
            handshake_done_ = true;
            xEventGroupSetBits(event_group_, kEventHandshakeDone);
            // Mark as initialized, but wait for IP_EVENT_ETH_GOT_IP for network ready
            initialized_ = true;
        }
        free(data);
        return;
    }

    // Forward to iot_eth stack
    if (mediator_) {
        // Note: mediator->stack_input takes ownership of data (frees it)
        mediator_->stack_input(mediator_, data, length);
    } else {
        free(data);
    }
}

void UartEthModem::HandleAtResponse(const char* data, size_t length) {
    std::string response(data, length);

    // Parse URC or response
    ParseAtResponse(response);

    // Only signal completion for final AT responses (OK/ERROR), not for URCs like ECRDY
    if (waiting_for_at_response_ &&
        (response.find("OK") != std::string::npos || response.find("ERROR") != std::string::npos)) {
        at_command_response_ = response;
        xEventGroupSetBits(event_group_, kEventAtResponse);
    }
}

esp_err_t UartEthModem::SendAtWithRetry(const std::string& cmd, std::string& response, uint32_t timeout_ms, int max_retries) {
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < max_retries; i++) {
        // Check stop flag before each retry
        if (stop_flag_.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        ret = SendAt(cmd, response, timeout_ms);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (i < max_retries - 1) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    return ret;
}

void UartEthModem::ParseAtResponse(const std::string& response) {
    if (debug_enabled_.load()) {
        ESP_LOGI(kTag, "AT<<< %s", response.c_str());
    }

    // Parse CEREG following at_modem.cc logic
    auto cereg_pos = response.find("+CEREG:");
    if (cereg_pos != std::string::npos) {
        int n = 0, stat = 0;
        char tac[16] = {0}, ci[16] = {0};
        int act = 0;
        
        // Try format 1: +CEREG: n,stat,"tac","ci",act (with n parameter)
        if (sscanf(response.c_str() + cereg_pos, "+CEREG: %d,%d,\"%15[^\"]\",\"%15[^\"]\",%d", &n, &stat, tac, ci, &act) == 5) {
            cell_info_.stat = stat;
            cell_info_.tac = tac;
            cell_info_.ci = ci;
            cell_info_.act = act;
        }
        // Try format 2: +CEREG: stat,"tac","ci",act (without n parameter)
        else if (sscanf(response.c_str() + cereg_pos, "+CEREG: %d,\"%15[^\"]\",\"%15[^\"]\",%d", &stat, tac, ci, &act) == 4) {
            cell_info_.stat = stat;
            cell_info_.tac = tac;
            cell_info_.ci = ci;
            cell_info_.act = act;
        }
        // Try format 3: +CEREG: n,stat (unsolicited with n)
        else if (sscanf(response.c_str() + cereg_pos, "+CEREG: %d,%d", &n, &stat) == 2) {
            cell_info_.stat = stat;
        }
        // Try format 4: +CEREG: stat (query response)
        else if (sscanf(response.c_str() + cereg_pos, "+CEREG: %d", &stat) == 1) {
            cell_info_.stat = stat;
        }

        // Check registration status (mirrored from at_modem.cc HandleUrc logic)
        bool new_network_ready = cell_info_.stat == 1 || cell_info_.stat == 5;
        if (cell_info_.stat == 2) {
            SetNetworkEvent(UartEthModemEvent::Connecting);
        } else if (cell_info_.stat == 3) {
            SetNetworkEvent(UartEthModemEvent::ErrorRegistrationDenied);
        } else if (new_network_ready) {
            if (initialized_.load()) {
                SetNetworkEvent(UartEthModemEvent::Connected);
            }
        }
    } else if (response.find("+ECNETDEVCTL: 1") != std::string::npos) {
        // Network device ready (link up)
    } else if (response.find("+ECNETDEVCTL: 0") != std::string::npos) {
        // Network device down (link down)
    }
}

esp_err_t UartEthModem::AtDetect() {
    std::string resp;
    esp_err_t ret;
    int baud_rates[] = {2000000, 3000000};
    ret = SendAtWithRetry("AT", resp, 500, 4);
    if (ret == ESP_OK) {
        detect_baud_rate_ = config_.baud_rate;
        return ESP_OK;
    }
    for (size_t i = 0; i < sizeof(baud_rates) / sizeof(baud_rates[0]); i++){
        uart_set_baudrate(config_.uart_num, baud_rates[i]);
        ESP_LOGI(kTag, "Trying baud rate: %d", baud_rates[i]);
        ret = SendAtWithRetry("AT", resp, 500, 4);
        if (ret == ESP_OK) {
            detect_baud_rate_ = baud_rates[i];
            ESP_LOGI(kTag, "Detected baud rate: %d", detect_baud_rate_);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t UartEthModem::ConfigurePdp() {
    std::string resp;
    esp_err_t ret;

    // Notify upper layer that we are about to configure PDP context. If the
    // event callback runs synchronously, it may call SetPdpContext() to inject
    // apn_/pdp_type_ before we read them below.
    SetNetworkEvent(UartEthModemEvent::RequestingPdpContext);

    if (apn_.empty()) {
        ESP_LOGI(kTag, "APN not set, using default");
        return ESP_OK;
    }
    ret = SendAt("AT+CGDCONT?", resp, 1000);
    if (ret == ESP_OK) {
        // Use trailing comma to avoid matching CIDs like 11, 10, etc.
        auto cgcont_pos = resp.find("+CGDCONT: 1,");
        if (cgcont_pos != std::string::npos) {
            char pdp_type[16] = {0};
            char apn[128] = {0};
            if (sscanf(resp.c_str() + cgcont_pos,
                       "+CGDCONT: 1,\"%15[^\"]\",\"%127[^\"]\"",
                       pdp_type, apn) == 2) {
                if (std::strcmp(apn, apn_.c_str()) == 0 &&
                    std::strcmp(pdp_type, pdp_type_.c_str()) == 0) {
                    ESP_LOGI(kTag, "APN already set: %s (%s)", apn, pdp_type);
                    return ESP_OK;
                }
            }
        }
        ESP_LOGI(kTag, "setting APN: %s, PDP Type: %s", apn_.c_str(), pdp_type_.c_str());
        esp_err_t cfun0_ret = SendAt("AT+CFUN=0", resp, 5000);
        if (cfun0_ret != ESP_OK) {
            ESP_LOGW(kTag, "CFUN=0 failed: %s", esp_err_to_name(cfun0_ret));
        }
        // 3GPP TS 27.007 requires PDP_type and APN to be quoted strings.
        esp_err_t cgd_ret = SendAt(
            "AT+CGDCONT=1,\"" + pdp_type_ + "\",\"" + apn_ + "\"", resp);
        if (cgd_ret != ESP_OK) {
            ESP_LOGW(kTag, "CGDCONT failed: %s", esp_err_to_name(cgd_ret));
        }
        ret = SendAt("AT+CFUN=1", resp, 5000);
    }
    return ret;
}

esp_err_t UartEthModem::RunFlightModeInitSequence() {
    std::string resp;
    esp_err_t ret;

    // Step 1: AT test
    ESP_LOGI(kTag, "Detecting modem...");
    ret = AtDetect();
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Modem not detected");
        SetNetworkEvent(UartEthModemEvent::ErrorInitFailed);
        return ret;
    }

    // Enter flight mode (CFUN=4)
    ret = SendAt("AT+CFUN=4", resp, 3000);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to enter flight mode");
        SetNetworkEvent(UartEthModemEvent::ErrorInitFailed);
        return ret;
    }

    ESP_LOGI(kTag, "Checking SIM card...");
    if (!CheckSimCard()) {
        // 没插 SIM 时不要 fail-fast。原行为是把 ErrorNoSim 抛上去让上层
        // Nt26Board 调 ScheduleAsyncStop()，结果 modem 整体被关掉，AT
        // 通道也没了 —— 用户没法通过 AT+ECSIMCFG=SimSlot,X 切到另一张
        // 卡。这里保留 AT 通道处于活动态，模组留在 CFUN=4 即可。
        ESP_LOGW(kTag, "SIM not ready, keep AT channel alive (no network)");
    } else {
        ESP_LOGI(kTag, "Querying modem info...");
        QueryModemInfo();
    }

    ESP_LOGI(kTag, "Flight mode initialization complete");
    initialized_ = true;
    SetNetworkEvent(UartEthModemEvent::InFlightMode);
    return ESP_OK;
}

esp_err_t UartEthModem::RunNormalModeInitSequence() {
    std::string resp;
    esp_err_t ret;
    bool modem_need_reset = false;

    // Step 1: AT test
    ESP_LOGI(kTag, "Detecting modem...");
    ret = AtDetect();
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Modem not detected");
        SetNetworkEvent(UartEthModemEvent::ErrorInitFailed);
        return ret;
    }

    const bool baud_changed = (detect_baud_rate_ != config_.baud_rate);

    // Check and configure network settings
    ESP_LOGI(kTag, "Checking network configuration...");
    ret = SendAt("AT+ECNETCFG?", resp, 1000);
    if (ret != ESP_OK || resp.find("+ECNETCFG: \"nat\",1") == std::string::npos) {
        // First-time configuration
        ESP_LOGI(kTag, "Configuring network (first-time setup)...");
        SendAtWithRetry("AT+ECPCFG=\"usbCtrl\",1", resp, 1000, 3);
        SendAtWithRetry("AT+ECNETCFG=\"nat\",1,\"192.168.10.2\"", resp, 1000, 3);
        modem_need_reset = true;
    }

    if (baud_changed) {
        ESP_LOGI(kTag, "Setting baud rate to configured value: %d", config_.baud_rate);
        SendAt("AT+XJCFG=netPortBaudRate," + std::to_string(config_.baud_rate), resp);
        modem_need_reset = true;
    }

    if (modem_need_reset) {
        modem_need_reset = false;
        // Reset after configuration
        xEventGroupClearBits(event_group_, kEventNetworkEventChanged);

        SendAt("AT+ECRST", resp, 500);
        if (baud_changed) {
            uart_set_baudrate(config_.uart_num, config_.baud_rate);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
        // Wait for modem to respond
        ret = SendAtWithRetry("AT", resp, 500, 20);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Modem not responding after reset");
            SetNetworkEvent(UartEthModemEvent::ErrorInitFailed);
            return ret;
        }
    }

    // Enter full functionality mode (CFUN=1)
    ret = SendAt("AT+CFUN=1", resp, 3000);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to enter full functionality mode");
        SetNetworkEvent(UartEthModemEvent::ErrorInitFailed);
        return ret;
    }

    ESP_LOGI(kTag, "Checking SIM card...");
    if (!CheckSimCard()) {
        // 没插 SIM 时不要 fail-fast。原行为是抛 ErrorNoSim 让 Nt26Board
        // ScheduleAsyncStop() 把整个 modem 关掉，结果 AT 通道也被一起拆
        // 了，用户没法用 AT+ECSIMCFG=SimSlot,X 再切到另一张卡。这里保
        // 留 modem 在 CFUN=1 状态：网络注册 / PDP / netdev 跳过，但 AT
        // 通道继续可用。
        ESP_LOGW(kTag, "SIM not ready, keep AT channel alive (no network)");
        QueryModemInfo();
        initialized_ = true;
        SetNetworkEvent(UartEthModemEvent::InFlightMode);
        return ESP_OK;
    }

    ESP_LOGI(kTag, "Querying modem info...");
    QueryModemInfo();

    ConfigurePdp();

    ESP_LOGI(kTag, "Waiting for network registration...");
    SetNetworkEvent(UartEthModemEvent::Connecting);

    // Enable CEREG URC
    SendAt("AT+CEREG=2", resp);
    if (!WaitForRegistration(60000)) {
        if (cell_info_.stat == 3) {
            ESP_LOGE(kTag, "Registration denied");
            SetNetworkEvent(UartEthModemEvent::ErrorRegistrationDenied);
        } else {
            ESP_LOGE(kTag, "Registration timeout");
            SetNetworkEvent(UartEthModemEvent::ErrorInitFailed);
        }
        return ESP_ERR_TIMEOUT;
    }

    int state=0;
    if (SendAt("AT+ECNETDEVCTL?", resp, 1000) == ESP_OK) {
        sscanf(resp.c_str(), "\r\n+ECNETDEVCTL: %*d,%*d,%*d,%d", &state);
    }
    if(state == 1){
        ESP_LOGI(kTag, "Network device already started");
        handshake_done_ = true;
        xEventGroupSetBits(event_group_, kEventHandshakeDone);
        // Mark as initialized, but wait for IP_EVENT_ETH_GOT_IP for network ready
        initialized_ = true;
    } else {
        ESP_LOGI(kTag, "Starting network device...");
        ret = SendAt("AT+ECNETDEVCTL=2,1,1", resp, 5000);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to start network device");
            SetNetworkEvent(UartEthModemEvent::ErrorInitFailed);
            return ret;
        }
        // Send handshake request
        ESP_LOGI(kTag, "Starting handshake...");
        ret = SendFrame(kHandshakeRequest, sizeof(kHandshakeRequest), FrameType::kEthernet);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Handshake failed");
            SetNetworkEvent(UartEthModemEvent::ErrorInitFailed);
            return ret;
        }
    }

    // Wait for handshake ACK
    auto bits = xEventGroupWaitBits(event_group_, kEventHandshakeDone | kEventStop, pdFALSE, pdFALSE, pdMS_TO_TICKS(kHandshakeTimeoutMs));
    if (bits & kEventStop) {
        ESP_LOGW(kTag, "Stop event received in WaitForHandshake");
        return ESP_ERR_INVALID_STATE;
    } else if (bits & kEventHandshakeDone) {
        ESP_LOGI(kTag, "Handshake successful");
    } else {
        ESP_LOGE(kTag, "Handshake timeout");
        SetNetworkEvent(UartEthModemEvent::ErrorInitFailed);
        return ESP_ERR_TIMEOUT;
    }

    // Set modem sleep parameters
    ret = SendAt("AT+ECSCLKEX=1," + std::to_string(kModemSleepTimeoutS) + ",30", resp, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to set modem sleep parameters");
        SetNetworkEvent(UartEthModemEvent::ErrorInitFailed);
        return ret;
    }

    ESP_LOGI(kTag, "Modem initialization complete!");
    return ESP_OK;
}


bool UartEthModem::CheckSimCard() {
    std::string resp;
    for (int i = 0; i < 10; i++) {
        // Check stop flag before each iteration
        if (stop_flag_.load()) {
            ESP_LOGW(kTag, "CheckSimCard aborted due to stop flag");
            return false;
        }
        if (SendAt("AT+CPIN?", resp, 1000) == ESP_OK) {
            if (resp.find("+CPIN: READY") != std::string::npos) {
                return true;
            }
        }
        if (resp.find("+CME ERROR: 10") != std::string::npos) {
            // SIM not inserted
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool UartEthModem::WaitForRegistration(uint32_t timeout_ms) {
    std::string resp;
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (!stop_flag_.load()) {
        if (SendAt("AT+CEREG?", resp, 1000) == ESP_OK) {
            ParseAtResponse(resp);
            if (cell_info_.stat == 1 || cell_info_.stat == 5) {
                return true;
            }
            if (cell_info_.stat == 3) {
                return false;  // Registration denied
            }
        }

        uint32_t elapsed = xTaskGetTickCount() * portTICK_PERIOD_MS - start;
        if (elapsed >= timeout_ms) {
            return false;
        }

        // Log progress
        if ((elapsed / 1000) % 10 == 0) {
            ESP_LOGI(kTag, "Waiting for registration... (%lu/%lu ms)", elapsed, timeout_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

void UartEthModem::QueryModemInfo() {
    GetImei();
    GetIccid();
    GetModuleRevision();
    GetImsi();
    ESP_LOGD(kTag, "Modem Info - IMEI: %s, ICCID: %s, Rev: %s", imei_.c_str(), iccid_.c_str(), module_revision_.c_str());
}

// Set MRDY level
void UartEthModem::SetMrdy(MrdyLevel level) {
    bool is_low = (level == MrdyLevel::Low);
    gpio_set_level(config_.mrdy_pin, is_low ? 0 : 1);
    mrdy_is_low_.store(is_low);
}

// Check if SRDY is low (slave is busy/has data)
bool UartEthModem::IsSrdyLow() {
    return gpio_get_level(config_.srdy_pin) == 0;
}

// Send ACK pulse: MRDY high for 50us
void UartEthModem::SendAckPulse() {
    // MRDY: low -> high (50us) -> low
    SetMrdy(MrdyLevel::High);  // High
    esp_rom_delay_us(kAckPulseUs);
    SetMrdy(MrdyLevel::Low);   // Low (back to busy)
}

// Configure SRDY interrupt type
// for_wakeup: true = LOW_LEVEL (for light sleep wakeup), false = edge detection
void UartEthModem::ConfigureSrdyInterrupt(bool for_wakeup) {
    if (for_wakeup) {
        // LOW_LEVEL trigger for light sleep wakeup
        gpio_set_intr_type(config_.srdy_pin, GPIO_INTR_LOW_LEVEL);
    } else {
        // Use POSEDGE to detect SRDY going high (slave ACK or entering sleep)
        gpio_set_intr_type(config_.srdy_pin, GPIO_INTR_ANYEDGE);
    }
    gpio_intr_enable(config_.srdy_pin);
}

// ISR handler for SRDY pin changes
// NOTE: Must use gpio_ll_* functions here for IRAM safety during Flash writes
void IRAM_ATTR UartEthModem::SrdyIsrHandler(void* arg) {
    auto* self = static_cast<UartEthModem*>(arg);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Disable interrupt to avoid repeated triggers (IRAM-safe)
    gpio_ll_intr_disable(&GPIO, self->config_.srdy_pin);

    // Determine event type based on current SRDY level (IRAM-safe)
    int level = gpio_ll_get_level(&GPIO, self->config_.srdy_pin);

    // Send event to queue for state machine processing
    Event event = {
        .type = (level == 0) ? EventType::SrdyLow : EventType::SrdyHigh,
        .rx_buffer = nullptr,
    };

    xQueueSendFromISR(self->event_queue_, &event, &xHigherPriorityTaskWoken);

    // Set event group bit for SRDY high (used by WaitForSrdyAck)
    xEventGroupSetBitsFromISR(self->event_group_, kEventSrdyHigh, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void UartEthModem::SetNetworkEvent(UartEthModemEvent event) {
    // Always update the event value
    UartEthModemEvent old_event = network_event_.exchange(event);
    // Set event bit to notify waiting tasks
    if (event_group_) {
        xEventGroupSetBits(event_group_, kEventNetworkEventChanged);
    }
    
    if (old_event != event) {
        ESP_LOGI(kTag, "Network event: %s -> %s", GetNetworkEventName(old_event), GetNetworkEventName(event));
        if (network_event_callback_) {
            network_event_callback_(event);
        }
    }
}

void UartEthModem::IpEventHandler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data) {
    auto* self = static_cast<UartEthModem*>(arg);
    
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        // Network is ready now
        self->SetNetworkEvent(UartEthModemEvent::Connected);
        if (self->event_group_) {
            xEventGroupSetBits(self->event_group_, kEventNetworkReady);
        }
    }
}

void UartEthModem::CleanupResources(bool cleanup_iot_eth) {
    // Cleanup iot_eth if requested
    if (cleanup_iot_eth) {
        DeinitIotEth();
    }

    // Cleanup UHCI controller
    uart_uhci_.Deinit();

    // Cleanup reassembly buffer
    if (reassembly_buffer_) {
        free(reassembly_buffer_);
        reassembly_buffer_ = nullptr;
    }
    reassembly_size_ = 0;
    reassembly_expected_ = 0;

    // Cleanup TX queue (free any pending frames)
    if (tx_queue_) {
        TxFrame frame;
        while (xQueueReceive(tx_queue_, &frame, 0) == pdTRUE) {
            if (frame.data) {
                free(frame.data);
            }
        }
        vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
    }

    // Cleanup event queue
    if (event_queue_) {
        Event event;
        while (xQueueReceive(event_queue_, &event, 0) == pdTRUE) {
            // No dynamic data in events now
        }
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }

    // Cleanup GPIO and UART
    DeinitGpio();
    DeinitUart();
}