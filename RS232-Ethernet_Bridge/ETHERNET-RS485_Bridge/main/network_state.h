

#pragma once
#include <stdbool.h>

typedef enum {
    NET_STATE_DOWN = 0,
    NET_STATE_CONNECTING,
    NET_STATE_UP
} net_state_t;

void network_state_init(void);

void network_set_state(net_state_t state);
net_state_t network_get_state(void);

