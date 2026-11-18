# Deploy to the F1 Starter Kit

## What you need

- **SG Wireless F1 Starter Kit.** The SIM is already inside and the LTE antenna is on the board.
  The separate antenna in the box is for LoRa and isn't needed here.
- **A USB webcam** that supports MJPEG at 640x480 or less. Tested with a Logitech webcam (046d:0829).
- **USB-C cable** for the top port and a **USB-C adapter** for the webcam on the bottom port.
- **[PlatformIO](https://platformio.org/install)**, either the VS Code extension or `pip install platformio`.
  It downloads the ESP32-S3 toolchain and ESP-IDF on the first build.

```
             ┌──────────────── F1 ────────────────┐
  laptop ────┤ J201 (top USB-C)    power + logs   │
             │                                    │
  webcam ────┤ J202 (bottom USB-C) USB host       │
             └────────────────────────────────────┘
```

## 1. Get the code

```sh
git clone <this repository> && cd f1-facial-recognition
```

Both models are included in `components/inference/`: the face finder in `ei/` and the face recogniser in
`ei-face-id/`. There is nothing to train or download.

## 2. Choose where events go

1. Open [webhook.site](https://webhook.site). It shows you a unique URL, and keep the page open.
2. Create the file `sdkconfig.defaults.local` in the project folder (git ignores it). Set your own
   Wi-Fi password for the [live preview](#5-live-preview-on-your-phone) here too (at least 8 characters):
   ```
   CONFIG_APP_PUBLISH_URL="http://webhook.site/<your-id>"
   CONFIG_APP_PREVIEW_PASSWORD="choose-a-password"
   ```
   Use `http://`. This demo keeps things simple and doesn't use TLS.

A free webhook.site URL accepts about 100 requests. The F1 sends a heartbeat every 5 minutes
(`CONFIG_APP_HEARTBEAT_PERIOD_S`), so one URL lasts about 8 hours. When it fills up, make a new one.

Other settings (APN, heartbeat period) are under **F1 FaceCam** in `pio run -t menuconfig`.

## 3. Build and flash

Connect the **top** USB-C port (J201) to your computer:

```sh
pio run -t upload -t monitor
```

The first build takes a few minutes, and later builds are much quicker. Leave with `Ctrl+C`.

Every upload **erases the whole flash** first (see `tools/erase_on_upload.py`), so the F1 starts from scratch:
registered people and Wi-Fi data are gone. Firmware flashed with another tool wipes them on its first boot.

> After changing `sdkconfig.defaults.local`, delete `sdkconfig.f1` so the new value is picked up.

## 4. What you should see

The board first checks both models on built-in photos, then starts the camera, then goes online:

```
I (1414) inference: model 'F1 face detection - FOMO' (Edge Impulse project 1161327), input 96x96
I (1627) inference: self-test astronaut (close-up)  PASS  (1 faces, best 1.00, 161 ms)
I (1827) inference: self-test astronaut (portrait)  PASS  (1 faces, best 0.99, 158 ms)
I (1889) camera: camera streaming MJPEG 320x240
I (2964) app: no face  score 0.00  at (0,0)  158 ms  brightness 32
I (6557) lte: attaching with APN "iot.1nce.net"
I (13552) lte: registered (roaming), CSQ 27
I (14284) lte: online, IP 10.59.195.41
I (14956) lte: sent {"device":"f1","event":"boot","uptime_s":13,"csq":27,"free_heap":245047}
```

| LED | Meaning |
|---|---|
| Yellow | Starting |
| Blue, blinking | Connecting to LTE-M |
| Green | Online |
| Magenta | A face is in view |
| Cyan | A face you [registered](face-recognition.md) is in view |

Look into the camera from 30 to 50 cm away, in good light. After two frames that agree, the F1 sends something like:

```json
{"device":"f1","event":"face","confidence":0.97,"faces":1,"inference_ms":157,"uptime_s":412,"csq":27}
```

When you leave, a `no_face` event follows. Events are sent at most once every 10 s.

## 5. Live preview on your phone

The preview is on by default; you need it to register people. Its Wi-Fi password is the one you set in step 2,
or `f1-facecam` if you didn't. To turn it off, add `CONFIG_APP_PREVIEW=n` to `sdkconfig.defaults.local`.
After changing that file, delete `sdkconfig.f1` and flash again.

At boot, look for:

```
I (1727) preview: join Wi-Fi "F1-FaceCam" and open http://192.168.4.1
```

Join the Wi-Fi network **F1-FaceCam** with your phone or laptop and open **http://192.168.4.1**. You see the
camera image, a dashed square (the part the model looks at) and a box on every face it finds, with the name if you
[registered that person](face-recognition.md#2-register-people). Phones may warn that the network has no internet;
stay connected anyway.

The preview runs next to LTE. Both work at the same time, because Wi-Fi uses the ESP32-S3's radio and LTE uses the modem.

Below the image is the list of people the F1 knows, where you [register](face-recognition.md#2-register-people),
rename and delete them.

## Troubleshooting

| Symptom | What to try |
|---|---|
| Self-test says FAIL for the astronaut photos | The model doesn't find faces at all. Run `git status` to check `components/inference/` is unchanged, then build and flash again. |
| `coffee cup FAIL` | Known: the face finder takes the shiny spoon for a face. Its training photos are all faces, with no "not a face" examples. |
| No `camera streaming` line | Replug the webcam on **J202**. The camera must offer MJPEG at 640x480 or less over USB Full-Speed. |
| `brightness` below about 20 | Too dark, or the lens is covered. |
| Stays blue | The first attach can take a few minutes. Check that you have LTE-M coverage, and the APN (`iot.1nce.net` for the kit's SIM). |
| `POST failed` | Open your URL in a browser. When the free webhook.site inbox is full, it rejects requests. |
| Preview page doesn't load | Check the log for `preview:`. The password needs at least 8 characters. Use `http://`, not `https://`. |
| Name shows "unknown" for a registered person | Stand at the distance you registered from, in similar light, or register again to add pictures. |
| Registered people are gone | Expected after flashing: every upload erases the board. Register them again. |
| `Please check pull-up resistances` or `Rx Break` warnings | Harmless: the board has external I2C pull-ups, and the modem's UART line glitches while it resets. |
