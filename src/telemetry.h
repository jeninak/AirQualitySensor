#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

#include "sensor.h"

int telemetry_send(const struct sensor_readings *readings,
                   uint8_t *failure_code);

#endif
