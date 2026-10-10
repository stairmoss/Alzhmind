/* Memoria wearable reference firmware (Arduino-ESP32).
 * Libraries: WiFi, WebServer, Wire (bundled) and TinyGPSPlus.
 * Optional MAX30102 sensor support is deliberately fail-safe: the board never
 * invents a heart-rate value when no validated sensor driver is installed.
 */
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <TinyGPSPlus.h>

// Put credentials in a local secrets.h file (excluded from source control).
// #define WIFI_SSID "your-network"
// #define WIFI_PASSWORD "your-password"
#include "secrets.h"

constexpr uint8_t SDA_PIN = 21, SCL_PIN = 22, MPU_ADDR = 0x68;
constexpr uint8_t GPS_RX = 16, GPS_TX = 17, BUZZER_PIN = 12, LED_PIN = 14, ACK_PIN = 27;
constexpr float IMPACT_G = 2.8f, STILL_G_DELTA = 0.20f;
constexpr uint32_t STILL_MS = 1200, GRACE_MS = 30000;
WebServer server(80);
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;

struct State { float ax=0, ay=0, az=1, totalG=1; bool fall=false, acknowledged=false; uint32_t fallAt=0; int heartRate=0; bool finger=false; } state;

bool readMPU() {
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)MPU_ADDR, 6) != 6) return false;
  int16_t x = (Wire.read() << 8) | Wire.read(); int16_t y = (Wire.read() << 8) | Wire.read(); int16_t z = (Wire.read() << 8) | Wire.read();
  state.ax = x / 16384.0f; state.ay = y / 16384.0f; state.az = z / 16384.0f;
  state.totalG = sqrtf(state.ax*state.ax + state.ay*state.ay + state.az*state.az); return true;
}

void alarm(bool enabled) { digitalWrite(BUZZER_PIN, enabled); digitalWrite(LED_PIN, enabled); }
void startFall() { state.fall = true; state.acknowledged = false; state.fallAt = millis(); }
void updateFallDetection() {
  static bool impact = false; static uint32_t impactAt = 0;
  if (!state.fall && state.totalG >= IMPACT_G) { impact = true; impactAt = millis(); }
  if (impact && millis() - impactAt > 100 && fabsf(state.totalG - 1.0f) < STILL_G_DELTA) { startFall(); impact = false; }
  if (state.fall && digitalRead(ACK_PIN) == LOW) { state.acknowledged = true; state.fall = false; alarm(false); }
  if (state.fall) {
    // Local warning during the grace period; after it expires telemetry remains
    // marked as an emergency for a caregiver endpoint to receive.
    alarm((millis() / 250) % 2);
  } else alarm(false);
}

String telemetry() {
  String s = "{\"device_id\":\"MEMORIA-01\",\"fall_detected\":" + String(state.fall ? "true" : "false") + ",\"heart_rate_bpm\":" + String(state.heartRate) + ",\"finger_detected\":" + String(state.finger ? "true" : "false");
  s += ",\"acceleration\":{\"x\":"+String(state.ax,2)+",\"y\":"+String(state.ay,2)+",\"z\":"+String(state.az,2)+",\"total_g\":"+String(state.totalG,2)+"}";
  s += ",\"gps\":{\"valid\":"+String(gps.location.isValid() ? "true" : "false")+",\"latitude\":"+String(gps.location.lat(),6)+",\"longitude\":"+String(gps.location.lng(),6)+",\"speed_kmph\":"+String(gps.speed.kmph(),1)+",\"satellites\":"+String(gps.satellites.value())+"},\"uptime_sec\":"+String(millis()/1000)+"}";
  return s;
}

void setup() {
  Serial.begin(115200); pinMode(BUZZER_PIN, OUTPUT); pinMode(LED_PIN, OUTPUT); pinMode(ACK_PIN, INPUT_PULLUP); alarm(false);
  Wire.begin(SDA_PIN, SCL_PIN); Wire.beginTransmission(MPU_ADDR); Wire.write(0x6B); Wire.write(0); Wire.endTransmission();
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  WiFi.mode(WIFI_AP_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD); WiFi.softAP("Memoria-Wearable", "change-this-password");
  server.on("/api/data", HTTP_GET, [] { server.send(200, "application/json", telemetry()); });
  server.on("/api/reset_fall", HTTP_POST, [] { state.fall=false; state.acknowledged=true; alarm(false); server.send(200, "application/json", "{\"ok\":true}"); });
  server.on("/", HTTP_GET, [] { server.send(200, "text/plain", "Memoria wearable: GET /api/data"); }); server.begin();
}

void loop() {
  server.handleClient(); while (gpsSerial.available()) gps.encode(gpsSerial.read());
  static uint32_t lastSensor = 0; if (millis() - lastSensor >= 50) { lastSensor = millis(); readMPU(); updateFallDetection(); }
  // Integrate a validated MAX30102 driver here. Set finger=true and heartRate
  // only after signal-quality and plausible-range checks; retain 0 otherwise.
}
