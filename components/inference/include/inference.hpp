#pragma once
// Thin wrapper around the Edge Impulse C++ SDK (run_classifier). No EI types leak out.
#include <cstdint>
#include <string>
#include <vector>
#include "esp_err.h"

namespace inference {

struct Box {
    std::string label;
    float value = 0;
    uint32_t x = 0, y = 0, w = 0, h = 0;  // in model-input pixels (e.g. 96x96)
};

struct Result {
    int err = 0;                 // EI_IMPULSE_ERROR (0 = EI_IMPULSE_OK)
    std::string label;           // best label ("" if nothing above threshold)
    float confidence = 0;
    std::vector<Box> boxes;      // FOMO / object detection only
    int dsp_ms = 0, inference_ms = 0;
    bool arena_in_psram = false; // where the last large (arena) allocation landed
};

enum class ArenaPolicy { Auto, Internal, Psram };

esp_err_t init();
int input_width();
int input_height();
void set_arena_policy(ArenaPolicy p);

// 8-bit grayscale frame of exactly input_width() x input_height() (resizing is the caller's job).
Result run(const uint8_t* gray, int w, int h);

// Runs the model on built-in public-domain test images (components/inference/test_images.h) and
// logs the results. Returns true if the face images produce a box and the no-face image does not.
bool self_test();

}  // namespace inference
