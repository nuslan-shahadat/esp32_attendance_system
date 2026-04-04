idf_component_register(
    SRCS
        "db.c"
        "db_schema.c"
        "db_students.c"
        "db_attendance.c"
        "db_config.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        sqlite3          # siara-cc/esp32-idf-sqlite3 component
        spiffs
        fatfs
        wear_levelling
        driver
        esp_timer
        lwip             # for sntp
        esp_netif
)
