# inference: Edge Impulse wrapper (Stage 2), WIP

## Drop in an Edge Impulse export
1. Studio, then Deployment, then **C++ library** (int8 quantized, EON compiler on). Unzip the export.
2. Copy `edge-impulse-sdk/`, `model-parameters/`, `tflite-model/` into `components/inference/ei/`
   (replace whatever is there). `ei/*` is gitignored. Remove that line from `.gitignore` if you want the export committed.
3. Enable `CONFIG_APP_ENABLE_INFERENCE=y` (menuconfig, or `sdkconfig.defaults.local`), then `rm sdkconfig.f1` and run `pio run`.

## How it is built
`CMakeLists.txt` follows EI's official `example-standalone-inferencing-espressif-esp32`
(`main/CMakeLists.txt` @ c1176b9). It uses the same defines: `EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1`,
`EIDSP_USE_ESP_DSP=1`, `EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1` on esp32s3. It also globs the same sources.
The one fix: the assembly is globbed as `*.S`. The example uses `*.s`, which matches nothing on Linux.

## Input format
`inference::run(gray, w, h)` takes 8-bit grayscale at the model's input size. Each pixel is packed as
`0xRRGGBB` with R=G=B=gray (see `ei_run_dsp.h` `extract_image_features`). This works for both
Grayscale and RGB impulses.

## Tensor arena
The SDK allocates the arena on each `run_classifier()` through `ei_calloc`. The wrapper overrides that call.
Allocations of 32 KB or more go to internal RAM if it still leaves at least 64 KB free, otherwise to PSRAM.
`set_arena_policy()` can force either one. `Result::arena_in_psram` reports where the arena ended up.

## Placeholder model
The placeholder is an EI FOMO face model, 96x96 **RGB**, from github.com/JacobShok/FACE_TRACKER_ESP32S3SENSE @ b9359d2.
It has no license, so use it locally only and do not commit it. The real "Face detection - FOMO" export
(96x96 grayscale) needs no code changes.
