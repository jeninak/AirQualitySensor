#include <zephyr/kernel.h>
#include <zephyr/drivers/led.h>
#include <zephyr/devicetree.h>

#include "led.h"

#define WIFI_FAILURE_PATTERN_CYCLES 3

static const struct led_dt_spec led_red =
    LED_DT_SPEC_GET(DT_ALIAS(led0));

static const struct led_dt_spec led_green =
    LED_DT_SPEC_GET(DT_ALIAS(led1));

static const struct led_dt_spec led_blue =
    LED_DT_SPEC_GET(DT_ALIAS(led2));

bool led_is_ready(void)
{
    return led_is_ready_dt(&led_red) &&
           led_is_ready_dt(&led_green) &&
           led_is_ready_dt(&led_blue);
}

void led_all_off(void)
{
    led_off_dt(&led_red);
    led_off_dt(&led_green);
    led_off_dt(&led_blue);
}

void led_wifi_connecting(void)
{
    led_all_off();
    led_on_dt(&led_blue);
}

void led_wifi_connecting_blink(bool blue_on)
{
    if (blue_on) {
        led_on_dt(&led_blue);
    } else {
        led_all_off();
    }
}

void led_error(void)
{
    led_all_off();
    led_on_dt(&led_red);
}

void led_failure_code(uint8_t code)
{
    for (int cycle = 0; cycle < WIFI_FAILURE_PATTERN_CYCLES; cycle++) {
        for (uint8_t pulse = 0; pulse < code; pulse++) {
            led_on_dt(&led_red);
            k_msleep(250);
            led_all_off();
            k_msleep(250);
        }

        k_sleep(K_MSEC(1000));
    }
}

void led_waiting_network(void)
{
    led_all_off();
    led_on_dt(&led_red);
    led_on_dt(&led_green);
}

void led_network_ready(void)
{
    led_all_off();
    led_on_dt(&led_green);
    led_on_dt(&led_blue);
}

void led_dns_lookup(void)
{
    led_all_off();
    led_on_dt(&led_blue);
}

void led_https_connecting(void)
{
    led_all_off();
    led_on_dt(&led_red);
    led_on_dt(&led_blue);
}

void led_http_posting(void)
{
    led_all_off();
    led_on_dt(&led_red);
    led_on_dt(&led_green);
}

void led_success(void)
{
    led_all_off();
    led_on_dt(&led_green);
}
