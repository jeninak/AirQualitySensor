#ifndef SENSOR_H
#define SENSOR_H

struct sensor_readings {
    double temperature_c;
    double humidity_pct;
    double pressure_hpa;
    double co2_ppm;
    double voc_ppm;
    int iaq;
    int iaq_accuracy;
    int gas_run_in;
    int gas_stability;
};

int sensor_initialize(void);
int sensor_read(struct sensor_readings *readings);
void sensor_print_readings(const struct sensor_readings *readings);

#endif
