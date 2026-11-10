# HANDOFF: F1 Face-Recognition Device (SG Wireless F1 + Edge Impulse + LTE-M)

You are Claude Code. This document is your brief. Read all of it before writing code.
Work in stages. Do not start a stage until the previous stage's acceptance criteria pass
on real hardware and the user has confirmed. Commit to git at the end of every stage.

---

## 1. Goal

An SG Wireless F1 Starter Kit that:
1. Captures frames from a USB webcam plugged into the board's USB OTG port.
2. Runs an Edge Impulse face model **fully on-device** (ESP32-S3).
3. When a face is detected/recognized, pushes a small JSON event to a server over **LTE-M** (on-board SIM).

The user is Milan (embedded systems background, prefers C++). He will plug the board into his
laptop over USB. **There is no camera available at the start.** Stages 0–2 must work without one.

---

## 2. Hardware facts (verified from vendor docs)

| Item | Value |
|---|---|
| Board | SG Wireless F1 Starter Kit (EU Cat-M1 variant, part SGW3501-EVK-EU-CATM1-8130D) |
| MCU | ESP32-S3, dual-core Xtensa LX7 @ 240 MHz |
| Memory | 512 KB internal SRAM, **8 MB PSRAM**, **16 MB flash** |
| Cellular modem | Sequans Monarch 2 **GM02S** (LTE Cat-M1 / NB1 / NB2), talks to the ESP32-S3 over UART |
| Other radios | Semtech SX1262 LoRa (not used), Wi-Fi, BLE |
| SIM | Pre-inserted global SIM (1NCE per SG Wireless's product page) |
| USB | **Top USB-C (J201)**: power + programming + serial. **Bottom USB-C (J202)**: USB OTG, can power an external device. Camera goes here. |
| I2C | Fuel gauge on module pins P19 (SDA) / P20 (SCL), also the default I2C bus |
| Other | RGB LED, micro-SD slot (only 1 data line wired), Qwiic connector, Arduino UNO headers (3.3 V only!) |

### Unknowns you must resolve in Stage 0 (do NOT guess)
- ESP32-S3 GPIOs for the **modem UART (TX/RX/RTS/CTS)** and the modem **power-on / reset / wake** pins.
- RGB LED GPIO and driver type.
- PSRAM mode (**quad vs octal**). The module's 1.8 V flash/PSRAM supply suggests octal, but that is an inference, not a fact.
- Whether J201 (top USB) uses a USB-to-UART bridge or the S3's native USB-Serial-JTAG.
  (If it's native USB, using J202 as USB host may affect the console. Find out before Stage 3.)
- Modem power-up sequence and any band/carrier configuration the SG SDK applies.

**Source of truth for all of these:** SG Wireless's own SDK, which already drives this exact board.
```
git clone https://github.com/sg-wireless/sg-sdk.git      # or sg-sdk-dev
```
Grep `src/platforms/F1/` (board config, LTE component, RGB LED, sdkconfig defaults).
Their LTE stack is documented as built on Espressif's `esp_modem` using **CMUX + PPP**, so mirror their pin
setup and init sequence. Record every pin you find with the file+line it came from in `docs/pinmap.md`.
If something cannot be found, stop and tell the user; he is in contact with SG Wireless and can ask them.

---

## 3. Toolchain decisions (already made, do not change without asking)

- **ESP-IDF framework (no Arduino)**, built with **PlatformIO** (official platform, which ships ESP-IDF 5.5.x).
- C++ (C++17) for application code.
- Pull Espressif components via `idf_component.yml` (component manager).
- No MicroPython.

Starting `platformio.ini`:
```ini
[env:f1]
platform = espressif32 @ 6.13.0      ; ESP-IDF 5.5.3
board = esp32-s3-devkitc-1
framework = espidf
board_build.flash_size = 16MB
board_build.partitions = partitions.csv
monitor_speed = 115200
monitor_filters = esp32_exception_decoder
```
- If PlatformIO's component-manager integration misbehaves, fall back to vendoring components
  under `components/`. Tell the user if you do.
- Custom `partitions.csv`: nvs, phy_init, a **large factory app (≥ 6 MB)**, plus a small storage/FAT partition
  for later (face DB / logs). No OTA partitions needed yet.
- `sdkconfig.defaults`: enable PSRAM (mode per Stage 0 finding), CPU 240 MHz, `CONFIG_SPIRAM_USE_MALLOC=y`,
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` small (e.g. 4096), lwIP PPP support, `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`
  (TLS buffers in PSRAM), FreeRTOS run-time stats on.

---

## 4. Project layout

```
f1-facecam/
├── platformio.ini
├── partitions.csv
├── sdkconfig.defaults
├── docs/
│   ├── pinmap.md            # Stage 0 output, with sources
│   └── memory-budget.md     # heap numbers recorded after every stage
├── src/
│   ├── main.cpp             # app_main: init + feature flags
│   ├── idf_component.yml
│   └── CMakeLists.txt
└── components/
    ├── board/               # pins, LED, heap telemetry, modem power control
    ├── lte/                 # esp_modem wrapper: connect, reconnect, publish()
    ├── inference/           # Edge Impulse wrapper: run(img) -> result
    └── camera/              # usb_host_uvc + JPEG decode + resize
```
Each stage is enabled by a Kconfig flag (`CONFIG_APP_ENABLE_LTE`, `..._INFERENCE`, `..._CAMERA`) so every
stage can be tested alone and earlier stages keep working.

**Memory telemetry (all stages):** every 10 s log
`heap_caps_get_free_size` and `heap_caps_get_largest_free_block` for `MALLOC_CAP_INTERNAL` and
`MALLOC_CAP_SPIRAM`. Internal SRAM is the resource most likely to run out (camera USB buffers + PPP/lwIP + TLS
+ inference arena all compete). Append the numbers to `docs/memory-budget.md` at the end of each stage.

---

## 5. Stages

### Stage 0: Board bring-up + recon (no network, no camera)
1. Clone SG SDK, extract the unknowns from §2, write `docs/pinmap.md`.
2. Scaffold the PlatformIO/ESP-IDF project above.
3. Firmware: boot log with chip info, PSRAM size detected, flash size, heap telemetry, RGB LED blink.
4. Flash via the top USB-C (J201). Ask the user which serial port appears.

**Acceptance:** builds clean; boots; reports ~8 MB PSRAM and 16 MB flash; LED blinks; heap telemetry prints.

### Stage 1: LTE-M (the riskiest part, so it goes first)
1. `espressif/esp_modem` component. Implement modem power-on per SG SDK sequence.
2. Start with a **generic DCE in plain PPP mode** (no CMUX) to get an IP. Then switch to CMUX (which is what SG uses)
   so AT commands (signal quality, registration) can run alongside data.
3. APN: make it a Kconfig string. **1NCE has two APNs:** `iot.1nce.net` (Platform 1.0 SIMs) and `sensor.net`
   (default for new Platform 2.0 SIMs). Also check which APN the SG SDK sets by default. If attach fails with one,
   try the other. Always set the APN explicitly; don't rely on auto-detect.
4. Log: IMEI, ICCID, operator, RAT (should be Cat-M1), RSSI/RSRP, assigned IP, DNS.
5. `lte::publish(json)` → **plain HTTP POST** first (no TLS) to an endpoint from Kconfig. Ask the user for the URL.
   For a quick test a request-inspector service is fine; payloads contain no personal data at this stage.
6. Then add TLS (esp_http_client + mbedTLS with buffers in PSRAM) and/or MQTT (`esp-mqtt`). Ask the user which
   backend he wants.
7. Reconnect logic: survive modem detach / PPP drop without reboot.

**Acceptance:** from cold boot, the board attaches on Cat-M1, gets an IP, and POSTs
`{"device":"f1","event":"boot","rssi":...,"heap_int":...}` every 60 s for 10 min without a crash;
unplugging/replugging antenna-less recovery works or is documented.

**Fallback if Stage 1 stalls (> 1 working session):** tell the user. Options: (a) port the
`dptechnics/walter-modem` component (same Monarch 2 modem family; remap pins), or (b) flash SG's stock
MicroPython firmware once and run their `lte` module just to prove SIM/network/APN are fine, then return to C++.

### Stage 2: Edge Impulse inference, no camera (static buffer)
Rationale: proves the model, the build integration and the timing **before** the camera exists.
1. Ask the user for his Edge Impulse project. If he has none yet, clone the public
   "Face detection - FOMO" project in Studio (96×96 grayscale), train, and export.
2. Deployment target: **C++ library**. Unzip into `components/inference/` and add a CMakeLists for it.
   Keep **ESP-NN enabled** (`EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1`). That's the whole point of using ESP-IDF.
3. Put the tensor arena in PSRAM if it doesn't fit internally; measure both.
4. Bake 2–3 test images into flash as C arrays (one face, one no-face). Run `run_classifier()` in a loop.
5. Log DSP time, inference time, results, and heap.
6. With LTE enabled at the same time, POST the result of each test run. This proves inference and networking
   coexist (this is where the previous attempt by someone else failed).

**Acceptance:** correct result on the test images; inference time logged (expect hundreds of ms, not seconds;
seconds means ESP-NN is not active); LTE and inference run together for 10 min without a crash or heap leak.

### Stage 3: USB webcam (user will have the camera at home)
1. `espressif/usb_host_uvc` (NOT the deprecated `usb_stream`).
2. Constraints: ESP32-S3 USB is **Full-Speed only**; camera must be **UVC + MJPEG**. Request **320×240 MJPEG**
   (640×480 @ 15 fps is the practical max). Log the camera's descriptor/format list on connect.
3. Frame path: MJPEG frame → JPEG decode (Espressif JPEG decoder component) → downscale + grayscale to the model
   input size → Stage 2 inference wrapper. All frame buffers in PSRAM.
4. Handle hot-plug (camera unplug/replug).
5. Confirm serial logging still works with J202 in host mode (see §2 unknown about J201).

**Acceptance:** live frames decoded, ≥ 1 inference/s, hot-plug survives, internal heap stable.

### Stage 4: Integration + demo polish
1. Two tasks: **core 1** = camera + inference; **core 0** = LTE. Connect with a FreeRTOS queue.
2. Event logic: debounce (e.g. send only on state change face↔no-face, or max 1 event / 10 s per identity).
3. Payload: `{"device","ts","label","confidence","inference_ms","rssi"}`. **Never send images by default.**
4. RGB LED states: booting / LTE attaching / LTE ready / face detected / error.
5. Optional later: open-set recognition with on-device enrollment via Espressif ESP-DL (`HumanFaceDetect` +
   `HumanFaceFeat`, ~255 ms/face on S3). Only if the user asks; it's a separate decision from Edge Impulse.

**Acceptance:** plug in board + camera → within ~1 min the server receives events when a face appears.

---

## 6. Rules for you (Claude Code)

- **Never invent pins, AT commands or API names.** Get pins from the SG SDK source, ESP-IDF APIs from the
  installed IDF headers, component APIs from the component's own headers/examples. Cite the source in comments.
- Prefer Espressif's official examples as starting points (esp_modem `pppos_client`, usb_host_uvc examples).
- Keep secrets (endpoint tokens) out of git: Kconfig + a gitignored `sdkconfig.defaults.local`.
- After each stage: update `docs/memory-budget.md`, commit with a clear message, give the user a 3-line summary
  plus the exact command to reproduce (`pio run -t upload -t monitor`).
- If a stage fails twice in the same way, stop and report what you tried instead of looping.
- Privacy: this device processes faces (biometric data under GDPR). Do not store or upload images, and do not
  enroll identities, unless the user explicitly asks.

---

## 7. Reference links

- SG Wireless SDK: https://github.com/sg-wireless/sg-sdk
- SG LTE module docs (CMUX + PPP over esp_modem): https://docs.sgwireless.com/v1.4.1/programming-references/lte.html
- F1 Starter Kit hardware: https://docs.sgwireless.com/sg-products/user-manual/hardware/
- esp_modem: https://docs.espressif.com/projects/esp-protocols/esp_modem/docs/latest/
- usb_host_uvc / USB camera limits: https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_host/usb_stream.html
- Edge Impulse C++ library deployment: https://docs.edgeimpulse.com/docs/run-inference/cpp-library
- Public FOMO face detection project: https://studio.edgeimpulse.com/public/87291/latest
- 1NCE APN (Platform 2.0 change): https://help.1nce.com/platform-migration/feature-changes/apn/
- PlatformIO platform-espressif32 releases: https://github.com/platformio/platform-espressif32/releases
- Walter modem (fallback, same modem family): https://components.espressif.com/components/dptechnics/walter-modem
