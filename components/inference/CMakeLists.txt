# Edge Impulse C++ library export, built as an ESP-IDF component with ESP-NN.
#
# Based on Edge Impulse's official ESP-IDF example:
#   https://github.com/edgeimpulse/example-standalone-inferencing-espressif-esp32
#   (main/CMakeLists.txt @ c1176b9be30885c5ab6ca1c7d42b8a9731ba8bb2)
# Drop the export's edge-impulse-sdk/, model-parameters/, tflite-model/ into ./ei/.
#
# Deviation from the example (deliberate): the example globs "*.s" for the ESP-NN /
# esp-dsp assembly, but the SDK ships them as "*.S". CMake's GLOB is case-sensitive on
# Linux, so the example's glob matches 0 files there (verified with cmake -P). We glob
# "*.S" so the ESP32-S3 SIMD kernels are actually compiled.

set(EI_DIR ${CMAKE_CURRENT_LIST_DIR}/ei)
set(EI_SDK_FOLDER ${EI_DIR}/edge-impulse-sdk)

set(srcs "")
set(incs "include")

if(CONFIG_APP_ENABLE_INFERENCE)
    if(NOT EXISTS ${EI_SDK_FOLDER} OR NOT EXISTS ${EI_DIR}/model-parameters OR NOT EXISTS ${EI_DIR}/tflite-model)
        message(FATAL_ERROR "CONFIG_APP_ENABLE_INFERENCE=y but no Edge Impulse export in ${EI_DIR}. See components/inference/README.md")
    endif()

    include(${EI_SDK_FOLDER}/cmake/utils.cmake)
    RECURSIVE_FIND_FILE_EXCLUDE_DIR(EI_CPP   "${EI_SDK_FOLDER}" "CMSIS" "*.cpp")
    RECURSIVE_FIND_FILE_EXCLUDE_DIR(EI_CC    "${EI_SDK_FOLDER}" "CMSIS" "*.cc")
    RECURSIVE_FIND_FILE_EXCLUDE_DIR(EI_C     "${EI_SDK_FOLDER}" "CMSIS" "*.c")
    RECURSIVE_FIND_FILE_EXCLUDE_DIR(EI_MODEL "${EI_DIR}/tflite-model" "CMSIS" "*.cpp")
    RECURSIVE_FIND_FILE_EXCLUDE_DIR(S_ESP_NN  "${EI_SDK_FOLDER}/porting/espressif/ESP-NN" "CMSIS" "*.S")
    RECURSIVE_FIND_FILE_EXCLUDE_DIR(S_ESP_DSP "${EI_SDK_FOLDER}/porting/espressif/esp-dsp/modules/fft" "CMSIS" "*.S")

    list(APPEND srcs "inference.cpp" ${EI_CPP} ${EI_CC} ${EI_C} ${EI_MODEL} ${S_ESP_NN} ${S_ESP_DSP})
    list(APPEND incs
        "ei"
        "ei/tflite-model"
        "ei/model-parameters"
        "ei/edge-impulse-sdk"
        "ei/edge-impulse-sdk/porting/espressif/esp-dsp/modules/fft/include"
        "ei/edge-impulse-sdk/porting/espressif/esp-dsp/modules/common/include")
endif()

# Public include dir is only "include" (inference.hpp has no EI types); EI dirs are private.
list(REMOVE_ITEM incs "include")
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ${incs}
                       PRIV_REQUIRES esp_timer esp_hw_support heap)

if(CONFIG_APP_ENABLE_INFERENCE)
    # Same defines as the example's main/CMakeLists.txt (ESP-NN + ESP-DSP; S3 kernels on esp32s3).
    # Must be on the command line (not only via ei_classifier_config.h) because the .S files test them.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1
        EIDSP_USE_ESP_DSP=1)
    if(IDF_TARGET STREQUAL "esp32s3")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1)
    endif()
    # Warning suppressions from the example's top-level CMakeLists.txt, scoped to this component.
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-unused-variable -Wno-maybe-uninitialized -Wno-error=format= -Wno-unused-but-set-parameter
        -Wno-error=nonnull-compare -Wno-error=stringop-truncation -Wno-error=uninitialized
        -Wno-stringop-truncation -Wno-deprecated-declarations -fno-if-conversion
        -Wno-error=stringop-overflow= -Wno-error=missing-field-initializers)
    target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
endif()
