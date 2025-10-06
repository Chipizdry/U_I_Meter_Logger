idf_component_register(SRCS "main.c"
                             "littlefs_manager.c"
                             "rs485_master.c"
                             "ethernet_manager.c"
                             "web_server.c"
                             "websocket_client.c"

                    INCLUDE_DIRS "."
                    REQUIRES 
                        esp_netif 
                        esp_eth 
                        lwip 
                        driver 
                        esp_event 
                        esp_littlefs
                        esp_system
                        freertos
                        esp_http_server
                        esp_websocket_client)


                        littlefs_create_partition_image( storage ${CMAKE_CURRENT_SOURCE_DIR}/data FLASH_IN_PROJECT)
