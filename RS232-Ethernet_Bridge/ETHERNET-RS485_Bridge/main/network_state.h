

#pragma once
#include <stdbool.h>

typedef enum {

    NET_STATE_WIFI_DOWN = 0,
    NET_STATE_ETHERNET_DOWN,
    NET_STATE_WIFI_CONNECTING,
    NET_STATE_WIFI_UP,
    NET_STATE_ETHERNET_CONNECTING,
    NET_STATE_ETHERNET_UP
} net_state_t;

void network_state_init(void);

void network_set_wifi_state(net_state_t state);
void network_set_ethernet_state(net_state_t state);
net_state_t network_get_wifi_state(void);
net_state_t network_get_ethernet_state(void);

const char* net_state_to_str(net_state_t state);
void network_notify_ws(void);