#include <stdio.h>
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

#include <drivers/bme68x_iaq.h>

#include "sensor.h"

static const struct device *const bme688 =
    DEVICE_DT_GET(DT_NODELABEL(bme688));

int sensor_initialize(void)
{
    if (!device_is_ready(bme688)) {
        printf("\nERROR: BME688 sensor is not ready!\n");
        return -ENODEV;
    }

    printf("\n");
    printf("========================================\n");
    printf(" Thingy:53 BME688 Home Environment Monitor\n");
    printf("========================================\n");
    printf("BME688 initialized successfully.\n");

    return 0;
}

static int read_channel(enum sensor_channel channel,
                        struct sensor_value *value,
                        const char *name)
{
    int ret = sensor_channel_get(bme688, channel, value);

    if (ret < 0) {
        printf("ERROR: Failed to read %s: %d\n", name, ret);
    }

    return ret;
}

int sensor_read(struct sensor_readings *readings)
{
    if (readings == NULL) {
        return -EINVAL;
    }

    int ret = sensor_sample_fetch(bme688);

    if (ret < 0) {
        printf("ERROR: Sensor fetch failed: %d\n", ret);
        return ret;
    }

    struct sensor_value temperature;
    struct sensor_value humidity;
    struct sensor_value pressure;
    struct sensor_value iaq;
    struct sensor_value iaq_accuracy;
    struct sensor_value co2;
    struct sensor_value voc;
    struct sensor_value gas_run_in;
    struct sensor_value gas_stability;

    if ((ret = read_channel(SENSOR_CHAN_AMBIENT_TEMP, &temperature,
                            "temperature")) < 0 ||
        (ret = read_channel(SENSOR_CHAN_HUMIDITY, &humidity,
                            "humidity")) < 0 ||
        (ret = read_channel(SENSOR_CHAN_PRESS, &pressure,
                            "pressure")) < 0 ||
        (ret = read_channel(SENSOR_CHAN_IAQ, &iaq, "IAQ")) < 0 ||
        (ret = read_channel(SENSOR_CHAN_IAQ_ACC, &iaq_accuracy,
                            "IAQ accuracy")) < 0 ||
        (ret = read_channel(SENSOR_CHAN_CO2, &co2,
                            "CO2 equivalent")) < 0 ||
        (ret = read_channel(SENSOR_CHAN_VOC, &voc,
                            "VOC equivalent")) < 0 ||
        (ret = read_channel(SENSOR_CHAN_GAS_RUN_IN, &gas_run_in,
                            "gas run-in status")) < 0 ||
        (ret = read_channel(SENSOR_CHAN_GAS_STAB, &gas_stability,
                            "gas stability")) < 0) {
        return ret;
    }

    readings->temperature_c = sensor_value_to_double(&temperature);
    readings->humidity_pct = sensor_value_to_double(&humidity);
    readings->pressure_hpa = sensor_value_to_double(&pressure) / 100.0;
    readings->co2_ppm = sensor_value_to_double(&co2);
    readings->voc_ppm = sensor_value_to_double(&voc);
    readings->iaq = iaq.val1;
    readings->iaq_accuracy = iaq_accuracy.val1;
    readings->gas_run_in = gas_run_in.val1;
    readings->gas_stability = gas_stability.val1;

    return 0;
}

void sensor_print_readings(const struct sensor_readings *readings)
{
    printf("----------------------------------------\n");
    printf("Temperature : %7.2f °C\n", readings->temperature_c);
    printf("Humidity    : %7.2f %%\n", readings->humidity_pct);
    printf("Pressure    : %7.2f hPa (%6.2f kPa)\n",
           readings->pressure_hpa, readings->pressure_hpa / 10.0);
    printf("\n");
    printf("IAQ         : %7d\n", readings->iaq);
    printf("IAQ accuracy: %7d\n", readings->iaq_accuracy);
    printf("CO2 eq.     : %7.1f ppm\n", readings->co2_ppm);
    printf("VOC eq.     : %7.2f ppm\n", readings->voc_ppm);
    printf("\n");
    printf("Gas run-in  : %7d\n", readings->gas_run_in);
    printf("Gas stable  : %7d\n", readings->gas_stability);
    printf("----------------------------------------\n");
}
