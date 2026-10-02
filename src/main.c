#include <stdio.h>
#include <stdint.h>
#include <errno.h>

#include <zephyr/kernel.h>

#include "led.h"
#include "sensor.h"
#include "telemetry.h"
#include "wifi.h"

#define SAMPLE_INTERVAL_MS 5000

static int connect_to_wifi(void)
{
    int setup_attempt = 0;

    while (true) {
        setup_attempt++;
        uint8_t failure_code = 4;

        printf("\nWi-Fi setup attempt %d (retry group %d/%d)\n",
               setup_attempt,
               ((setup_attempt - 1) % WIFI_CONNECT_ATTEMPTS) + 1,
               WIFI_CONNECT_ATTEMPTS);

        int ret = wifi_connect();

        if (ret == 0) {
            ret = wifi_wait_for_connection();

            if (ret != 0) {
                failure_code = wifi_failure_code(ret);
            }
        }

        if (ret == 0) {
            ret = wifi_wait_for_dhcp();

            if (ret != 0) {
                failure_code = 5;
            }
        }

        if (ret == 0) {
            return 0;
        }

        printf("Wi-Fi setup attempt %d failed: %d. Retrying in %d seconds.\n",
               setup_attempt, ret, WIFI_RETRY_DELAY_SECONDS);

        int disconnect_ret = wifi_disconnect();

        if (disconnect_ret != 0) {
            printf("Wi-Fi disconnect before retry returned: %d\n",
                   disconnect_ret);
        }

        printf("Wi-Fi failure LED code: %u red pulses.\n", failure_code);
        led_failure_code(failure_code);
        k_sleep(K_SECONDS(WIFI_RETRY_DELAY_SECONDS));
    }
}

static int reconnect_to_wifi(void)
{
    printf("Wi-Fi is not connected. Reconnecting...\n");
    led_error();

    int reconnect_ret = -ETIMEDOUT;

    for (int attempt = 1; attempt <= WIFI_CONNECT_ATTEMPTS; attempt++) {
        printf("Reconnection attempt %d/%d\n",
               attempt, WIFI_CONNECT_ATTEMPTS);

        reconnect_ret = wifi_connect();

        if (reconnect_ret == 0) {
            reconnect_ret = wifi_wait_for_connection();
        }

        if (reconnect_ret == 0) {
            reconnect_ret = wifi_wait_for_dhcp();
        }

        if (reconnect_ret == 0) {
            return 0;
        }

        if (attempt < WIFI_CONNECT_ATTEMPTS) {
            led_error();
            k_sleep(K_SECONDS(WIFI_RETRY_DELAY_SECONDS));
        }
    }

    printf("Wi-Fi reconnect failed: %d\n", reconnect_ret);
    return reconnect_ret;
}

int main(void)
{
    if (!led_is_ready()) {
        printf("ERROR: Thingy:53 RGB LED is not ready.\n");
        return 0;
    }

    led_all_off();
    wifi_initialize();

    printf("\nWaiting for Wi-Fi interface...\n");
    k_sleep(K_SECONDS(2));

    int ret = wifi_configure_credentials();

    if (ret != 0) {
        printf("ERROR: Wi-Fi credential setup failed: %d\n", ret);
        led_error();
        return 0;
    }

    connect_to_wifi();
    printf("Wi-Fi and network connection are ready.\n");

    if (sensor_initialize() != 0) {
        led_error();
        return 0;
    }

    printf("Sampling every %d seconds.\n\n", SAMPLE_INTERVAL_MS / 1000);

    while (true) {
        if (!wifi_is_connected() && reconnect_to_wifi() != 0) {
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        int64_t sample_start_ms = k_uptime_get();
        struct sensor_readings readings;

        ret = sensor_read(&readings);

        if (ret < 0) {
            led_error();
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        sensor_print_readings(&readings);

        uint8_t failure_code = 0;
        ret = telemetry_send(&readings, &failure_code);

        if (ret < 0) {
            printf("Telemetry transmission failed: %d\n", ret);

            if (failure_code > 0) {
                led_failure_code(failure_code);
            } else {
                led_error();
            }
        } else {
            printf("Telemetry transmission completed.\n");
            led_success();
        }

        k_sleep(K_SECONDS(2));
        led_all_off();

        int64_t elapsed_ms = k_uptime_get() - sample_start_ms;

        if (elapsed_ms < SAMPLE_INTERVAL_MS) {
            k_msleep(SAMPLE_INTERVAL_MS - (int32_t)elapsed_ms);
        }
    }

    return 0;
}
