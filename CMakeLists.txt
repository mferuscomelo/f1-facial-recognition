cmake_minimum_required(VERSION 3.16)
# Secrets (e.g. CONFIG_APP_PUBLISH_URL) go in the gitignored sdkconfig.defaults.local
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(f1-facecam)
