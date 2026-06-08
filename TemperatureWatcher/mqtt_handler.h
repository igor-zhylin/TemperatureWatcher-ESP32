#pragma once
#include <stdint.h>
#include "config.h"

#ifdef MQTT_BROKER
void mqttSetup();
void mqttLoop(float t, float hpa, float mmhg, float alt,
              float tmin, float tmax, int8_t trend, uint32_t uptime_s);
#else
inline void mqttSetup() {}
inline void mqttLoop(float, float, float, float, float, float, int8_t, uint32_t) {}
#endif
