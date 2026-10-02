#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi_credentials.h>

#include "led.h"
#include "wifi.h"
#include "wifi_config.h"

#define WIFI_TIMEOUT_SECONDS 30
#define WIFI_REQUEST_TIMEOUT_SECONDS 15
#define DHCP_TIMEOUT_SECONDS 15

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback net_cb;

static volatile bool wifi_connected;
static volatile bool wifi_connect_result_received;
static volatile bool dhcp_bound;
static volatile int wifi_connect_status = -1;

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
static K_THREAD_STACK_DEFINE(wifi_request_stack, 4096);
static bool wifi_request_worker_started;
static bool wifi_request_in_progress;

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
            printf("Wi-Fi connection result received without status.\n");
            led_error();
            return;
        }

        wifi_connect_status = status->status;

        if (status->status == WIFI_STATUS_CONN_SUCCESS) {
            wifi_connected = true;
            printf("Wi-Fi connected successfully.\n");
            led_waiting_network();
        } else {
            wifi_connected = false;
            printf("Wi-Fi connection failed. Status: %d\n",
                   status->status);

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
        printf("Wi-Fi disconnected.\n");
        led_error();
    }
}

static void net_event_handler(struct net_mgmt_event_callback *cb,
                              uint64_t event,
                              struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (event == NET_EVENT_IPV4_DHCP_BOUND) {
        dhcp_bound = true;
        printf("DHCP completed successfully.\n");
        led_network_ready();
    }
}

int wifi_initialize(void)
{
    net_mgmt_init_event_callback(
        &wifi_cb,
        wifi_event_handler,
        NET_EVENT_WIFI_CONNECT_RESULT |
            NET_EVENT_WIFI_DISCONNECT_RESULT
    );
    net_mgmt_add_event_callback(&wifi_cb);

    net_mgmt_init_event_callback(
        &net_cb,
        net_event_handler,
        NET_EVENT_IPV4_DHCP_BOUND
    );
    net_mgmt_add_event_callback(&net_cb);

    return 0;
}

int wifi_configure_credentials(void)
{
    bool ssid_configured = WIFI_SSID[0] != '\0';
    bool password_configured = WIFI_PASSWORD[0] != '\0';

    if (!ssid_configured && !password_configured) {
        if (wifi_credentials_is_empty()) {
            printf("ERROR: No Wi-Fi credentials are configured or saved on this device.\n");
            led_error();
            return -ENOENT;
        }

        printf("Using Wi-Fi credentials saved in device settings.\n");
        return 0;
    }

    if (!ssid_configured || !password_configured) {
        printf("ERROR: Both WIFI_SSID and WIFI_PASSWORD must be set, or both left empty to use saved credentials.\n");
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
        printf("Wi-Fi credential store is full; replacing saved networks with the firmware-configured network.\n");

        ret = wifi_credentials_delete_all();

        if (ret != 0) {
            printf("ERROR: Failed to clear saved Wi-Fi credentials: %d\n",
                   ret);
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
        printf("ERROR: Failed to store Wi-Fi credentials: %d\n", ret);
        led_error();
        return ret;
    }

    printf("Wi-Fi credentials stored successfully.\n");
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

int wifi_connect(void)
{
    struct net_if *iface = net_if_get_first_wifi();

    if (iface == NULL) {
        printf("ERROR: No Wi-Fi interface found.\n");
        led_error();
        return -ENODEV;
    }

    wifi_request_worker_start();

    if (wifi_request_in_progress) {
        if (k_sem_take(&wifi_request_done_sem, K_NO_WAIT) == 0) {
            wifi_request_in_progress = false;
        } else {
            printf("ERROR: Previous Wi-Fi driver request is still running.\n");
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
        wifi_credentials_for_each_ssid(get_first_saved_wifi_ssid, &network);
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
        params.sae_password_length = wifi_job.credentials.password_len;
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
    printf("Connecting to saved Wi-Fi network...\n");

    wifi_request_in_progress = true;
    k_sem_reset(&wifi_request_done_sem);
    k_sem_give(&wifi_request_sem);

    bool blue_on = true;

    for (int tick = 0; tick < WIFI_REQUEST_TIMEOUT_SECONDS * 4; tick++) {
        if (k_sem_take(&wifi_request_done_sem, K_MSEC(250)) == 0) {
            wifi_request_in_progress = false;
            ret = wifi_job.result;
            printf("Wi-Fi connection request returned: %d\n", ret);
            break;
        }

        led_wifi_connecting_blink(blue_on);
        blue_on = !blue_on;
    }

    if (wifi_request_in_progress) {
        printf("ERROR: Wi-Fi driver request did not return within %d seconds.\n",
               WIFI_REQUEST_TIMEOUT_SECONDS);
        return -EBUSY;
    }

    if (ret != 0 && ret != -EALREADY) {
        printf("Wi-Fi connection request failed: %d\n", ret);
        led_error();
        return ret;
    }

    printf("Wi-Fi connection request sent.\n");
    return 0;
}

int wifi_wait_for_connection(void)
{
    printf("Waiting for Wi-Fi connection result...\n");

    bool blue_on = true;
    int wait_ticks = WIFI_TIMEOUT_SECONDS * 4;

    for (int tick = 0; tick < wait_ticks; tick++) {
        if (wifi_connect_result_received) {
            break;
        }

        led_wifi_connecting_blink(blue_on);
        blue_on = !blue_on;
        k_msleep(250);
    }

    if (!wifi_connect_result_received) {
        printf("ERROR: Wi-Fi connection timed out after %d seconds.\n",
               WIFI_TIMEOUT_SECONDS);
        led_error();
        return -ETIMEDOUT;
    }

    if (!wifi_connected) {
        printf("ERROR: Wi-Fi connection failed with status %d.\n",
               wifi_connect_status);
        led_error();
        return -ECONNREFUSED;
    }

    printf("Wi-Fi connection confirmed.\n");
    led_waiting_network();
    return 0;
}

uint8_t wifi_failure_code(int error)
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

int wifi_wait_for_dhcp(void)
{
    printf("Waiting for DHCP...\n");
    led_waiting_network();

    for (int seconds = 0; seconds < DHCP_TIMEOUT_SECONDS; seconds++) {
        if (dhcp_bound) {
            printf("Network is ready.\n");
            led_network_ready();
            return 0;
        }

        k_sleep(K_SECONDS(1));
    }

    printf("ERROR: DHCP did not complete within %d seconds.\n",
           DHCP_TIMEOUT_SECONDS);
    led_error();
    return -ETIMEDOUT;
}

bool wifi_is_connected(void)
{
    return wifi_connected;
}

int wifi_disconnect(void)
{
    struct net_if *iface = net_if_get_first_wifi();

    if (iface == NULL || !wifi_connected) {
        return 0;
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    return ret;
}
