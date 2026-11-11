// Used instead of ei/tflite-model/trained_model_ops_define.h when the face recognition model is built in.
//
// Each Edge Impulse export lists the kernel variants its own model does not need
// (EI_TFLITE_DISABLE_...), to save flash. With two models in one firmware, the detector's list
// would switch off kernels the recognition model needs (for example int8 FULLY_CONNECTED), so
// this empty file keeps every kernel available.
#pragma once
