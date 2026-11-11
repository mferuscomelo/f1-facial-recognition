idf_component_register(SRCS "camera.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos
                       PRIV_REQUIRES heap usb usb_host_uvc)
