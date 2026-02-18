#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ==================== KONFIGURASI ====================
const char* ssid = "smk unnes semarang";
const char* password = "12345678";

#define FIREBASE_HOST "projectecofarm-98727-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "z2fgLdOr7LsdHq9yNUGKhbxuGVfy6GPeuKsbFDyY"

#define NUM_NODES 4

// ==================== FIREBASE OBJECTS ====================
FirebaseData fbdo;
FirebaseData fbdo_stream;
FirebaseAuth auth;
FirebaseConfig config;

WebServer server(8080);

// ==================== STRUKTUR DATA ====================
struct NodeControl {
  bool manual_mode;
  bool actuator_command;
  float setpoint;
  unsigned long timestamp;
};

struct NodeStatus {
  unsigned long last_seen;
  bool online;
  int node_type;
  
  float temperature;
  float humidity;
  int exhaust_status;
  
  float current_ppm;
  float current_ph;
  bool pump_active;
  
  String mode;
};

// ==================== STRUKTUR PENGUJIAN ====================
struct LatencyData {
  unsigned long sequence_no;
  unsigned long t_send;
  unsigned long t_receive;
  unsigned long latency;
  int rssi;
  String status;
};

struct PDRData {
  unsigned long packets_sent;
  unsigned long packets_received;
  unsigned long packets_lost;
  float pdr_percentage;
  String status;
  unsigned long last_reset;
};

NodeControl controls[NUM_NODES];
NodeStatus node_status[NUM_NODES];
PDRData pdr_stats[NUM_NODES];

bool firebaseReady = false;
bool streamReady = false;
unsigned long lastStreamCheck = 0;

// ==================== FUZZY MAMDANI ====================
struct FuzzyResult {
  float mu_temp_low;
  float mu_temp_normal;
  float mu_temp_high;
  float mu_hum_low;
  float mu_hum_normal;
  float mu_hum_high;
  float out_off;
  float out_medium;
  float out_high;
  float crisp_output;
  int exhaust_status;
};

float membershipSuhuRendah(float temp) {
  if (temp <= 25.0f) return 1.0f;
  if (temp >= 28.0f) return 0.0f;
  return (28.0f - temp) / 3.0f;
}

float membershipSuhuNormal(float temp) {
  if (temp <= 25.0f || temp >= 32.0f) return 0.0f;
  if (temp >= 28.0f && temp <= 30.0f) return 1.0f;
  if (temp < 28.0f) return (temp - 25.0f) / 3.0f;
  return (32.0f - temp) / 2.0f;
}

float membershipSuhuTinggi(float temp) {
  if (temp <= 30.0f) return 0.0f;
  if (temp >= 35.0f) return 1.0f;
  return (temp - 30.0f) / 5.0f;
}

float membershipHumidityRendah(float hum) {
  if (hum <= 60.0f) return 1.0f;
  if (hum >= 70.0f) return 0.0f;
  return (70.0f - hum) / 10.0f;
}

float membershipHumidityNormal(float hum) {
  if (hum <= 60.0f || hum >= 85.0f) return 0.0f;
  if (hum >= 70.0f && hum <= 80.0f) return 1.0f;
  if (hum < 70.0f) return (hum - 60.0f) / 10.0f;
  return (85.0f - hum) / 5.0f;
}

float membershipHumidityTinggi(float hum) {
  if (hum <= 80.0f) return 0.0f;
  if (hum >= 90.0f) return 1.0f;
  return (hum - 80.0f) / 10.0f;
}

float defuzzifyExhaust(float mati, float sedang, float menyala) {
  float num = (mati * 0.0f) + (sedang * 1.0f) + (menyala * 2.0f);
  float den = mati + sedang + menyala;
  
  if (den == 0.0f) return 0.0f;
  float result = num / den;
  
  if (result < 0.5f) return 0.0f;
  if (result < 1.5f) return 1.0f;
  return 2.0f;
}

FuzzyResult fuzzyControlExhaust(float temperature, float humidity) {
  FuzzyResult result;
  
  result.mu_temp_low = membershipSuhuRendah(temperature);
  result.mu_temp_normal = membershipSuhuNormal(temperature);
  result.mu_temp_high = membershipSuhuTinggi(temperature);
  
  result.mu_hum_low = membershipHumidityRendah(humidity);
  result.mu_hum_normal = membershipHumidityNormal(humidity);
  result.mu_hum_high = membershipHumidityTinggi(humidity);
  
  float rule1_mati = result.mu_temp_low;
  float rule2_mati = min(result.mu_temp_normal, result.mu_temp_low);
  float rule3_sedang = result.mu_temp_normal;
  float rule4_sedang = min(result.mu_temp_normal, result.mu_temp_high);
  float rule5_sedang = min(result.mu_temp_high, result.mu_temp_normal);
  float rule6_menyala = result.mu_temp_high;
  float rule7_menyala = min(result.mu_temp_high, 1.0f - result.mu_temp_normal);
  float rule8_menyala = (temperature > 35.0f) ? 1.0f : 0.0f;
  float rule9_sedang = min(result.mu_temp_normal, min(1.0f - result.mu_temp_low, 1.0f - result.mu_temp_high));
  
  result.out_off = max(rule1_mati, rule2_mati);
  result.out_medium = max(max(rule3_sedang, rule4_sedang), max(rule5_sedang, rule9_sedang));
  result.out_high = max(max(rule6_menyala, rule7_menyala), rule8_menyala);
  
  result.crisp_output = defuzzifyExhaust(result.out_off, result.out_medium, result.out_high);
  result.exhaust_status = (int)result.crisp_output;
  
  Serial.println("\n========== FUZZY COMPUTATION ==========");
  Serial.printf("INPUT -> Temp=%.1f°C, Hum=%.1f%%\n", temperature, humidity);
  Serial.printf("μ_Temp -> Low=%.3f, Normal=%.3f, High=%.3f\n", 
                result.mu_temp_low, result.mu_temp_normal, result.mu_temp_high);
  Serial.printf("μ_Hum  -> Low=%.3f, Normal=%.3f, High=%.3f\n",
                result.mu_hum_low, result.mu_hum_normal, result.mu_hum_high);
  Serial.printf("OUTPUT -> Off=%.3f, Medium=%.3f, High=%.3f\n",
                result.out_off, result.out_medium, result.out_high);
  Serial.printf("CRISP  -> %.3f -> Status=%d\n", result.crisp_output, result.exhaust_status);
  Serial.println("========================================\n");
  
  return result;
}

// ==================== FUNGSI PDR ====================
void updatePDRStats(int node_id, bool packet_received) {
  int idx = node_id - 1;
  
  pdr_stats[idx].packets_sent++;
  
  if (packet_received) {
    pdr_stats[idx].packets_received++;
  } else {
    pdr_stats[idx].packets_lost++;
  }
  
  // Hitung PDR
  if (pdr_stats[idx].packets_sent > 0) {
    pdr_stats[idx].pdr_percentage = 
      (float)pdr_stats[idx].packets_received / (float)pdr_stats[idx].packets_sent * 100.0f;
  }
  
  // Tentukan status berdasarkan PDR
  if (pdr_stats[idx].pdr_percentage >= 90.0f) {
    pdr_stats[idx].status = "Excellent";
  } else if (pdr_stats[idx].pdr_percentage >= 75.0f) {
    pdr_stats[idx].status = "Good";
  } else if (pdr_stats[idx].pdr_percentage >= 50.0f) {
    pdr_stats[idx].status = "Fair";
  } else {
    pdr_stats[idx].status = "Poor";
  }
  
  // ===== OUTPUT PDR KE SERIAL MONITOR =====
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║         PDR STATISTICS UPDATE          ║");
  Serial.println("╠════════════════════════════════════════╣");
  Serial.printf("║ Timestamp      : %-21lu║\n", millis());
  Serial.printf("║ Node ID        : %-21d║\n", node_id);
  Serial.printf("║ Packets Sent   : %-21lu║\n", pdr_stats[idx].packets_sent);
  Serial.printf("║ Packets Rcvd   : %-21lu║\n", pdr_stats[idx].packets_received);
  Serial.printf("║ Packets Lost   : %-21lu║\n", pdr_stats[idx].packets_lost);
  Serial.printf("║ PDR (%%)        : %-20.2f%%║\n", pdr_stats[idx].pdr_percentage);
  Serial.printf("║ Status         : %-21s║\n", pdr_stats[idx].status.c_str());
  Serial.println("╚════════════════════════════════════════╝\n");
}

bool sendPDRToFirebase(int node_id) {
  if (!firebaseReady) {
    Serial.println("[PDR-FIREBASE] ⚠ Firebase not ready");
    return false;
  }
  
  int idx = node_id - 1;
  String path = "/testing/pdr/node_" + String(node_id);
  
  FirebaseJson json;
  json.set("timestamp", millis());
  json.set("node_id", node_id);
  json.set("packets_sent", pdr_stats[idx].packets_sent);
  json.set("packets_received", pdr_stats[idx].packets_received);
  json.set("packets_lost", pdr_stats[idx].packets_lost);
  json.set("pdr_percentage", pdr_stats[idx].pdr_percentage);
  json.set("status", pdr_stats[idx].status);
  
  bool success = Firebase.RTDB.pushJSON(&fbdo, path.c_str(), &json);
  
  if (success) {
    Serial.println("\n┌────────────────────────────────────────┐");
    Serial.println("│   PDR DATA SENT TO FIREBASE ✓          │");
    Serial.println("├────────────────────────────────────────┤");
    Serial.printf("│ Node ID        : %-21d│\n", node_id);
    Serial.printf("│ Packets Sent   : %-21lu│\n", pdr_stats[idx].packets_sent);
    Serial.printf("│ Packets Rcvd   : %-21lu│\n", pdr_stats[idx].packets_received);
    Serial.printf("│ Packets Lost   : %-21lu│\n", pdr_stats[idx].packets_lost);
    Serial.printf("│ PDR (%%)        : %-20.2f%%│\n", pdr_stats[idx].pdr_percentage);
    Serial.printf("│ Status         : %-21s│\n", pdr_stats[idx].status.c_str());
    Serial.printf("│ Firebase Path  : %-21s│\n", path.c_str());
    Serial.println("└────────────────────────────────────────┘\n");
  } else {
    Serial.println("\n[PDR-FIREBASE] ❌ Failed to send PDR data");
    Serial.printf("Error: %s\n\n", fbdo.errorReason().c_str());
  }
  
  return success;
}

bool sendLatencyToFirebase(int node_id, const LatencyData& latency) {
  if (!firebaseReady) {
    Serial.println("[LATENCY-FIREBASE] ⚠ Firebase not ready");
    return false;
  }
  
  String path = "/testing/latency/node_" + String(node_id);
  
  FirebaseJson json;
  json.set("timestamp", millis());
  json.set("node_id", node_id);
  json.set("sequence_no", latency.sequence_no);
  json.set("t_send_ms", latency.t_send);
  json.set("t_receive_ms", latency.t_receive);
  json.set("latency_ms", latency.latency);
  json.set("rssi_dbm", latency.rssi);
  json.set("status", latency.status);
  
  bool success = Firebase.RTDB.pushJSON(&fbdo, path.c_str(), &json);
  
  if (success) {
    Serial.println("\n┌────────────────────────────────────────┐");
    Serial.println("│  LATENCY DATA SENT TO FIREBASE ✓       │");
    Serial.println("├────────────────────────────────────────┤");
    Serial.printf("│ Node ID        : %-21d│\n", node_id);
    Serial.printf("│ Sequence No    : %-21lu│\n", latency.sequence_no);
    Serial.printf("│ T_send (ms)    : %-21lu│\n", latency.t_send);
    Serial.printf("│ T_receive (ms) : %-21lu│\n", latency.t_receive);
    Serial.printf("│ Latency (ms)   : %-21lu│\n", latency.latency);
    Serial.printf("│ RSSI (dBm)     : %-21d│\n", latency.rssi);
    Serial.printf("│ Status         : %-21s│\n", latency.status.c_str());
    Serial.printf("│ Firebase Path  : %-21s│\n", path.c_str());
    Serial.println("└────────────────────────────────────────┘\n");
  } else {
    Serial.println("\n[LATENCY-FIREBASE] ❌ Failed to send latency data");
    Serial.printf("Error: %s\n\n", fbdo.errorReason().c_str());
  }
  
  return success;
}

// ==================== FIREBASE STREAM ====================
void streamCallback(FirebaseStream data) {
  Serial.println("\n========== FIREBASE STREAM UPDATE ==========");
  Serial.printf("Path: %s\n", data.dataPath().c_str());
  
  String path = data.dataPath();
  
  if (path.startsWith("/nodes/node_")) {
    int nodeStart = path.indexOf("node_") + 5;
    int nodeEnd = path.indexOf("/", nodeStart);
    if (nodeEnd == -1) nodeEnd = path.length();
    
    String nodeStr = path.substring(nodeStart, nodeEnd);
    int nodeId = nodeStr.toInt();
    
    if (nodeId >= 1 && nodeId <= NUM_NODES && path.indexOf("/control") != -1) {
      FirebaseJson json = data.jsonObject();
      FirebaseJsonData jsonData;
      
      NodeControl& ctrl = controls[nodeId - 1];
      
      if (json.get(jsonData, "manual_mode")) {
        ctrl.manual_mode = jsonData.boolValue;
      }
      if (json.get(jsonData, "actuator_command")) {
        ctrl.actuator_command = jsonData.boolValue;
      }
      if (json.get(jsonData, "setpoint")) {
        ctrl.setpoint = jsonData.floatValue;
      }
      
      ctrl.timestamp = millis();
      
      Serial.printf("✓ Node %d Updated: Mode=%s, Command=%s, Setpoint=%.1f\n", 
                    nodeId,
                    ctrl.manual_mode ? "MANUAL" : "AUTO",
                    ctrl.actuator_command ? "ON" : "OFF",
                    ctrl.setpoint);
    }
  }
  Serial.println("============================================\n");
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("[STREAM] ⚠ Timeout - will reconnect");
    streamReady = false;
  }
}

void setupFirebaseStream() {
  if (!firebaseReady) {
    Serial.println("[FIREBASE] Not ready, skip stream setup");
    return;
  }
  
  Serial.println("[FIREBASE] Setting up real-time stream...");
  
  if (Firebase.RTDB.beginStream(&fbdo_stream, "/nodes")) {
    Firebase.RTDB.setStreamCallback(&fbdo_stream, streamCallback, streamTimeoutCallback);
    streamReady = true;
    Serial.println("[FIREBASE] ✅ Stream active!");
  } else {
    Serial.printf("[FIREBASE] ❌ Stream failed: %s\n", fbdo_stream.errorReason().c_str());
    streamReady = false;
  }
}

void syncControlFromFirebase() {
  if (!firebaseReady) return;
  
  Serial.println("\n[FIREBASE] Syncing control data...");
  
  for (int i = 1; i <= NUM_NODES; i++) {
    String path = "/nodes/node_" + String(i) + "/control";
    
    if (Firebase.RTDB.getJSON(&fbdo, path.c_str())) {
      if (fbdo.dataType() == "json") {
        FirebaseJson json = fbdo.jsonObject();
        FirebaseJsonData jsonData;
        
        NodeControl& ctrl = controls[i - 1];
        
        if (json.get(jsonData, "manual_mode")) {
          ctrl.manual_mode = jsonData.boolValue;
        }
        if (json.get(jsonData, "actuator_command")) {
          ctrl.actuator_command = jsonData.boolValue;
        }
        if (json.get(jsonData, "setpoint")) {
          ctrl.setpoint = jsonData.floatValue;
        }
        
        ctrl.timestamp = millis();
        
        Serial.printf("  ✓ Node %d: Mode=%s, Setpoint=%.1f\n", 
                      i, ctrl.manual_mode ? "MANUAL" : "AUTO", ctrl.setpoint);
      }
    }
    delay(50);
  }
  
  Serial.println("[FIREBASE] Sync complete!\n");
}

bool sendToFirebase(int node_id, const DynamicJsonDocument& sensorDoc) {
  if (!firebaseReady) {
    Serial.println("[FIREBASE] ⚠ Not connected");
    return false;
  }
  
  int idx = node_id - 1;
  node_status[idx].last_seen = millis();
  node_status[idx].online = true;
  
  if (sensorDoc.containsKey("temperature")) {
    node_status[idx].node_type = 1;
    node_status[idx].temperature = sensorDoc["temperature"];
    node_status[idx].humidity = sensorDoc["humidity"];
    if (sensorDoc.containsKey("exhaust_status")) {
      node_status[idx].exhaust_status = sensorDoc["exhaust_status"];
    }
  } else if (sensorDoc.containsKey("ppm")) {
    node_status[idx].node_type = 2;
    node_status[idx].current_ppm = round(sensorDoc["ppm"].as<float>() * 10) / 10.0f;
    node_status[idx].current_ph = round(sensorDoc["ph"].as<float>() * 10) / 10.0f;
    if (sensorDoc.containsKey("pump_active")) {
      node_status[idx].pump_active = sensorDoc["pump_active"];
    }
  }
  
  if (sensorDoc.containsKey("mode")) {
    node_status[idx].mode = sensorDoc["mode"].as<String>();
  }
  
  String dataPath = "/nodes/node_" + String(node_id) + "/sensor_data";
  String statusPath = "/nodes/node_" + String(node_id) + "/status";
  
  FirebaseJson dataJson;
  dataJson.setJsonData(sensorDoc.as<String>());
  
  bool dataSuccess = Firebase.RTDB.pushJSON(&fbdo, dataPath.c_str(), &dataJson);
  
  if (dataSuccess) {
    FirebaseJson statusJson;
    statusJson.set("online", true);
    statusJson.set("last_seen", millis());
    statusJson.set("node_type", node_status[idx].node_type);
    
    if (node_status[idx].node_type == 1) {
      statusJson.set("temperature", node_status[idx].temperature);
      statusJson.set("humidity", node_status[idx].humidity);
      statusJson.set("exhaust_status", node_status[idx].exhaust_status);
    } else if (node_status[idx].node_type == 2) {
      statusJson.set("current_ppm", node_status[idx].current_ppm);
      statusJson.set("current_ph", node_status[idx].current_ph);
      statusJson.set("pump_active", node_status[idx].pump_active);
    }
    
    statusJson.set("mode", node_status[idx].mode);
    
    Firebase.RTDB.setJSON(&fbdo, statusPath.c_str(), &statusJson);
    
    Serial.printf("[FIREBASE] ✅ Data sent for Node %d\n", node_id);
    return true;
  } else {
    Serial.printf("[FIREBASE] ❌ Failed to send: %s\n", fbdo.errorReason().c_str());
    return false;
  }
}

// ==================== HTTP HANDLERS ====================
void setCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  setCORSHeaders();
  server.send(200, "text/plain", "");
}

void handlePing() {
  setCORSHeaders();
  
  DynamicJsonDocument doc(256);
  doc["status"] = "ok";
  doc["gateway"] = "greenhouse-monitoring";
  doc["ip"] = WiFi.localIP().toString();
  doc["uptime"] = millis() / 1000;
  doc["firebase"] = firebaseReady ? "connected" : "disconnected";
  doc["stream"] = streamReady ? "active" : "inactive";
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
  
  Serial.println("[PING] ✓ Received");
}

void handleNodeSync() {
  setCORSHeaders();
  
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method not allowed\"}");
    return;
  }
  
  String body = server.arg("plain");
  
  Serial.println("\n========== NODE SYNC ==========");
  
  if (body.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"Empty body\"}");
    return;
  }
  
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    Serial.printf("JSON Error: %s\n", error.c_str());
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  if (!doc.containsKey("node_id")) {
    server.send(400, "application/json", "{\"error\":\"Missing node_id\"}");
    return;
  }
  
  int node_id = doc["node_id"];
  
  if (node_id < 1 || node_id > NUM_NODES) {
    server.send(400, "application/json", "{\"error\":\"Invalid node_id\"}");
    return;
  }
  
  // ==================== PROSES LATENCY ====================
  unsigned long t_receive = millis();
  
  if (doc.containsKey("t_send") && doc.containsKey("sequence_no")) {
    LatencyData latency;
    latency.t_send = doc["t_send"];
    latency.t_receive = t_receive;
    latency.latency = t_receive - latency.t_send;
    latency.sequence_no = doc["sequence_no"];
    latency.rssi = doc.containsKey("rssi") ? doc["rssi"].as<int>() : -100;
    
    // Tentukan status berdasarkan latency
    if (latency.latency < 50) {
      latency.status = "Excellent";
    } else if (latency.latency < 100) {
      latency.status = "Good";
    } else if (latency.latency < 200) {
      latency.status = "Fair";
    } else {
      latency.status = "Poor";
    }
    
    // ===== OUTPUT LATENCY KE SERIAL MONITOR =====
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║       LATENCY MEASUREMENT RESULT       ║");
    Serial.println("╠════════════════════════════════════════╣");
    Serial.printf("║ Timestamp      : %-21lu║\n", millis());
    Serial.printf("║ Node ID        : %-21d║\n", node_id);
    Serial.printf("║ Sequence No    : %-21lu║\n", latency.sequence_no);
    Serial.printf("║ T_send (ms)    : %-21lu║\n", latency.t_send);
    Serial.printf("║ T_receive (ms) : %-21lu║\n", latency.t_receive);
    Serial.printf("║ Latency (ms)   : %-21lu║\n", latency.latency);
    Serial.printf("║ RSSI (dBm)     : %-21d║\n", latency.rssi);
    Serial.printf("║ Status         : %-21s║\n", latency.status.c_str());
    Serial.println("╚════════════════════════════════════════╝\n");
    
    // Kirim ke Firebase
    sendLatencyToFirebase(node_id, latency);
  }
  
  // ==================== UPDATE PDR ====================
  bool packet_valid = (doc.containsKey("temperature") || doc.containsKey("ppm"));
  updatePDRStats(node_id, packet_valid);
  
  // Kirim PDR setiap 10 paket
  int idx = node_id - 1;
  if (pdr_stats[idx].packets_sent % 10 == 0) {
    sendPDRToFirebase(node_id);
  }
  
  doc["server_timestamp"] = millis();
  
  NodeControl& ctrl = controls[idx];
  
  // FUZZY LOGIC UNTUK NODE SENSOR 1
  if (doc.containsKey("temperature") && doc.containsKey("humidity") && !ctrl.manual_mode) {
    float temp = doc["temperature"];
    float hum = doc["humidity"];
    
    FuzzyResult fuzzyResult = fuzzyControlExhaust(temp, hum);
    
    ctrl.actuator_command = (fuzzyResult.exhaust_status > 0);
    
    doc["fuzzy_defuzzification"]["mu_temp_low"] = fuzzyResult.mu_temp_low;
    doc["fuzzy_defuzzification"]["mu_temp_normal"] = fuzzyResult.mu_temp_normal;
    doc["fuzzy_defuzzification"]["mu_temp_high"] = fuzzyResult.mu_temp_high;
    doc["fuzzy_defuzzification"]["mu_hum_low"] = fuzzyResult.mu_hum_low;
    doc["fuzzy_defuzzification"]["mu_hum_normal"] = fuzzyResult.mu_hum_normal;
    doc["fuzzy_defuzzification"]["mu_hum_high"] = fuzzyResult.mu_hum_high;
    doc["fuzzy_defuzzification"]["out_off"] = fuzzyResult.out_off;
    doc["fuzzy_defuzzification"]["out_medium"] = fuzzyResult.out_medium;
    doc["fuzzy_defuzzification"]["out_high"] = fuzzyResult.out_high;
    doc["fuzzy_defuzzification"]["crisp_output"] = fuzzyResult.crisp_output;
    doc["fuzzy_defuzzification"]["exhaust_status"] = fuzzyResult.exhaust_status;
    
    Serial.printf("[AUTO-FUZZY] Node %d: Temp=%.1f°C, Hum=%.1f%% -> Exhaust=%d\n", 
                  node_id, temp, hum, fuzzyResult.exhaust_status);
  }
  
  bool firebaseSuccess = sendToFirebase(node_id, doc);
  
  DynamicJsonDocument response(512);
  response["status"] = "success";
  response["firebase"] = firebaseSuccess ? "success" : "failed";
  response["mode"] = ctrl.manual_mode ? "MANUAL" : "AUTO";
  response["actuator_command"] = ctrl.actuator_command;
  response["setpoint"] = ctrl.setpoint;
  response["timestamp"] = ctrl.timestamp;
  response["t_receive"] = t_receive;
  
  // Tambahkan informasi PDR ke response
  response["pdr_stats"]["packets_sent"] = pdr_stats[idx].packets_sent;
  response["pdr_stats"]["packets_received"] = pdr_stats[idx].packets_received;
  response["pdr_stats"]["pdr_percentage"] = pdr_stats[idx].pdr_percentage;
  response["pdr_stats"]["status"] = pdr_stats[idx].status;
  
  if (doc.containsKey("temperature") && doc.containsKey("humidity") && !ctrl.manual_mode) {
    if (doc.containsKey("fuzzy_defuzzification")) {
      response["fuzzy_exhaust_status"] = doc["fuzzy_defuzzification"]["exhaust_status"];
    }
  }
  
  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);
  
  Serial.println("===============================\n");
}

void handleControl() {
  setCORSHeaders();
  
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method not allowed\"}");
    return;
  }
  
  String body = server.arg("plain");
  DynamicJsonDocument doc(512);
  
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  int node_id = doc["node_id"];
  
  if (node_id < 1 || node_id > NUM_NODES) {
    server.send(400, "application/json", "{\"error\":\"Invalid node_id\"}");
    return;
  }
  
  int idx = node_id - 1;
  
  if (doc.containsKey("manual_mode")) controls[idx].manual_mode = doc["manual_mode"];
  if (doc.containsKey("actuator_command")) controls[idx].actuator_command = doc["actuator_command"];
  if (doc.containsKey("setpoint")) controls[idx].setpoint = doc["setpoint"];
  
  controls[idx].timestamp = millis();
  
  String path = "/nodes/node_" + String(node_id) + "/control";
  FirebaseJson json;
  json.set("manual_mode", controls[idx].manual_mode);
  json.set("actuator_command", controls[idx].actuator_command);
  json.set("setpoint", controls[idx].setpoint);
  json.set("timestamp", controls[idx].timestamp);
  
  bool firebaseSuccess = Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json);
  
  DynamicJsonDocument response(256);
  response["status"] = "success";
  response["firebase"] = firebaseSuccess ? "success" : "failed";
  response["node_id"] = node_id;
  
  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);
  
  Serial.printf("[CONTROL] Node %d: Mode=%s, Command=%s, Setpoint=%.1f, Firebase=%s\n",
                node_id,
                controls[idx].manual_mode ? "MANUAL" : "AUTO",
                controls[idx].actuator_command ? "ON" : "OFF",
                controls[idx].setpoint,
                firebaseSuccess ? "✓" : "✗");
}

void handleGetStatus() {
  setCORSHeaders();
  
  DynamicJsonDocument doc(3072);
  JsonArray nodes = doc.createNestedArray("nodes");
  
  unsigned long now = millis();
  
  for (int i = 0; i < NUM_NODES; i++) {
    JsonObject node = nodes.createNestedObject();
    node["node_id"] = i + 1;
    node["online"] = (now - node_status[i].last_seen < 30000);
    node["last_seen_sec"] = (now - node_status[i].last_seen) / 1000;
    node["node_type"] = node_status[i].node_type;
    
    if (node_status[i].node_type == 1) {
      node["temperature"] = node_status[i].temperature;
      node["humidity"] = node_status[i].humidity;
      node["exhaust_status"] = node_status[i].exhaust_status;
    } else if (node_status[i].node_type == 2) {
      node["current_ppm"] = node_status[i].current_ppm;
      node["current_ph"] = node_status[i].current_ph;
      node["pump_active"] = node_status[i].pump_active;
    }
    
    node["mode"] = node_status[i].mode;
    node["manual_mode"] = controls[i].manual_mode;
    node["actuator_command"] = controls[i].actuator_command;
    node["setpoint"] = controls[i].setpoint;
    
    // Tambahkan PDR stats
    JsonObject pdr = node.createNestedObject("pdr");
    pdr["packets_sent"] = pdr_stats[i].packets_sent;
    pdr["packets_received"] = pdr_stats[i].packets_received;
    pdr["packets_lost"] = pdr_stats[i].packets_lost;
    pdr["pdr_percentage"] = pdr_stats[i].pdr_percentage;
    pdr["status"] = pdr_stats[i].status;
  }
  
  doc["gateway_ip"] = WiFi.localIP().toString();
  doc["uptime"] = millis() / 1000;
  doc["firebase_connected"] = firebaseReady;
  doc["firebase_stream"] = streamReady;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// Reset PDR stats untuk testing
void handleResetPDR() {
  setCORSHeaders();
  
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method not allowed\"}");
    return;
  }
  
  String body = server.arg("plain");
  DynamicJsonDocument doc(256);
  
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  int node_id = doc.containsKey("node_id") ? doc["node_id"].as<int>() : 0;
  
  if (node_id == 0) {
    // Reset semua nodes
    for (int i = 0; i < NUM_NODES; i++) {
      pdr_stats[i].packets_sent = 0;
      pdr_stats[i].packets_received = 0;
      pdr_stats[i].packets_lost = 0;
      pdr_stats[i].pdr_percentage = 0.0f;
      pdr_stats[i].status = "No Data";
      pdr_stats[i].last_reset = millis();
    }
    Serial.println("[PDR] ✓ Reset all nodes");
  } else if (node_id >= 1 && node_id <= NUM_NODES) {
    // Reset node tertentu
    int idx = node_id - 1;
    pdr_stats[idx].packets_sent = 0;
    pdr_stats[idx].packets_received = 0;
    pdr_stats[idx].packets_lost = 0;
    pdr_stats[idx].pdr_percentage = 0.0f;
    pdr_stats[idx].status = "No Data";
    pdr_stats[idx].last_reset = millis();
    Serial.printf("[PDR] ✓ Reset node %d\n", node_id);
  } else {
    server.send(400, "application/json", "{\"error\":\"Invalid node_id\"}");
    return;
  }
  
  server.send(200, "application/json", "{\"status\":\"success\"}");
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║  ESP32 GREENHOUSE GATEWAY + FUZZY     ║");
  Serial.println("║        WITH LATENCY & PDR TESTING      ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  // Initialize arrays
  for (int i = 0; i < NUM_NODES; i++) {
    controls[i] = {false, false, 800.0f, 0};
    node_status[i] = {0, false, 0, 0.0f, 0.0f, 0, 0.0f, 0.0f, false, "AUTO"};
    pdr_stats[i] = {0, 0, 0, 0.0f, "No Data", 0};
  }
  
  // WiFi Connection
  Serial.println("Connecting to WiFi...");
  Serial.printf("SSID: %s\n", ssid);
  
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
    Serial.println("\n✅✅✅ WIFI CONNECTED! ✅✅✅\n");
    
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║     ⚠️  GATEWAY IP ADDRESS ⚠️          ║");
    Serial.println("╠════════════════════════════════════════╣");
    Serial.print("║        ");
    Serial.print(WiFi.localIP());
    Serial.println("               ║");
    Serial.println("╠════════════════════════════════════════╣");
    Serial.println("║  UPDATE IP DI CODE NODE!               ║");
    Serial.print("║  const char* gatewayIP = \"");
    Serial.print(WiFi.localIP());
    Serial.println("\";  ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
  } else {
    Serial.println("\n❌ WIFI FAILED!\n");
    return;
  }
  
  // Firebase Configuration
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║  CONNECTING TO FIREBASE...             ║");
  Serial.println("╚════════════════════════════════════════╝");
  
  config.database_url = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  config.timeout.serverResponse = 10 * 1000;
  
  config.cert.data = NULL;
  config.cert.file = "";
  config.cert.file_storage = mem_storage_type_flash;
  
  fbdo.setBSSLBufferSize(4096, 1024);
  fbdo_stream.setBSSLBufferSize(4096, 1024);
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  delay(1000);
  
  if (Firebase.ready()) {
    firebaseReady = true;
    Serial.println("\n✅✅✅ FIREBASE CONNECTED! ✅✅✅\n");
    
    syncControlFromFirebase();
    setupFirebaseStream();
    
  } else {
    Serial.println("\n❌ FIREBASE CONNECTION FAILED!\n");
    firebaseReady = false;
  }
  
  // Setup HTTP Server
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  SETTING UP HTTP SERVER...             ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  server.on("/ping", HTTP_GET, handlePing);
  server.on("/ping", HTTP_OPTIONS, handleOptions);
  
  server.on("/api/node/sync", HTTP_POST, handleNodeSync);
  server.on("/api/node/sync", HTTP_OPTIONS, handleOptions);
  
  server.on("/api/control", HTTP_POST, handleControl);
  server.on("/api/control", HTTP_OPTIONS, handleOptions);
  
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  
  server.on("/api/pdr/reset", HTTP_POST, handleResetPDR);
  server.on("/api/pdr/reset", HTTP_OPTIONS, handleOptions);
  
  server.onNotFound([]() {
    setCORSHeaders();
    server.send(404, "application/json", "{\"error\":\"Not found\"}");
  });
  
  server.begin();
  
  Serial.println("\n✅✅✅ SERVER STARTED! ✅✅✅");
  Serial.printf("Gateway ready at http://%s:8080\n", WiFi.localIP().toString().c_str());
  Serial.println("\nEndpoints:");
  Serial.println("  GET  /ping");
  Serial.println("  POST /api/node/sync");
  Serial.println("  POST /api/control");
  Serial.println("  GET  /api/status");
  Serial.println("  POST /api/pdr/reset");
  Serial.println("\nTesting Features:");
  Serial.println("  ✓ Latency measurement (auto)");
  Serial.println("  ✓ PDR tracking (auto)");
  Serial.println("  ✓ Firebase logging\n");
}

// ==================== LOOP ====================
void loop() {
  server.handleClient();
  
  if (firebaseReady && streamReady) {
    Firebase.RTDB.readStream(&fbdo_stream);
  }
  
  if (firebaseReady && !streamReady && millis() - lastStreamCheck > 30000) {
    lastStreamCheck = millis();
    setupFirebaseStream();
  }
  
  delay(10);
}
