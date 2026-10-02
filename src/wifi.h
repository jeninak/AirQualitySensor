#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stdint.h>

#define WIFI_CONNECT_ATTEMPTS 3
#define WIFI_RETRY_DELAY_SECONDS 2

int wifi_initialize(void);
int wifi_configure_credentials(void);
int wifi_connect(void);
int wifi_wait_for_connection(void);
int wifi_wait_for_dhcp(void);
uint8_t wifi_failure_code(int error);
bool wifi_is_connected(void);
int wifi_disconnect(void);

#endif
