#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/led.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi_credentials.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>

#include <drivers/bme68x_iaq.h>

#include "wifi_config.h"

#define SAMPLE_INTERVAL_MS 5000
#define WIFI_TIMEOUT_SECONDS 30
#define DHCP_TIMEOUT_SECONDS 15
#define WIFI_CONNECT_ATTEMPTS 3
#define WIFI_RETRY_DELAY_SECONDS 2

#define TELEMETRY_HOST "www.cc.puv.fi"
#define TELEMETRY_PORT "443"
#define TELEMETRY_PATH "/~e2301774/Dashboard/telemetry.php"

// Thingy:53 LD1 RGB LED
// led0 = red
// led1 = green
// led2 = blue

static const struct led_dt_spec led_red =
    LED_DT_SPEC_GET(DT_ALIAS(led0));

static const struct led_dt_spec led_green =
    LED_DT_SPEC_GET(DT_ALIAS(led1));

static const struct led_dt_spec led_blue =
    LED_DT_SPEC_GET(DT_ALIAS(led2));

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback net_cb;

static volatile bool wifi_connected = false;
static volatile bool wifi_connect_result_received = false;
static volatile bool dhcp_bound = false;
static volatile int wifi_connect_status = -1;

static volatile bool http_response_received = false;
static volatile uint16_t http_status_code = 0;

// Turn off all RGB channels
static void led_all_off(void)
{
    led_off_dt(&led_red);
    led_off_dt(&led_green);
    led_off_dt(&led_blue);
}

// Blue = Wi-Fi connection attempt
static void led_wifi_connecting(void)
{
    led_all_off();
    led_on_dt(&led_blue);
}

// Red = connection or telemetry failure
static void led_error(void)
{
    led_all_off();
    led_on_dt(&led_red);
}

// Yellow = Wi-Fi connected, waiting for DHCP
static void led_waiting_network(void)
{
    led_all_off();
    led_on_dt(&led_red);
    led_on_dt(&led_green);
}

// Purple = network ready, HTTPS telemetry in progress
static void led_sending(void)
{
    led_all_off();
    led_on_dt(&led_red);
    led_on_dt(&led_blue);
}

// Green = telemetry successfully accepted by server
static void led_success(void)
{
    led_all_off();
    led_on_dt(&led_green);
}

// Wi-Fi event handler
static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t event,
                               struct net_if *iface)
{
    ARG_UNUSED(iface);

    if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
        const struct wifi_status *status =
            (const struct wifi_status *)cb->info;

        wifi_connect_result_received = true;

        if (status == NULL) {
            wifi_connect_status = -EINVAL;
            wifi_connected = false;

            printf(
                "Wi-Fi connection result received without status.\n"
            );

            led_error();

            return;
        }

        wifi_connect_status = status->status;

        if (status->status == WIFI_STATUS_CONN_SUCCESS) {
            wifi_connected = true;

            printf(
                "Wi-Fi connected successfully.\n"
            );

            led_waiting_network();
        } else {
            wifi_connected = false;

            printf(
                "Wi-Fi connection failed. Status: %d\n",
                status->status
            );

            if (status->status == WIFI_STATUS_CONN_WRONG_PASSWORD) {
                printf("Reason: WRONG PASSWORD\n");
            } else if (status->status == WIFI_STATUS_CONN_TIMEOUT) {
                printf("Reason: CONNECTION TIMEOUT\n");
            } else if (status->status == WIFI_STATUS_CONN_AP_NOT_FOUND) {
                printf("Reason: ACCESS POINT NOT FOUND\n");
            } else {
                printf("Reason: UNKNOWN WIFI ERROR\n");
            }

            led_error();
        }
    }

    if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        wifi_connected = false;
        dhcp_bound = false;

        printf(
            "Wi-Fi disconnected.\n"
        );

        led_error();
    }
}

// Network event handler
static void net_event_handler(struct net_mgmt_event_callback *cb,
                              uint64_t event,
                              struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (event == NET_EVENT_IPV4_DHCP_BOUND) {
        dhcp_bound = true;

        printf(
            "DHCP completed successfully.\n"
        );

        led_sending();
    }
}

// Store the configured Wi-Fi credentials
static int wifi_store_credentials(void)
{
    bool ssid_configured = WIFI_SSID[0] != '\0';
    bool password_configured = WIFI_PASSWORD[0] != '\0';

    if (!ssid_configured && !password_configured) {
        if (wifi_credentials_is_empty()) {
            printf(
                "ERROR: No Wi-Fi credentials are configured or saved on this device.\n"
            );

            led_error();

            return -ENOENT;
        }

        printf(
            "Using Wi-Fi credentials saved in device settings.\n"
        );

        return 0;
    }

    if (!ssid_configured || !password_configured) {
        printf(
            "ERROR: Both WIFI_SSID and WIFI_PASSWORD must be set, or both left empty to use saved credentials.\n"
        );

        led_error();

        return -EINVAL;
    }

    int ret = wifi_credentials_set_personal(
        WIFI_SSID,
        strlen(WIFI_SSID),
        WIFI_SECURITY_TYPE_PSK,
        NULL,
        0,
        WIFI_PASSWORD,
        strlen(WIFI_PASSWORD),
        0,
        WIFI_CHANNEL_ANY,
        WIFI_TIMEOUT_SECONDS
    );

    if (ret != 0) {
        printf(
            "ERROR: Failed to store Wi-Fi credentials: %d\n",
            ret
        );

        led_error();

        return ret;
    }

    printf(
        "Wi-Fi credentials stored successfully.\n"
    );

    return 0;
}

// Connect to the stored Wi-Fi network
static int wifi_connect(void)
{
    struct net_if *iface = net_if_get_first_wifi();

    if (iface == NULL) {
        printf(
            "ERROR: No Wi-Fi interface found.\n"
        );

        led_error();

        return -ENODEV;
    }

    wifi_connected = false;
    wifi_connect_result_received = false;
    wifi_connect_status = -1;
    dhcp_bound = false;

    led_wifi_connecting();

    printf(
        "Connecting to stored Wi-Fi network...\n"
    );

    int ret = net_mgmt(
        NET_REQUEST_WIFI_CONNECT_STORED,
        iface,
        NULL,
        0
    );

    printf(
        "Wi-Fi connection request returned: %d\n",
        ret
    );

    if (ret != 0 && ret != -EALREADY) {
        printf(
            "Wi-Fi connection request failed: %d\n",
            ret
        );

        led_error();

        return ret;
    }

    printf(
        "Wi-Fi connection request sent.\n"
    );

    return 0;
}

// Wait for the Wi-Fi connection result
//
// Blue LED blinks while waiting.
// This lets us distinguish an active connection attempt
// from a firmware state that is simply stuck.
static int wait_for_wifi_connection(void)
{
    printf(
        "Waiting for Wi-Fi connection result...\n"
    );

    bool blue_on = true;

    for (int seconds = 0;
         seconds < WIFI_TIMEOUT_SECONDS;
         seconds++) {

        if (wifi_connect_result_received) {
            break;
        }

        if (blue_on) {
            led_on_dt(&led_blue);
        } else {
            led_all_off();
        }

        blue_on = !blue_on;

        k_sleep(K_SECONDS(1));
    }

    if (!wifi_connect_result_received) {
        printf(
            "ERROR: Wi-Fi connection timed out after %d seconds.\n",
            WIFI_TIMEOUT_SECONDS
        );

        led_error();

        return -ETIMEDOUT;
    }

    if (!wifi_connected) {
        printf(
            "ERROR: Wi-Fi connection failed with status %d.\n",
            wifi_connect_status
        );

        led_error();

        return -ECONNREFUSED;
    }

    printf(
        "Wi-Fi connection confirmed.\n"
    );

    led_waiting_network();

    return 0;
}

// Wait for DHCP to provide network connectivity
static int wait_for_dhcp(void)
{
    printf(
        "Waiting for DHCP...\n"
    );

    led_waiting_network();

    for (int seconds = 0;
         seconds < DHCP_TIMEOUT_SECONDS;
         seconds++) {

        if (dhcp_bound) {
            printf(
                "Network is ready.\n"
            );

            led_sending();

            return 0;
        }

        k_sleep(K_SECONDS(1));
    }

    printf(
        "ERROR: DHCP did not complete within %d seconds.\n",
        DHCP_TIMEOUT_SECONDS
    );

    led_error();

    return -ETIMEDOUT;
}

// HTTP response callback
static int http_response_callback(
    struct http_response *response,
    enum http_final_call final_data,
    void *user_data)
{
    ARG_UNUSED(user_data);

    if (response == NULL) {
        printf(
            "ERROR: HTTP response was NULL.\n"
        );

        return 0;
    }

    if (final_data == HTTP_DATA_FINAL) {
        http_response_received = true;
        http_status_code = response->http_status_code;

        printf(
            "HTTP response received. Status code: %u\n",
            response->http_status_code
        );

        printf(
            "HTTP status: %s\n",
            response->http_status
        );
    }

    return 0;
}

// Send sensor data to the school server
//
// HTTPS encryption is enabled, but certificate verification is
// disabled for this first connection test.
static int send_telemetry(
    double temperature,
    double humidity,
    double pressure,
    int iaq,
    int iaq_accuracy,
    double co2,
    double voc,
    int gas_stability)
{
    char payload[512];

    int payload_len = snprintf(
        payload,
        sizeof(payload),
        "{"
        "\"temperature\":%.2f,"
        "\"humidity\":%.2f,"
        "\"pressure\":%.2f,"
        "\"iaq\":%d,"
        "\"iaq_accuracy\":%d,"
        "\"co2\":%.1f,"
        "\"voc\":%.2f,"
        "\"gas_stability\":%d"
        "}",
        temperature,
        humidity,
        pressure,
        iaq,
        iaq_accuracy,
        co2,
        voc,
        gas_stability
    );

    if (payload_len < 0 || payload_len >= sizeof(payload)) {
        printf(
            "ERROR: Telemetry payload too large.\n"
        );

        led_error();

        return -ENOMEM;
    }

    printf(
        "Sending telemetry...\n"
    );

    led_sending();

    // Resolve the server hostname
    struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TLS_1_2,
    };

    struct zsock_addrinfo *res = NULL;

    int ret = zsock_getaddrinfo(
        TELEMETRY_HOST,
        TELEMETRY_PORT,
        &hints,
        &res
    );

    if (ret != 0 || res == NULL) {
        printf(
            "ERROR: DNS lookup failed: %d\n",
            ret
        );

        led_error();

        return -EHOSTUNREACH;
    }

    printf(
        "DNS lookup successful.\n"
    );

    // Create a TLS 1.2 socket
    int sock = zsock_socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TLS_1_2
    );

    if (sock < 0) {
        int err = errno;

        printf(
            "ERROR: Could not create TLS socket: %d\n",
            err
        );

        zsock_freeaddrinfo(res);

        led_error();

        return -err;
    }

    printf(
        "TLS socket created.\n"
    );

    // Disable certificate verification for the first connection test
    int verify = ZSOCK_TLS_PEER_VERIFY_NONE;

    ret = zsock_setsockopt(
        sock,
        ZSOCK_SOL_TLS,
        ZSOCK_TLS_PEER_VERIFY,
        &verify,
        sizeof(verify)
    );

    if (ret < 0) {
        int err = errno;

        printf(
            "ERROR: Could not configure TLS verification: %d\n",
            err
        );

        zsock_close(sock);
        zsock_freeaddrinfo(res);

        led_error();

        return -err;
    }

    // Tell TLS which hostname we are connecting to
    ret = zsock_setsockopt(
        sock,
        ZSOCK_SOL_TLS,
        ZSOCK_TLS_HOSTNAME,
        TELEMETRY_HOST,
        strlen(TELEMETRY_HOST) + 1
    );

    if (ret < 0) {
        int err = errno;

        printf(
            "ERROR: Could not set TLS hostname: %d\n",
            err
        );

        zsock_close(sock);
        zsock_freeaddrinfo(res);

        led_error();

        return -err;
    }

    // Connect to the HTTPS server
    printf(
        "Connecting to HTTPS server...\n"
    );

    ret = zsock_connect(
        sock,
        res->ai_addr,
        res->ai_addrlen
    );

    zsock_freeaddrinfo(res);

    if (ret < 0) {
        int err = errno;

        printf(
            "ERROR: HTTPS connection failed: %d\n",
            err
        );

        zsock_close(sock);

        led_error();

        return -err;
    }

    printf(
        "HTTPS connection established.\n"
    );

    // HTTP response buffer
    static uint8_t recv_buf[512];

    http_response_received = false;
    http_status_code = 0;

    // Create HTTP POST request
    struct http_request request = {0};

    request.method = HTTP_POST;
    request.url = TELEMETRY_PATH;
    request.host = TELEMETRY_HOST;
    request.port = TELEMETRY_PORT;
    request.protocol = "HTTP/1.1";

    request.content_type_value = "application/json";

    request.payload = payload;
    request.payload_len = payload_len;

    request.recv_buf = recv_buf;
    request.recv_buf_len = sizeof(recv_buf);

    request.response = http_response_callback;

    // Send HTTP POST request
    printf(
        "Sending HTTPS POST request...\n"
    );

    ret = http_client_req(
        sock,
        &request,
        10000,
        NULL
    );

    if (ret < 0) {
        printf(
            "ERROR: HTTP POST failed: %d\n",
            ret
        );

        zsock_close(sock);

        led_error();

        return ret;
    }

    printf(
        "HTTP request completed.\n"
    );

    zsock_close(sock);

    // Only consider telemetry successful when the server
    // actually returned HTTP 200.
    if (!http_response_received) {
        printf(
            "ERROR: No final HTTP response received.\n"
        );

        led_error();

        return -EIO;
    }

    if (http_status_code != 200) {
        printf(
            "ERROR: Server returned HTTP status %u.\n",
            http_status_code
        );

        led_error();

        return -EIO;
    }

    printf(
        "Telemetry accepted by server.\n"
    );

    led_success();

    return 0;
}

int main(void)
{
    const struct device *bme688 =
        DEVICE_DT_GET(DT_NODELABEL(bme688));

    // Check LD1 RGB LED
    if (!led_is_ready_dt(&led_red) ||
        !led_is_ready_dt(&led_green) ||
        !led_is_ready_dt(&led_blue)) {

        printf(
            "ERROR: Thingy:53 RGB LED is not ready.\n"
        );

        return 0;
    }

    // Start with LED off
    led_all_off();

    // Wi-Fi event callback
    net_mgmt_init_event_callback(
        &wifi_cb,
        wifi_event_handler,
        NET_EVENT_WIFI_CONNECT_RESULT |
        NET_EVENT_WIFI_DISCONNECT_RESULT
    );

    net_mgmt_add_event_callback(&wifi_cb);

    // DHCP event callback
    net_mgmt_init_event_callback(
        &net_cb,
        net_event_handler,
        NET_EVENT_IPV4_DHCP_BOUND
    );

    net_mgmt_add_event_callback(&net_cb);

    // Wait for Wi-Fi interface
    printf(
        "\nWaiting for Wi-Fi interface...\n"
    );

    k_sleep(K_SECONDS(2));

    // Store the configured Wi-Fi credentials
    int credential_ret = wifi_store_credentials();

    if (credential_ret != 0) {
        printf(
            "ERROR: Wi-Fi credential setup failed: %d\n",
            credential_ret
        );

        led_error();

        return 0;
    }

    // Keep retrying so temporary AP or DHCP outages do not stop telemetry.
    int setup_attempt = 0;

    while (true) {
        setup_attempt++;

        printf(
            "\nWi-Fi setup attempt %d (retry group %d/%d)\n",
            setup_attempt,
            ((setup_attempt - 1) % WIFI_CONNECT_ATTEMPTS) + 1,
            WIFI_CONNECT_ATTEMPTS
        );

        int network_ret = wifi_connect();

        if (network_ret == 0) {
            network_ret = wait_for_wifi_connection();
        }

        if (network_ret == 0) {
            network_ret = wait_for_dhcp();
        }

        if (network_ret == 0) {
            break;
        }

        printf(
            "Wi-Fi setup attempt %d failed: %d. Retrying in %d seconds.\n",
            setup_attempt,
            network_ret,
            WIFI_RETRY_DELAY_SECONDS
        );

        struct net_if *iface = net_if_get_first_wifi();

        if (iface != NULL && wifi_connected) {
            int disconnect_ret = net_mgmt(
                NET_REQUEST_WIFI_DISCONNECT,
                iface,
                NULL,
                0
            );

            if (disconnect_ret != 0) {
                printf(
                    "Wi-Fi disconnect before retry returned: %d\n",
                    disconnect_ret
                );
            }
        }

        led_error();
        k_sleep(K_SECONDS(WIFI_RETRY_DELAY_SECONDS));
    }

    printf(
        "Wi-Fi and network connection are ready.\n"
    );

    // Check BME688
    if (!device_is_ready(bme688)) {
        printf("\n");
        printf(
            "ERROR: BME688 sensor is not ready!\n"
        );

        led_error();

        return 0;
    }

    printf("\n");
    printf(
        "========================================\n"
    );

    printf(
        " Thingy:53 BME688 Home Environment Monitor\n"
    );

    printf(
        "========================================\n"
    );

    printf(
        "BME688 initialized successfully.\n"
    );

    printf(
        "Sampling every %d seconds.\n\n",
        SAMPLE_INTERVAL_MS / 1000
    );

    while (1) {
        // Make sure Wi-Fi is still connected before sending telemetry
        if (!wifi_connected) {
            printf(
                "Wi-Fi is not connected. Reconnecting...\n"
            );

            led_error();

            int reconnect_ret = -ETIMEDOUT;

            for (int attempt = 1;
                 attempt <= WIFI_CONNECT_ATTEMPTS;
                 attempt++) {

                printf(
                    "Reconnection attempt %d/%d\n",
                    attempt,
                    WIFI_CONNECT_ATTEMPTS
                );

                reconnect_ret = wifi_connect();

                if (reconnect_ret == 0) {
                    reconnect_ret =
                        wait_for_wifi_connection();
                }

                if (reconnect_ret == 0) {
                    reconnect_ret =
                        wait_for_dhcp();
                }

                if (reconnect_ret == 0) {
                    break;
                }

                if (attempt < WIFI_CONNECT_ATTEMPTS) {
                    led_error();

                    k_sleep(
                        K_SECONDS(WIFI_RETRY_DELAY_SECONDS)
                    );
                }
            }

            if (reconnect_ret != 0) {
                printf(
                    "Wi-Fi reconnect failed: %d\n",
                    reconnect_ret
                );

                k_msleep(SAMPLE_INTERVAL_MS);

                continue;
            }
        }

        int64_t sample_start_ms = k_uptime_get();

        int ret = sensor_sample_fetch(bme688);

        if (ret < 0) {
            printf(
                "ERROR: Sensor fetch failed: %d\n",
                ret
            );

            led_error();

            k_msleep(SAMPLE_INTERVAL_MS);

            continue;
        }

        struct sensor_value temperature;
        struct sensor_value humidity;
        struct sensor_value pressure;
        struct sensor_value iaq;
        struct sensor_value iaq_accuracy;
        struct sensor_value co2;
        struct sensor_value voc;
        struct sensor_value gas_run_in;
        struct sensor_value gas_stab;

        // Temperature
        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_AMBIENT_TEMP,
            &temperature
        );

        if (ret < 0) {
            printf(
                "ERROR: Failed to read temperature: %d\n",
                ret
            );

            led_error();

            k_msleep(SAMPLE_INTERVAL_MS);

            continue;
        }

        // Humidity
        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_HUMIDITY,
            &humidity
        );

        if (ret < 0) {
            printf(
                "ERROR: Failed to read humidity: %d\n",
                ret
            );

            led_error();

            k_msleep(SAMPLE_INTERVAL_MS);

            continue;
        }

        // Pressure
        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_PRESS,
            &pressure
        );

        if (ret < 0) {
            printf(
                "ERROR: Failed to read pressure: %d\n",
                ret
            );

            led_error();

            k_msleep(SAMPLE_INTERVAL_MS);

            continue;
        }

        // IAQ
        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_IAQ,
            &iaq
        );

        if (ret < 0) {
            printf(
                "ERROR: Failed to read IAQ: %d\n",
                ret
            );

            led_error();

            k_msleep(SAMPLE_INTERVAL_MS);

            continue;
        }

        // IAQ accuracy
        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_IAQ_ACC,
            &iaq_accuracy
        );

        if (ret < 0) {
            printf(
                "ERROR: Failed to read IAQ accuracy: %d\n",
                ret
            );

            led_error();

            k_msleep(SAMPLE_INTERVAL_MS);

            continue;
        }

        // CO2 equivalent
        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_CO2,
            &co2
        );

        if (ret < 0) {
            printf(
                "ERROR: Failed to read CO2 equivalent: %d\n",
                ret
            );

            led_error();

            k_msleep(SAMPLE_INTERVAL_MS);

            continue;
        }

        // VOC equivalent
        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_VOC,
            &voc
        );

        if (ret < 0) {
            printf(
                "ERROR: Failed to read VOC equivalent: %d\n",
                ret
            );

            led_error();

            k_msleep(SAMPLE_INTERVAL_MS);

            continue;
        }

        // Gas run-in status
        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_GAS_RUN_IN,
            &gas_run_in
        );

        if (ret < 0) {
            printf(
                "ERROR: Failed to read gas run-in status: %d\n",
                ret
            );

            led_error();

            k_msleep(SAMPLE_INTERVAL_MS);

            continue;
        }

        // Gas stability
        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_GAS_STAB,
            &gas_stab
        );

        if (ret < 0) {
            printf(
                "ERROR: Failed to read gas stability: %d\n",
                ret
            );

            led_error();

            k_msleep(SAMPLE_INTERVAL_MS);

            continue;
        }

        // Convert sensor values
        double temperature_c =
            sensor_value_to_double(&temperature);

        double humidity_pct =
            sensor_value_to_double(&humidity);

        double pressure_pa =
            sensor_value_to_double(&pressure);

        double pressure_hpa =
            pressure_pa / 100.0;

        double pressure_kpa =
            pressure_pa / 1000.0;

        double co2_ppm =
            sensor_value_to_double(&co2);

        double voc_ppm =
            sensor_value_to_double(&voc);

        // Print sensor readings
        printf(
            "----------------------------------------\n"
        );

        printf(
            "Temperature : %7.2f °C\n",
            temperature_c
        );

        printf(
            "Humidity    : %7.2f %%\n",
            humidity_pct
        );

        printf(
            "Pressure    : %7.2f hPa (%6.2f kPa)\n",
            pressure_hpa,
            pressure_kpa
        );

        printf("\n");

        printf(
            "IAQ         : %7d\n",
            iaq.val1
        );

        printf(
            "IAQ accuracy: %7d\n",
            iaq_accuracy.val1
        );

        printf(
            "CO2 eq.     : %7.1f ppm\n",
            co2_ppm
        );

        printf(
            "VOC eq.     : %7.2f ppm\n",
            voc_ppm
        );

        printf("\n");

        printf(
            "Gas run-in  : %7d\n",
            gas_run_in.val1
        );

        printf(
            "Gas stable  : %7d\n",
            gas_stab.val1
        );

        printf(
            "----------------------------------------\n"
        );

        // Send actual sensor values to the server
        int send_ret = send_telemetry(
            temperature_c,
            humidity_pct,
            pressure_hpa,
            iaq.val1,
            iaq_accuracy.val1,
            co2_ppm,
            voc_ppm,
            gas_stab.val1
        );

        if (send_ret < 0) {
            printf(
                "Telemetry transmission failed: %d\n",
                send_ret
            );

            led_error();
        } else {
            printf(
                "Telemetry transmission completed.\n"
            );

            led_success();
        }

        // Keep the successful state visible briefly
        k_sleep(K_SECONDS(2));

        // Turn LED off between telemetry cycles
        led_all_off();

        // Wait until the next measurement
        int64_t elapsed_ms = k_uptime_get() - sample_start_ms;

        if (elapsed_ms < SAMPLE_INTERVAL_MS) {
            k_msleep(SAMPLE_INTERVAL_MS - (int32_t)elapsed_ms);
        }
    }

    return 0;
}
