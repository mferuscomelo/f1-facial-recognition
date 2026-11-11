# Builds the Edge Impulse C++ library export (in ./ei) together with ESP-NN, Espressif's
# optimised neural-network kernels for the ESP32-S3.
# Based on github.com/edgeimpulse/example-standalone-inferencing-espressif-esp32.
set(EI ${CMAKE_CURRENT_LIST_DIR}/ei)
set(SDK ${EI}/edge-impulse-sdk)
if(NOT EXISTS ${SDK} OR NOT EXISTS ${EI}/model-parameters OR NOT EXISTS ${EI}/tflite-model)
    message(FATAL_ERROR "No Edge Impulse model in ${EI}. See docs/deploy-to-f1.md")
endif()

include(${SDK}/cmake/utils.cmake)
RECURSIVE_FIND_FILE_EXCLUDE_DIR(SDK_CPP "${SDK}" "CMSIS" "*.cpp")
RECURSIVE_FIND_FILE_EXCLUDE_DIR(SDK_CC  "${SDK}" "CMSIS" "*.cc")
RECURSIVE_FIND_FILE_EXCLUDE_DIR(SDK_C   "${SDK}" "CMSIS" "*.c")
RECURSIVE_FIND_FILE_EXCLUDE_DIR(MODEL   "${EI}/tflite-model" "CMSIS" "*.cpp")
# Assembly kernels: the SDK names them *.S (upper case); the official example globs *.s,
# which matches nothing on Linux.
RECURSIVE_FIND_FILE_EXCLUDE_DIR(ESP_NN  "${SDK}/porting/espressif/ESP-NN" "CMSIS" "*.S")
RECURSIVE_FIND_FILE_EXCLUDE_DIR(ESP_DSP "${SDK}/porting/espressif/esp-dsp/modules/fft" "CMSIS" "*.S")

idf_component_register(
    SRCS "inference.cpp" ${SDK_CPP} ${SDK_CC} ${SDK_C} ${MODEL} ${ESP_NN} ${ESP_DSP}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "ei" "ei/tflite-model" "ei/model-parameters" "ei/edge-impulse-sdk"
                      "ei/edge-impulse-sdk/porting/espressif/esp-dsp/modules/fft/include"
                      "ei/edge-impulse-sdk/porting/espressif/esp-dsp/modules/common/include"
    PRIV_REQUIRES esp_timer heap)

# Turn on the ESP32-S3 optimised kernels (the .S files check these, so they must be compiler flags).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1 EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1 EIDSP_USE_ESP_DSP=1)
# The SDK is third-party code: silence its warnings (same list as the official example).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable -Wno-maybe-uninitialized -Wno-error=format= -Wno-unused-but-set-parameter
    -Wno-error=nonnull-compare -Wno-error=stringop-truncation -Wno-error=uninitialized
    -Wno-stringop-truncation -Wno-deprecated-declarations -fno-if-conversion
    -Wno-error=stringop-overflow= -Wno-error=missing-field-initializers
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
