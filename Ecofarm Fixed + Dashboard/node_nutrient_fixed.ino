/*
 * ESP32 NUTRIENT CONTROL SYSTEM - NODE SENSOR 2
 * Features: TDS/PPM Control (Fuzzy Logic 2 Input: Error & Delta Error)
 * Mode: Auto (Fuzzy di Node) / Manual (Command dari Gateway)
 * TAMBAHAN: Kirim data fuzzy lengkap (fuzzifikasi, inferensi, defuzzifikasi) ke gateway
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "GravityTDS.h"
#include <EEPROM.h>

// ==================== KONFIGURASI NETWORK ====================
const char* ssid       = "EcoFarm-Gateway";
const char* password   = "ecofarm123";
const char* gatewayIP  = "192.168.4.1";      // IP static gateway AP — tidak perlu diubah lagi
const int   gatewayPort = 8080;
const int   nodeID      = 3;                 // ⚠️ UBAH UNTUK SETIAP NODE (1-4)

// ==================== KONFIGURASI HARDWARE ====================
const int PUMP_NUTRISI_PIN = 12;  // Satu relay, pin 12
const int TDS_SENSOR_PIN     = 35;
const bool RELAY_ACTIVE_LOW  = true;

// ==================== PARAMETER KONTROL ====================
float setpointPPM      = 800.0;
bool  isAutoMode       = true;
bool  manualPumpCommand = false;

// ==================== FUZZY LOGIC PARAMETERS (2 INPUT) ====================
// ERROR (%)
const float ERR_LOW_A  = 0.0,  ERR_LOW_B  = 0.0,  ERR_LOW_C  = 10.0, ERR_LOW_D  = 30.0;
const float ERR_MED_A  = 10.0, ERR_MED_B  = 30.0, ERR_MED_C  = 60.0;
const float ERR_HIGH_A = 30.0, ERR_HIGH_B = 60.0, ERR_HIGH_C = 100.0, ERR_HIGH_D = 100.0;

// DELTA ERROR (%/s)
const float DELTA_DEC_A    = -10.0, DELTA_DEC_B    = -10.0, DELTA_DEC_C    = -5.0, DELTA_DEC_D = 0.0;
const float DELTA_STABLE_A = -3.0,  DELTA_STABLE_B =  0.0,  DELTA_STABLE_C =  3.0;
const float DELTA_INC_A    =  0.0,  DELTA_INC_B    =  5.0,  DELTA_INC_C    = 10.0, DELTA_INC_D = 10.0;

// Output Duration (ms) : Bisa diganti menyesuaikan spesifikasi Tandon dan Pompa nya. jangan lupa ganti juga untuk delay homogen (mix)
const unsigned long DUR_VERY_SHORT = 1000;
const unsigned long DUR_SHORT      = 5000;
const unsigned long DUR_MEDIUM     = 9000;
const unsigned long DUR_LONG       = 12000;
const unsigned long DUR_VERY_LONG  = 15000;

// ==================== STRUCT FUZZY MEMBERSHIP ====================
struct FuzzyMembership2Input {
  // Fuzzifikasi
  float err_low,    err_medium,    err_high;
  float delta_decreasing, delta_stable, delta_increasing;
  
  // Inferensi (firing strength tiap rule)
  float rule1_vs;  // ErrLow  + DeltaDec    -> VeryShort
  float rule2_s;   // ErrLow  + DeltaStable -> Short
  float rule3_m;   // ErrLow  + DeltaInc    -> Medium
  float rule4_s;   // ErrMed  + DeltaDec    -> Short
  float rule5_m;   // ErrMed  + DeltaStable -> Medium
  float rule6_l;   // ErrMed  + DeltaInc    -> Long
  float rule7_m;   // ErrHigh + DeltaDec    -> Medium
  float rule8_l;   // ErrHigh + DeltaStable -> Long
  float rule9_vl;  // ErrHigh + DeltaInc    -> VeryLong
  
  // Agregasi output
  float out_very_short, out_short, out_medium, out_long, out_very_long;
};

// ==================== SENSOR VARIABLES ====================
GravityTDS gravityTds;
float sensorPPM    = 0.0;
float previousPPM  = 0.0;
float errorPPM     = 0.0;
float previousError = 0.0;
float deltaError   = 0.0;

// Simpan hasil fuzzy terakhir untuk dikirim ke gateway
FuzzyMembership2Input lastFuzzyResult;
unsigned long         lastFuzzyDurationMs = 0;
bool                  fuzzyWasComputed    = false;

// ==================== PUMP CONTROL STATE ====================
bool          pumpRunning  = false;
unsigned long pumpStartTime = 0;
unsigned long pumpDuration  = 0;

// ==================== HOMOGENIZATION STATE ====================
bool          waitingMix  = false;
unsigned long mixStartTime = 0;
const unsigned long MIX_DELAY = 15000; // Ganti sesuai spesifikasi Tandon dan Pompa

// ==================== TDS SMOOTHING ====================
#define BUFFER_SIZE 7
float tdsBuffer[BUFFER_SIZE];
int   bufferIndex = 0;
bool  bufferReady = false;

// ==================== TIMING ====================
unsigned long lastSensorRead   = 0;
unsigned long lastGatewaySync  = 0;
unsigned long lastSerialPrint  = 0;
unsigned long lastWiFiCheck    = 0;
unsigned long lastConfigSave   = 0;
unsigned long lastErrorCalc    = 0;

const unsigned long SENSOR_INTERVAL    = 1000;
const unsigned long GATEWAY_INTERVAL   = 5000;
const unsigned long PRINT_INTERVAL     = 10000;
const unsigned long WIFI_CHECK_INTERVAL = 10000;
const unsigned long CONFIG_SAVE_INTERVAL = 60000;
const unsigned long ERROR_CALC_INTERVAL  = 1000;

// ==================== CONNECTION STATUS ====================
bool          gatewayOnline  = false;
unsigned long lastSuccessSync = 0;
int           syncFailCount  = 0;
const int     MAX_SYNC_FAILS = 10;

unsigned long sequenceNo = 0;

// ==================== EEPROM ====================
Preferences preferences;
bool        configChanged = false;

void loadConfig() {
  preferences.begin("node-config", false);
  if (preferences.isKey("setpoint")) {
    setpointPPM = preferences.getFloat("setpoint", 800.0);
    Serial.printf("[CONFIG] Loaded setpoint: %.1f ppm\n", setpointPPM);
  } else {
    preferences.putFloat("setpoint", setpointPPM);
    Serial.println("[CONFIG] Initialized default setpoint: 800 ppm");
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
  bool level = RELAY_ACTIVE_LOW ? !state : state;
  digitalWrite(PUMP_NUTRISI_PIN, level);
}
void turnPumpOn()  { setPump(true);  pumpRunning = true;  }
void turnPumpOff() { setPump(false); pumpRunning = false; }

// ==================== SENSOR READING ====================
void readSensors() {
  gravityTds.setTemperature(25.0);
  gravityTds.update();
  float tdsValue = gravityTds.getTdsValue();
  if (isnan(tdsValue) || tdsValue < 0) tdsValue = 0;

  tdsBuffer[bufferIndex] = tdsValue;
  bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
  if (bufferIndex == 0) bufferReady = true;

  float sum = 0;
  int count = bufferReady ? BUFFER_SIZE : bufferIndex;
  for (int i = 0; i < count; i++) sum += tdsBuffer[i];
  previousPPM = sensorPPM;
  sensorPPM   = (count > 0) ? (sum / count) : 0;
}

// ==================== FUZZY MEMBERSHIP FUNCTIONS ====================
float trapezoidalMF(float x, float a, float b, float c, float d) {
  if (x <= a || x >= d) return 0.0;
  if (x >= b && x <= c) return 1.0;
  if (x > a && x < b)   return (x - a) / (b - a);
  return (d - x) / (d - c);
}

float triangularMF(float x, float a, float b, float c) {
  if (x <= a || x >= c) return 0.0;
  if (x == b)           return 1.0;
  if (x > a && x < b)   return (x - a) / (b - a);
  return (c - x) / (c - b);
}

// ==================== FUZZY LOGIC ENGINE ====================
FuzzyMembership2Input fuzzification(float error, float deltaErr) {
  FuzzyMembership2Input mf;
  
  // === FUZZIFIKASI ===
  mf.err_low    = trapezoidalMF(error, ERR_LOW_A,  ERR_LOW_B,  ERR_LOW_C,  ERR_LOW_D);
  mf.err_medium = triangularMF (error, ERR_MED_A,  ERR_MED_B,  ERR_MED_C);
  mf.err_high   = trapezoidalMF(error, ERR_HIGH_A, ERR_HIGH_B, ERR_HIGH_C, ERR_HIGH_D);
  
  mf.delta_decreasing  = trapezoidalMF(deltaErr, DELTA_DEC_A,    DELTA_DEC_B,    DELTA_DEC_C,    DELTA_DEC_D);
  mf.delta_stable      = triangularMF (deltaErr, DELTA_STABLE_A, DELTA_STABLE_B, DELTA_STABLE_C);
  mf.delta_increasing  = trapezoidalMF(deltaErr, DELTA_INC_A,    DELTA_INC_B,    DELTA_INC_C,    DELTA_INC_D);
  
  // Init inferensi
  mf.rule1_vs = mf.rule2_s = mf.rule3_m = mf.rule4_s = mf.rule5_m = 0.0;
  mf.rule6_l  = mf.rule7_m = mf.rule8_l = mf.rule9_vl = 0.0;
  
  // Init agregasi
  mf.out_very_short = mf.out_short = mf.out_medium = mf.out_long = mf.out_very_long = 0.0;
  
  return mf;
}

void applyFuzzyRules(FuzzyMembership2Input &mf) {
  // === INFERENSI (firing strength) ===
  mf.rule1_vs  = min(mf.err_low,    mf.delta_decreasing);   // -> VeryShort
  mf.rule2_s   = min(mf.err_low,    mf.delta_stable);        // -> Short
  mf.rule3_m   = min(mf.err_low,    mf.delta_increasing);    // -> Medium
  mf.rule4_s   = min(mf.err_medium, mf.delta_decreasing);    // -> Short
  mf.rule5_m   = min(mf.err_medium, mf.delta_stable);        // -> Medium
  mf.rule6_l   = min(mf.err_medium, mf.delta_increasing);    // -> Long
  mf.rule7_m   = min(mf.err_high,   mf.delta_decreasing);    // -> Medium
  mf.rule8_l   = min(mf.err_high,   mf.delta_stable);        // -> Long
  mf.rule9_vl  = min(mf.err_high,   mf.delta_increasing);    // -> VeryLong
  
  // === AGREGASI (max dari rule dengan output yang sama) ===
  mf.out_very_short = mf.rule1_vs;
  mf.out_short      = max(mf.rule2_s,  mf.rule4_s);
  mf.out_medium     = max(max(mf.rule3_m, mf.rule5_m), mf.rule7_m);
  mf.out_long       = max(mf.rule6_l,  mf.rule8_l);
  mf.out_very_long  = mf.rule9_vl;
}

unsigned long defuzzifyDuration(FuzzyMembership2Input mf) {
  // === DEFUZZIFIKASI (centroid) ===
  float numerator = (mf.out_very_short * DUR_VERY_SHORT) +
                    (mf.out_short      * DUR_SHORT)      +
                    (mf.out_medium     * DUR_MEDIUM)     +
                    (mf.out_long       * DUR_LONG)       +
                    (mf.out_very_long  * DUR_VERY_LONG);
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
  if (errorPPM < 0)   errorPPM = 0;
  if (errorPPM > 100) errorPPM = 100;
  
  float dt = ERROR_CALC_INTERVAL / 1000.0;
  deltaError = (errorPPM - previousError) / dt;
  if (deltaError < -10.0) deltaError = -10.0;
  if (deltaError >  10.0) deltaError =  10.0;
}

// ==================== CONTROL LOGIC ====================
void executeControl() {
  unsigned long now = millis();
  
  if (!isAutoMode) {
    pumpRunning = false;
    waitingMix  = false;
    setPump(manualPumpCommand);
    return;
  }
  
  if (pumpRunning) {
    if (now - pumpStartTime >= pumpDuration) {
      turnPumpOff();
      waitingMix  = true;
      mixStartTime = now;
      Serial.printf("[FUZZY] Pump OFF - Mix wait (%ds)\n", MIX_DELAY / 1000);
    }
    return;
  }
  
  if (waitingMix) {
    if (now - mixStartTime >= MIX_DELAY) {
      waitingMix = false;
      Serial.println("[FUZZY] Mix complete");
    }
    return;
  }
  
  if (sensorPPM < setpointPPM && sensorPPM > 10.0 && errorPPM > 5.0) {
    
    // Fuzzifikasi
    FuzzyMembership2Input fuzzyMF = fuzzification(errorPPM, deltaError);
    
    // Inferensi & Agregasi
    applyFuzzyRules(fuzzyMF);
    
    // Defuzzifikasi
    pumpDuration = defuzzifyDuration(fuzzyMF);
    
    // Simpan hasil fuzzy terakhir untuk dikirim ke gateway
    lastFuzzyResult     = fuzzyMF;
    lastFuzzyDurationMs = pumpDuration;
    fuzzyWasComputed    = true;
    
    // Print serial
    Serial.println("\n========== FUZZY COMPUTATION (2 INPUT) ==========");
    Serial.printf("INPUT    -> Error: %.1f%%, ΔError: %.2f %%/s\n", errorPPM, deltaError);
    Serial.println("--- FUZZIFIKASI ---");
    Serial.printf("μ_Err    -> Low=%.3f, Med=%.3f, High=%.3f\n",
                  fuzzyMF.err_low, fuzzyMF.err_medium, fuzzyMF.err_high);
    Serial.printf("μ_ΔErr   -> Dec=%.3f, Stable=%.3f, Inc=%.3f\n",
                  fuzzyMF.delta_decreasing, fuzzyMF.delta_stable, fuzzyMF.delta_increasing);
    Serial.println("--- INFERENSI ---");
    Serial.printf("R1(VS):  %.3f  R2(S):  %.3f  R3(M):  %.3f\n", fuzzyMF.rule1_vs, fuzzyMF.rule2_s,  fuzzyMF.rule3_m);
    Serial.printf("R4(S):   %.3f  R5(M):  %.3f  R6(L):  %.3f\n", fuzzyMF.rule4_s,  fuzzyMF.rule5_m,  fuzzyMF.rule6_l);
    Serial.printf("R7(M):   %.3f  R8(L):  %.3f  R9(VL): %.3f\n", fuzzyMF.rule7_m,  fuzzyMF.rule8_l,  fuzzyMF.rule9_vl);
    Serial.println("--- AGREGASI ---");
    Serial.printf("OUT -> VS=%.3f, S=%.3f, M=%.3f, L=%.3f, VL=%.3f\n",
                  fuzzyMF.out_very_short, fuzzyMF.out_short, fuzzyMF.out_medium,
                  fuzzyMF.out_long, fuzzyMF.out_very_long);
    Serial.printf("--- DEFUZZIFIKASI ---\n");
    Serial.printf("CRISP -> Duration: %lu ms (%.1f sec)\n", pumpDuration, pumpDuration / 1000.0);
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
  http.setTimeout(10000);
  
  #if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument jsonDoc;
  #else
    DynamicJsonDocument jsonDoc(2048);
  #endif
  
  jsonDoc["node_id"]     = nodeID;
  jsonDoc["ppm"]         = round(sensorPPM * 10) / 10.0;
  jsonDoc["ph"]          = 7.0;
  jsonDoc["pump_active"] = pumpRunning || (manualPumpCommand && !isAutoMode);
  jsonDoc["mode"]        = isAutoMode ? "AUTO" : "MANUAL";
  jsonDoc["setpoint"]    = setpointPPM;
  jsonDoc["timestamp"]   = millis();
  
  // Testing data
  jsonDoc["sequence_no"] = sequenceNo++;
  jsonDoc["t_send"]      = millis();
  jsonDoc["rssi"]        = WiFi.RSSI();
  
  // Error & Delta Error
  jsonDoc["error_pct"]   = errorPPM;
  jsonDoc["delta_error"] = deltaError;
  
  // ============================================================
  // KIRIM DATA FUZZY LENGKAP KE GATEWAY
  // Hanya dikirim jika fuzzy pernah dikomputasi
  // ============================================================
  if (fuzzyWasComputed) {
    JsonObject fd = jsonDoc.createNestedObject("fuzzy_detail");
    
    // Fuzzifikasi
    fd["mu_err_low"]     = lastFuzzyResult.err_low;
    fd["mu_err_medium"]  = lastFuzzyResult.err_medium;
    fd["mu_err_high"]    = lastFuzzyResult.err_high;
    fd["mu_delta_dec"]   = lastFuzzyResult.delta_decreasing;
    fd["mu_delta_stable"] = lastFuzzyResult.delta_stable;
    fd["mu_delta_inc"]   = lastFuzzyResult.delta_increasing;
    
    // Inferensi
    fd["rule1_vs"] = lastFuzzyResult.rule1_vs;
    fd["rule2_s"]  = lastFuzzyResult.rule2_s;
    fd["rule3_m"]  = lastFuzzyResult.rule3_m;
    fd["rule4_s"]  = lastFuzzyResult.rule4_s;
    fd["rule5_m"]  = lastFuzzyResult.rule5_m;
    fd["rule6_l"]  = lastFuzzyResult.rule6_l;
    fd["rule7_m"]  = lastFuzzyResult.rule7_m;
    fd["rule8_l"]  = lastFuzzyResult.rule8_l;
    fd["rule9_vl"] = lastFuzzyResult.rule9_vl;
    
    // Agregasi output
    fd["out_vs"] = lastFuzzyResult.out_very_short;
    fd["out_s"]  = lastFuzzyResult.out_short;
    fd["out_m"]  = lastFuzzyResult.out_medium;
    fd["out_l"]  = lastFuzzyResult.out_long;
    fd["out_vl"] = lastFuzzyResult.out_very_long;
    
    // Defuzzifikasi
    fd["crisp_ms"]  = (float)lastFuzzyDurationMs;
    fd["crisp_sec"] = lastFuzzyDurationMs / 1000.0f;
  }
  
  String jsonPayload;
  serializeJson(jsonDoc, jsonPayload);
  
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode == 200) {
    gatewayOnline  = true;
    lastSuccessSync = millis();
    syncFailCount  = 0;
    
    String response = http.getString();
    
    #if ARDUINOJSON_VERSION_MAJOR >= 7
      JsonDocument responseDoc;
    #else
      DynamicJsonDocument responseDoc(512);
    #endif
    
    if (!deserializeJson(responseDoc, response)) {
      if (responseDoc.containsKey("setpoint")) {
        float newSetpoint = responseDoc["setpoint"];
        if (newSetpoint >= 200 && newSetpoint <= 2000 && abs(newSetpoint - setpointPPM) > 1.0) {
          Serial.printf("[GW] Setpoint: %.1f -> %.1f ppm\n", setpointPPM, newSetpoint);
          setpointPPM   = newSetpoint;
          configChanged = true;
        }
      }
      if (responseDoc.containsKey("mode")) {
        bool newMode = (responseDoc["mode"].as<String>() == "AUTO");
        if (newMode != isAutoMode) {
          Serial.printf("[GW] Mode: %s -> %s\n",
                        isAutoMode ? "AUTO" : "MANUAL",
                        newMode    ? "AUTO" : "MANUAL");
          isAutoMode = newMode;
        }
      }
      if (responseDoc.containsKey("actuator_command")) {
        bool newCmd = responseDoc["actuator_command"];
        if (newCmd != manualPumpCommand) {
          Serial.printf("[GW] Manual pump: %s -> %s\n",
                        manualPumpCommand ? "ON" : "OFF",
                        newCmd            ? "ON" : "OFF");
          manualPumpCommand = newCmd;
        }
      }
      Serial.printf("[SYNC] ✓ OK (Node %d)\n", nodeID);
    }
  } else {
    gatewayOnline = false;
    syncFailCount++;
    if (syncFailCount >= MAX_SYNC_FAILS && !isAutoMode) {
      Serial.println("[GW] Too many fails - forcing AUTO");
      isAutoMode        = true;
      manualPumpCommand = false;
    }
    if (syncFailCount % 5 == 0) {
      Serial.printf("[SYNC] ✗ Fail (%d): HTTP %d\n", syncFailCount, httpResponseCode);
      if (httpResponseCode < 0)
        Serial.printf("[SYNC] Error: %s\n", http.errorToString(httpResponseCode).c_str());
    }
  }
  http.end();
}

// ==================== WIFI MANAGEMENT ====================
void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Reconnecting...");
    WiFi.reconnect();
    int att = 0;
    while (WiFi.status() != WL_CONNECTED && att < 20) { delay(500); att++; }
    if (WiFi.status() == WL_CONNECTED) Serial.println("[WiFi] ✓ Reconnected!");
  }
}

// ==================== STATUS PRINT ====================
void printStatus() {
  unsigned long now = millis();
  Serial.println("\n===============================================");
  Serial.printf("[NODE %d] Nutrient Control - 2-Input Fuzzy\n", nodeID);
  Serial.println("===============================================");
  Serial.printf("[SENSOR]  PPM=%.1f | Setpoint=%.0f ppm\n", sensorPPM, setpointPPM);
  Serial.printf("[ERROR]   Err=%.1f%% | ΔErr=%.2f %%/s\n", errorPPM, deltaError);
  
  if (fuzzyWasComputed) {
    Serial.println("[FUZZY]   Last computation:");
    Serial.printf("          μErr: L=%.3f M=%.3f H=%.3f\n",
                  lastFuzzyResult.err_low, lastFuzzyResult.err_medium, lastFuzzyResult.err_high);
    Serial.printf("          μΔErr: D=%.3f S=%.3f I=%.3f\n",
                  lastFuzzyResult.delta_decreasing, lastFuzzyResult.delta_stable, lastFuzzyResult.delta_increasing);
    Serial.printf("          Out: VS=%.3f S=%.3f M=%.3f L=%.3f VL=%.3f\n",
                  lastFuzzyResult.out_very_short, lastFuzzyResult.out_short,
                  lastFuzzyResult.out_medium, lastFuzzyResult.out_long, lastFuzzyResult.out_very_long);
    Serial.printf("          Duration: %.1f sec\n", lastFuzzyDurationMs / 1000.0f);
  }
  
  Serial.printf("[GATEWAY] ");
  if (WiFi.status() != WL_CONNECTED) Serial.println("WiFi Disconnected");
  else if (gatewayOnline)             Serial.printf("ONLINE (%ds ago)\n", (int)((now - lastSuccessSync) / 1000));
  else                                Serial.printf("OFFLINE (Fails:%d)\n", syncFailCount);
  
  Serial.printf("[MODE]    %s | ", isAutoMode ? "AUTO" : "MANUAL");
  if      (pumpRunning)                         Serial.printf("PUMPING (%ds/%ds)\n", (int)((now - pumpStartTime) / 1000), (int)(pumpDuration / 1000));
  else if (waitingMix)                          Serial.printf("MIXING (%ds left)\n", (int)((MIX_DELAY - (now - mixStartTime)) / 1000));
  else if (!isAutoMode && manualPumpCommand)    Serial.println("PUMP ON (Manual)");
  else                                          Serial.println("STANDBY");
  Serial.println("===============================================\n");
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n===============================================");
  Serial.println("   ESP32 NUTRIENT CONTROL - NODE SENSOR 2");
  Serial.println("   FUZZY 2-INPUT + FULL LOG TO FIREBASE");
  Serial.println("===============================================\n");
  
  loadConfig();
  
  pinMode(PUMP_NUTRISI_PIN, OUTPUT);
  pinMode(TDS_SENSOR_PIN, INPUT);
  turnPumpOff();
  
  // Init fuzzy struct
  memset(&lastFuzzyResult, 0, sizeof(lastFuzzyResult));
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  
  int attempts = 0;
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("✓ WiFi OK | IP: %s | GW: %s:%d\n",
                  WiFi.localIP().toString().c_str(), gatewayIP, gatewayPort);
  } else {
    Serial.println("✗ WiFi Failed - standalone AUTO mode");
  }
  
  EEPROM.begin(32);
  gravityTds.setPin(TDS_SENSOR_PIN);
  gravityTds.setAref(3.3);
  gravityTds.setAdcRange(4096);
  gravityTds.begin();
  Serial.println("✓ TDS Sensor OK");
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.printf("║  Node ID      : %-23d║\n", nodeID);
  Serial.printf("║  Setpoint     : %-20.0f ppm║\n", setpointPPM);
  Serial.println("║  Fuzzy Data   : Dikirim ke Gateway     ║");
  Serial.println("║  Firebase Path: /fuzzy_log/node_X      ║");
  Serial.println("╠════════════════════════════════════════╣");
  Serial.println("║  Data yang dikirim:                    ║");
  Serial.println("║  - error_pct & delta_error             ║");
  Serial.println("║  - fuzzy_detail.fuzzifikasi            ║");
  Serial.println("║  - fuzzy_detail.inferensi (9 rules)    ║");
  Serial.println("║  - fuzzy_detail.agregasi               ║");
  Serial.println("║  - fuzzy_detail.defuzzifikasi          ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  readSensors();
  calculateError();
  Serial.println("System ready!\n");
}

// ==================== LOOP ====================
void loop() {
  unsigned long now = millis();
  
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    readSensors();
    lastSensorRead = now;
  }
  if (now - lastErrorCalc >= ERROR_CALC_INTERVAL) {
    calculateError();
    lastErrorCalc = now;
  }
  
  executeControl();
  
  if (now - lastGatewaySync >= GATEWAY_INTERVAL) {
    syncWithGateway();
    lastGatewaySync = now;
  }
  if (now - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
    checkWiFiConnection();
    lastWiFiCheck = now;
  }
  if (configChanged && (now - lastConfigSave >= CONFIG_SAVE_INTERVAL)) {
    saveConfig();
    lastConfigSave = now;
  }
  if (now - lastSerialPrint >= PRINT_INTERVAL) {
    printStatus();
    lastSerialPrint = now;
  }
  
  yield();
}
