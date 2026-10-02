#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>

#include "led.h"
#include "telemetry.h"

#define TELEMETRY_HOST "www.cc.puv.fi"
#define TELEMETRY_PORT "443"
#define TELEMETRY_PORT_NUMBER 443
#define TELEMETRY_PATH "/~e2301774/Dashboard/telemetry.php"
#define TELEMETRY_HTTP_TIMEOUT_MS 10000

static volatile bool http_response_received;
static volatile uint16_t http_status_code;

K_SEM_DEFINE(telemetry_dns_sem, 0, 1);
static struct net_sockaddr telemetry_server_addr;
static net_socklen_t telemetry_server_addr_len;
static uint16_t telemetry_dns_query_id;
static bool telemetry_dns_address_found;
static int telemetry_dns_result;

static void telemetry_dns_callback(enum dns_resolve_status status,
                                   struct dns_addrinfo *info,
                                   void *user_data)
{
    ARG_UNUSED(user_data);

    if (status == DNS_EAI_INPROGRESS) {
        if (info != NULL &&
            info->ai_family == NET_AF_INET &&
            info->ai_addrlen > 0 &&
            info->ai_addrlen <= sizeof(telemetry_server_addr)) {
            memcpy(&telemetry_server_addr, &info->ai_addr, info->ai_addrlen);
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

static int http_response_callback(struct http_response *response,
                                  enum http_final_call final_data,
                                  void *user_data)
{
    ARG_UNUSED(user_data);

    if (response == NULL) {
        printf("ERROR: HTTP response was NULL.\n");
        return 0;
    }

    if (final_data == HTTP_DATA_FINAL) {
        http_response_received = true;
        http_status_code = response->http_status_code;
        printf("HTTP response received. Status code: %u\n",
               response->http_status_code);
        printf("HTTP status: %s\n", response->http_status);
    }

    return 0;
}

int telemetry_send(const struct sensor_readings *readings,
                   uint8_t *failure_code)
{
    if (readings == NULL || failure_code == NULL) {
        return -EINVAL;
    }

    *failure_code = 0;

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
        readings->temperature_c,
        readings->humidity_pct,
        readings->pressure_hpa,
        readings->iaq,
        readings->iaq_accuracy,
        readings->co2_ppm,
        readings->voc_ppm,
        readings->gas_stability
    );

    if (payload_len < 0 || payload_len >= sizeof(payload)) {
        printf("ERROR: Telemetry payload too large.\n");
        *failure_code = 8;
        return -ENOMEM;
    }

    printf("Sending telemetry...\n");
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
        printf("ERROR: Could not start DNS lookup: %d\n", ret);
        *failure_code = 6;
        return ret;
    }

    ret = k_sem_take(
        &telemetry_dns_sem,
        K_MSEC(CONFIG_NET_SOCKETS_DNS_TIMEOUT + 500)
    );

    if (ret < 0) {
        int cancel_ret = dns_cancel_addr_info(telemetry_dns_query_id);

        if (cancel_ret < 0) {
            printf("DNS query cancellation returned: %d\n", cancel_ret);
        }

        printf("ERROR: DNS lookup exceeded %d ms.\n",
               CONFIG_NET_SOCKETS_DNS_TIMEOUT);
        *failure_code = 6;
        return -ETIMEDOUT;
    }

    if (telemetry_dns_result < 0) {
        printf("ERROR: DNS lookup failed: %d\n", telemetry_dns_result);
        *failure_code = 6;
        return telemetry_dns_result;
    }

    printf("DNS lookup successful.\n");
    net_sin(&telemetry_server_addr)->sin_port =
        net_htons(TELEMETRY_PORT_NUMBER);

    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);

    if (sock < 0) {
        int err = errno;
        printf("ERROR: Could not create TLS socket: %d\n", err);
        *failure_code = 7;
        return -err;
    }

    printf("TLS socket created. Connecting to HTTPS server on port %d...\n",
           TELEMETRY_PORT_NUMBER);

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
        printf("ERROR: Could not configure TLS verification: %d\n", err);
        zsock_close(sock);
        *failure_code = 7;
        return -err;
    }

    ret = zsock_setsockopt(
        sock,
        ZSOCK_SOL_TLS,
        ZSOCK_TLS_HOSTNAME,
        TELEMETRY_HOST,
        strlen(TELEMETRY_HOST) + 1
    );

    if (ret < 0) {
        int err = errno;
        printf("ERROR: Could not set TLS hostname: %d\n", err);
        zsock_close(sock);
        *failure_code = 7;
        return -err;
    }

    led_https_connecting();
    ret = zsock_connect(sock, &telemetry_server_addr,
                        telemetry_server_addr_len);
    led_all_off();

    if (ret < 0) {
        int err = errno;
        int socket_error = 0;
        net_socklen_t socket_error_len = sizeof(socket_error);

        printf("ERROR: HTTPS connection failed: %d\n", err);
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
        *failure_code =
            (err == ENOENT && socket_error == 0) ? 9 : 7;
        return -err;
    }

    printf("HTTPS connection established.\n");

    static uint8_t recv_buf[512];
    http_response_received = false;
    http_status_code = 0;

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

    printf("Sending HTTPS POST request...\n");
    led_http_posting();
    ret = http_client_req(sock, &request, TELEMETRY_HTTP_TIMEOUT_MS, NULL);
    led_all_off();

    if (ret < 0) {
        printf("ERROR: HTTP POST failed: %d\n", ret);
        zsock_close(sock);
        *failure_code = 8;
        return ret;
    }

    printf("HTTP request completed.\n");
    zsock_close(sock);

    if (!http_response_received) {
        printf("ERROR: No final HTTP response received.\n");
        *failure_code = 8;
        return -EIO;
    }

    if (http_status_code != 200) {
        printf("ERROR: Server returned HTTP status %u.\n", http_status_code);
        *failure_code = 8;
        return -EIO;
    }

    printf("Telemetry accepted by server.\n");
    led_success();
    return 0;
}
