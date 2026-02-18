/*
 * ESP32 NUTRIENT CONTROL SYSTEM - NODE SENSOR 2
 * Features: TDS/PPM Control (Fuzzy Logic 2 Input: Error & Delta Error)
 * Mode: Auto (Fuzzy di Node) / Manual (Command dari Gateway)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "GravityTDS.h"
#include <EEPROM.h>

// ==================== KONFIGURASI NETWORK ====================
const char* ssid = "smk unnes semarang"; 
const char* password = "12345678";  
const char* gatewayIP = "10.59.225.254";  // ⚠️ SESUAIKAN DENGAN IP GATEWAY!
const int gatewayPort = 8080;
const int nodeID = 3;  // ⚠️ UBAH UNTUK SETIAP NODE (1-4)

// ==================== KONFIGURASI HARDWARE ====================
const int PUMP_NUTRISI_A_PIN = 12;
const int PUMP_NUTRISI_B_PIN = 14;
const int TDS_SENSOR_PIN = 35;

const bool RELAY_ACTIVE_LOW = true;

// ==================== PARAMETER KONTROL ====================
float setpointPPM = 800.0;  // Default - akan diambil dari EEPROM
bool isAutoMode = true;
bool manualPumpCommand = false;

// ==================== FUZZY LOGIC PARAMETERS (2 INPUT) ====================
// Membership Function Parameters - ERROR (%)
const float ERR_LOW_A = 0.0;
const float ERR_LOW_B = 0.0;
const float ERR_LOW_C = 10.0;
const float ERR_LOW_D = 30.0;

const float ERR_MED_A = 10.0;
const float ERR_MED_B = 30.0;
const float ERR_MED_C = 60.0;

const float ERR_HIGH_A = 30.0;
const float ERR_HIGH_B = 60.0;
const float ERR_HIGH_C = 100.0;
const float ERR_HIGH_D = 100.0;

// Membership Function Parameters - DELTA ERROR (%/s)
const float DELTA_DEC_A = -10.0;
const float DELTA_DEC_B = -10.0;
const float DELTA_DEC_C = -5.0;
const float DELTA_DEC_D = 0.0;

const float DELTA_STABLE_A = -3.0;
const float DELTA_STABLE_B = 0.0;
const float DELTA_STABLE_C = 3.0;

const float DELTA_INC_A = 0.0;
const float DELTA_INC_B = 5.0;
const float DELTA_INC_C = 10.0;
const float DELTA_INC_D = 10.0;

// Output Duration (ms)
const unsigned long DUR_VERY_SHORT = 1000; // 5 Detik
const unsigned long DUR_SHORT = 5000;     // 10 Detik
const unsigned long DUR_MEDIUM = 9000;    // 20 Detik
const unsigned long DUR_LONG = 120000;     // 40 Detik
const unsigned long DUR_VERY_LONG = 15000; // 50 Detik

struct FuzzyMembership2Input {
  // Error memberships
  float err_low;
  float err_medium;
  float err_high;
  
  // Delta Error memberships
  float delta_decreasing;
  float delta_stable;
  float delta_increasing;
  
  // Output memberships
  float out_very_short;
  float out_short;
  float out_medium;
  float out_long;
  float out_very_long;
};

// ==================== SENSOR VARIABLES ====================
GravityTDS gravityTds;
float sensorPPM = 0.0;
float previousPPM = 0.0;
float errorPPM = 0.0;
float previousError = 0.0;
float deltaError = 0.0;

// ==================== PUMP CONTROL STATE ====================
bool pumpRunning = false;
unsigned long pumpStartTime = 0;
unsigned long pumpDuration = 0;

// ==================== HOMOGENIZATION STATE ====================
bool waitingMix = false;
unsigned long mixStartTime = 0;
const unsigned long MIX_DELAY = 15000; // masih 15 detik nanti diganti 2 menit

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
unsigned long lastErrorCalc = 0;

const unsigned long SENSOR_INTERVAL = 1000;
const unsigned long GATEWAY_INTERVAL = 5000;
const unsigned long PRINT_INTERVAL = 10000;
const unsigned long WIFI_CHECK_INTERVAL = 10000;
const unsigned long CONFIG_SAVE_INTERVAL = 60000;
const unsigned long ERROR_CALC_INTERVAL = 1000;

// ==================== CONNECTION STATUS ====================
bool gatewayOnline = false;
unsigned long lastSuccessSync = 0;
int syncFailCount = 0;
const int MAX_SYNC_FAILS = 10;

// Testing variables
unsigned long sequenceNo = 0;

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
  gravityTds.setTemperature(25.0);
  gravityTds.update();
  float tdsValue = gravityTds.getTdsValue();

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
  
  previousPPM = sensorPPM;
  sensorPPM = (count > 0) ? (sum / count) : 0;
}

// ==================== FUZZY MEMBERSHIP FUNCTIONS ====================
// Trapezoidal Membership Function
float trapezoidalMF(float x, float a, float b, float c, float d) {
  if (x <= a || x >= d) return 0.0;
  if (x >= b && x <= c) return 1.0;
  if (x > a && x < b) return (x - a) / (b - a);
  return (d - x) / (d - c);
}

// Triangular Membership Function
float triangularMF(float x, float a, float b, float c) {
  if (x <= a || x >= c) return 0.0;
  if (x == b) return 1.0;
  if (x > a && x < b) return (x - a) / (b - a);
  return (c - x) / (c - b);
}

// ==================== FUZZY LOGIC ENGINE (2 INPUT) ====================
FuzzyMembership2Input fuzzification(float error, float deltaErr) {
  FuzzyMembership2Input mf;
  
  // Fuzzifikasi ERROR (%)
  mf.err_low = trapezoidalMF(error, ERR_LOW_A, ERR_LOW_B, ERR_LOW_C, ERR_LOW_D);
  mf.err_medium = triangularMF(error, ERR_MED_A, ERR_MED_B, ERR_MED_C);
  mf.err_high = trapezoidalMF(error, ERR_HIGH_A, ERR_HIGH_B, ERR_HIGH_C, ERR_HIGH_D);
  
  // Fuzzifikasi DELTA ERROR (%/s)
  mf.delta_decreasing = trapezoidalMF(deltaErr, DELTA_DEC_A, DELTA_DEC_B, DELTA_DEC_C, DELTA_DEC_D);
  mf.delta_stable = triangularMF(deltaErr, DELTA_STABLE_A, DELTA_STABLE_B, DELTA_STABLE_C);
  mf.delta_increasing = trapezoidalMF(deltaErr, DELTA_INC_A, DELTA_INC_B, DELTA_INC_C, DELTA_INC_D);
  
  // Initialize outputs
  mf.out_very_short = 0.0;
  mf.out_short = 0.0;
  mf.out_medium = 0.0;
  mf.out_long = 0.0;
  mf.out_very_long = 0.0;
  
  return mf;
}

void applyFuzzyRules(FuzzyMembership2Input &mf) {
  // RULE BASE (9 rules):
  // Error Low + Delta Decreasing -> Very Short
  mf.out_very_short = max(mf.out_very_short, min(mf.err_low, mf.delta_decreasing));
  
  // Error Low + Delta Stable -> Short
  mf.out_short = max(mf.out_short, min(mf.err_low, mf.delta_stable));
  
  // Error Low + Delta Increasing -> Medium
  mf.out_medium = max(mf.out_medium, min(mf.err_low, mf.delta_increasing));
  
  // Error Medium + Delta Decreasing -> Short
  mf.out_short = max(mf.out_short, min(mf.err_medium, mf.delta_decreasing));
  
  // Error Medium + Delta Stable -> Medium
  mf.out_medium = max(mf.out_medium, min(mf.err_medium, mf.delta_stable));
  
  // Error Medium + Delta Increasing -> Long
  mf.out_long = max(mf.out_long, min(mf.err_medium, mf.delta_increasing));
  
  // Error High + Delta Decreasing -> Medium
  mf.out_medium = max(mf.out_medium, min(mf.err_high, mf.delta_decreasing));
  
  // Error High + Delta Stable -> Long
  mf.out_long = max(mf.out_long, min(mf.err_high, mf.delta_stable));
  
  // Error High + Delta Increasing -> Very Long
  mf.out_very_long = max(mf.out_very_long, min(mf.err_high, mf.delta_increasing));
}

unsigned long defuzzifyDuration(FuzzyMembership2Input mf) {
  // Centroid defuzzification
  float numerator = (mf.out_very_short * DUR_VERY_SHORT) + 
                    (mf.out_short * DUR_SHORT) + 
                    (mf.out_medium * DUR_MEDIUM) + 
                    (mf.out_long * DUR_LONG) + 
                    (mf.out_very_long * DUR_VERY_LONG);
  
  float denominator = mf.out_very_short + mf.out_short + mf.out_medium + 
                      mf.out_long + mf.out_very_long;
  
  if (denominator == 0) return DUR_MEDIUM;
  
  return (unsigned long)(numerator / denominator);
}

// ==================== ERROR CALCULATION ====================
void calculateError() {
  if (setpointPPM <= 0) return;
  
  previousError = errorPPM;
  errorPPM = ((setpointPPM - sensorPPM) / setpointPPM) * 100.0;
  
  // Clamp error to 0-100%
  if (errorPPM < 0) errorPPM = 0;
  if (errorPPM > 100) errorPPM = 100;
  
  // Calculate delta error (%/s)
  float dt = ERROR_CALC_INTERVAL / 1000.0;  // convert to seconds
  deltaError = (errorPPM - previousError) / dt;
  
  // Clamp delta error to -10 to +10 %/s
  if (deltaError < -10.0) deltaError = -10.0;
  if (deltaError > 10.0) deltaError = 10.0;
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

  // MODE AUTO - FUZZY LOGIC (2 INPUT)
  
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

  // FUZZY CONTROL DECISION (2 INPUT: Error & Delta Error)
  if (sensorPPM < setpointPPM && sensorPPM > 10.0 && errorPPM > 5.0) {
    
    // Fuzzification
    FuzzyMembership2Input fuzzyMF = fuzzification(errorPPM, deltaError);
    
    // Rule evaluation
    applyFuzzyRules(fuzzyMF);
    
    // Defuzzification
    pumpDuration = defuzzifyDuration(fuzzyMF);
    
    Serial.println("\n========== FUZZY COMPUTATION (2 INPUT) ==========");
    Serial.printf("INPUT  -> Error: %.1f%%, Delta Error: %.2f %%/s\n", errorPPM, deltaError);
    Serial.printf("μ_Err  -> Low=%.3f, Med=%.3f, High=%.3f\n", 
                  fuzzyMF.err_low, fuzzyMF.err_medium, fuzzyMF.err_high);
    Serial.printf("μ_ΔErr -> Dec=%.3f, Stable=%.3f, Inc=%.3f\n",
                  fuzzyMF.delta_decreasing, fuzzyMF.delta_stable, fuzzyMF.delta_increasing);
    Serial.printf("OUTPUT -> VS=%.3f, S=%.3f, M=%.3f, L=%.3f, VL=%.3f\n",
                  fuzzyMF.out_very_short, fuzzyMF.out_short, fuzzyMF.out_medium,
                  fuzzyMF.out_long, fuzzyMF.out_very_long);
    Serial.printf("CRISP  -> Duration: %lu ms (%.1f sec)\n", pumpDuration, pumpDuration/1000.0);
    Serial.println("=================================================\n");
    
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
    DynamicJsonDocument jsonDoc(1024);
  #endif
  
  jsonDoc["node_id"] = nodeID;
  jsonDoc["ppm"] = round(sensorPPM * 10) / 10.0;
  jsonDoc["ph"] = 7.0;  // pH dihapus tapi tetap kirim nilai default untuk kompatibilitas
  jsonDoc["pump_active"] = pumpRunning || (manualPumpCommand && !isAutoMode);
  jsonDoc["mode"] = isAutoMode ? "AUTO" : "MANUAL";
  jsonDoc["setpoint"] = setpointPPM;
  jsonDoc["timestamp"] = millis();
  
  // Testing data
  jsonDoc["sequence_no"] = sequenceNo++;
  jsonDoc["t_send"] = millis();
  jsonDoc["rssi"] = WiFi.RSSI();
  
  // Fuzzy debug data
  jsonDoc["error_pct"] = errorPPM;
  jsonDoc["delta_error"] = deltaError;
  
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
  Serial.printf("[NODE %d] Nutrient Control System (2-Input Fuzzy)\n", nodeID);
  Serial.println("===============================================");
  
  // Sensor data
  Serial.printf("[SENSOR]  PPM: %.1f ppm | Setpoint: %.0f ppm\n", 
                sensorPPM, setpointPPM);
  Serial.printf("[ERROR]   Error: %.1f%% | ΔError: %.2f %%/s\n",
                errorPPM, deltaError);
  
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
  Serial.print(isAutoMode ? "AUTO (Fuzzy 2-Input)" : "MANUAL (Gateway)");
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
  Serial.println("   FUZZY LOGIC: 2 INPUT (Error & Delta Error)");
  Serial.println("===============================================\n");
  
  // Load config
  loadConfig();
  
  // Setup pins
  pinMode(PUMP_NUTRISI_A_PIN, OUTPUT);
  pinMode(PUMP_NUTRISI_B_PIN, OUTPUT);
  pinMode(TDS_SENSOR_PIN, INPUT);
  
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
  EEPROM.begin(32);
  gravityTds.setPin(TDS_SENSOR_PIN);
  gravityTds.setAref(3.3);
  gravityTds.setAdcRange(4096);
  gravityTds.begin();
  
  Serial.println("✓ TDS Sensor Initialized");
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.printf("║  Node ID      : %-23d║\n", nodeID);
  Serial.printf("║  Setpoint PPM : %-23.0f║\n", setpointPPM);
  Serial.printf("║  Mode         : %-23s║\n", "AUTO (Fuzzy)");
  Serial.println("╠════════════════════════════════════════╣");
  Serial.println("║  Fuzzy Logic: 2 INPUT                  ║");
  Serial.println("║  - Error (%)                           ║");
  Serial.println("║  - Delta Error (%/s)                   ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  readSensors();
  calculateError();
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

  // Calculate error and delta error
  if (now - lastErrorCalc >= ERROR_CALC_INTERVAL) {
    calculateError();
    lastErrorCalc = now;
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
