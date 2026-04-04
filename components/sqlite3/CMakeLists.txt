set(COMPONENT_ADD_INCLUDEDIRS "include")

idf_component_register(SRCS "sqlite3.c" "esp32.c" "shox96_0_2.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       REQUIRES mbedtls spi_flash esp_rom
                       PRIV_REQUIRES console spiffs)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu99 -g3
    -fno-stack-protector -ffunction-sections -fdata-sections
    -fstrict-volatile-bitfields -mlongcalls 
    -Wpointer-arith
    # Suppress warnings unavoidable in the SQLite amalgamation
    -Wno-error=unused-value
    -Wno-error=unused-label
    -Wno-error=unused-function
    -Wno-error=unused-but-set-variable
    -Wno-error=unused-variable
    -Wno-error=deprecated-declarations
    -Wno-error=char-subscripts
    -Wno-error=maybe-uninitialized
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-old-style-declaration
    -Wno-error=comment
    -DNDEBUG
    # NOTE: Removed Arduino-era -DF_CPU=240000000L -DESP32 -DCORE_DEBUG_LEVEL=0
    # These are not needed and -DF_CPU / -DCORE_DEBUG_LEVEL are Arduino macros.
)
