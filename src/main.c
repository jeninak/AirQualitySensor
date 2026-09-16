#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include <drivers/bme68x_iaq.h>

#define SAMPLE_INTERVAL_MS 5000

int main(void)
{
    const struct device *bme688 =
        DEVICE_DT_GET(DT_NODELABEL(bme688));

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

        // Fetch one complete BME688/BSEC sample.

        int ret = sensor_sample_fetch(bme688);

        if (ret < 0) {
            printf("ERROR: Sensor fetch failed: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        // BME688 environmental values.

        struct sensor_value temperature;
        struct sensor_value humidity;
        struct sensor_value pressure;

        // Bosch BSEC-derived air-quality values.

        struct sensor_value iaq;
        struct sensor_value iaq_accuracy;
        struct sensor_value co2;
        struct sensor_value voc;

        // BSEC gas status.

        struct sensor_value gas_run_in;
        struct sensor_value gas_stab;

        // Read the environmental measurements.

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_AMBIENT_TEMP,
            &temperature);

        if (ret < 0) {
            printf("ERROR: Failed to read temperature: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_HUMIDITY,
            &humidity);

        if (ret < 0) {
            printf("ERROR: Failed to read humidity: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_PRESS,
            &pressure);

        if (ret < 0) {
            printf("ERROR: Failed to read pressure: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        // Read BSEC air-quality values.

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_IAQ,
            &iaq);

        if (ret < 0) {
            printf("ERROR: Failed to read IAQ: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_IAQ_ACC,
            &iaq_accuracy);

        if (ret < 0) {
            printf("ERROR: Failed to read IAQ accuracy: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_CO2,
            &co2);

        if (ret < 0) {
            printf("ERROR: Failed to read CO2 equivalent: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_VOC,
            &voc);

        if (ret < 0) {
            printf("ERROR: Failed to read VOC equivalent: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        // Read BSEC gas status.

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_GAS_RUN_IN,
            &gas_run_in);

        if (ret < 0) {
            printf("ERROR: Failed to read gas run-in status: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        ret = sensor_channel_get(
            bme688,
            SENSOR_CHAN_GAS_STAB,
            &gas_stab);

        if (ret < 0) {
            printf("ERROR: Failed to read gas stability: %d\n", ret);
            k_msleep(SAMPLE_INTERVAL_MS);
            continue;
        }

        // Convert sensor values to normal units.
        //
        // Temperature -> °C
        // Humidity    -> %
        // Pressure    -> Pa, converted to hPa and kPa
        // CO2         -> ppm
        // VOC         -> ppm

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

        // Print the measurements.

        printf("----------------------------------------\n");

        printf("Temperature : %7.2f °C\n",
               temperature_c);

        printf("Humidity    : %7.2f %%\n",
               humidity_pct);

        printf("Pressure    : %7.2f hPa (%6.2f kPa)\n",
               pressure_hpa,
               pressure_kpa);

        printf("\n");

        printf("IAQ         : %7d\n",
               iaq.val1);

        printf("IAQ accuracy: %7d\n",
               iaq_accuracy.val1);

        printf("CO2 eq.     : %7.1f ppm\n",
               co2_ppm);

        printf("VOC eq.     : %7.2f ppm\n",
               voc_ppm);

        printf("\n");

        printf("Gas run-in  : %7d\n",
               gas_run_in.val1);

        printf("Gas stable  : %7d\n",
               gas_stab.val1);

        printf("----------------------------------------\n\n");

        k_msleep(SAMPLE_INTERVAL_MS);
    }

    return 0;
}
