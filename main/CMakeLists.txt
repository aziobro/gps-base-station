idf_component_register(
    SRCS
        "app_main.cpp"
        "base_station.cpp"
        "display.cpp"
        "local_caster.cpp"
        "log_buffer.cpp"
        "net_health.cpp"
        "ntrip_push.cpp"
        "rinex_logger.cpp"
        "sd_delete.cpp"
        "sd_manager.cpp"
        "storage.cpp"
        "survey.cpp"
        "ui.cpp"
        "um980.cpp"
        "wifi_manager.cpp"
        "web_server.cpp"
        "lv_mem_custom.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_driver_gpio
        esp_driver_uart
        esp_driver_spi
        esp_driver_sdspi
        sdmmc
        fatfs
        esp_event
        esp_http_server
        esp_https_server
        esp_netif
        esp_system
        esp_timer
        esp_wifi
        esp_hosted
        app_update
        mbedtls
        nvs_flash
        esp_lcd
    EMBED_FILES
        # C6 coprocessor firmware — update this file when rebuilding c6_update/slave.
        # Run: cp c6_update/slave/build/network_adapter.bin main/c6_slave_fw.bin
        "c6_slave_fw.bin"
    EMBED_TXTFILES
        "certs/server-cert.pem"
        "certs/server-key.pem"
        "certs/ca-cert.pem"
)

# Force the linker to include lv_mem_custom.c.obj from libmain.a.
# LVGL's custom-malloc mode calls these as external symbols; without -u they
# would be GC'd since no main-side code references them directly.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,lv_malloc_core"
    "-Wl,-u,lv_free_core"
    "-Wl,-u,lv_realloc_core"
    "-Wl,-u,lv_mem_init"
    "-Wl,-u,lv_mem_deinit"
)
