# MetalioClaw4

**[中文](README.md)** | [English](README_en.md)

**MetalioClaw4** 是基于 **ESP32-P4** 的 3.95 寸（720×720）触控 AI 语音交互设备固件，对应开发板 **[xingzhi-395](main/boards/xingzhi-395/)**。

工程在 [小智 AI](https://github.com/78/xiaozhi-esp32) 架构上针对 MetalioClaw4 硬件定制，板级代码位于 `main/boards/xingzhi-395/`。

---

## 快速开始

### 环境要求

| 项 | 说明 |
|:---|:---|
| **ESP-IDF** | **v5.5.4**（必须使用此版本，与仓库内 `sdkconfig` 一致） |
| **目标芯片** | ESP32-P4（已预配置，**无需**手动 `set-target`） |
| **开发板** | MetalioClaw4（板级目录 `main/boards/xingzhi-395/`） |

### 安装 ESP-IDF

```bash
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32p4
. ./export.sh
```

### 编译与烧录

```bash
git clone <本仓库地址>
cd MetalioClaw4

# 拉取代码后直接编译，无需 idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

> **说明**：仓库已包含针对 ESP32-P4 与 MetalioClaw4（xingzhi-395）的 `sdkconfig`，首次克隆后执行 `idf.py build` 即可。仅当你清除了 `sdkconfig` 或切换芯片时，才需要重新 `idf.py set-target esp32p4`。

将 `/dev/ttyUSB0` 替换为实际串口。Windows 下通常为 `COM3` 等。

---

## 硬件规格（MetalioClaw4 / xingzhi-395）

| 项 | 说明 |
|:---|:---|
| **主控** | ESP32-P4 |
| **屏幕** | 3.95 寸 MIPI DSI，720×720 触控 |
| **音频** | I2S 麦克风 + 扬声器，离线唤醒（ESP-SR） |
| **网络** | Wi-Fi / 4G 可切换 |
| **蓝牙** | 独立蓝牙音频模块（UART AT 控制） |
| **外设** | SD 卡、电池电量、IO 扩展、GPS、振动等 |

板级引脚与屏参见 `main/boards/xingzhi-395/config.h`。

---

## 主要功能

- **语音对话**：离线唤醒 + 流式 ASR / LLM / TTS，支持 WebSocket 或 MQTT+UDP 与云端通信
- **720×720 触控 UI**：LVGL 9 多应用桌面（聊天、天气、GPS、相机、音乐、OpenClaw 等）
- **双网连接**：Wi-Fi / 4G（ML307）可切换
- **蓝牙音频**：经 UART 控制独立蓝牙模块，支持配对与蓝牙音箱模式

---

## 蓝牙使用

设备通过 UART 向独立蓝牙音频芯片发送 AT 指令，ESP32 作为主控切换模块工作模式。

| 模式 | 用途 | 如何进入 |
|:---:|:---|:---|
| **模式 1** | 开机默认，日常待机 | 开机自动 / 离开音乐页 |
| **模式 2** | 蓝牙配置：扫描、配对耳机/音箱 | 首页「蓝牙配置」 |
| **模式 3** | 音乐页：手机连本机当音箱 | 首页「音乐」 |

- **用手机连设备放歌** → 打开「音乐」
- **配对耳机/音箱** → 「蓝牙配置」→ 模式 2 → 扫描 → 连接

详细说明见 [docs/bluetooth-mode.md](docs/bluetooth-mode.md)。

---

## 相关文档

| 文档 | 说明 |
|:---|:---|
| [docs/bluetooth-mode.md](docs/bluetooth-mode.md) | 蓝牙三种模式与切换时机 |
| [docs/openclaw-api.md](docs/openclaw-api.md) | OpenClaw HTTP 接口（后端联调） |
| [docs/websocket.md](docs/websocket.md) | WebSocket 通信协议 |
| [docs/mcp-protocol.md](docs/mcp-protocol.md) | 设备端 MCP 协议 |
| [docs/mqtt-udp.md](docs/mqtt-udp.md) | MQTT + UDP 通信 |
| [docs/custom-board.md](docs/custom-board.md) | 自定义开发板指南 |

---

## 开发者说明

### 目录结构

```
main/
├── application.cc              # 启动、状态机、协议
├── boards/xingzhi-395/         # MetalioClaw4 板级初始化
│   ├── config.h                # 引脚、屏参
│   ├── config.json             # 构建配置（name: MetalioClaw4）
│   └── xingzhi-395.cc          # 板级启动入口
├── display/screen/             # 各功能页面
├── audio/                      # 录音、播放、唤醒词
└── protocols/                  # WebSocket / MQTT 等
```

