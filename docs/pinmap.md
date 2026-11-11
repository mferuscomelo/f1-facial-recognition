# F1 Starter Kit (SGW3501) pin map

Source: `sg-wireless/sg-sdk` @ 13b998fa24ccd8e2830a9d0874d5a5953d9225ee (2026-07-07).
Paths below are relative to `src/platforms/F1/` in that repo.

## Console / programming
| Item | Value | Source |
|---|---|---|
| J201 (top USB-C) | **Silicon Labs CP210x USB-UART bridge** (VID:PID 10c4:ea60) → UART0 console | Observed with `lsusb` on the host, enumerates as `/dev/ttyUSB0` |
| Consequence | Native USB (GPIO19/20) is not the console, so J202 can be USB host without losing logs | Confirmed: logs keep working while a webcam streams on J202 |

## Memory
| Item | Value | Source |
|---|---|---|
| PSRAM mode | **QUAD**, 80 MHz, CS=GPIO26, CLK=GPIO30 | `configs/sdkconfig.spiram_sx` |
| Flash | 16 MB, **DIO @ 40 MHz** | `configs/sdkconfig.board` |
| VDD_SDIO | `CONFIG_BOOTLOADER_VDDSDIO_BOOST_1_9V=y` | `configs/sdkconfig.board` |

## RGB LED
| Item | Value | Source |
|---|---|---|
| Type | WS2812, 1 LED, GRB order | `comps/rgbled-if/defaults.config`, `comps/rgbled-if/rgbled.c:58-63,123-128` |
| Data GPIO | **GPIO21** | `comps/rgbled-if/rgbled.c:59` (`RGBLED_GPIO_PIN 21`) |

## LTE modem (Sequans GM02S)
| Signal | ESP32-S3 | Source |
|---|---|---|
| UART port | UART1 | `comps/lte/espmodem.h:45` |
| TX | GPIO47 | `comps/lte/espmodem.h:46` |
| RX | GPIO48 | `comps/lte/espmodem.h:47` |
| RTS | GPIO33 | `comps/lte/espmodem.h:48` |
| CTS | GPIO6 | `comps/lte/espmodem.h:49` |
| Baud | boots at 115200, SG switches to 921600 (`AT+IPR`, non-persistent) | `comps/lte/espmodem.c:474-799` |

Modem power / reset are **not** ESP32 GPIOs. They go through a PCAL6408A I2C IO expander:

| Item | Value | Source |
|---|---|---|
| I2C | I2C_NUM_0, SCL=GPIO7, SDA=GPIO8, 400 kHz, external pull-ups | `comps/ioexp-if/ioexp.c:155-160,311-312` |
| Address | 0x21 | `comps/lte/espmodem.h:56` |
| INT_N | GPIO9 | `comps/ioexp-if/ioexp.c:161` |
| P0 | lora_power | `comps/ioexp-if/ioexp.c:144` |
| P1 | **lte_power** | `comps/ioexp-if/ioexp.c:145` |
| P2 | lora_reset_n | `comps/ioexp-if/ioexp.c:146` |
| P3 | **lte_reset_n** (active low) | `comps/ioexp-if/ioexp.c:147` |
| P4 | secure_en | `comps/ioexp-if/ioexp.c:148` |
| P7 | **lte_ring** (input) | `comps/ioexp-if/ioexp.c:151` |

Power-on sequence (`comps/ioexp-if/ioexp.c:814-829`, `comps/lte/espmodem.c:661-799`):
1. Configure UART1 at 115200 first (so `+SYSSTART` isn't missed).
2. Set lte_reset_n high, then lte_power high.
3. Wait up to 8 s for `+SYSSTART` URC, otherwise poll `AT`.
4. Optional: set carrier conformance (`AT+SQNCTM`, causes modem reset) at 115200.
5. Switch to 921600 and hand over to esp_modem (CMUX + PPP).

Reset: lte_reset_n low ~100 ms, then high (`comps/ioexp-if/ioexp.c:849-865`).

## Other
| Item | Value | Source |
|---|---|---|
| Fuel gauge (BQ27421) I2C | default SCL=GPIO16, SDA=GPIO15 | `comps/fuel-gauge-if/src/fuel_gauge.c:59-68` |
| Antennas | LTE = on-board PCB antenna ANT100; LoRa = IPEX JT103 (external) | https://docs.sgwireless.com/v1.4.1/hardware/f1-starter-kit.html |
| Default APN in SG docs | `iot.1nce.net` (examples only, SDK default is empty/carrier default) | `comps/lte/modlte.md:38,113` |

## USB host (webcam)
| Item | Value | Source |
|---|---|---|
| J202 (bottom USB-C) | ESP32-S3 native USB (GPIO19/20), host mode, supplies 5 V to the webcam | Verified on the board with a Logitech 046d:0829 |
| Modem flow control | Hardware RTS/CTS, 115200 baud | `components/lte/lte.cpp`, verified on the board |
