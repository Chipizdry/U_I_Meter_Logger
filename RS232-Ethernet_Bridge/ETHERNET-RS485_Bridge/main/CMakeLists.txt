

idf_component_register(
    SRCS 
        "main.c"
        "littlefs_manager.c"
        "rs485_master.c"
        "ethernet_manager.c"
        "web_server.c"
        "ws_device_settings.c"
        "websocket_client.c"
        "websocket_events.c"
        "network_state.c"
        "nvs_settings.c"
        "wifi_manager.c"
        "gpio_manager.c"
        "ota_update.c"
        "ws_server.c"
        "web_auth.c"
        "ota_pull.c"
        "dns_server.c"
        "server_handlers.c"
        "modbus_tcp_client.c"
        

    INCLUDE_DIRS "."
    REQUIRES 
        esp_netif 
        esp_eth 
        lwip 
        json
        driver
        app_update 
        esp_wifi
        esp_event 
        esp_littlefs
        esp_system
        freertos
        nvs_flash
        esp_partition
        esp_https_ota
        esp_http_server
        esp_websocket_client
    EMBED_TXTFILES "ca_cert.pem"
)

littlefs_create_partition_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/data FLASH_IN_PROJECT)

# Получаем переменные из родительского CMakeLists.txt
if(DEFINED BUILD_NUMBER)
    message(STATUS "main: BUILD_NUMBER = ${BUILD_NUMBER}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE 
        BUILD_NUMBER=${BUILD_NUMBER}
        BUILD_DATE="${BUILD_DATE}"
    )
else()
    message(WARNING "main: BUILD_NUMBER not defined!")
endif()
