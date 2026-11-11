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

## 2. Add your Edge Impulse model

Unzip the C++ library export from [Train your model](train-your-model.md) into `components/inference/ei/`, so you get:

```
components/inference/ei/
├── edge-impulse-sdk/
├── model-parameters/
└── tflite-model/
```

No code changes are needed. The firmware reads the input size and labels from `model-parameters/`.

Optional: a second export, the face recognition model, goes into `components/inference/ei-face-id/`.
See [Face recognition](face-recognition.md).

## 3. Choose where events go

1. Open [webhook.site](https://webhook.site). It shows you a unique URL, and keep the page open.
2. Create the file `sdkconfig.defaults.local` in the project folder (git ignores it):
   ```
   CONFIG_APP_PUBLISH_URL="http://webhook.site/<your-id>"
   ```
   Use `http://`. This demo keeps things simple and doesn't use TLS.

A free webhook.site URL accepts about 100 requests. The F1 sends a heartbeat every 5 minutes
(`CONFIG_APP_HEARTBEAT_PERIOD_S`), so one URL lasts about 8 hours. When it fills up, make a new one.

Other settings (APN, heartbeat period) are under **F1 FaceCam** in `pio run -t menuconfig`.

## 4. Build and flash

Connect the **top** USB-C port (J201) to your computer:

```sh
pio run -t upload -t monitor
```

The first build takes a few minutes, and later builds are much quicker. Leave with `Ctrl+C`.

> After changing `sdkconfig.defaults.local`, delete `sdkconfig.f1` so the new value is picked up.

## 5. What you should see

The board first checks the model on three built-in photos, then starts the camera, then goes online:

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

## 6. Optional: live preview on your phone

Add to `sdkconfig.defaults.local` (choose your own password, at least 8 characters):

```
CONFIG_APP_PREVIEW=y
CONFIG_APP_PREVIEW_PASSWORD="choose-a-password"
```

Delete `sdkconfig.f1`, flash again, and look for:

```
I (1727) preview: join Wi-Fi "F1-FaceCam" and open http://192.168.4.1
```

Join the Wi-Fi network **F1-FaceCam** with your phone or laptop and open **http://192.168.4.1**. You see the
camera image, a dashed square (the part the model looks at) and a box on every face it finds, with the name if you
[registered that person](face-recognition.md#4-register-people). Phones may warn that the network has no internet;
stay connected anyway.

The preview runs next to LTE. Both work at the same time, because Wi-Fi uses the ESP32-S3's radio and LTE uses the modem.

The same page has two more tools, below.

## 7. Collect training images

Under **Collect training images**, type a label (for example `desk` or your name), pick a rate and press **Start**.
Your browser keeps the camera frames; the F1 stores nothing. **Download .zip** gives you the images plus an
`info.labels` file with the F1's own face boxes and a training/testing split.

In your Edge Impulse face project, open **Data acquisition → Add data → Upload data**, select all unzipped files
(including `info.labels`) and upload. Check the boxes, fix the ones that are off, and retrain.
Frames with nobody in them come in with no boxes: that's how the model learns what is *not* a face.

## 8. Update the firmware over Wi-Fi

After the first USB flash, new firmware (for example with a retrained model) can go over Wi-Fi:

1. Put the new export in place and run `pio run`.
2. On the preview page, under **Firmware**, choose `.pio/build/f1/firmware.bin` and press **Update**.
3. The F1 writes it into its second firmware slot, checks it and restarts. The page shows the new version.

If the new firmware crashes before its boot self-test is done, the F1 goes back to the previous one by itself.
Registered people are kept: they live in their own flash partition.

## Troubleshooting

| Symptom | What to try |
|---|---|
| Build stops with *No Edge Impulse model* | The export isn't in `components/inference/ei/` (step 2). |
| Self-test says FAIL for the astronaut photos | The model doesn't find faces at all. Check you exported the right project, and retrain. |
| `coffee cup FAIL` | Known: the public dataset has no "not a face" examples. See [Make it better](train-your-model.md#make-it-better). |
| No `camera streaming` line | Replug the webcam on **J202**. The camera must offer MJPEG at 640x480 or less over USB Full-Speed. |
| `brightness` below about 20 | Too dark, or the lens is covered. |
| Stays blue | The first attach can take a few minutes. Check that you have LTE-M coverage, and the APN (`iot.1nce.net` for the kit's SIM). |
| `POST failed` | Open your URL in a browser. When the free webhook.site inbox is full, it rejects requests. |
| Preview page doesn't load | Check the log for `preview:`. The password needs at least 8 characters. Use `http://`, not `https://`. |
| Name shows "unknown" for a registered person | Stand at the distance you registered from, in similar light, or register again to add pictures. |
| `Update failed` on the page | Upload `firmware.bin` (not `bootloader.bin` or the `.elf`), and stay on the F1's Wi-Fi until it restarts. |
| `Please check pull-up resistances` or `Rx Break` warnings | Harmless: the board has external I2C pull-ups, and the modem's UART line glitches while it resets. |
