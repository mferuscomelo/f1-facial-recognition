# camera (Stage 3)

USB UVC webcam on the bottom USB-C (J202, S3 native USB OTG in host mode) -> MJPEG ->
`espressif/esp_jpeg` -> centre-crop + box downscale + grayscale -> model input (default 96x96).
Enabled with `CONFIG_APP_ENABLE_CAMERA` (default **n**). API: `include/camera.hpp`
(`camera::start(cb)`, `camera::get_latest(dst, size, &info, timeout)`, `camera::stats()`).

Components: `espressif/usb_host_uvc` ^2.6.0 (uses ESP-IDF's built-in `usb` on IDF 5.x),
`espressif/esp_jpeg` ^1.3.2.

## Camera requirements
- USB Video Class (UVC 1.x) device with an **MJPEG** format at or below **640x480**.
  YUY2-only, H.264-only cameras are rejected (logged).
- Must work at **USB Full-Speed** (12 Mbit/s). The ESP32-S3 has no High-Speed PHY; isochronous
  bandwidth is ~1 MB/s. Many HD webcams only offer small alt-settings at FS; 320x240 MJPEG is the target.
- Connected directly to J202 (hub support is not enabled: `CONFIG_USB_HOST_HUBS_SUPPORTED` off).

## Power (J202)
- J202 must supply VBUS (5 V) to the camera. Check on the F1 schematic/SG docs that VBUS on J202 is
  actually switched on in host mode and how much current it can source; webcams typically draw
  150-500 mA (more with IR LEDs). If enumeration fails or the camera resets, power it through a
  powered hub/Y-cable (hub support would then need `CONFIG_USB_HOST_HUBS_SUPPORTED=y`).
- Brown-outs during LTE TX bursts + camera load are possible on battery: watch for resets.

## Memory
- Static: ~+1.9 KB DIRAM, ~+83 KB flash (flag on). Flag off: nothing linked (0 bytes).
- Runtime **internal RAM** (DMA-capable, required by the S3's USB DWC DMA - IDF `usb_private.c`
  `DATA_BUFFER_CAPS` = `MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL` on S3):
  URBs `URB_COUNT x URB_SIZE` (default 3 x ~4-5 KB = ~15 KB), control transfer buffers
  (`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` 4 KB, x2-3), ISOC descriptor lists, driver objects.
  Plus task stacks (usb_lib 4 K, UVC 4 K, cam_mgr 4 K, cam_decode 4 K, cam_stats 3 K) and the
  4 KB TJpgDec work pool. Estimate **~45-55 KB internal** while streaming. Measured value is
  logged on stream open ("stream internal RAM cost") and every period by the main.cpp hook.
- PSRAM: UVC frame buffers (3 x 40 KB at 320x240), JPEG slot (100 KB), RGB scratch (57.6 KB at
  160x120), gray buffers (2 x 9 KB).

## Behaviour
- Requests `APP_CAMERA_REQ_WIDTH x HEIGHT @ FPS` MJPEG; else nearest MJPEG <= 640x480. FPS snapped to
  the camera's advertised intervals, retry with camera default on negotiation failure.
- Frames are dropped (never queued) while the decoder is busy.
- Hot-plug: disconnect closes the stream; the next connect re-opens it. No reboot.
- Privacy: frames only exist in RAM and are overwritten; nothing is stored or uploaded.

## Test steps (when the camera arrives)
1. `echo CONFIG_APP_ENABLE_CAMERA=y >> sdkconfig.defaults.local; rm -f sdkconfig.f1; pio run -t upload -t monitor`
   (console stays on J201 / CP210x).
2. Plug the camera into J202. Expect: "UVC device connected", the format list, the descriptor dump,
   "streaming 320x240 @ 15.0 fps".
3. Every 5 s: `cam UP ... rx X fps, out Y fps | decode Z ms` + heap line. `mean` luma should change
   when covering the lens. `err` should stay ~0 (if not: Huffman/marker issue, see esp_jpeg README).
4. Unplug -> "camera disconnected", "stream closed"; replug -> streams again. Repeat 5x and check
   internal heap returns to the same value.
5. Run with LTE enabled for 10 min; record heap numbers in `docs/memory-budget.md`.
