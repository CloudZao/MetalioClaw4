# Metalio Claw4 Product Wiki

[中文](README.md) | **English**

**Quick Links**

- Firmware repo: [CloudZao/MetalioClaw4](https://github.com/CloudZao/MetalioClaw4)
- Upstream architecture: [Xiaozhi AI (xiaozhi-esp32)](https://github.com/78/xiaozhi-esp32)
- ESP-IDF docs: [ESP32-P4 Getting Started v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/get-started/index.html)

---

## Table of Contents

1. [Product Overview](#1-product-overview)
2. [Core Features](#2-core-features)
3. [Application Scenarios](#3-application-scenarios)
4. [System Architecture](#4-system-architecture)
5. [Hardware Specifications](#5-hardware-specifications)
6. [Dual-Chip Architecture](#6-dual-chip-architecture)
7. [Peripherals & Pins](#7-peripherals--pins)
8. [Schematic Notes](#8-schematic-notes)
9. [Software Architecture](#9-software-architecture)
10. [OpenClaw](#10-openclaw)
11. [Built-in Apps](#11-built-in-apps)
12. [Communication Protocols](#12-communication-protocols)
13. [Development Environment](#13-development-environment)
14. [Build & Flash](#14-build--flash)
15. [Debugging & FAQ](#15-debugging--faq)

---

## 1. Product Overview

**Metalio Claw4** is an open-source portable AI device for developers and makers. The unit is roughly **8×8 cm**, palm-sized, with a 3.95-inch 720×720 touch display, microphone array, speaker, camera, GPS, wireless charging, and dual-mode networking. After offline wake-word detection, it interacts with cloud AI over voice.

| Attribute | Details |
|:---|:---|
| **Product name** | Metalio Claw4 |
| **Main SoC** | ESP32-P4 (dual-core RISC-V, 480 MHz; 32 MB Flash, 32 MB PSRAM) |
| **Co-processor** | ESP32-C5 (2.4 / 5 GHz Wi-Fi via ESP-Hosted SDIO) |
| **4G module** | NT26 (4G LTE); built-in 4G patch SIM at factory, external SIM supported |
| **Display** | 3.95" MIPI-DSI, 720×720, GT911 capacitive touch |

---

## 2. Core Features

### 2.1 AI Voice Interaction

- **Offline wake word**: ESP-SR based
- **Streaming ASR / LLM / TTS**: Real-time speech recognition, LLM inference, and synthesis
- **On-device AEC**: Echo cancellation for full-duplex conversation
- **Dual cloud protocols**: WebSocket or MQTT+UDP for flexible backend integration

### 2.2 OpenClaw

**OpenClaw** is the cloud AI Agent platform on Metalio Claw4. Use the **OpenClaw App** on the home screen for multi-turn natural-language conversations with the cloud Agent.

- **Voice chat**: Press and hold to speak; multi-turn interaction with the cloud Agent
- **Session management**: View and clear conversation history

API and on-device capabilities: see [§10 OpenClaw](#10-openclaw).

### 2.3 Multimodal Sensing

- **Camera preview**: OV2710 MIPI-CSI, **2 MP** (1920×1080), live preview and capture in the Camera app
- **Outdoor positioning**: GPS, Wi-Fi, and cell-tower tabs in the Location app; the latter two via the 4G module for indoor or weak-GPS scenarios
- **Magnetometer**: Real-time 3-axis data in the Magnet app
- **Spirit level**: Tilt angle from the accelerometer in the Spirit Level app

### 2.4 Connectivity

- **Wi-Fi**: ESP32-C5 co-processor over SDIO; **2.4 GHz** and **5 GHz** dual-band
- **4G LTE**: NT26 cellular module with factory **built-in patch SIM**; **built-in / external SIM** slots (see below)
- **Bluetooth audio**: Dedicated BT audio chip for voice codec, BT speaker, and BT headset use cases (see [§12.1](#121-bluetooth-audio-three-modes))

#### 4G & SIM Cards

Metalio Claw4 ships with a **built-in 4G patch SIM** (internal card). An **external SIM slot** is also available. In **4G network mode**, switch between internal and external cards in the Network Config app under **SIM switch**; the home status bar shows **Internal** or **External**.

| SIM type | Description | Voice calls |
|:---|:---|:---:|
| **Internal** (patch SIM, SimSlot=1) | Factory-installed; mainly for **4G data** | ❌ Not supported |
| **External** (SimSlot=0) | User-installed standard SIM | ✅ Supported |

- **Phone calls**: Only the **external SIM** works in the Phone app; with the internal card, the app prompts you to switch
- **4G data**: Both cards work for cellular data (switch to 4G mode in Network Config)

#### 4G-Assisted Location (Location App)

Besides GPS, the NT26 module supports cellular-assisted positioning in separate tabs:

| Tab | Method | Notes |
|:---|:---|:---|
| **GPS** | On-board GPS module | NMEA satellite fix |
| **Wi-Fi location** | 4G module Wi-Fi scan | `AT+ECWIFISCAN`; requires 4G mode |
| **Cell location** | 4G module cell info | `AT+ECBCINFO`; requires 4G mode |

Wi-Fi / cell location requires **4G network mode** and successful module registration. Coordinates and a static map are shown in the app.

#### Dedicated Bluetooth Audio Solution

Metalio Claw4 uses a **dedicated Bluetooth audio chip** instead of a discrete ES8311 + ES7210 design, covering three use cases: **daily Xiaozhi chat** (Mode 1), **BT headset/speaker chat** (Mode 2), and **phone-as-remote speaker** (Mode 3). Mode switching, usage, and AT commands: [§12.1 Bluetooth Audio & Three Modes](#121-bluetooth-audio-three-modes).

### 2.5 Power & Battery

- **Single-cell Li-ion** + TI BQ27220 fuel gauge (I2C 0x55)
- **Wireless charging**: NU1680 receiver (I2C 0x60), Qi; [charge current config](#nu1680-charge-current-control)
- **USB charge detect**: IO expander `USB_INSERT_DET`
- **Low-battery protection**: Auto power-off at 0% on boot when not charging
- **Home idle shutdown**: On the home screen, **5 minutes** without touch or swipe triggers shutdown; timer stops in other apps; any touch resets it
- **Power management IC**: ~**1 s** long-press to power on, ~**5 s** for hardware force-off; firmware can software-shutdown via IO expander pulse (see [below](#power-management-ic--power-key))

#### Power Management IC & Power Key

Power on/off is handled by a dedicated **power management IC**, separate from the main MCU. The side power key connects to both that IC and the TCA9555 IO expander: the MCU reads `PWR_KEY` and drives `PWR_KEY_PULSE` to simulate key presses.

| Action | Type | Description |
|:---|:---:|:---|
| Long-press ~**1 s** when off | Hardware | PMIC powers on the device |
| Long-press ~**5 s** when on | Hardware | PMIC force power-off; no firmware needed |
| **PWR_KEY_PULSE** pulse sequence | Firmware | Simulated short press via TCA9555 P0-4 → shutdown |

**Firmware pins** (see [§7.3](#73-tca9555-io-expander-i2c-16-bit)):

| Signal | TCA9555 | Dir | Role |
|:---|:---:|:---:|:---|
| `PWR_KEY` | P0-5 | IN | Read user power key |
| `PWR_KEY_PULSE` | P0-4 | OUT | Simulated key pulse to PMIC |

Shutdown pulse: **100 ms high / 100 ms low, 10 times**. Used for UI shutdown, low-battery protection, and home idle timeout. See `home_screen.cc` (`PwrShutdownPulseTask`) and `metalio-claw-4.cc` (boot battery check).

When on, a ~**1.5 s** long-press on `PWR_KEY` opens the **Reboot / Shutdown** dialog; shutdown uses the same pulse. The dialog also notes: **long-press 5 s for hardware force-off**.

#### NU1680 Charge Current Control

NU1680 register **0x1E** (`MTP_ILIM_SET`) bits **[2:0]** set over-current limit (R/W, default `0x00`). When writing, **only change [2:0]; keep upper 5 bits**.

| [2:0] | Current limit |
|:---:|:---:|
| `000` | 1.4 A |
| `001` | 1.65 A |
| `010` | 1.1 A |
| `011` | 0.74 A |
| `100` | 0.365 A |
| `101` | 0.45 A |
| `110` | 0.29 A |
| `111` | 0.215 A |

When NU1680 is detected (I2C 0x60), firmware writes `0x00` to `0x1E` (**1.4 A**) and `0x00` to `0x15` to disable temperature protection. See `main/boards/metalio-claw-4/metalio-claw-4.cc`.

---

## 3. Application Scenarios

Metalio Claw4 includes 20+ built-in apps. Developers can **combine, trim, or extend** them into vertical scenarios. The table below is **example reference only**, not a fixed factory product shape:

| Scenario (example) | Related apps / capabilities |
|:---|:---|
| **Photo learning** | Camera, digital human, SD card assets |
| **Meeting** | Chat, OpenClaw, Bluetooth audio, Phone |
| **Smart home hub** | Voice chat + MCP IoT control |
| **Outdoor navigation** | GPS (GPS / Wi-Fi / cell tabs), 4G |
| **Entertainment** | Music (BT speaker mode), 2048, themes |
| **Dev & debug** | Pin test, system info, magnet / spirit level |

---

## 4. System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     User interaction                             │
│   720×720 LVGL 9 touch UI  ·  Wake word  ·  Key (PWR_KEY)      │
├─────────────────────────────────────────────────────────────────┤
│                     Application layer                            │
│  Chat │ OpenClaw │ Camera │ GPS │ Weather │ Music │ Avatar │ …  │
├─────────────────────────────────────────────────────────────────┤
│                     Service layer                                │
│  AudioService │ GpsService │ SdCardManager │ MCP Server          │
├─────────────────────────────────────────────────────────────────┤
│                     Protocol layer                               │
│  WebSocket │ MQTT+UDP │ OpenClaw HTTP API                        │
├─────────────────────────────────────────────────────────────────┤
│                     Board abstraction                            │
│  DualNetworkBoard │ Display │ AudioCodec │ Backlight │ Gauge     │
├─────────────────────────────────────────────────────────────────┤
│                     Driver layer                                 │
│  MIPI-DSI │ MIPI-CSI │ I2S │ SDMMC │ I2C │ UART │ IOExpander    │
├─────────────────────────────────────────────────────────────────┤
│  ESP32-P4 (Host)          │  ESP32-C5 (Slave)  │  NT26 (4G)     │
│  Main · UI · AV · Camera  │  Wi-Fi (SDIO)      │  Cellular      │
└─────────────────────────────────────────────────────────────────┘
                              │
                    OpenClaw cloud
                    ASR · LLM · TTS · Agent
```

### 4.1 Data Flow (Voice Chat)

```
User speaks → I2S mic → ESP-SR wake / VAD
           → AudioService encode → WebSocket / MQTT
           → Cloud ASR → LLM → TTS
           → Device plays audio → I2S speaker
           → LVGL UI update (chat bubbles / avatar)
```

---

## 5. Hardware Specifications

| Category | Spec |
|:---|:---|
| **Size** | ~8×8 cm (palm-sized) |
| **Main SoC** | ESP32-P4, dual-core RISC-V 480 MHz; 32 MB Flash, 32 MB PSRAM |
| **Storage** | 32 MB onboard Flash (`partitions/`) + microSD slot |
| **Display** | 3.95" square MIPI-DSI, 720×720, 24 bpp |
| **Touch** | GT911 capacitive (I2C) |
| **Audio** | Dedicated BT audio chip (replaces ES8311 + ES7210); I2S mic/speaker 16 kHz; codec / BT speaker / BT headset |
| **Camera** | OV2710, MIPI-CSI, 2 MP (1920×1080 @ 25fps), 24 MHz XCLK |
| **Positioning** | GPS module (UART NMEA-0183, 9600 baud) |
| **Sensors** | QMC6309 magnetometer, accelerometer (I2C / INT) |
| **Network** | 2.4 / 5 GHz Wi-Fi (ESP32-C5) + 4G LTE (NT26); built-in patch SIM, external SIM |
| **Bluetooth** | Dedicated BT audio chip (UART 115200, AT); see [§12.1](#121-bluetooth-audio-three-modes) |
| **Power** | Single-cell Li-ion + BQ27220 gauge |
| **Charging** | USB wired + Qi wireless ([NU1680](#nu1680-charge-current-control)) |
| **Vibration** | Vibration motor (GPIO / IO expander) |
| **Button** | Power key (`PWR_KEY` / `PWR_KEY_PULSE` via IO expander; see [power management](#power-management-ic--power-key)) |

---

## 6. Dual-Chip Architecture

Metalio Claw4 uses **ESP32-P4 + ESP32-C5** with Espressif **ESP-Hosted**:

```
┌──────────────────┐         SDIO 4-bit          ┌──────────────────┐
│    ESP32-P4      │ ◄────────────────────────► │    ESP32-C5      │
│    (Host)        │   CMD=50 CLK=51 D0=49      │    (Slave)       │
│                  │   D1=34 D2=31 D3=53        │                  │
│  · LVGL UI       │   RESET=54                 │  · Wi-Fi STA/AP  │
│  · Audio/Video   │                            │  · Network stack │
│  · Camera        │                            │                  │
│  · GPS / sensors │                            │                  │
└──────────────────┘                            └──────────────────┘
         │
         │ UART (NT26: TX=28 RX=29 MRDY=13 SRDY=4)
         ▼
┌──────────────────┐
│   NT26 4G module │
│   LTE cellular   │
└──────────────────┘
```

| Chip | Role | Interface | Responsibility |
|:---|:---|:---|:---|
| **ESP32-P4** | Host | — | UI, audio, camera, GPS, SD, protocols, OpenClaw / MCP |
| **ESP32-C5** | Slave co-processor | SDIO Slot 1 | 2.4 / 5 GHz Wi-Fi, network stack (ESP-Hosted RPC) |
| **NT26** | Cellular module | UART + MRDY/SRDY | 4G LTE data; switchable with Wi-Fi |

> P4 has no Wi-Fi RF; C5 provides it. Wi-Fi and 4G are managed by `DualNetworkBoard`; switch in the Network Config app.

---

## 7. Peripherals & Pins

Pin definitions: `main/boards/metalio-claw-4/config.h`  
IO expander map: `main/boards/common/IOExpander.hpp`  
P4 ↔ C5 SDIO pins: `sdkconfig` `CONFIG_ESP_HOSTED_SDIO_*` (configurable on P4 Host; fixed IOMUX on C5 Slave)

### 7.1 ESP32-P4 Direct GPIO

| Function | GPIO | Notes |
|:---|:---:|:---|
| I2C SDA | 7 | Shared: GT911, TCA9555, BQ27220, QMC6309, NU1680 |
| I2C SCL | 8 | |
| I2S mic WS | 10 | Audio in |
| I2S mic DIN | 11 | |
| I2S spk BCLK | 12 | Audio out |
| I2S spk DOUT | 9 | |
| NT26 SRDY | 4 | 4G flow control |
| NT26 MRDY | 13 | 4G flow control |
| NT26 TX → module RX | 28 | UART |
| NT26 RX ← module TX | 29 | UART |
| Camera XCLK | 32 | 24 MHz clock out |
| GPS TX → module RX | 38 | UART0 |
| GPS RX ← module TX | 37 | UART0 |
| Boot button | 35 | |
| BT audio TX | 26 | UART2, 115200 |
| BT audio RX | 27 | |
| LCD reset | 3 | Shared with camera reset |
| Backlight PWM | 52 | |
| SDMMC CLK | 43 | 4-bit SD |
| SDMMC CMD | 44 | |
| SDMMC D0–D3 | 39–42 | |

### 7.2 ESP-Hosted SDIO (P4 ↔ C5)

Metalio Claw4 connects P4 and C5 over **SDIO Slot 1**, **4-bit** bus at **40 MHz**. P4 Host pins are **custom-configured** in `sdkconfig` (not ESP-Hosted default Slot 1 pins). C5 Slave SDIO pins are **fixed** by chip IOMUX and cannot be changed in software.

| Signal | ESP32-P4 GPIO (Host) | ESP32-C5 GPIO (Slave) | Notes |
|:---|:---:|:---:|:---|
| CMD | 50 | 10 | |
| CLK | 51 | 9 | |
| D0 | 49 | 8 | |
| D1 | 34 | 7 | |
| D2 | 31 | 14 | |
| D3 | 53 | 13 | |
| RESET | 54 | RST/EN | P4 output, **active high**; resets C5 on every P4 boot |

| Parameter | Value | `sdkconfig` key |
|:---|:---|:---|
| SDIO slot | Slot 1 | `CONFIG_ESP_HOSTED_SDIO_SLOT_1` |
| Bus width | 4-bit | `CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS` |
| Clock | 40 MHz | `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000` |
| Reset polarity | Active high | `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH` |

> P4 default Slot 1 pins are CLK=18 / CMD=19 / D0–D3=14–17. Metalio Claw4 uses the table above due to PCB routing. After changing P4-side pins, rebuild and flash **both** the P4 host firmware and the **C5 co-processor firmware** (C5 pins must match hardware).

### 7.3 TCA9555 IO Expander (I2C 16-bit)

| Logical pin | Line | Dir | Function |
|:---|:---:|:---:|:---|
| GPS_POWER | P0-0 | OUT | GPS power (**active high**) |
| PA_SWITCH | P0-1 | OUT | PA source (low=4G, high=Wi-Fi) |
| CAM_PWDN | P0-2 | OUT | Camera power (**active low**) |
| SD | P0-3 | OUT | SD card power (**active low**) |
| PWR_KEY_PULSE | P0-4 | OUT | Pulse to PMIC (software shutdown) |
| PWR_KEY | P0-5 | IN | Side power key |
| BT_POWER | P0-6 | OUT | BT chip power (**active high**) |
| RST_4G | P0-7 | OUT | 4G module power (**active high**) |
| PA | P1-0 | OUT | Audio PA enable (**active high**) |
| ACCEL_INT | P1-1 | IN | Accelerometer interrupt |
| USB_INSERT_DET | P1-2 | IN | USB insert detect |
| WIRELESS_CHARGE_DET | P1-3 | IN | Wireless charge detect |

### 7.4 I2C Device Addresses

| Device | 7-bit addr | Notes |
|:---|:---:|:---|
| GT911 touch | 0x5D / 0x14 | Auto-detect |
| TCA9555 IO expander | 0x20 | 16-bit |
| BQ27220 gauge | 0x55 | Single-cell Li-ion |
| NU1680 wireless charger | 0x60 | Qi receiver; [`0x1E[2:0]` current limit](#nu1680-charge-current-control) |
| QMC6309 magnetometer | 0x7C | 3-axis |
| OV2710 camera SCCB | 0x36 | MIPI-CSI |

### 7.5 Peripheral Block Diagram

```
                    ┌─────────────────────────────────────┐
                    │           ESP32-P4                  │
                    │                                     │
  I2S Mic ─────────►│ GPIO 10/11    MIPI-DSI ────────────►│──► 720×720 LCD
  I2S Spk ◄─────────│ GPIO 9/12                           │──► GT911 Touch
                    │                                     │
  I2C Bus ─────────►│ GPIO 7/8  ──► TCA9555 ──┬──► GPS    │
                    │              BQ27220     ├──► CAM     │
                    │              NU1680       ├──► BT      │
                    │              QMC6309     ├──► 4G Power │
                    │                          └──► SD/PA   │
                    │                                     │
  MIPI-CSI ◄───────►│ OV2710 Camera                       │
  SDMMC 4-bit ◄────►│ microSD                             │
  UART0 ◄──────────►│ GPS Module                          │
  UART2 ◄──────────►│ BT Audio Module                     │
  UART ◄───────────►│ NT26 4G Module                      │
  SDIO ◄───────────►│ ESP32-C5 (Wi-Fi)                    │
                    └─────────────────────────────────────┘
```

---

## 8. Schematic Notes

Schematics, PCB, and BOM will be published with the hardware open-source repo; not included here yet. For pins and connections, see [§7 Peripherals & Pins](#7-peripherals--pins) and board `config.h`.

---

## 9. Software Architecture

Firmware is based on [Xiaozhi AI (xiaozhi-esp32)](https://github.com/78/xiaozhi-esp32), customized for the `metalio-claw-4` board.

### 9.1 Layered Structure

| Layer | Path / module | Role |
|:---|:---|:---|
| **Entry** | `main.cc` → `Application` | Boot, event loop, state machine |
| **Board** | `boards/metalio-claw-4/` | Hardware init, pin config |
| **Display** | `display/screen/*` | LVGL 9 app screens |
| **Audio** | `audio/` | Codec, wake word, AEC |
| **Protocol** | `protocols/` | WebSocket, MQTT+UDP |
| **MCP** | `mcp_server.cc` | Device-side Model Context Protocol |
| **Common drivers** | `boards/common/` | GPS, SD, gauge, IO expander, dual network |

### 9.2 State Machine

`Application` device states:

```
starting → configuring → idle ⇄ connecting ⇄ listening ⇄ speaking
                              ↘ upgrading / activating / fatal_error
```

- **idle**: Standby, waiting for wake word
- **listening**: Recording, streaming ASR upload
- **speaking**: Playing TTS response
- **connecting**: Establishing WebSocket / MQTT

### 9.3 Board Init Sequence

`metalio-claw-4.cc` constructor order:

1. I2C bus (GPIO 7/8)
2. IO expander (TCA9555) — peripheral power/reset (BT, PA, 4G module on; camera/SD off by default)
3. BQ27220 gauge + low-battery protection
4. BT audio UART + default Mode 1
5. SD card mount
6. MIPI-DSI LCD init
7. GT911 touch
8. LVGL display adapter
9. NU1680 wireless charge detect task
10. System monitor task (CPU / memory / battery)

### 9.4 Project Layout

```
main/
├── application.cc              # Boot, state machine, protocols
├── boards/metalio-claw-4/      # Metalio Claw4 board init
│   ├── config.h                # GPIO, display params
│   ├── config.json             # Build config
│   └── metalio-claw-4.cc       # Board entry
├── display/screen/             # LVGL app screens
├── audio/                      # Record, playback, wake word
├── protocols/                  # WebSocket / MQTT
└── boards/common/              # GPS, SD, gauge, etc.
```

---

## 10. OpenClaw

OpenClaw is Metalio's cloud AI Agent platform. The device talks to it over HTTP:

| API | Path | Purpose |
|:---|:---|:---|
| Device status | `GET /api/v1/devices/status` | Report / query device status |
| Conversation list | `GET /api/v1/conversation?page=1&size=100` | History |
| Messages | `GET /api/v1/conversation/{id}/messages` | Session messages |
| Clear sessions | `POST /api/v1/conversation/removeAll` | Clear all sessions |

Base URL: `main/api_endpoints.h`.

**OpenClaw App** (`openclaw_screen`):

- Press and hold to send voice commands
- Message bubbles
- Multi-turn cloud Agent chat

---

## 11. Built-in Apps

Home screen app list (`home_screen.cc` → `kApps[]`):

| App | ID | Description |
|:---|:---|:---|
| Chat | chat | Xiaozhi AI voice chat |
| Network Config | wifi | Wi-Fi / 4G switch, SIM switch (internal / external) |
| Digital Human | digital_people | SD card SJPG avatar animation |
| Phone | call | 4G voice (**external SIM only**) |
| Music | music | BT speaker mode (BT Mode 3), phone lyric push |
| Calendar | calendar | Calendar view |
| OpenClaw | openclaw | Cloud Agent chat |
| Location | gps | GPS / Wi-Fi / cell location (latter two need 4G mode) |
| Camera | camera | OV2710 preview & capture (1920×1080) |
| Spirit Level | spirit_level | Tilt angle |
| Magnet | magnet | QMC6309 3-axis visualization |
| Vibrate | vibrate | Vibration motor test |
| Bluetooth Config | bluetooth | BT Mode 2, scan & pair |
| Calculator | calculator | Basic arithmetic |
| Weather | weather | City weather |
| SD Card | sd | Storage management |
| Pin Test | pin | GPIO test |
| 2048 | 2048 | Mini game |
| Backlight | backlight | Brightness |
| System Info | info | Firmware / chip / MAC |
| Theme | theme | 4 icon themes |

---

## 12. Communication Protocols

| Protocol | Purpose |
|:---|:---|
| **WebSocket** | Streaming voice (ASR/LLM/TTS) |
| **MQTT + UDP** | Alternate cloud channel |
| **MCP** | Device capabilities for LLM |
| **OpenClaw HTTP** | Cloud Agent API |
| **Bluetooth AT** | BT audio module control |

### 12.1 Bluetooth Audio & Three Modes

ESP32-P4 sends **AT commands** over **UART** (115200, GPIO 26/27) to switch BT chip modes. Hardware overview: [§2.4](#dedicated-bluetooth-audio-solution). BT UART and `BT_POWER`: [§7.1](#71-esp32-p4-direct-gpio) / [§7.3](#73-tca9555-io-expander-i2c-16-bit).

#### Mode Overview

Three modes in plain terms: **talk to Xiaozhi**, **talk via BT headset/speaker**, **use phone as remote to play music**. Firmware handles most switching; you rarely need AT commands manually.

**Mode 1 — Daily Xiaozhi chat (default at boot)**

The device **starts in Mode 1**. The BT chip uses I2S for Xiaozhi record/playback — wake word or open Chat, no extra setup. Leaving the **Music** app **automatically returns to Mode 1**.

**Mode 2 — External BT device for Xiaozhi chat**

To pair headphones/speakers or chat via **BT headset / mic-enabled BT speaker**: Home → **Bluetooth Config** → **Mode 2** → scan and connect. Audio routes through the paired device — **the peripheral must have a microphone**; playback-only speakers cannot pick up voice. Return to **Mode 1** manually in Bluetooth Config when done.

**Mode 3 — BT speaker; phone connects to device**

To use Metalio Claw4 as a speaker:

1. Open **Music** on the device — **entering the app switches to Mode 3** automatically.
2. On your phone, open **Bluetooth settings**, find **`MetalioClaw4`**, and pair/connect.
3. Open your **music app** and play; audio comes from Metalio Claw4.
4. Song info and lyrics (if supported) show on device — enable lyric push in the phone app if missing.
5. **Exit Music** to **auto-return to Mode 1**.

On-screen prev/next/play-pause and volume buttons work after the phone is connected.

| Mode | Summary | Enter | Exit |
|:---:|:---|:---|:---|
| **Mode 1** | Daily Xiaozhi chat; default at boot | Boot; leave Music | Usually stay default |
| **Mode 2** | Pair BT headset/speaker for Xiaozhi | Bluetooth Config → Mode 2 | Manual → Mode 1 |
| **Mode 3** | Phone connects; speaker + lyrics | Open Music app | Exit Music app |

#### Mode Switch AT Commands

Send `AT+RX` / `AT+TX` first, **wait ~700 ms**, then `AT+MODE`. Firmware handles timing in a background task.

| Target | Sequence (each line ends with `\r\n`) | Notes |
|:---:|:---|:---|
| **Mode 1** | `AT+RX=2` → 700ms → `AT+MODE=1` | Default at boot |
| **Mode 2** | `AT+TX=1` → 700ms → `AT+MODE=2` | TX / pairing |
| **Mode 3** | `AT+RX=1` → 700ms → `AT+MODE=3` | Music receive / speaker |

Module echoes `SET MODE 1` / `SET MODE 2` / `SET MODE 3` on success.

**Auto switch (firmware)**

| Event | Commands |
|:---|:---|
| Boot | `AT+RX=2` → `AT+MODE=1` (Mode 1) |
| Enter Music | `AT+RX=1` → `AT+MODE=3` (Mode 3) |
| Leave Music | `AT+RX=2` → `AT+MODE=1` (Mode 1) |
| Bluetooth Config mode button | Per table above |

#### Mode 2: Scan & Connect

After switching to Mode 2 in Bluetooth Config:

> **Note**: For Xiaozhi voice chat over BT, the device **must support audio input** (built-in mic), e.g. BT earbuds or smart speakers with mic; playback-only speakers cannot capture voice.

| Action | AT command | Notes |
|:---|:---|:---|
| Scan | `AT+INQUIRING` | Echo `INQUIRING START`; lines `AT+BT:<12-char addr><name>` |
| Connect | `AT+CONNECT=<12 hex digits>` | e.g. `AT+CONNECT=AABBCCDDEEFF` |
| Scan done | — | Echo `INQ COMPLETE` |
| Connected | — | Echo `CONNECT SUCCESS` |
| Timeout | — | Echo `CONNECT TIMEOUT` |

After connect, Mode 2 panel:

| Action | AT sequence | Notes |
|:---|:---|:---|
| Call mode (SCO) | `AT+PP=1` → `AT+BTSCO=1` | SCO for voice |
| Music mode (A2DP) | `AT+BTSCO=0` → `AT+PP=1` | Disconnect SCO, music |

#### Mode 3: Music Screen Controls

Music app auto-switches to Mode 3; shows song info and scrolling lyrics (3 lines). Button AT commands:

| Action | AT command |
|:---|:---|
| Previous | `AT+PREV` |
| Next | `AT+NEXT` |
| Play | `AT+MPLAY=1` |
| Pause | `AT+MPAUSE=1` |
| Volume up | `AT+VOLUP` |
| Volume down | `AT+VOLDOWN` |
| Play/pause | `AT+PP` |

#### BT Reset (Maintenance)

**Reset Bluetooth** (top-right in Bluetooth Config) toggles `BT_POWER` via the IO expander for power-cycle / download mode. **Maintenance only** (BT firmware flash). Steps: [§14.5 BT Chip Flashing](#145-bt-chip-flashing).

---

## 13. Development Environment

### 13.1 Requirements

| Item | Requirement |
|:---|:---|
| **ESP-IDF** | **v5.5.4** (must match repo `sdkconfig`) |
| **Target** | ESP32-P4 (preconfigured; **no** manual `idf.py set-target`) |
| **Board** | Metalio Claw4 (`main/boards/metalio-claw-4/`) |
| **OS** | Linux / macOS / Windows (WSL2 recommended) |
| **Python** | 3.8+ (ESP-IDF venv) |
| **Serial driver** | USB-UART (CH340 / CP2102, etc.) |

### 13.2 Install ESP-IDF

**Official docs (recommended):**

> [ESP32-P4 Getting Started — ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/get-started/index.html)

**Linux / macOS quick install:**

```bash
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32p4
. ./export.sh
```

**Windows:** See [Windows toolchain setup](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/get-started/windows-setup.html), or use WSL2 with the Linux flow.

**Verify:**

```bash
idf.py --version
```

### 13.3 Clone Firmware

```bash
git clone https://github.com/CloudZao/MetalioClaw4.git
cd MetalioClaw4
```

> **Clone and build directly**: `sdkconfig` is preconfigured for ESP32-P4 and Metalio Claw4. Run `idf.py build` after clone — **no** `idf.py set-target`. See [§14.2](#142-about-sdkconfig) for `sdkconfig` notes.

### 13.4 Key Config

| Setting | Value | Notes |
|:---|:---|:---|
| ESP-IDF | v5.5.4 | Must match |
| Target | esp32p4 | Preconfigured |
| ESP-Hosted | SDIO → ESP32-C5 | Wi-Fi co-processor |
| Device AEC | Enabled | Full-duplex echo cancellation |
| Panel driver | NV3051F (default) | FL7707N optional |

---

## 14. Build & Flash

> **Tip**: With ESP-IDF ready, run `idf.py build` directly (§14.1). `sdkconfig` details: [§14.2](#142-about-sdkconfig).

### 14.1 Build

```bash
# Activate ESP-IDF environment
. ~/esp-idf/export.sh   # adjust path as needed

# Build directly — no set-target
idf.py build
```

Output is under `build/`.

### 14.2 About sdkconfig

**Do not modify `sdkconfig` unless necessary.** It is tuned for Metalio Claw4 (ESP-Hosted SDIO Wi-Fi co-processor, MIPI-DSI panel, PSRAM, etc.). **Unless you know exactly what you are changing, edits may cause abnormal behavior**, e.g.:

- Display stays off
- Wi-Fi / 4G fails
- Camera or SD init fails

For customization, prefer `sdkconfig.defaults` or board `config.json` → `sdkconfig_append`.

### 14.3 USB Debug Ports

When the device is **powered** over USB, the host usually sees **four serial ports**. Port names (`COM3`, `/dev/ttyUSB0`, etc.) may vary — use the **device descriptor** below.

| Purpose | Device descriptor | Notes |
|:---|:---|:---|
| **ESP32-P4** flash / firmware log | `USB JTAG/serial debug unit` | `idf.py flash monitor` — use this port |
| **BT chip** comm / flash | `USB Serial` (`CH340K`) | BT module USB; BT firmware flash ([§14.5](#145-bt-chip-flashing)) |
| **4G module** log | `log` | NT26 runtime log |
| **4G module** AT debug | `at` | Send AT commands directly (baud per module spec) |

> **Tip**: P4 talks to the BT chip over onboard UART (GPIO 26/27) daily; **BT firmware flash** uses the CH340K USB port. 4G AT debug uses the `at` port — no on-device UI needed.

### 14.4 ESP32-P4 Flash & Monitor

```bash
# Replace port with ESP32-P4 «USB JTAG/serial debug unit»
# Linux often /dev/ttyACM0; Windows COMx
idf.py -p /dev/ttyACM0 flash monitor
```

| Platform | Finding ESP32-P4 port |
|:---|:---|
| Windows | Device Manager → **USB JTAG/serial debug unit** |
| Linux | `ls /dev/ttyACM*` or `dmesg` |
| macOS | `/dev/cu.usbmodem*` etc. |

Press `Ctrl+]` to exit `monitor`.

**Flash only:**

```bash
idf.py -p /dev/ttyACM0 flash
```

**Monitor only:**

```bash
idf.py -p /dev/ttyACM0 monitor
```

### 14.5 BT Chip Flashing

The BT audio chip has its own USB-UART (CH340K). **Do not** confuse it with the ESP32-P4 JTAG port.

**Steps:**

1. Power device over USB; find **`USB Serial` (`CH340K`)** port.
2. Open the **BT chip flashing tool**; select the CH340K port.
3. Enter **download mode**: Home → **Bluetooth Config** → **Reset Bluetooth**; or **power-cycle** the device — then flash in the tool.
   - If the tool cannot find the device, run this step first and retry.
4. After flash, **Reset Bluetooth** or power-cycle again to boot new firmware.

> **Reset Bluetooth** power-cycles `BT_POWER` via the IO expander — **maintenance only**.

### 14.6 SD Card Assets

Digital human and other assets: [`sd_images/`](sd_images/). Copy to SD card root (keep directory structure). See [sd_images/README.md](sd_images/README.md).

---

## 15. Debugging & FAQ

### 15.1 Log Tags

| Tag | Module |
|:---|:---|
| `METALIO_CLAW_4` | Board init |
| `IOExpander` | TCA9555 |
| `GpsService` | GPS NMEA |
| `CameraScreen` | Camera preview |
| `OpenClawScreen` | OpenClaw chat |
| `系统监控` | CPU / memory / battery periodic log |

### 15.2 System Monitor

A background task logs CPU usage, free memory, and battery every second after boot.

### 15.3 Pin Test App

Home → **Pin Test** for quick GPIO / peripheral checks (`pin_test_screen`).

### 15.4 FAQ

**Q: ESP-IDF version mismatch on build**

Use **v5.5.4** and run `. ./export.sh` before each build.

**Q: Display stays off**

1. Check [`sdkconfig` was not changed accidentally](#142-about-sdkconfig)
2. MIPI-DSI LDO (chan 3, 2500mV)
3. Panel driver: default NV3051F; optional FL7707N (`METALIO_CLAW_4_USE_FL7707N`)

**Q: Wi-Fi scan finds nothing**

Wi-Fi is via **ESP-Hosted** SDIO to **ESP32-C5**. If co-processor firmware is missing or SDIO wiring is wrong, Wi-Fi won't work. See [Dual-Chip Architecture](#6-dual-chip-architecture) and [Peripherals & Pins](#7-peripherals--pins).

**Q: 4G won't register**

1. Switch to 4G mode (Network Config app)
2. Valid, non-arrears SIM (built-in patch or external)
3. Check NT26 `AT+CEREG` in serial log

**Q: Phone app cannot dial**

Internal SIM is data-only. Insert external SIM and switch in Network Config — see [§2.4 4G & SIM](#4g--sim-cards).

**Q: Wi-Fi / cell location unavailable**

Requires **4G network mode** and registered module — see [§2.4 4G-Assisted Location](#4g-assisted-location-location-app).

**Q: SD card mount fails**

1. FAT32 format
2. Confirm SD slot **external power** is on: IO expander `SD` (P0-3) is **active low**; firmware pulls it low at boot
3. ESP32-P4 SDMMC **high-speed mode (4-bit SDR50)** also requires the on-chip **SDMMC PHY power domain** (`SDMMC_LDO_CHAN_ID` in `config.h`, default LDO **chan 4**). `SdCardManager` acquires this LDO automatically during mount; if serial logs show `Failed to create SD power control driver`, the power domain was not ready
4. SDMMC pins in `config.h`

**Q: Device shuts down on home screen after a while**

Expected: [§2.5 Power & Battery](#25-power--battery) — 5-minute home idle timeout.

**Q: Screen suddenly turns blue (blue screen)**

If the screen **suddenly turns blue** during use, the device has **crashed** and the system has automatically restarted. After reboot you should see the boot animation and return to the home screen; operation usually continues normally. If blue screens occur repeatedly, connect over serial to inspect crash logs and confirm you are on the latest firmware; open an Issue if the problem persists.

---

*This document is updated with firmware releases. If pins or features don't match hardware, please open an Issue.*
