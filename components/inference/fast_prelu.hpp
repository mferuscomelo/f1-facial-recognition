#pragma once
#include "edge-impulse-sdk/tensorflow/lite/micro/micro_op_resolver.h"

// Wraps an op resolver (the list of operations Edge Impulse generated for a model) and swaps in
// a faster PReLU. Every other operation comes from `base` unchanged. See fast_prelu.cpp.
class FastPreluResolver : public tflite::MicroOpResolver {
public:
    explicit FastPreluResolver(const tflite::MicroOpResolver& base);
    const TfLiteRegistration* FindOp(tflite::BuiltinOperator op) const override;
    const TfLiteRegistration* FindOp(const char* op) const override;
    tflite::TfLiteBridgeBuiltinParseFunction GetOpDataParser(tflite::BuiltinOperator op) const override;

private:
    const tflite::MicroOpResolver& base_;
    TfLiteRegistration prelu_;
};
