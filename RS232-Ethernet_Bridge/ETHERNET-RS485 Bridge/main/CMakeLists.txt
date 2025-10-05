idf_component_register(SRCS "main.c"
                             "littlefs_manager.c"
                             "rs485_master.c"
                    INCLUDE_DIRS "."
                    REQUIRES 
                        esp_netif 
                        esp_eth 
                        lwip 
                        driver 
                        esp_event 
                        esp_littlefs
                        esp_system
                        freertos)