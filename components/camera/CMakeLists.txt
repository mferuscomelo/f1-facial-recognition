# espressif/usb_host_uvc and espressif/esp_jpeg come from idf_component.yml (the component manager
# adds them to this component's requirements). camera.cpp compiles to stubs unless
# CONFIG_APP_ENABLE_CAMERA=y, and the linker drops the unused USB/JPEG code in that case.
idf_component_register(SRCS "camera.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos
                       PRIV_REQUIRES esp_timer heap usb usb_host_uvc)
