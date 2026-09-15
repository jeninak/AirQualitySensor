#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <drivers/bme68x_iaq.h>

int main(void)
{
    const struct device *bme688 =
        DEVICE_DT_GET(DT_NODELABEL(bme688));

    if (!device_is_ready(bme688)) {
        printf("BME688 sensor is not ready!\n");
        return 0;
    }

    printf("\n");
    printf("Thingy:53 BME688 BSEC2 test\n");
    printf("Sensor initialized successfully.\n\n");

    while (1) {
        int ret = sensor_sample_fetch(bme688);

        if (ret < 0) {
            printf("Sensor fetch failed: %d\n", ret);
            k_sleep(K_SECONDS(5));
            continue;
        }

        struct sensor_value temperature;
        struct sensor_value humidity;
        struct sensor_value pressure;
        struct sensor_value iaq;
        struct sensor_value co2;
        struct sensor_value voc;

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

        sensor_channel_get(
            bme688,
            SENSOR_CHAN_IAQ,
            &iaq
        );

        sensor_channel_get(
            bme688,
            SENSOR_CHAN_CO2,
            &co2
        );

        sensor_channel_get(
            bme688,
            SENSOR_CHAN_VOC,
            &voc
        );

        printf("Temperature: %d.%06d C\n",
               temperature.val1, temperature.val2);

        printf("Humidity:    %d.%06d %%\n",
               humidity.val1, humidity.val2);

        printf("Pressure:    %d.%06d kPa\n",
               pressure.val1, pressure.val2);

        printf("IAQ:         %d.%06d\n",
               iaq.val1, iaq.val2);

        printf("CO2 eq:      %d.%06d ppm\n",
               co2.val1, co2.val2);

        printf("VOC eq:      %d.%06d ppm\n",
               voc.val1, voc.val2);

        printf("-----------------------------\n");

        k_sleep(K_SECONDS(5));
    }

    return 0;
}
