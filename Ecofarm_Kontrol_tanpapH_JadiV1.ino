/*
 * ESP32 Nutrient Control Node
 * Static Gateway IP - No more IP changes!
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include "GravityTDS.h"

// ===== NETWORK CONFIGURATION =====
const char* ssid = "RAFALEON";
const char* password = "11112222";

//STATIC GATEWAY IP - TIDAK PERLU GANTI LAGI!
const char* server_ip = "192.168.137.100";  // ← IP Gateway yang FIXED
const int server_port = 8080;

// ===== NODE CONFIGURATION =====
#define NODE_ID 3
#define NODE_TYPE "nutrient"

// ===== PIN CONFIGURATION =====
const int PUMP_NUTRISI_A_PIN = 12;
const int PUMP_NUTRISI_B_PIN = 14;
const int TDS_SENSOR_PIN = 35;

// ===== CONTROL CONFIGURATION =====
const bool RELAY_ACTIVE_LOW = false;
float TARGET_PPM = 800.0;

// ===== TDS SENSOR CONFIGURATION =====
#define EEPROM_SIZE 512
GravityTDS gravityTds;
float waterTemperature = 25.0;

// Moving Average Buffer
#define BUFFER_SIZE 7
float tdsBuffer[BUFFER_SIZE];
int tdsBufferIndex = 0;
bool tdsBufferFull = false;

// ===== SYSTEM VARIABLES =====
float currentPPM = 0.0;
float rawPPM = 0.0;
float targetPPM = TARGET_PPM;
bool manual_mode = false;
bool pumpsActive = false;

unsigned long pumpStartTime = 0;
unsigned long pumpDuration = 0;
bool waitingHomogenization = false;
unsigned long homogenizationStartTime = 0;
const unsigned long HOMOGENIZATION_DELAY = 120000;

// ===== TIMING =====
unsigned long last_sensor_read = 0;
unsigned long last_data_send = 0;
unsigned long last_control_check = 0;
unsigned long last_serial_print = 0;
const unsigned long SENSOR_INTERVAL = 1000;
const unsigned long SEND_INTERVAL = 10000;
const unsigned long CONTROL_INTERVAL = 2000;
const unsigned long SERIAL_PRINT_INTERVAL = 2000;
unsigned long last_control_timestamp = 0;

// ===== FUZZY LOGIC (4 RULES) =====
enum FuzzyMode { FAST, MEDIUM, LONG, VERY_LONG };
FuzzyMode currentFuzzyMode = MEDIUM;

const float FUZZY_ERROR_LOW = 40.0;
const float FUZZY_ERROR_MEDIUM = 80.0;
const float FUZZY_ERROR_HIGH = 100.0;

const unsigned long DURATION_FAST = 1000;
const unsigned long DURATION_MEDIUM = 3000;
const unsigned long DURATION_LONG = 8000;
const unsigned long DURATION_VERY_LONG = 12000;

struct FuzzyMembership {
  float fast, medium, long_duration, very_long;
};

// ===== FUNCTION PROTOTYPES =====
void connectWiFi();
void readSensors();
void sendSensorData();
void checkControlCommands();
void setPumps(bool state);
void autoControlPump();
void addToTDSBuffer(float value);
float getSmoothedTDS();
float calculateErrorPercentage(float current, float target);
FuzzyMode determineFuzzyMode(float errorPercentage);
FuzzyMembership calculateMembership(float errorPercentage);
unsigned long defuzzification(FuzzyMembership membership);
void startPumps(unsigned long duration);
void stopPumps();
const char* fuzzyModeToString(FuzzyMode mode);
void printSensorStatus();

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("ESP32 Nutrient Control Node");
  Serial.println("AB Mix System with Static Gateway IP");
  Serial.printf("Node ID: %d\n", NODE_ID);
  Serial.printf("Gateway IP: %s (FIXED)\n", server_ip);
  Serial.printf("Target PPM: %.1f\n", targetPPM);
  Serial.println("========================================");
  
  EEPROM.begin(EEPROM_SIZE);
  
  gravityTds.setPin(TDS_SENSOR_PIN);
  gravityTds.setAref(3.3);
  gravityTds.setAdcRange(4096);
  gravityTds.begin();
  Serial.println("✓ GravityTDS Library initialized");
  
  pinMode(PUMP_NUTRISI_A_PIN, OUTPUT);
  pinMode(PUMP_NUTRISI_B_PIN, OUTPUT);
  setPumps(false);
  Serial.println("✓ Pompa A & B initialized (OFF)");
  
  for (int i = 0; i < BUFFER_SIZE; i++) tdsBuffer[i] = 0;
  
  connectWiFi();
  checkControlCommands();
  
  Serial.println("✓ System Ready - AUTO Mode Active");
  Serial.println("========================================\n");
  
  Serial.println("Warming up TDS sensor (5 samples)...");
  for (int i = 0; i < 5; i++) {
    gravityTds.setTemperature(waterTemperature);
    gravityTds.update();
    float tds = gravityTds.getTdsValue();
    addToTDSBuffer(tds);
    Serial.printf("  Sample %d: %.1f ppm\n", i+1, tds);
    delay(1000);
  }
  Serial.println("✓ Sensor ready!\n");
}

void loop() {
  unsigned long current_time = millis();
  
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  
  if (current_time - last_sensor_read >= SENSOR_INTERVAL) {
    readSensors();
    last_sensor_read = current_time;
  }
  
  if (current_time - last_serial_print >= SERIAL_PRINT_INTERVAL) {
    printSensorStatus();
    last_serial_print = current_time;
  }
  
  if (current_time - last_data_send >= SEND_INTERVAL) {
    sendSensorData();
    last_data_send = current_time;
  }
  
  if (current_time - last_control_check >= CONTROL_INTERVAL) {
    checkControlCommands();
    last_control_check = current_time;
  }
  
  if (waitingHomogenization) {
    if (current_time - homogenizationStartTime >= HOMOGENIZATION_DELAY) {
      waitingHomogenization = false;
      Serial.println("✓ Homogenisasi selesai\n");
    }
  }
  
  if (!manual_mode && !waitingHomogenization) autoControlPump();
  if (pumpsActive && (current_time - pumpStartTime >= pumpDuration)) stopPumps();
  
  delay(100);
}

void connectWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected!");
    Serial.print("Node IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("Gateway IP: %s (Static)\n", server_ip);
  }
}

void readSensors() {
  gravityTds.setTemperature(waterTemperature);
  gravityTds.update();
  rawPPM = gravityTds.getTdsValue();
  addToTDSBuffer(rawPPM);
  currentPPM = getSmoothedTDS();
}

void addToTDSBuffer(float value) {
  tdsBuffer[tdsBufferIndex] = value;
  tdsBufferIndex = (tdsBufferIndex + 1) % BUFFER_SIZE;
  if (tdsBufferIndex == 0) tdsBufferFull = true;
}

float getSmoothedTDS() {
  float sum = 0;
  int count = tdsBufferFull ? BUFFER_SIZE : tdsBufferIndex;
  if (count == 0) return rawPPM;
  for (int i = 0; i < count; i++) sum += tdsBuffer[i];
  return sum / count;
}

float calculateErrorPercentage(float current, float target) {
  if (target == 0) return 0;
  return (abs(target - current) / target) * 100.0;
}

FuzzyMode determineFuzzyMode(float errorPercentage) {
  if (errorPercentage > FUZZY_ERROR_HIGH) return VERY_LONG;
  if (errorPercentage > FUZZY_ERROR_MEDIUM) return LONG;
  if (errorPercentage > FUZZY_ERROR_LOW) return MEDIUM;
  return FAST;
}

FuzzyMembership calculateMembership(float errorPercentage) {
  FuzzyMembership m = {0, 0, 0, 0};
  
  if (errorPercentage <= FUZZY_ERROR_LOW) {
    m.fast = 1.0 - (errorPercentage / FUZZY_ERROR_LOW);
  }
  if (errorPercentage > FUZZY_ERROR_LOW && errorPercentage <= FUZZY_ERROR_MEDIUM) {
    m.medium = (errorPercentage - FUZZY_ERROR_LOW) / (FUZZY_ERROR_MEDIUM - FUZZY_ERROR_LOW);
  }
  if (errorPercentage > FUZZY_ERROR_MEDIUM && errorPercentage <= FUZZY_ERROR_HIGH) {
    m.long_duration = (errorPercentage - FUZZY_ERROR_MEDIUM) / (FUZZY_ERROR_HIGH - FUZZY_ERROR_MEDIUM);
  }
  if (errorPercentage > FUZZY_ERROR_HIGH) {
    m.very_long = 1.0;
  }
  
  return m;
}

unsigned long defuzzification(FuzzyMembership m) {
  float num = (m.fast * DURATION_FAST) + (m.medium * DURATION_MEDIUM) + 
              (m.long_duration * DURATION_LONG) + (m.very_long * DURATION_VERY_LONG);
  float den = m.fast + m.medium + m.long_duration + m.very_long;
  if (den == 0) return DURATION_MEDIUM;
  unsigned long duration = (unsigned long)(num / den);
  if (duration < 500) duration = 500;
  if (duration > 15000) duration = 15000;
  return duration;
}

void printSensorStatus() {
  float errorPct = calculateErrorPercentage(currentPPM, targetPPM);
  Serial.println("┌─────────────────────────────────────────────┐");
  Serial.printf("│ 📊 Raw: %.1f | Smooth: %.1f | Target: %.0f ppm\n", rawPPM, currentPPM, targetPPM);
  Serial.printf("│ Error: %.1f%% | Temp: %.1f°C\n", errorPct, waterTemperature);
  Serial.println("├─────────────────────────────────────────────┤");
  Serial.printf("│ Mode: %s | Pumps: %s | Fuzzy: %s\n", 
                manual_mode ? "MANUAL" : "AUTO  ", 
                pumpsActive ? "ON " : "OFF",
                fuzzyModeToString(currentFuzzyMode));
  if (waitingHomogenization) {
    unsigned long rem = HOMOGENIZATION_DELAY - (millis() - homogenizationStartTime);
    Serial.printf("│ Homogenisasi: %lu s tersisa\n", rem / 1000);
  }
  Serial.println("└─────────────────────────────────────────────┘\n");
}

void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  http.begin(String("http://") + server_ip + ":" + server_port + "/sensor-data");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  
  JsonDocument doc;
  doc["node_id"] = NODE_ID;
  doc["node_type"] = NODE_TYPE;
  doc["ppm"] = currentPPM;
  doc["rawPPM"] = rawPPM;
  doc["targetPPM"] = targetPPM;
  doc["temperature"] = waterTemperature;
  doc["pumpAState"] = pumpsActive;
  doc["pumpBState"] = pumpsActive;
  doc["manual_mode"] = manual_mode;
  doc["fuzzyMode"] = fuzzyModeToString(currentFuzzyMode);
  doc["homogenizing"] = waitingHomogenization;
  doc["timestamp"] = millis();
  
  String json_string;
  serializeJson(doc, json_string);
  
  Serial.printf("[SEND→Gateway] %s | PPM: %.1f\n", server_ip, currentPPM);
  int code = http.POST(json_string);
  Serial.printf("[%s] HTTP %d\n", code > 0 ? "✓" : "✗", code);
  
  http.end();
}

void checkControlCommands() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  http.begin(String("http://") + server_ip + ":" + server_port + "/api/control/" + NODE_ID);
  http.setTimeout(3000);
  
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      if (doc["manual_mode"].is<bool>() && doc["relay_command"].is<bool>()) {
        bool new_manual = doc["manual_mode"];
        bool new_relay = doc["relay_command"];
        unsigned long srv_ts = doc["timestamp"];
        
        if (doc["targetPPM"].is<float>()) {
          float newTarget = doc["targetPPM"];
          if (newTarget != targetPPM) {
            Serial.printf("[UPDATE] Target: %.0f → %.0f ppm\n", targetPPM, newTarget);
            targetPPM = newTarget;
          }
        }
        if (doc["temperature"].is<float>()) waterTemperature = doc["temperature"];
        
        if (srv_ts > last_control_timestamp) {
          if (new_manual != manual_mode) {
            manual_mode = new_manual;
            Serial.printf("[MODE] %s\n", manual_mode ? "MANUAL" : "AUTO");
            if (!manual_mode) stopPumps();
          }
          
          if (manual_mode) {
            if (new_relay) {
              startPumps(3000);
              Serial.println("[MANUAL] Pumps ON");
            } else {
              stopPumps();
              Serial.println("[MANUAL] Pumps OFF");
            }
          }
          
          last_control_timestamp = srv_ts;
        }
        manual_mode = new_manual;
      }
    }
  }
  http.end();
}

void autoControlPump() {
  if (pumpsActive || waitingHomogenization) return;
  
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();
  
  if (currentPPM < 0) return;
  
  float errorPct = calculateErrorPercentage(currentPPM, targetPPM);
  FuzzyMembership m = calculateMembership(errorPct);
  currentFuzzyMode = determineFuzzyMode(errorPct);
  unsigned long dur = defuzzification(m);
  
  if (abs(targetPPM - currentPPM) > 10.0 && currentPPM < targetPPM) {
    Serial.println("═══════════════════════════════════");
    Serial.printf("🧠 FUZZY: %.1f→%.0f ppm (Err: %.1f%%)\n", currentPPM, targetPPM, errorPct);
    Serial.printf("Mode: %s | Duration: %lu ms\n", fuzzyModeToString(currentFuzzyMode), dur);
    Serial.printf("μ: F=%.2f M=%.2f L=%.2f VL=%.2f\n", m.fast, m.medium, m.long_duration, m.very_long);
    startPumps(dur);
    Serial.println("⚡ POMPA A+B ON");
    Serial.println("═══════════════════════════════════\n");
  }
}

void setPumps(bool state) {
  digitalWrite(PUMP_NUTRISI_A_PIN, (RELAY_ACTIVE_LOW ? !state : state) ? HIGH : LOW);
  digitalWrite(PUMP_NUTRISI_B_PIN, (RELAY_ACTIVE_LOW ? !state : state) ? HIGH : LOW);
}

void startPumps(unsigned long duration) {
  setPumps(true);
  pumpsActive = true;
  pumpStartTime = millis();
  pumpDuration = duration;
}

void stopPumps() {
  setPumps(false);
  pumpsActive = false;
  waitingHomogenization = true;
  homogenizationStartTime = millis();
  Serial.println("⏸️  POMPA OFF - Homogenisasi 120s\n");
}

const char* fuzzyModeToString(FuzzyMode mode) {
  switch (mode) {
    case FAST: return "FAST";
    case MEDIUM: return "MEDIUM";
    case LONG: return "LONG";
    case VERY_LONG: return "VERY_LONG";
    default: return "UNKNOWN";
  }
}