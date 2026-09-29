#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_ip.h>

#include <drivers/bme68x_iaq.h>

#include "wifi_credentials.h"

#define SAMPLE_INTERVAL_MS 5000

static struct net_mgmt_event_callback wifi_cb;

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t event,
                               struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
        printf("Wi-Fi connection result received.\n");
    }

    if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        printf("Wi-Fi disconnected.\n");
    }
}

static int wifi_connect(void)
{
    struct net_if *iface = net_if_get_default();

    struct wifi_connect_req_params params = {
        .ssid = WIFI_SSID,
        .ssid_length = strlen(WIFI_SSID),
        .psk = WIFI_PASSWORD,
        .psk_length = strlen(WIFI_PASSWORD),
        .security = WIFI_SECURITY_TYPE_PSK,
        .channel = WIFI_CHANNEL_ANY,
        .timeout = SYS_FOREVER_MS,
    };

    printf("\nConnecting to Wi-Fi...\n");

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT,
                       iface,
                       &params,
                       sizeof(params));

    if (ret != 0) {
        printf("Wi-Fi connection request failed: %d\n", ret);
        return ret;
    }

    printf("Wi-Fi connection request sent.\n");

    return 0;
}

int main(void)
{
    const struct device *bme688 =
        DEVICE_DT_GET(DT_NODELABEL(bme688));

    // Wi-Fi event callback
    net_mgmt_init_event_callback(
        &wifi_cb,
        wifi_event_handler,
        NET_EVENT_WIFI_CONNECT_RESULT |
        NET_EVENT_WIFI_DISCONNECT_RESULT
    );

    net_mgmt_add_event_callback(&wifi_cb);

    // Start Wi-Fi
    int wifi_ret = wifi_connect();

    if (wifi_ret != 0) {
        printf("WARNING: Wi-Fi connection could not be started.\n");
    }


    // BME688
    if (!device_is_ready(bme688)) {
        printf("\n");
        printf("ERROR: BME688 sensor is not ready!\n");
        return 0;
    }

    printf("\n");
    printf("========================================\n");
    printf(" Thingy:53 BME688 Home Environment Monitor\n");
    printf("========================================\n");
    printf("BME688 initialized successfully.\n");
    printf("Sampling every %d seconds.\n\n",
           SAMPLE_INTERVAL_MS / 1000);

    while (1) {

        int ret = sensor_sample_fetch(bme688);

        if (ret < 0) {
            printf("ERROR: Sensor fetch failed: %d\n", ret);
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

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_AMBIENT_TEMP,
            &temperature
        );

        if (ret < 0) {
            printf("ERROR: Failed to read temperature: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_HUMIDITY,
            &humidity
        );

        if (ret < 0) {
            printf("ERROR: Failed to read humidity: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_PRESS,
            &pressure
        );

        if (ret < 0) {
            printf("ERROR: Failed to read pressure: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_IAQ,
            &iaq
        );

        if (ret < 0) {
            printf("ERROR: Failed to read IAQ: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_IAQ_ACC,
            &iaq_accuracy
        );

        if (ret < 0) {
            printf("ERROR: Failed to read IAQ accuracy: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_CO2,
            &co2
        );

        if (ret < 0) {
            printf("ERROR: Failed to read CO2 equivalent: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_VOC,
            &voc
        );

        if (ret < 0) {
            printf("ERROR: Failed to read VOC equivalent: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_GAS_RUN_IN,
            &gas_run_in
        );

        if (ret < 0) {
            printf("ERROR: Failed to read gas run-in status: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_GAS_STAB,
            &gas_stab
        );

        if (ret < 0) {
            printf("ERROR: Failed to read gas stability: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

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

        printf("----------------------------------------\n");
        printf("Temperature : %7.2f °C\n", temperature_c);
        printf("Humidity    : %7.2f %%\n", humidity_pct);
        printf("Pressure    : %7.2f hPa (%6.2f kPa)\n",
               pressure_hpa,
               pressure_kpa);

        printf("\n");

        printf("IAQ         : %7d\n", iaq.val1);
        printf("IAQ accuracy: %7d\n", iaq_accuracy.val1);
        printf("CO2 eq.     : %7.1f ppm\n", co2_ppm);
        printf("VOC eq.     : %7.2f ppm\n", voc_ppm);

        printf("\n");

        printf("Gas run-in  : %7d\n", gas_run_in.val1);
        printf("Gas stable  : %7d\n", gas_stab.val1);

        printf("----------------------------------------\n\n");

        k_msleep(SAMPLE_INTERVAL_MS);
    }

    return 0;
}
