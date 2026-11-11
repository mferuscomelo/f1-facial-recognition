// A faster int8 PReLU for TensorFlow Lite Micro.
//
// MobileFaceNet has 33 PReLU layers. The SDK only has the generic reference version, which works
// out 4-D broadcast indexes for every single value: about 1.1 s of the 2.9 s per face on the F1.
// Here the slope ("alpha") has one value per channel, so we walk the data in order instead, and
// look up the positive half in a 256-entry table. The results are bit-for-bit the same.
#include "fast_prelu.hpp"

#include <algorithm>

#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/common.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/kernel_util.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/prelu.h"

namespace tflite {
TfLiteStatus PreluEval(TfLiteContext* context, TfLiteNode* node);  // the SDK's version (prelu.cc)
void* PreluInit(TfLiteContext* context, const char* buffer, size_t length);
}  // namespace tflite

namespace {

TfLiteStatus FastPreluEval(TfLiteContext* context, TfLiteNode* node) {
    const auto& p = *static_cast<const tflite::PreluParams*>(node->user_data);
    const TfLiteEvalTensor* input = tflite::micro::GetEvalInput(context, node, 0);
    const TfLiteEvalTensor* alpha = tflite::micro::GetEvalInput(context, node, 1);
    TfLiteEvalTensor* output = tflite::micro::GetEvalOutput(context, node, 0);
    const tflite::RuntimeShape in_shape = tflite::micro::GetTensorShape(input);
    const int channels = in_shape.Dims(in_shape.DimensionsCount() - 1);
    // Anything other than int8 with one slope per channel: use the SDK's version.
    if (input->type != kTfLiteInt8 || tflite::micro::GetTensorShape(alpha).FlatSize() != channels ||
        tflite::micro::GetTensorShape(output).FlatSize() != in_shape.FlatSize()) {
        return tflite::PreluEval(context, node);
    }

    auto clamp = [&](int32_t v) { return int8_t(std::min<int32_t>(127, std::max<int32_t>(-128, v + p.output_offset))); };
    int8_t positive[256];  // output for every input byte that is >= 0 after the offset
    for (int x = -128; x < 128; ++x) {
        const int32_t v = p.input_offset + x;
        positive[x + 128] = clamp(tflite::MultiplyByQuantizedMultiplier(v, p.output_multiplier_1, p.output_shift_1));
    }

    const int8_t* in = tflite::micro::GetTensorData<int8_t>(input);
    const int8_t* a = tflite::micro::GetTensorData<int8_t>(alpha);
    int8_t* out = tflite::micro::GetTensorData<int8_t>(output);
    const int n = in_shape.FlatSize();
    for (int i = 0, c = 0; i < n; ++i) {
        const int32_t v = p.input_offset + in[i];
        out[i] = v >= 0 ? positive[in[i] + 128]
                        : clamp(tflite::MultiplyByQuantizedMultiplier(v * (p.alpha_offset + a[c]),
                                                                      p.output_multiplier_2, p.output_shift_2));
        if (++c == channels) c = 0;
    }
    return kTfLiteOk;
}

}  // namespace

FastPreluResolver::FastPreluResolver(const tflite::MicroOpResolver& base)
    : base_(base), prelu_(tflite::micro::RegisterOp(tflite::PreluInit, tflite::PreluPrepare, FastPreluEval)) {}

const TfLiteRegistration* FastPreluResolver::FindOp(tflite::BuiltinOperator op) const {
    return op == tflite::BuiltinOperator_PRELU ? &prelu_ : base_.FindOp(op);
}
const TfLiteRegistration* FastPreluResolver::FindOp(const char* op) const { return base_.FindOp(op); }
tflite::TfLiteBridgeBuiltinParseFunction FastPreluResolver::GetOpDataParser(tflite::BuiltinOperator op) const {
    return base_.GetOpDataParser(op);
}
