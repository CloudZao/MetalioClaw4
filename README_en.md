# MetalioClaw4

[中文](README.md) | **English**

**MetalioClaw4** is firmware for a 3.95-inch (720×720) touch AI voice interaction device based on **ESP32-P4**, built for the **[xingzhi-395](main/boards/xingzhi-395/)** development board.

The project is customized for MetalioClaw4 hardware on top of the [Xiaozhi AI](https://github.com/78/xiaozhi-esp32) architecture. Board support code lives in `main/boards/xingzhi-395/`.

---

## Quick Start

### Requirements

| Item | Details |
|:---|:---|
| **ESP-IDF** | **v5.5.4** (required; matches the `sdkconfig` in this repo) |
| **Target SoC** | ESP32-P4 (preconfigured; **no** manual `set-target` needed) |
| **Board** | MetalioClaw4 (board directory: `main/boards/xingzhi-395/`) |

### Install ESP-IDF

```bash
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32p4
. ./export.sh
```

### Build and Flash

```bash
git clone <this-repo-url>
cd MetalioClaw4

# Build right after cloning — no idf.py set-target esp32p4 required
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

> **Note:** This repo ships with a `sdkconfig` preconfigured for ESP32-P4 and MetalioClaw4 (xingzhi-395). Run `idf.py build` after your first clone. You only need `idf.py set-target esp32p4` again if you delete `sdkconfig` or switch to a different chip.

Replace `/dev/ttyUSB0` with your actual serial port (e.g. `COM3` on Windows).

---

## Hardware (MetalioClaw4 / xingzhi-395)

| Item | Details |
|:---|:---|
| **SoC** | ESP32-P4 |
| **Display** | 3.95" MIPI DSI, 720×720 touch |
| **Audio** | I2S microphone + speaker, offline wake word (ESP-SR) |
| **Connectivity** | Wi-Fi / 4G (switchable) |
| **Bluetooth** | External BT audio module (UART AT commands) |
| **Peripherals** | SD card, battery gauge, IO expander, GPS, vibration motor, etc. |

Pin definitions and display settings: `main/boards/xingzhi-395/config.h`.

---

## Features

- **Voice chat:** Offline wake word + streaming ASR / LLM / TTS; cloud via WebSocket or MQTT+UDP
- **720×720 touch UI:** LVGL 9 multi-app desktop (chat, weather, GPS, camera, music, OpenClaw, etc.)
- **Dual connectivity:** Switchable Wi-Fi / 4G (ML307)
- **Bluetooth audio:** UART-controlled external BT module; pairing and speaker mode supported

---

## Bluetooth Usage

The device sends AT commands over UART to an external Bluetooth audio chip. The ESP32 acts as the host and switches the module between operating modes.

| Mode | Purpose | How to enter |
|:---:|:---|:---|
| **Mode 1** | Default at boot; standby | Auto on boot / leave Music screen |
| **Mode 2** | BT setup: scan and pair headphones/speakers | Home → "Bluetooth Settings" |
| **Mode 3** | Music screen: phone connects to device as speaker | Home → "Music" |

- **Stream music from your phone** → open "Music"
- **Pair headphones/speakers** → "Bluetooth Settings" → Mode 2 → Scan → Connect

See [docs/bluetooth-mode.md](docs/bluetooth-mode.md) for details.

---

## Documentation

| Document | Description |
|:---|:---|
| [docs/bluetooth-mode.md](docs/bluetooth-mode.md) | Bluetooth modes and when they switch |
| [docs/openclaw-api.md](docs/openclaw-api.md) | OpenClaw HTTP API (backend integration) |
| [docs/websocket.md](docs/websocket.md) | WebSocket protocol |
| [docs/mcp-protocol.md](docs/mcp-protocol.md) | Device-side MCP protocol |
| [docs/mqtt-udp.md](docs/mqtt-udp.md) | MQTT + UDP communication |
| [docs/custom-board.md](docs/custom-board.md) | Custom board guide |

---

## For Developers

### Directory Layout

```
main/
├── application.cc              # Boot, state machine, protocols
├── boards/xingzhi-395/         # MetalioClaw4 board init
│   ├── config.h                # Pins, display params
│   ├── config.json             # Build config (name: MetalioClaw4)
│   └── xingzhi-395.cc          # Board entry point
├── display/screen/             # App screens
├── audio/                      # Record, playback, wake word
└── protocols/                  # WebSocket / MQTT, etc.
```
