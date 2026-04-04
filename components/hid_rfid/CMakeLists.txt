idf_component_register(
    SRCS
        "hid_rfid.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        usb_host_hid
        freertos
        esp_timer
)
# hid_host.h and hid_usage_keyboard.h are bundled as shim headers under
# components/hid_rfid/include/usb/ — no managed component needed.
