

#ifndef ETHERNET_MANAGER_H_
#define ETHERNET_MANAGER_H_

#include "esp_err.h"
#include "esp_netif.h"

// ======================= Конфигурация Ethernet =======================

// Использовать DHCP (1) или статический IP (0)
#define ETHERNET_USE_DHCP   0

// Статическая IP-конфигурация (если ETHERNET_USE_DHCP == 0)
#define ETHERNET_STATIC_IP      "192.168.154.189"
#define ETHERNET_NETMASK        "255.255.255.0"
#define ETHERNET_GATEWAY        "192.168.154.1"

// ======================= API =======================

// Инициализация Ethernet (создание netif, запуск драйвера)
esp_err_t ethernet_init(void);

// Завершение работы Ethernet (остановка драйвера)
esp_err_t ethernet_deinit(void);

// Проверить подключение
bool ethernet_is_connected(void);

// Получить текущую IP-конфигурацию
esp_netif_ip_info_t ethernet_get_ip_info(void);

#endif /* ETHERNET_MANAGER_H_ */






