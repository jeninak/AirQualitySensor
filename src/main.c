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
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>

#include <drivers/bme68x_iaq.h>

#include "wifi_config.h"

#define SAMPLE_INTERVAL_MS 5000
#define WIFI_TIMEOUT_SECONDS 30
#define WIFI_REQUEST_TIMEOUT_SECONDS 15
#define DHCP_TIMEOUT_SECONDS 15
#define WIFI_CONNECT_ATTEMPTS 3
#define WIFI_RETRY_DELAY_SECONDS 2
#define WIFI_FAILURE_PATTERN_CYCLES 3

#define TELEMETRY_HOST "www.cc.puv.fi"
#define TELEMETRY_PORT "443"
#define TELEMETRY_PORT_NUMBER 443
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
static uint8_t telemetry_failure_code;

K_SEM_DEFINE(telemetry_dns_sem, 0, 1);
static struct net_sockaddr telemetry_server_addr;
static net_socklen_t telemetry_server_addr_len;
static uint16_t telemetry_dns_query_id;
static bool telemetry_dns_address_found;
static int telemetry_dns_result;

static void telemetry_dns_callback(
    enum dns_resolve_status status,
    struct dns_addrinfo *info,
    void *user_data)
{
    ARG_UNUSED(user_data);

    if (status == DNS_EAI_INPROGRESS) {
        if (info != NULL &&
            info->ai_family == NET_AF_INET &&
            info->ai_addrlen > 0 &&
            info->ai_addrlen <= sizeof(telemetry_server_addr)) {
            memcpy(
                &telemetry_server_addr,
                &info->ai_addr,
                info->ai_addrlen
            );
            telemetry_server_addr_len = info->ai_addrlen;
            telemetry_dns_address_found = true;
        }

        return;
    }

    if (status == DNS_EAI_ALLDONE && telemetry_dns_address_found) {
        telemetry_dns_result = 0;
    } else {
        telemetry_dns_result = -EHOSTUNREACH;
    }

    k_sem_give(&telemetry_dns_sem);
}

struct saved_wifi_network {
    char ssid[WIFI_SSID_MAX_LEN];
    size_t ssid_len;
    bool found;
};

struct wifi_connect_job {
    struct net_if *iface;
    struct wifi_connect_req_params params;
    struct wifi_credentials_personal credentials;
    int result;
};

static struct wifi_connect_job wifi_job;
static struct k_sem wifi_request_sem;
static struct k_sem wifi_request_done_sem;
static struct k_thread wifi_request_thread;
static K_THREAD_STACK_DEFINE(
    wifi_request_stack,
    4096
);
static bool wifi_request_worker_started = false;
static bool wifi_request_in_progress = false;

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

static void led_failure_code(uint8_t code)
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

// Yellow = Wi-Fi connected, waiting for DHCP
static void led_waiting_network(void)
{
    led_all_off();
    led_on_dt(&led_red);
    led_on_dt(&led_green);
}

// Cyan = DHCP completed and the network is ready
static void led_network_ready(void)
{
    led_all_off();
    led_on_dt(&led_green);
    led_on_dt(&led_blue);
}

// Blue = resolving the telemetry server hostname
static void led_dns_lookup(void)
{
    led_all_off();
    led_on_dt(&led_blue);
}

// Purple = connecting to the HTTPS server and completing TLS
static void led_https_connecting(void)
{
    led_all_off();
    led_on_dt(&led_red);
    led_on_dt(&led_blue);
}

// Yellow = sending the HTTPS request and awaiting its response
static void led_http_posting(void)
{
    led_all_off();
    led_on_dt(&led_red);
    led_on_dt(&led_green);
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

        led_network_ready();
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

    if (ret == -ENOBUFS) {
        printf(
            "Wi-Fi credential store is full; replacing saved networks with the firmware-configured network.\n"
        );

        ret = wifi_credentials_delete_all();

        if (ret != 0) {
            printf(
                "ERROR: Failed to clear saved Wi-Fi credentials: %d\n",
                ret
            );

            led_error();

            return ret;
        }

        ret = wifi_credentials_set_personal(
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
    }

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

static void get_first_saved_wifi_ssid(void *user_data,
                                      const char *ssid,
                                      size_t ssid_len)
{
    struct saved_wifi_network *network = user_data;

    if (network->found || ssid_len == 0 ||
        ssid_len > sizeof(network->ssid)) {
        return;
    }

    memcpy(network->ssid, ssid, ssid_len);
    network->ssid_len = ssid_len;
    network->found = true;
}

static void wifi_request_worker(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    while (true) {
        k_sem_take(&wifi_request_sem, K_FOREVER);

        wifi_job.result = net_mgmt(
            NET_REQUEST_WIFI_CONNECT,
            wifi_job.iface,
            &wifi_job.params,
            sizeof(wifi_job.params)
        );

        k_sem_give(&wifi_request_done_sem);
    }
}

static void wifi_request_worker_start(void)
{
    if (wifi_request_worker_started) {
        return;
    }

    k_sem_init(&wifi_request_sem, 0, 1);
    k_sem_init(&wifi_request_done_sem, 0, 1);

    k_thread_create(
        &wifi_request_thread,
        wifi_request_stack,
        K_THREAD_STACK_SIZEOF(wifi_request_stack),
        wifi_request_worker,
        NULL,
        NULL,
        NULL,
        K_PRIO_PREEMPT(5),
        0,
        K_NO_WAIT
    );

    wifi_request_worker_started = true;
}

// Connect directly with the saved credentials.
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

    wifi_request_worker_start();

    if (wifi_request_in_progress) {
        if (k_sem_take(&wifi_request_done_sem, K_NO_WAIT) == 0) {
            wifi_request_in_progress = false;
        } else {
            printf(
                "ERROR: Previous Wi-Fi driver request is still running.\n"
            );

            led_error();

            return -EBUSY;
        }
    }

    if (wifi_connected) {
        return 0;
    }

    struct saved_wifi_network network = {0};

    if (WIFI_SSID[0] != '\0') {
        network.ssid_len = strlen(WIFI_SSID);

        if (network.ssid_len > sizeof(network.ssid)) {
            printf("ERROR: Configured Wi-Fi SSID is too long.\n");
            led_error();
            return -EINVAL;
        }

        memcpy(network.ssid, WIFI_SSID, network.ssid_len);
        network.found = true;
    } else {
        wifi_credentials_for_each_ssid(
            get_first_saved_wifi_ssid,
            &network
        );
    }

    if (!network.found) {
        printf("ERROR: No saved Wi-Fi network is available to connect to.\n");
        led_error();
        return -ENOENT;
    }

    memset(&wifi_job.credentials, 0, sizeof(wifi_job.credentials));
    int ret = wifi_credentials_get_by_ssid_personal_struct(
        network.ssid,
        network.ssid_len,
        &wifi_job.credentials
    );

    if (ret != 0) {
        printf("ERROR: Could not load saved Wi-Fi credentials: %d\n", ret);
        led_error();
        return ret;
    }

    struct wifi_connect_req_params params = {
        .ssid = (const uint8_t *)wifi_job.credentials.header.ssid,
        .ssid_length = wifi_job.credentials.header.ssid_len,
        .psk = (const uint8_t *)wifi_job.credentials.password,
        .psk_length = wifi_job.credentials.password_len,
        .channel = wifi_job.credentials.header.channel,
        .security = wifi_job.credentials.header.type,
        .mfp = WIFI_MFP_OPTIONAL,
        .timeout = wifi_job.credentials.header.timeout != 0
                       ? wifi_job.credentials.header.timeout
                       : WIFI_TIMEOUT_SECONDS,
    };

    if (wifi_job.credentials.header.flags & WIFI_CREDENTIALS_FLAG_2_4GHz) {
        params.band = WIFI_FREQ_BAND_2_4_GHZ;
    } else if (wifi_job.credentials.header.flags & WIFI_CREDENTIALS_FLAG_5GHz) {
        params.band = WIFI_FREQ_BAND_5_GHZ;
    } else if (wifi_job.credentials.header.flags & WIFI_CREDENTIALS_FLAG_6GHz) {
        params.band = WIFI_FREQ_BAND_6_GHZ;
    } else {
        params.band = WIFI_FREQ_BAND_UNKNOWN;
    }

    if (wifi_job.credentials.header.flags & WIFI_CREDENTIALS_FLAG_MFP_DISABLED) {
        params.mfp = WIFI_MFP_DISABLE;
    } else if (wifi_job.credentials.header.flags & WIFI_CREDENTIALS_FLAG_MFP_REQUIRED) {
        params.mfp = WIFI_MFP_REQUIRED;
    }

    if (params.security == WIFI_SECURITY_TYPE_SAE) {
        params.sae_password =
            (const uint8_t *)wifi_job.credentials.password;
        params.sae_password_length =
            wifi_job.credentials.password_len;
        params.psk = NULL;
        params.psk_length = 0;
    }

    wifi_job.iface = iface;
    wifi_job.params = params;
    wifi_connected = false;
    wifi_connect_result_received = false;
    wifi_connect_status = -1;
    dhcp_bound = false;

    led_wifi_connecting();

    printf(
        "Connecting to saved Wi-Fi network...\n"
    );

    wifi_request_in_progress = true;
    k_sem_reset(&wifi_request_done_sem);
    k_sem_give(&wifi_request_sem);

    bool blue_on = true;

    for (int tick = 0;
         tick < WIFI_REQUEST_TIMEOUT_SECONDS * 4;
         tick++) {
        if (k_sem_take(&wifi_request_done_sem, K_MSEC(250)) == 0) {
            wifi_request_in_progress = false;
            ret = wifi_job.result;

            printf(
                "Wi-Fi connection request returned: %d\n",
                ret
            );

            break;
        }

        if (blue_on) {
            led_on_dt(&led_blue);
        } else {
            led_all_off();
        }

        blue_on = !blue_on;
    }

    if (wifi_request_in_progress) {
        printf(
            "ERROR: Wi-Fi driver request did not return within %d seconds.\n",
            WIFI_REQUEST_TIMEOUT_SECONDS
        );

        return -EBUSY;
    }

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
    int wait_ticks = WIFI_TIMEOUT_SECONDS * 4;

    for (int tick = 0; tick < wait_ticks; tick++) {
        if (wifi_connect_result_received) {
            break;
        }

        if (blue_on) {
            led_on_dt(&led_blue);
        } else {
            led_all_off();
        }

        blue_on = !blue_on;

        k_msleep(250);
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

static uint8_t wifi_failure_code(int error)
{
    if (error == -ETIMEDOUT) {
        return 3;
    }

    if (!wifi_connect_result_received) {
        return 4;
    }

    switch (wifi_connect_status) {
    case WIFI_STATUS_CONN_AP_NOT_FOUND:
        return 1;
    case WIFI_STATUS_CONN_WRONG_PASSWORD:
        return 2;
    case WIFI_STATUS_CONN_TIMEOUT:
        return 3;
    default:
        return 4;
    }
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

            led_network_ready();

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

        telemetry_failure_code = 8;

        return -ENOMEM;
    }

    printf(
        "Sending telemetry...\n"
    );

    telemetry_failure_code = 0;
    led_dns_lookup();
    printf("Resolving telemetry server hostname...\n");

    k_sem_reset(&telemetry_dns_sem);
    telemetry_dns_address_found = false;
    telemetry_dns_result = -EHOSTUNREACH;
    telemetry_server_addr_len = 0;

    int ret = dns_get_addr_info(
        TELEMETRY_HOST,
        DNS_QUERY_TYPE_A,
        &telemetry_dns_query_id,
        telemetry_dns_callback,
        NULL,
        CONFIG_NET_SOCKETS_DNS_TIMEOUT
    );

    if (ret < 0) {
        printf(
            "ERROR: Could not start DNS lookup: %d\n",
            ret
        );

        telemetry_failure_code = 6;

        return ret;
    }

    ret = k_sem_take(
        &telemetry_dns_sem,
        K_MSEC(CONFIG_NET_SOCKETS_DNS_TIMEOUT + 500)
    );

    if (ret < 0) {
        int cancel_ret = dns_cancel_addr_info(telemetry_dns_query_id);

        if (cancel_ret < 0) {
            printf(
                "DNS query cancellation returned: %d\n",
                cancel_ret
            );
        }

        printf(
            "ERROR: DNS lookup exceeded %d ms.\n",
            CONFIG_NET_SOCKETS_DNS_TIMEOUT
        );

        telemetry_failure_code = 6;

        return -ETIMEDOUT;
    }

    if (telemetry_dns_result < 0) {
        printf(
            "ERROR: DNS lookup failed: %d\n",
            telemetry_dns_result
        );

        telemetry_failure_code = 6;

        return telemetry_dns_result;
    }

    printf(
        "DNS lookup successful.\n"
    );

    net_sin(&telemetry_server_addr)->sin_port =
        net_htons(TELEMETRY_PORT_NUMBER);

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

        telemetry_failure_code = 7;

        return -err;
    }

    printf(
        "TLS socket created. Connecting to HTTPS server on port %d...\n",
        TELEMETRY_PORT_NUMBER
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
        telemetry_failure_code = 7;

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
        telemetry_failure_code = 7;

        return -err;
    }

    // Connect to the HTTPS server
    led_https_connecting();

    ret = zsock_connect(
        sock,
        &telemetry_server_addr,
        telemetry_server_addr_len
    );

    led_all_off();

    if (ret < 0) {
        int err = errno;
        int socket_error = 0;
        net_socklen_t socket_error_len = sizeof(socket_error);

        printf(
            "ERROR: HTTPS connection failed: %d\n",
            err
        );

        int getsockopt_ret = zsock_getsockopt(
            sock,
            ZSOCK_SOL_SOCKET,
            ZSOCK_SO_ERROR,
            &socket_error,
            &socket_error_len
        );

        if (getsockopt_ret == 0) {
            printf("TLS socket internal error: %d\n", socket_error);
        } else {
            printf("Could not read TLS socket error: %d\n", errno);
        }

        zsock_close(sock);

        telemetry_failure_code =
            (err == ENOENT && socket_error == 0) ? 9 : 7;

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

    led_http_posting();

    ret = http_client_req(
        sock,
        &request,
        10000,
        NULL
    );

    led_all_off();

    if (ret < 0) {
        printf(
            "ERROR: HTTP POST failed: %d\n",
            ret
        );

        zsock_close(sock);

        telemetry_failure_code = 8;

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

        telemetry_failure_code = 8;

        return -EIO;
    }

    if (http_status_code != 200) {
        printf(
            "ERROR: Server returned HTTP status %u.\n",
            http_status_code
        );

        telemetry_failure_code = 8;

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
        uint8_t failure_code = 4;

        printf(
            "\nWi-Fi setup attempt %d (retry group %d/%d)\n",
            setup_attempt,
            ((setup_attempt - 1) % WIFI_CONNECT_ATTEMPTS) + 1,
            WIFI_CONNECT_ATTEMPTS
        );

        int network_ret = wifi_connect();

        if (network_ret == 0) {
            network_ret = wait_for_wifi_connection();

            if (network_ret != 0) {
                failure_code = wifi_failure_code(network_ret);
            }
        }

        if (network_ret == 0) {
            network_ret = wait_for_dhcp();

            if (network_ret != 0) {
                failure_code = 5;
            }
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

        printf(
            "Wi-Fi failure LED code: %u red pulses.\n",
            failure_code
        );

        led_failure_code(failure_code);
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

            if (telemetry_failure_code > 0) {
                led_failure_code(telemetry_failure_code);
            } else {
                led_error();
            }
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
