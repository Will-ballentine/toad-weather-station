// Toad tank weather station, stage 3: read sensors on a timer, timestamp via NTP, publish JSON over MQTT.
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_BME280.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD, MQTT_HOST (kept out of git)

// ---- Pins ----
const int ONEWIRE_PIN  = 4;
const int MOISTURE_PIN = 34;  // must be ADC1 (GPIO 32-39); ADC2 pins stop working when WiFi is on

// ---- MQTT ----
const char* MQTT_CLIENT_ID = "toad-station1";              // must be unique per device on the broker
const char* TOPIC_SENSORS  = "toadtank/station1/sensors";  // JSON readings
const char* TOPIC_STATUS   = "toadtank/station1/status";   // "online" / "offline" (retained)

// ---- Timing ----
const unsigned long INTERVAL_MS = 5UL * 60 * 1000;

// ---- Moisture calibration (raw ADC). Replace with your measured values. ----
const int MOISTURE_DRY = 3000;  // sensor in dry air / dry substrate
const int MOISTURE_WET = 1300;  // sensor in water / saturated substrate

WiFiClient net;            // the TCP connection
PubSubClient mqtt(net);    // MQTT on top of it
unsigned long lastMqttAttempt = 0;

OneWire oneWire(ONEWIRE_PIN);
DallasTemperature soilProbe(&oneWire);
Adafruit_BME280 bme;
bool bmeOk = false;
bool firstReading = true;
unsigned long lastReading = 0;

// Average several reads; the ESP32 ADC is noisy.
int readMoistureRaw() {
  long sum = 0;
  for (int i = 0; i < 16; i++) { sum += analogRead(MOISTURE_PIN); delay(5); }
  return sum / 16;
}

// Write a value rounded to 2 decimals, or null if the sensor failed.
void put(JsonDocument& doc, const char* key, float v, bool valid) {
  if (valid) doc[key] = roundf(v * 100) / 100;
  else doc[key] = nullptr;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("toad-station");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(250); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) Serial.printf(" connected, IP %s\n", WiFi.localIP().toString().c_str());
  else Serial.println(" failed; will retry each reading");
}

// Connect to the broker. The "will" tells the broker to publish "offline" for us if we vanish.
void connectMQTT() {
  Serial.print("Connecting to MQTT... ");
  if (mqtt.connect(MQTT_CLIENT_ID, nullptr, nullptr, TOPIC_STATUS, 0, true, "offline")) {
    mqtt.publish(TOPIC_STATUS, "online", true);  // retained: new subscribers see it immediately
    Serial.println("connected");
  } else {
    Serial.printf("failed (state %d), will retry\n", mqtt.state());
  }
}

void readSensors(JsonDocument& doc) {
  time_t now = time(nullptr);  // seconds since 1970 (UTC); stays near 0 until NTP syncs
  if (now > 1700000000) doc["ts"] = (uint32_t)now;
  else doc["ts"] = nullptr;

  soilProbe.requestTemperatures();
  float soilT = soilProbe.getTempCByIndex(0);
  put(doc, "soil_temp_c", soilT, soilT != DEVICE_DISCONNECTED_C);

  if (!bmeOk) bmeOk = bme.begin(0x76);  // retry if it failed earlier
  float airT = bmeOk ? bme.readTemperature() : NAN;
  float hum  = bmeOk ? bme.readHumidity() : NAN;
  float pres = bmeOk ? bme.readPressure() / 100.0F : NAN;
  put(doc, "air_temp_c", airT, !isnan(airT));
  put(doc, "humidity_pct", hum, !isnan(hum));
  put(doc, "pressure_hpa", pres, !isnan(pres));

  int raw = readMoistureRaw();
  doc["soil_moisture_raw"] = raw;
  doc["soil_moisture_pct"] = constrain(map(raw, MOISTURE_DRY, MOISTURE_WET, 0, 100), 0, 100);
  doc["uptime_s"] = millis() / 1000;
  if (WiFi.status() == WL_CONNECTED) doc["rssi_dbm"] = WiFi.RSSI();  // signal strength
  else doc["rssi_dbm"] = nullptr;
}

void setup() {
  Serial.begin(115200);
  soilProbe.begin();
  bmeOk = bme.begin(0x76);
  if (!bmeOk) Serial.println("BME280 not found at 0x76; will retry each reading");
  connectWiFi();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // UTC; syncs in background, re-syncs hourly
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);  // default is 256 bytes; our packets are ~210, so give headroom for added fields
  Serial.printf("DS18B20 devices found: %d\n", soilProbe.getDeviceCount());
}

void loop() {
  // Keep the MQTT connection alive; retry every 5 s if it drops
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected() && millis() - lastMqttAttempt > 5000) {
    lastMqttAttempt = millis();
    connectMQTT();
  }
  mqtt.loop();  // handles keepalive pings; must run often

  if (firstReading || millis() - lastReading >= INTERVAL_MS) {
    firstReading = false;
    lastReading = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();

    JsonDocument doc;
    readSensors(doc);
    char payload[512];
    serializeJson(doc, payload);
    Serial.println(payload);

    if (doc["ts"].isNull()) Serial.println("  not published: clock not synced yet");
    else if (!mqtt.connected()) Serial.println("  not published: no MQTT connection");
    else if (mqtt.publish(TOPIC_SENSORS, payload)) Serial.println("  published");
    else Serial.println("  publish failed");
  }
}
