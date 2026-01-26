#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ==================== KONFIGURASI NETWORK ====================
const char* ssid = "smk unnes semarang";
const char* password = "12345678";
const char* gatewayIP = "192.168.137.198";  // ⚠️ SESUAIKAN IP GATEWAY
const int gatewayPort = 8080;

// ==================== KONFIGURASI HARDWARE ====================
#define DHT_PIN 4
#define RELAY_PIN 2
#define DHT_TYPE DHT22
#define NODE_ID 1
#define RELAY_ACTIVE_LOW true

// ==================== SENSOR & KONTROL ====================
DHT dht(DHT_PIN, DHT_TYPE);

int exhaust_status = 0;      // 0=MATI, 1=SEDANG, 2=MENYALA
bool manual_mode = false;

// Sensor readings
float current_temperature = NAN;
float current_humidity = NAN;

// ==================== LATENCY MEASUREMENT ====================
uint32_t sequence_no = 0;   // untuk latency & packet loss

// ==================== TIMING ====================
unsigned long last_sensor_read = 0;
unsigned long last_gateway_sync = 0;
unsigned long last_status_print = 0;

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long SYNC_INTERVAL   = 5000;
const unsigned long PRINT_INTERVAL  = 10000;

// ==================== RELAY CONTROL ====================
void setExhaust(int status) {
  bool relay_on = (status > 0);
  bool output_level = RELAY_ACTIVE_LOW ? !relay_on : relay_on;

  digitalWrite(RELAY_PIN, output_level);
  exhaust_status = status;

  const char* status_names[] = {"MATI", "SEDANG", "MENYALA"};
  Serial.printf("[EXHAUST] %s (%d)\n", status_names[status], status);
}

// ==================== SENSOR READ ====================
void readSensors() {
  current_temperature = dht.readTemperature();
  current_humidity = dht.readHumidity();

  int retry = 3;
  while ((isnan(current_temperature) || isnan(current_humidity)) && retry--) {
    delay(500);
    current_temperature = dht.readTemperature();
    current_humidity = dht.readHumidity();
  }

  if (isnan(current_temperature) || isnan(current_humidity)) {
    Serial.println("[ERROR] DHT read failed");
    current_temperature = 0.0;
    current_humidity = 0.0;
  }
}

// ==================== SYNC TO GATEWAY ====================
void syncWithGateway() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://" + String(gatewayIP) + ":" + String(gatewayPort) + "/api/node/sync";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  DynamicJsonDocument doc(1024);
  doc["node_id"] = NODE_ID;
  doc["node_type"] = "exhaust";
  doc["temperature"] = round(current_temperature * 10) / 10.0;
  doc["humidity"] = round(current_humidity * 10) / 10.0;
  doc["exhaust_status"] = exhaust_status;
  doc["mode"] = manual_mode ? "MANUAL" : "AUTO";

  // ===== LATENCY METADATA =====
  sequence_no++;
  doc["t_send"] = millis();
  doc["sequence_no"] = sequence_no;
  doc["rssi"] = WiFi.RSSI();

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    String response = http.getString();
    DynamicJsonDocument res(512);

    if (!deserializeJson(res, response)) {

      if (res.containsKey("mode")) {
        manual_mode = (res["mode"].as<String>() == "MANUAL");
      }

      if (manual_mode && res.containsKey("actuator_command")) {
        bool cmd = res["actuator_command"];
        setExhaust(cmd ? 2 : 0);
      }

      if (!manual_mode && res.containsKey("fuzzy_exhaust_status")) {
        int fuzzy_status = res["fuzzy_exhaust_status"];
        setExhaust(fuzzy_status);
      }

      Serial.printf(
        "[SYNC] OK | seq=%lu | RSSI=%d dBm | mode=%s\n",
        sequence_no,
        WiFi.RSSI(),
        manual_mode ? "MANUAL" : "AUTO-FUZZY"
      );
    }
  } else {
    Serial.printf("[SYNC] HTTP Error %d\n", httpCode);
  }

  http.end();
}

// ==================== PRINT STATUS ====================
void printStatus() {
  Serial.println("\n================ NODE STATUS ================");
  Serial.printf("Node ID   : %d\n", NODE_ID);
  Serial.printf("Temp      : %.1f °C\n", current_temperature);
  Serial.printf("Humidity  : %.1f %%\n", current_humidity);
  Serial.printf("Exhaust   : %d\n", exhaust_status);
  Serial.printf("Mode      : %s\n", manual_mode ? "MANUAL" : "AUTO-FUZZY");
  Serial.printf("Sequence  : %lu\n", sequence_no);
  Serial.printf("RSSI      : %d dBm\n", WiFi.RSSI());
  Serial.println("============================================\n");
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(RELAY_PIN, OUTPUT);
  setExhaust(0);

  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected!");
  Serial.println(WiFi.localIP());

  readSensors();
  syncWithGateway();
}

// ==================== LOOP ====================
void loop() {
  unsigned long now = millis();

  if (now - last_sensor_read >= SENSOR_INTERVAL) {
    readSensors();
    last_sensor_read = now;
  }

  if (now - last_gateway_sync >= SYNC_INTERVAL) {
    syncWithGateway();
    last_gateway_sync = now;
  }

  if (now - last_status_print >= PRINT_INTERVAL) {
    printStatus();
    last_status_print = now;
  }

  delay(100);
}
