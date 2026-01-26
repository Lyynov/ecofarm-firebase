/*
 * ESP32 NUTRIENT CONTROL SYSTEM - NODE SENSOR 2
 * Features: TDS/PPM Control (Fuzzy Logic), pH Monitoring
 * Mode: Auto (Fuzzy di Node) / Manual (Command dari Gateway)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "GravityTDS.h"

// ==================== KONFIGURASI NETWORK ====================
const char* ssid = "smk unnes semarang"; 
const char* password = "12345678";  
const char* gatewayIP = "192.168.137.198";  // ⚠️ SESUAIKAN DENGAN IP GATEWAY!
const int gatewayPort = 8080;
const int nodeID = 3;  // ⚠️ UBAH UNTUK SETIAP NODE (1-4)

// ==================== KONFIGURASI HARDWARE ====================
const int PUMP_NUTRISI_A_PIN = 12;
const int PUMP_NUTRISI_B_PIN = 14;
const int TDS_SENSOR_PIN = 35;
const int PH_SENSOR_PIN = 34;

const bool RELAY_ACTIVE_LOW = true;

// ==================== PARAMETER KONTROL ====================
float setpointPPM = 800.0;  // Default - akan diambil dari EEPROM
bool isAutoMode = true;
bool manualPumpCommand = false;

// ==================== FUZZY LOGIC PARAMETERS ====================
const float ERR_LOW = 10.0;
const float ERR_MED = 30.0;
const float ERR_HIGH = 60.0;

const unsigned long DUR_FAST = 1000;
const unsigned long DUR_MED = 3000;
const unsigned long DUR_LONG = 7000;
const unsigned long DUR_VERY_LONG = 12000;

struct FuzzyMembership { 
  float fast, medium, long_dur, very_long; 
};

// ==================== SENSOR VARIABLES ====================
GravityTDS gravityTds;
float sensorPPM = 0.0;
float sensorPH = 7.0;

// ==================== PUMP CONTROL STATE ====================
bool pumpRunning = false;
unsigned long pumpStartTime = 0;
unsigned long pumpDuration = 0;

// ==================== HOMOGENIZATION STATE ====================
bool waitingMix = false;
unsigned long mixStartTime = 0;
const unsigned long MIX_DELAY = 15000;

// ==================== TDS SMOOTHING BUFFER ====================
#define BUFFER_SIZE 7
float tdsBuffer[BUFFER_SIZE];
int bufferIndex = 0;
bool bufferReady = false;

// ==================== TIMING ====================
unsigned long lastSensorRead = 0;
unsigned long lastGatewaySync = 0;
unsigned long lastSerialPrint = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastConfigSave = 0;

const unsigned long SENSOR_INTERVAL = 1000;
const unsigned long GATEWAY_INTERVAL = 5000;  // Sync setiap 5 detik
const unsigned long PRINT_INTERVAL = 10000;
const unsigned long WIFI_CHECK_INTERVAL = 10000;
const unsigned long CONFIG_SAVE_INTERVAL = 60000;

// ==================== CONNECTION STATUS ====================
bool gatewayOnline = false;
unsigned long lastSuccessSync = 0;
int syncFailCount = 0;
const int MAX_SYNC_FAILS = 10;

// ==================== EEPROM STORAGE ====================
Preferences preferences;
bool configChanged = false;

// ==================== EEPROM FUNCTIONS ====================
void loadConfig() {
  preferences.begin("node-config", false);
  
  if (preferences.isKey("setpoint")) {
    setpointPPM = preferences.getFloat("setpoint", 800.0);
    Serial.printf("[CONFIG] Loaded setpoint: %.1f ppm\n", setpointPPM);
  } else {
    preferences.putFloat("setpoint", setpointPPM);
    Serial.println("[CONFIG] Initialized with default setpoint: 800 ppm");
  }
  
  preferences.end();
}

void saveConfig() {
  if (!configChanged) return;
  
  preferences.begin("node-config", false);
  preferences.putFloat("setpoint", setpointPPM);
  preferences.end();
  
  configChanged = false;
  Serial.printf("[CONFIG] Saved setpoint: %.1f ppm\n", setpointPPM);
}

// ==================== RELAY CONTROL ====================
void setPump(bool state) {
  bool outputLevel = RELAY_ACTIVE_LOW ? !state : state;
  digitalWrite(PUMP_NUTRISI_A_PIN, outputLevel);
  digitalWrite(PUMP_NUTRISI_B_PIN, outputLevel);
}

void turnPumpOn() {
  setPump(true);
  pumpRunning = true;
}

void turnPumpOff() {
  setPump(false);
  pumpRunning = false;
}

// ==================== SENSOR READING ====================
void readSensors() {
  // Read TDS Sensor
  int rawADC = analogRead(TDS_SENSOR_PIN);
  float voltage = rawADC * (3.3 / 4095.0);
  
  gravityTds.setTemperature(25.0);
  gravityTds.update();
  float tdsValue = gravityTds.getTdsValue();

  if (tdsValue < 1.0 && voltage > 0.1) {
    tdsValue = (133.42 * pow(voltage, 3) - 255.86 * pow(voltage, 2) + 857.39 * voltage) * 0.5;
  }
  if (isnan(tdsValue) || tdsValue < 0) tdsValue = 0;

  // Moving Average Filter
  tdsBuffer[bufferIndex] = tdsValue;
  bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
  if (bufferIndex == 0) bufferReady = true;
  
  float sum = 0;
  int count = bufferReady ? BUFFER_SIZE : bufferIndex;
  for(int i = 0; i < count; i++) {
    sum += tdsBuffer[i];
  }
  sensorPPM = (count > 0) ? (sum / count) : 0;

  // Read pH Sensor
  int rawPH = analogRead(PH_SENSOR_PIN);
  float voltagePH = rawPH * (3.3 / 4095.0);
  sensorPH = 7.0 - ((voltagePH - 2.5) / 0.18);
  
  if (sensorPH < 0) sensorPH = 0;
  if (sensorPH > 14) sensorPH = 14;
}

// ==================== FUZZY LOGIC ENGINE ====================
FuzzyMembership calculateFuzzyMembership(float errorPercent) {
  FuzzyMembership membership = {0, 0, 0, 0};
  
  if (errorPercent <= ERR_LOW) {
    membership.fast = 1.0;
  } 
  else if (errorPercent <= ERR_MED) {
    membership.medium = (errorPercent - ERR_LOW) / (ERR_MED - ERR_LOW);
    membership.fast = 1.0 - membership.medium;
  } 
  else if (errorPercent <= ERR_HIGH) {
    membership.long_dur = (errorPercent - ERR_MED) / (ERR_HIGH - ERR_MED);
    membership.medium = 1.0 - membership.long_dur;
  } 
  else {
    membership.very_long = 1.0;
  }
  
  return membership;
}

unsigned long defuzzifyDuration(FuzzyMembership m) {
  float numerator = (m.fast * DUR_FAST) + 
                    (m.medium * DUR_MED) + 
                    (m.long_dur * DUR_LONG) + 
                    (m.very_long * DUR_VERY_LONG);
  
  float denominator = m.fast + m.medium + m.long_dur + m.very_long;
  
  if (denominator == 0) return DUR_MED;
  
  return (unsigned long)(numerator / denominator);
}

// ==================== CONTROL LOGIC ====================
void executeControl() {
  unsigned long now = millis();

  // MODE MANUAL
  if (!isAutoMode) {
    pumpRunning = false;
    waitingMix = false;
    setPump(manualPumpCommand);
    return;
  }

  // MODE AUTO - FUZZY LOGIC
  
  if (pumpRunning) {
    if (now - pumpStartTime >= pumpDuration) {
      turnPumpOff();
      waitingMix = true;
      mixStartTime = now;
      Serial.printf("[FUZZY] Pump OFF - Waiting for mix (%d sec)\n", MIX_DELAY/1000);
    }
    return;
  }

  if (waitingMix) {
    if (now - mixStartTime >= MIX_DELAY) {
      waitingMix = false;
      Serial.println("[FUZZY] Mix complete - Ready for next cycle");
    }
    return;
  }

  // FUZZY CONTROL DECISION
  if (sensorPPM < setpointPPM && sensorPPM > 10.0) {
    float errorPct = ((setpointPPM - sensorPPM) / setpointPPM) * 100.0;
    
    FuzzyMembership fuzzyMembership = calculateFuzzyMembership(errorPct);
    pumpDuration = defuzzifyDuration(fuzzyMembership);
    
    Serial.printf("[FUZZY] Error: %.1f%% -> Duration: %lu ms\n", errorPct, pumpDuration);
    
    pumpStartTime = now;
    turnPumpOn();
  }
}

// ==================== GATEWAY COMMUNICATION ====================
void syncWithGateway() {
  if (WiFi.status() != WL_CONNECTED) {
    gatewayOnline = false;
    return;
  }

  HTTPClient http;
  String url = "http://" + String(gatewayIP) + ":" + String(gatewayPort) + "/api/node/sync";
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  // Prepare data
  #if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument jsonDoc;
  #else
    DynamicJsonDocument jsonDoc(512);
  #endif
  
  jsonDoc["node_id"] = nodeID;
  jsonDoc["ppm"] = round(sensorPPM * 10) / 10.0;
  jsonDoc["ph"] = round(sensorPH * 100) / 100.0;
  jsonDoc["pump_active"] = pumpRunning || (manualPumpCommand && !isAutoMode);
  jsonDoc["mode"] = isAutoMode ? "AUTO" : "MANUAL";
  jsonDoc["setpoint"] = setpointPPM;
  jsonDoc["timestamp"] = millis();
  
  String jsonPayload;
  serializeJson(jsonDoc, jsonPayload);

  // POST request
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode == 200) {
    gatewayOnline = true;
    lastSuccessSync = millis();
    syncFailCount = 0;
    
    String response = http.getString();
    
    #if ARDUINOJSON_VERSION_MAJOR >= 7
      JsonDocument responseDoc;
    #else
      DynamicJsonDocument responseDoc(512);
    #endif
    
    DeserializationError error = deserializeJson(responseDoc, response);
    
    if (!error) {
      // Update setpoint dari gateway
      if (responseDoc.containsKey("setpoint")) {
        float newSetpoint = responseDoc["setpoint"];
        if (newSetpoint >= 200 && newSetpoint <= 2000) {
          if (abs(newSetpoint - setpointPPM) > 1.0) {
            Serial.printf("[GATEWAY] Setpoint updated: %.1f -> %.1f ppm\n", setpointPPM, newSetpoint);
            setpointPPM = newSetpoint;
            configChanged = true;
          }
        }
      }
      
      // Update mode
      if (responseDoc.containsKey("mode")) {
        String mode = responseDoc["mode"].as<String>();
        bool newMode = (mode == "AUTO");
        if (newMode != isAutoMode) {
          Serial.printf("[GATEWAY] Mode changed: %s -> %s\n", 
                        isAutoMode ? "AUTO" : "MANUAL", 
                        newMode ? "AUTO" : "MANUAL");
          isAutoMode = newMode;
        }
      }
      
      // Update manual pump command
      if (responseDoc.containsKey("actuator_command")) {
        bool newPumpCmd = responseDoc["actuator_command"];
        if (newPumpCmd != manualPumpCommand) {
          Serial.printf("[GATEWAY] Manual pump: %s -> %s\n", 
                        manualPumpCommand ? "ON" : "OFF", 
                        newPumpCmd ? "ON" : "OFF");
          manualPumpCommand = newPumpCmd;
        }
      }
      
      Serial.printf("[SYNC] ✓ Success (Node %d)\n", nodeID);
    }
  } else {
    gatewayOnline = false;
    syncFailCount++;
    
    // Fallback ke AUTO jika gateway offline terlalu lama
    if (syncFailCount >= MAX_SYNC_FAILS && !isAutoMode) {
      Serial.println("[GATEWAY] Too many failures - forcing AUTO mode");
      isAutoMode = true;
      manualPumpCommand = false;
    }
    
    if (syncFailCount % 5 == 0) {
      Serial.printf("[SYNC] ✗ Failed (%d): HTTP %d\n", syncFailCount, httpResponseCode);
    }
  }
  
  http.end();
}

// ==================== WIFI MANAGEMENT ====================
void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Reconnecting...");
    WiFi.reconnect();
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WiFi] ✓ Reconnected!");
    }
  }
}

// ==================== STATUS MONITORING ====================
void printStatus() {
  unsigned long now = millis();
  
  Serial.println("\n===============================================");
  Serial.printf("[NODE %d] Nutrient Control System\n", nodeID);
  Serial.println("===============================================");
  
  // Sensor data
  Serial.printf("[SENSOR]  PPM: %.1f ppm | pH: %.2f | Setpoint: %.0f ppm\n", 
                sensorPPM, sensorPH, setpointPPM);
  
  // Gateway status
  Serial.printf("[GATEWAY] ");
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
  } else if (gatewayOnline) {
    int secAgo = (now - lastSuccessSync) / 1000;
    Serial.printf("ONLINE - Last sync: %ds ago\n", secAgo);
  } else {
    Serial.printf("OFFLINE (Fails: %d)\n", syncFailCount);
  }
  
  // Operation status
  Serial.printf("[MODE]    ");
  Serial.print(isAutoMode ? "AUTO (Fuzzy)" : "MANUAL (Gateway)");
  Serial.print(" | Status: ");
  
  if (pumpRunning) {
    int elapsed = (now - pumpStartTime) / 1000;
    int total = pumpDuration / 1000;
    Serial.printf("PUMPING (%d/%ds)\n", elapsed, total);
  } 
  else if (waitingMix) {
    int remaining = (MIX_DELAY - (now - mixStartTime)) / 1000;
    Serial.printf("MIXING (%ds left)\n", remaining);
  } 
  else if (!isAutoMode && manualPumpCommand) {
    Serial.println("PUMP ON (Manual)");
  }
  else {
    Serial.println("STANDBY");
  }
  
  Serial.println("===============================================\n");
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n===============================================");
  Serial.println("   ESP32 NUTRIENT CONTROL - NODE SENSOR 2");
  Serial.println("   TDS/PPM: Fuzzy Control | pH: Monitor");
  Serial.println("===============================================\n");
  
  // Load config
  loadConfig();
  
  // Setup pins
  pinMode(PUMP_NUTRISI_A_PIN, OUTPUT);
  pinMode(PUMP_NUTRISI_B_PIN, OUTPUT);
  pinMode(TDS_SENSOR_PIN, INPUT);
  pinMode(PH_SENSOR_PIN, INPUT);
  
  turnPumpOff();
  Serial.println("✓ GPIO Pins Configured");

  // WiFi connection
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✓ WiFi Connected!");
    Serial.printf("  Node IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  Gateway: %s:%d\n", gatewayIP, gatewayPort);
  } else {
    Serial.println("✗ WiFi Failed - Running standalone AUTO mode");
  }

  // TDS sensor init
  gravityTds.setPin(TDS_SENSOR_PIN);
  gravityTds.setAref(3.3);
  gravityTds.setAdcRange(4096);
  gravityTds.begin();
  
  Serial.println("✓ TDS Sensor Initialized");
  Serial.println("✓ pH Sensor Initialized");
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.printf("║  Node ID      : %-23d║\n", nodeID);
  Serial.printf("║  Setpoint PPM : %-23.0f║\n", setpointPPM);
  Serial.printf("║  Mode         : %-23s║\n", "AUTO (Fuzzy)");
  Serial.println("╠════════════════════════════════════════╣");
  Serial.println("║  Fuzzy Logic berjalan di NODE ini     ║");
  Serial.println("║  Gateway hanya untuk monitoring       ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  readSensors();
  Serial.println("System ready!\n");
}

// ==================== LOOP ====================
void loop() {
  unsigned long now = millis();

  // Read sensors
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    readSensors();
    lastSensorRead = now;
  }

  // Execute control
  executeControl();

  // Sync with gateway
  if (now - lastGatewaySync >= GATEWAY_INTERVAL) {
    syncWithGateway();
    lastGatewaySync = now;
  }

  // Check WiFi
  if (now - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
    checkWiFiConnection();
    lastWiFiCheck = now;
  }

  // Save config
  if (configChanged && (now - lastConfigSave >= CONFIG_SAVE_INTERVAL)) {
    saveConfig();
    lastConfigSave = now;
  }

  // Print status
  if (now - lastSerialPrint >= PRINT_INTERVAL) {
    printStatus();
    lastSerialPrint = now;
  }
  
  yield();
}
