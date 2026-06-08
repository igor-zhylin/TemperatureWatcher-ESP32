#include "mqtt_handler.h"

#ifdef MQTT_BROKER
#include <WiFi.h>
#include <PubSubClient.h>

static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static bool mqttConnect() {
  if (mqtt.connected()) return true;
  // Non-blocking single attempt; caller retries on next interval.
  return mqtt.connect(MQTT_CLIENT_ID);
}

void mqttSetup() {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
}

void mqttLoop(float t, float hpa, float mmhg, float alt,
              float tmin, float tmax, int8_t trend, uint32_t uptime_s) {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < MQTT_INTERVAL) return;
  lastMs = millis();

  if (WiFi.status() != WL_CONNECTED) return;
  if (!mqttConnect()) return;
  mqtt.loop();

  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f", t);
  mqtt.publish(MQTT_TOPIC_PREFIX "/temperature_c", buf);
  snprintf(buf, sizeof(buf), "%.1f", hpa);
  mqtt.publish(MQTT_TOPIC_PREFIX "/pressure_hpa", buf);
  snprintf(buf, sizeof(buf), "%.1f", mmhg);
  mqtt.publish(MQTT_TOPIC_PREFIX "/pressure_mmhg", buf);
  snprintf(buf, sizeof(buf), "%.1f", alt);
  mqtt.publish(MQTT_TOPIC_PREFIX "/altitude_m", buf);
  snprintf(buf, sizeof(buf), "%.1f", tmin);
  mqtt.publish(MQTT_TOPIC_PREFIX "/temp_min_24h", buf);
  snprintf(buf, sizeof(buf), "%.1f", tmax);
  mqtt.publish(MQTT_TOPIC_PREFIX "/temp_max_24h", buf);
  snprintf(buf, sizeof(buf), "%d", (int)trend);
  mqtt.publish(MQTT_TOPIC_PREFIX "/trend", buf);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)uptime_s);
  mqtt.publish(MQTT_TOPIC_PREFIX "/uptime_s", buf);
}
#endif
