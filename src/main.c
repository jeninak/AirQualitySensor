#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#define SAMPLE_INTERVAL_MS 5000

// test
int main(void)
{
    const struct device *bme688 =
        DEVICE_DT_GET(DT_NODELABEL(bme688));

    if (!device_is_ready(bme688)) {
        printf("BME688 sensor is not ready!\n");
        return 0;
    }

    printf("Thingy:53 Home Environment Monitor\n");
    printf("BME688 initialized successfully.\n\n");

    while (1) {

        int ret = sensor_sample_fetch(bme688);

        if (ret < 0) {
            printf("Failed to fetch sensor data: %d\n", ret);
            k_sleep(K_MSEC(SAMPLE_INTERVAL_MS));
            continue;
        }

        struct sensor_value temperature;
        struct sensor_value humidity;
        struct sensor_value pressure;

        sensor_channel_get(
            bme688,
            SENSOR_CHAN_AMBIENT_TEMP,
            &temperature
        );

        sensor_channel_get(
            bme688,
            SENSOR_CHAN_HUMIDITY,
            &humidity
        );

        sensor_channel_get(
            bme688,
            SENSOR_CHAN_PRESS,
            &pressure
        );

        printf(
            "Temperature: %d.%06d °C\n",
            temperature.val1,
            temperature.val2
        );

        printf(
            "Humidity:    %d.%06d %%\n",
            humidity.val1,
            humidity.val2
        );

        printf(
            "Pressure:    %d.%06d kPa\n",
            pressure.val1,
            pressure.val2
        );

        printf("-----------------------------\n");

        k_sleep(K_MSEC(SAMPLE_INTERVAL_MS));
    }

    return 0;
}
