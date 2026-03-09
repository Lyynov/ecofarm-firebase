// ============================================================
// NODE 1 - EXHAUST CONTROL (FIXED FOR ARDUINOJSON V7)
// ============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ==================== KONFIGURASI NETWORK ====================
const char* ssid        = "EcoFarm-Gateway";
const char* password    = "ecofarm123";
const char* gatewayIP   = "192.168.4.1";
const int   gatewayPort = 8080;

// ==================== KONFIGURASI HARDWARE ====================
#define DHT_PIN          4
#define RELAY_PIN        2
#define DHT_TYPE         DHT22
#define NODE_ID          1
#define RELAY_ACTIVE_LOW true

// ==================== STATE VARIABLES ====================
DHT dht(DHT_PIN, DHT_TYPE);

int   exhaust_status   = 0;     // 0=OFF, 1=MEDIUM, 2=ON
bool manual_mode       = false;

float current_temperature = 0.0;
float current_humidity    = 0.0;

uint32_t sequence_no = 0;

// ==================== TIMING ====================
unsigned long last_sensor_read  = 0;
unsigned long last_gateway_sync = 0;
unsigned long last_status_print = 0;

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long SYNC_INTERVAL   = 5000;
const unsigned long PRINT_INTERVAL  = 10000;

// ==================== RELAY CONTROL ====================
void setExhaust(int status) {
  if (status < 0) status = 0;
  if (status > 2) status = 2;
  bool relay_on     = (status > 0);
  bool output_level = RELAY_ACTIVE_LOW ? !relay_on : relay_on;
  digitalWrite(RELAY_PIN, output_level);
  exhaust_status = status;
  const char* names[] = {"OFF", "MEDIUM", "ON"};
  Serial.printf("[EXHAUST] -> %s (status=%d) pin=%s\n",
    names[status], status, relay_on ? "HIGH" : "LOW");
}

// ==================== WIFI ====================
void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("[WIFI] Reconnecting...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  int att = 0;
  while (WiFi.status() != WL_CONNECTED && att < 20) {
    delay(500); Serial.print("."); att++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("[WIFI] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("[WIFI] Reconnect failed");
}

// ==================== SENSOR READ ====================
void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int retry = 3;
  while ((isnan(t) || isnan(h)) && retry-- > 0) {
    delay(500);
    t = dht.readTemperature();
    h = dht.readHumidity();
  }
  if (isnan(t) || isnan(h)) {
    Serial.println("[DHT] Read failed - keeping last value");
  } else {
    current_temperature = t;
    current_humidity    = h;
  }
}

// ==================== HANDLE GATEWAY RESPONSE ====================
void handleGatewayResponse(const JsonDocument& res) {

  // STEP 1: Parse mode (Fixed for ArduinoJson V7)
  bool new_manual_mode = manual_mode;
  if (res.containsKey("mode")) {
    String modeStr = res["mode"].as<String>();
    modeStr.toUpperCase();
    new_manual_mode = (modeStr == "MANUAL");
  } else if (res.containsKey("manual_mode")) {
    // Cek apakah data berupa int atau boolean secara aman
    if (res["manual_mode"].is<int>()) {
        new_manual_mode = (res["manual_mode"].as<int>() == 1);
    } else {
        new_manual_mode = res["manual_mode"].as<bool>();
    }
  }

  if (new_manual_mode != manual_mode) {
    Serial.printf("[MODE] %s -> %s\n",
      manual_mode ? "MANUAL" : "AUTO",
      new_manual_mode ? "MANUAL" : "AUTO");
  }
  manual_mode = new_manual_mode;

  // STEP 2: Parse actuator command (Fixed for ArduinoJson V7)
  bool actuator_cmd = false;
  bool cmd_received = false;

  if (res.containsKey("relay_command")) {
    cmd_received = true;
    if (res["relay_command"].is<int>()) {
        actuator_cmd = (res["relay_command"].as<int>() == 1);
    } else {
        actuator_cmd = res["relay_command"].as<bool>();
    }
  } else if (res.containsKey("actuator_command")) {
    cmd_received = true;
    if (res["actuator_command"].is<int>()) {
        actuator_cmd = (res["actuator_command"].as<int>() == 1);
    } else {
        actuator_cmd = res["actuator_command"].as<bool>();
    }
  }

  // STEP 3: Eksekusi ke relay sesuai mode
  if (manual_mode) {
    if (cmd_received) {
      int target = actuator_cmd ? 2 : 0;
      if (target != exhaust_status) {
        Serial.printf("[MANUAL] CMD=%s -> setExhaust(%d)\n",
          actuator_cmd ? "ON" : "OFF", target);
        setExhaust(target);
      }
    } else {
      Serial.println("[MANUAL] No actuator command in response");
    }
  } else {
    if (res.containsKey("fuzzy_exhaust_status")) {
      int fuzzy_status = res["fuzzy_exhaust_status"].as<int>();
      if (fuzzy_status != exhaust_status) {
        Serial.printf("[AUTO] Fuzzy: %d -> %d\n", exhaust_status, fuzzy_status);
        setExhaust(fuzzy_status);
      }
    }
  }

  // STEP 4: Log PDR
  if (res.containsKey("pdr_stats")) {
    float pdr = res["pdr_stats"]["pdr_percentage"] | 0.0f;
    const char* st = res["pdr_stats"]["status"] | "Unknown";
    Serial.printf("[PDR] %.1f%% (%s)\n", pdr, st);
  }
}

// ==================== SYNC TO GATEWAY ====================
void syncWithGateway() {
  ensureWiFiConnected();
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://" + String(gatewayIP) + ":" + String(gatewayPort) + "/api/node/sync";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  JsonDocument doc; // V7 automatically manages memory
  doc["node_id"]        = NODE_ID;
  doc["node_type"]      = "exhaust";
  doc["temperature"]    = round(current_temperature * 10) / 10.0;
  doc["humidity"]       = round(current_humidity    * 10) / 10.0;
  doc["exhaust_status"] = exhaust_status;
  doc["mode"]           = manual_mode ? "MANUAL" : "AUTO";
  sequence_no++;
  doc["sequence_no"]    = sequence_no;
  doc["t_send"]         = millis();
  doc["rssi"]           = WiFi.RSSI();

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    String response = http.getString();
    JsonDocument res;
    DeserializationError err = deserializeJson(res, response);
    if (!err) {
      handleGatewayResponse(res);
    }
  }
  http.end();
}

void printStatus() {
  const char* ex_names[] = {"OFF", "MEDIUM", "ON"};
  Serial.println("\n===== NODE 1 STATUS =====");
  Serial.printf("Temp     : %.1f C\n",  current_temperature);
  Serial.printf("Humidity : %.1f %%\n", current_humidity);
  Serial.printf("Exhaust  : %s (%d)\n", ex_names[exhaust_status], exhaust_status);
  Serial.printf("Mode     : %s\n",      manual_mode ? "MANUAL" : "AUTO-FUZZY");
  Serial.println("=========================\n");
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  setExhaust(0);
  dht.begin();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nConnected.");
}

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