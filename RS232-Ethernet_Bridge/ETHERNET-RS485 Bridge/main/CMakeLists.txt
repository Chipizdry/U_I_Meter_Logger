idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES 
                        esp_netif 
                        esp_eth 
                        lwip 
                        driver 
                        esp_event 
                        esp_system
                        freertos)