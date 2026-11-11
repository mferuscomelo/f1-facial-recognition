#pragma once
#include <cstdint>

// Runs the Edge Impulse model exported into components/inference/ei/.
namespace inference {

struct Box {
    int x, y, width, height;   // in input pixels
    float score;
};

struct Result {
    bool ok = false;           // false if the model could not run
    bool face = false;         // at least one face above the model's threshold (0.5)
    float confidence = 0;      // score of the best face, 0..1
    int x = 0, y = 0;          // centre of the best face, in input pixels (0..95)
    int count = 0;             // number of faces found
    Box boxes[10] = {};        // the first `count` entries are valid
    int time_ms = 0;           // time spent in the model
};

int input_width();   // 96 for the face model
int input_height();

// `gray` is an input_width() x input_height() 8-bit grayscale image.
Result run(const uint8_t* gray);

// Logs the model's details and checks it on three built-in test photos.
void self_test();

}  // namespace inference
