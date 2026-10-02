#ifndef LED_H
#define LED_H

#include <stdbool.h>
#include <stdint.h>

bool led_is_ready(void);
void led_all_off(void);
void led_error(void);
void led_failure_code(uint8_t code);
void led_wifi_connecting(void);
void led_wifi_connecting_blink(bool blue_on);
void led_waiting_network(void);
void led_network_ready(void);
void led_dns_lookup(void);
void led_https_connecting(void);
void led_http_posting(void);
void led_success(void);

#endif
