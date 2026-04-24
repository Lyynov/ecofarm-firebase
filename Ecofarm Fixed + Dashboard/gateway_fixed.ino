#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ==================== KONFIGURASI ====================
// [STA] WiFi Router - untuk internet & Firebase
const char* sta_ssid     = "smk unnes semarang";
const char* sta_password = "12345678";

// [AP] Access Point Gateway - yang dipakai node sensor
const char* ap_ssid     = "EcoFarm-Gateway";
const char* ap_password = "ecofarm123";

// [AP] IP Static Gateway - TIDAK AKAN BERUBAH, set ini di semua node sensor
IPAddress ap_local_ip(192, 168, 4, 1);
IPAddress ap_gateway(192, 168, 4, 1);
IPAddress ap_subnet(255, 255, 255, 0);

#define FIREBASE_HOST "projectecofarm-98727-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "z2fgLdOr7LsdHq9yNUGKhbxuGVfy6GPeuKsbFDyY"

#define NUM_NODES 4

// ==================== FIREBASE OBJECTS ====================
FirebaseData fbdo;         // untuk push sensor/fuzzy log
FirebaseData fbdo_stream;  // untuk realtime stream (read control)
FirebaseData fbdo_ctrl;    // khusus write control — tidak boleh bentrok dengan fbdo
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

bool firebaseReady     = false;
bool streamReady       = false;
bool internetConnected = false;
unsigned long lastStreamCheck   = 0;
unsigned long lastInternetCheck = 0;
unsigned long lastControlPoll   = 0;   // polling backup control tiap 3 detik

// ==================== FUZZY MAMDANI (NODE EXHAUST) ====================
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
  float den  = mati + sedang + menyala;
  if (den == 0.0f) return 0.0f;
  float result = num / den;
  if (result < 0.5f) return 0.0f;
  if (result < 1.5f) return 1.0f;
  return 2.0f;
}

FuzzyResult fuzzyControlExhaust(float temperature, float humidity) {
  FuzzyResult result;
  result.mu_temp_low    = membershipSuhuRendah(temperature);
  result.mu_temp_normal = membershipSuhuNormal(temperature);
  result.mu_temp_high   = membershipSuhuTinggi(temperature);
  result.mu_hum_low     = membershipHumidityRendah(humidity);
  result.mu_hum_normal  = membershipHumidityNormal(humidity);
  result.mu_hum_high    = membershipHumidityTinggi(humidity);

  float rule1_mati    = result.mu_temp_low;
  float rule2_mati    = min(result.mu_temp_normal, result.mu_temp_low);
  float rule3_sedang  = result.mu_temp_normal;
  float rule4_sedang  = min(result.mu_temp_normal, result.mu_temp_high);
  float rule5_sedang  = min(result.mu_temp_high,   result.mu_temp_normal);
  float rule6_menyala = result.mu_temp_high;
  float rule7_menyala = min(result.mu_temp_high, 1.0f - result.mu_temp_normal);
  float rule8_menyala = (temperature > 35.0f) ? 1.0f : 0.0f;
  float rule9_sedang  = min(result.mu_temp_normal,
                         min(1.0f - result.mu_temp_low, 1.0f - result.mu_temp_high));

  result.out_off    = max(rule1_mati, rule2_mati);
  result.out_medium = max(max(rule3_sedang, rule4_sedang), max(rule5_sedang, rule9_sedang));
  result.out_high   = max(max(rule6_menyala, rule7_menyala), rule8_menyala);

  result.crisp_output   = defuzzifyExhaust(result.out_off, result.out_medium, result.out_high);
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

// ==================== KIRIM FUZZY NUTRIENT KE FIREBASE ====================
bool sendFuzzyNutrientToFirebase(int node_id, const DynamicJsonDocument& doc) {
  if (!firebaseReady) return false;
  if (!doc.containsKey("fuzzy_detail")) return false;

  String path = "/fuzzy_log/node_" + String(node_id);
  FirebaseJson json;
  json.set("timestamp", millis());
  json.set("node_id",   node_id);
  json.set("node_type", "nutrient_pump");

  json.set("input/error_pct",   doc.containsKey("error_pct")   ? doc["error_pct"].as<float>()   : 0.0f);
  json.set("input/delta_error", doc.containsKey("delta_error") ? doc["delta_error"].as<float>() : 0.0f);
  json.set("input/ppm",         doc.containsKey("ppm")         ? doc["ppm"].as<float>()         : 0.0f);
  json.set("input/setpoint",    doc.containsKey("setpoint")    ? doc["setpoint"].as<float>()    : 0.0f);

  JsonObjectConst fd = doc["fuzzy_detail"].as<JsonObjectConst>();

  json.set("fuzzifikasi/mu_err_low",          fd.containsKey("mu_err_low")      ? fd["mu_err_low"].as<float>()      : 0.0f);
  json.set("fuzzifikasi/mu_err_medium",        fd.containsKey("mu_err_medium")   ? fd["mu_err_medium"].as<float>()   : 0.0f);
  json.set("fuzzifikasi/mu_err_high",          fd.containsKey("mu_err_high")     ? fd["mu_err_high"].as<float>()     : 0.0f);
  json.set("fuzzifikasi/mu_delta_decreasing",  fd.containsKey("mu_delta_dec")    ? fd["mu_delta_dec"].as<float>()    : 0.0f);
  json.set("fuzzifikasi/mu_delta_stable",      fd.containsKey("mu_delta_stable") ? fd["mu_delta_stable"].as<float>() : 0.0f);
  json.set("fuzzifikasi/mu_delta_increasing",  fd.containsKey("mu_delta_inc")    ? fd["mu_delta_inc"].as<float>()    : 0.0f);

  json.set("inferensi/rule1_ErrLow_DeltaDec_VeryShort", fd.containsKey("rule1_vs") ? fd["rule1_vs"].as<float>() : 0.0f);
  json.set("inferensi/rule2_ErrLow_DeltaStable_Short",  fd.containsKey("rule2_s")  ? fd["rule2_s"].as<float>()  : 0.0f);
  json.set("inferensi/rule3_ErrLow_DeltaInc_Medium",    fd.containsKey("rule3_m")  ? fd["rule3_m"].as<float>()  : 0.0f);
  json.set("inferensi/rule4_ErrMed_DeltaDec_Short",     fd.containsKey("rule4_s")  ? fd["rule4_s"].as<float>()  : 0.0f);
  json.set("inferensi/rule5_ErrMed_DeltaStable_Medium", fd.containsKey("rule5_m")  ? fd["rule5_m"].as<float>()  : 0.0f);
  json.set("inferensi/rule6_ErrMed_DeltaInc_Long",      fd.containsKey("rule6_l")  ? fd["rule6_l"].as<float>()  : 0.0f);
  json.set("inferensi/rule7_ErrHigh_DeltaDec_Medium",   fd.containsKey("rule7_m")  ? fd["rule7_m"].as<float>()  : 0.0f);
  json.set("inferensi/rule8_ErrHigh_DeltaStable_Long",  fd.containsKey("rule8_l")  ? fd["rule8_l"].as<float>()  : 0.0f);
  json.set("inferensi/rule9_ErrHigh_DeltaInc_VeryLong", fd.containsKey("rule9_vl") ? fd["rule9_vl"].as<float>() : 0.0f);

  json.set("agregasi/out_very_short", fd.containsKey("out_vs") ? fd["out_vs"].as<float>() : 0.0f);
  json.set("agregasi/out_short",      fd.containsKey("out_s")  ? fd["out_s"].as<float>()  : 0.0f);
  json.set("agregasi/out_medium",     fd.containsKey("out_m")  ? fd["out_m"].as<float>()  : 0.0f);
  json.set("agregasi/out_long",       fd.containsKey("out_l")  ? fd["out_l"].as<float>()  : 0.0f);
  json.set("agregasi/out_very_long",  fd.containsKey("out_vl") ? fd["out_vl"].as<float>() : 0.0f);

  json.set("defuzzifikasi/crisp_duration_ms",  fd.containsKey("crisp_ms")  ? fd["crisp_ms"].as<float>()  : 0.0f);
  json.set("defuzzifikasi/crisp_duration_sec", fd.containsKey("crisp_sec") ? fd["crisp_sec"].as<float>() : 0.0f);
  json.set("defuzzifikasi/pump_active",
           doc.containsKey("pump_active") ? doc["pump_active"].as<bool>() : false);

  bool success = Firebase.RTDB.pushJSON(&fbdo, path.c_str(), &json);
  if (success) {
    Serial.printf("[FUZZY-FB] ✅ Node %d fuzzy log sent\n", node_id);
  } else {
    Serial.printf("[FUZZY-FB] ❌ Failed: %s\n", fbdo.errorReason().c_str());
  }
  return success;
}

// ==================== KIRIM FUZZY EXHAUST KE FIREBASE ====================
// Dipanggil setelah fuzzyControlExhaust() selesai diproses di gateway
// Path: /fuzzy_log/node_exhaust_X  (node 1 atau 2)
bool sendFuzzyExhaustToFirebase(int node_id, float temperature, float humidity,
                                 const FuzzyResult& fr, int exhaust_status) {
  if (!firebaseReady) return false;

  String path = "/fuzzy_log/node_" + String(node_id);
  FirebaseJson json;

  // ── Metadata ──
  json.set("timestamp",  millis());
  json.set("node_id",    node_id);
  json.set("node_type",  "exhaust_fan");

  // ── INPUT: suhu & kelembaban dari node sensor ──
  json.set("input/temperature", temperature);
  json.set("input/humidity",    humidity);

  // ── FUZZIFIKASI: derajat keanggotaan suhu ──
  json.set("fuzzifikasi/mu_temp_low",    fr.mu_temp_low);
  json.set("fuzzifikasi/mu_temp_normal", fr.mu_temp_normal);
  json.set("fuzzifikasi/mu_temp_high",   fr.mu_temp_high);

  // ── FUZZIFIKASI: derajat keanggotaan humidity ──
  json.set("fuzzifikasi/mu_hum_low",     fr.mu_hum_low);
  json.set("fuzzifikasi/mu_hum_normal",  fr.mu_hum_normal);
  json.set("fuzzifikasi/mu_hum_high",    fr.mu_hum_high);

  // ── INFERENSI: firing strength tiap rule ──
  // Rule base exhaust (suhu dominan):
  // R1: Suhu Low               → MATI
  // R2: Suhu Normal & Low      → MATI
  // R3: Suhu Normal            → SEDANG
  // R4: Suhu Normal & High     → SEDANG
  // R5: Suhu High & Normal     → SEDANG
  // R6: Suhu High              → MENYALA
  // R7: Suhu High & !Normal    → MENYALA
  // R8: Suhu > 35°C (crisp)   → MENYALA
  // R9: Suhu Normal tengah     → SEDANG
  float rule1 = fr.mu_temp_low;
  float rule2 = min(fr.mu_temp_normal, fr.mu_temp_low);
  float rule3 = fr.mu_temp_normal;
  float rule4 = min(fr.mu_temp_normal, fr.mu_temp_high);
  float rule5 = min(fr.mu_temp_high,   fr.mu_temp_normal);
  float rule6 = fr.mu_temp_high;
  float rule7 = min(fr.mu_temp_high, 1.0f - fr.mu_temp_normal);
  float rule8 = (temperature > 35.0f) ? 1.0f : 0.0f;
  float rule9 = min(fr.mu_temp_normal, min(1.0f - fr.mu_temp_low, 1.0f - fr.mu_temp_high));

  json.set("inferensi/rule1_TempLow_MATI",          rule1);
  json.set("inferensi/rule2_TempNormal_TempLow_MATI", rule2);
  json.set("inferensi/rule3_TempNormal_SEDANG",      rule3);
  json.set("inferensi/rule4_TempNormal_TempHigh_SEDANG", rule4);
  json.set("inferensi/rule5_TempHigh_TempNormal_SEDANG", rule5);
  json.set("inferensi/rule6_TempHigh_MENYALA",       rule6);
  json.set("inferensi/rule7_TempHigh_NotNormal_MENYALA", rule7);
  json.set("inferensi/rule8_Temp35_MENYALA",         rule8);
  json.set("inferensi/rule9_TempNormal_Tengah_SEDANG", rule9);

  // ── AGREGASI: output membership per kategori ──
  json.set("agregasi/out_mati",    fr.out_off);
  json.set("agregasi/out_sedang",  fr.out_medium);
  json.set("agregasi/out_menyala", fr.out_high);

  // ── DEFUZZIFIKASI ──
  json.set("defuzzifikasi/crisp_output",  fr.crisp_output);
  json.set("defuzzifikasi/exhaust_status", exhaust_status);
  // Mapping status ke label
  const char* status_label = (exhaust_status == 0) ? "MATI" :
                              (exhaust_status == 1) ? "SEDANG" : "MENYALA";
  json.set("defuzzifikasi/exhaust_label", status_label);
  json.set("defuzzifikasi/fan_active",    (exhaust_status > 0));

  bool success = Firebase.RTDB.pushJSON(&fbdo, path.c_str(), &json);
  if (success) {
    Serial.printf("[FUZZY-EXHAUST-FB] ✅ Node %d exhaust log sent → /fuzzy_log/node_%d\n",
                  node_id, node_id);
  } else {
    Serial.printf("[FUZZY-EXHAUST-FB] ❌ Failed: %s\n", fbdo.errorReason().c_str());
  }
  return success;
}

// ==================== FUNGSI PDR ====================
void updatePDRStats(int node_id, bool packet_received) {
  int idx = node_id - 1;
  pdr_stats[idx].packets_sent++;
  if (packet_received) pdr_stats[idx].packets_received++;
  else                 pdr_stats[idx].packets_lost++;

  if (pdr_stats[idx].packets_sent > 0)
    pdr_stats[idx].pdr_percentage =
      (float)pdr_stats[idx].packets_received / (float)pdr_stats[idx].packets_sent * 100.0f;

  if      (pdr_stats[idx].pdr_percentage >= 90.0f) pdr_stats[idx].status = "Excellent";
  else if (pdr_stats[idx].pdr_percentage >= 75.0f) pdr_stats[idx].status = "Good";
  else if (pdr_stats[idx].pdr_percentage >= 50.0f) pdr_stats[idx].status = "Fair";
  else                                              pdr_stats[idx].status = "Poor";

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║         PDR STATISTICS UPDATE          ║");
  Serial.println("╠════════════════════════════════════════╣");
  Serial.printf( "║ Timestamp      : %-21lu║\n", millis());
  Serial.printf( "║ Node ID        : %-21d║\n", node_id);
  Serial.printf( "║ Packets Sent   : %-21lu║\n", pdr_stats[idx].packets_sent);
  Serial.printf( "║ Packets Rcvd   : %-21lu║\n", pdr_stats[idx].packets_received);
  Serial.printf( "║ Packets Lost   : %-21lu║\n", pdr_stats[idx].packets_lost);
  Serial.printf( "║ PDR (%%)        : %-20.2f%%║\n", pdr_stats[idx].pdr_percentage);
  Serial.printf( "║ Status         : %-21s║\n", pdr_stats[idx].status.c_str());
  Serial.println("╚════════════════════════════════════════╝\n");
}

bool sendPDRToFirebase(int node_id) {
  if (!firebaseReady) { Serial.println("[PDR-FIREBASE] ⚠ Firebase not ready"); return false; }
  int idx = node_id - 1;
  String path = "/testing/pdr/node_" + String(node_id);
  FirebaseJson json;
  json.set("timestamp",        millis());
  json.set("node_id",          node_id);
  json.set("packets_sent",     pdr_stats[idx].packets_sent);
  json.set("packets_received", pdr_stats[idx].packets_received);
  json.set("packets_lost",     pdr_stats[idx].packets_lost);
  json.set("pdr_percentage",   pdr_stats[idx].pdr_percentage);
  json.set("status",           pdr_stats[idx].status);
  bool success = Firebase.RTDB.pushJSON(&fbdo, path.c_str(), &json);
  Serial.printf("[PDR-FB] Node %d: %s\n", node_id, success ? "✅ Sent" : "❌ Failed");
  return success;
}

bool sendLatencyToFirebase(int node_id, const LatencyData& latency) {
  if (!firebaseReady) { Serial.println("[LATENCY-FIREBASE] ⚠ Firebase not ready"); return false; }
  String path = "/testing/latency/node_" + String(node_id);
  FirebaseJson json;
  json.set("timestamp",    millis());
  json.set("node_id",      node_id);
  json.set("sequence_no",  latency.sequence_no);
  json.set("t_send_ms",    latency.t_send);
  json.set("t_receive_ms", latency.t_receive);
  json.set("latency_ms",   latency.latency);
  json.set("rssi_dbm",     latency.rssi);
  json.set("status",       latency.status);
  bool success = Firebase.RTDB.pushJSON(&fbdo, path.c_str(), &json);
  Serial.printf("[LAT-FB] Node %d: latency=%lums %s\n", node_id, latency.latency, success ? "✅" : "❌");
  return success;
}

// ==================== FIREBASE STREAM ====================
void streamCallback(FirebaseStream data) {
  Serial.println("\n========== FIREBASE STREAM UPDATE ==========");
  String path     = data.dataPath();
  String dataType = data.dataType();
  Serial.printf("Path: %s | Type: %s\n", path.c_str(), dataType.c_str());

  // Path harus di bawah /nodes/node_X/control
  if (!path.startsWith("/nodes/node_") && path != "/") {
    // Path relatif dari stream root "/nodes" — tambah prefix
  }

  // Ekstrak node ID dari path
  // Path bisa berupa:
  //   /node_3/control            (update seluruh control object)
  //   /node_3/control/setpoint   (update field tunggal)
  //   /node_3/control/manual_mode
  //   /                          (initial load seluruh /nodes)

  // Cari "node_" di path
  int nIdx = path.indexOf("node_");
  if (nIdx == -1) {
    Serial.println("[STREAM] Bukan path node, skip");
    Serial.println("============================================\n");
    return;
  }

  int nStart = nIdx + 5;
  int nEnd   = path.indexOf("/", nStart);
  if (nEnd == -1) nEnd = path.length();
  int nodeId = path.substring(nStart, nEnd).toInt();

  if (nodeId < 1 || nodeId > NUM_NODES) {
    Serial.println("[STREAM] Node ID tidak valid, skip");
    Serial.println("============================================\n");
    return;
  }

  // Harus mengandung "/control"
  if (path.indexOf("/control") == -1) {
    Serial.println("[STREAM] Bukan path control, skip");
    Serial.println("============================================\n");
    return;
  }

  NodeControl& ctrl = controls[nodeId - 1];

  // ── CASE 1: Update seluruh control object (dataType = json) ──
  if (dataType == "json") {
    FirebaseJson    json = data.jsonObject();
    FirebaseJsonData jd;
    if (json.get(jd, "manual_mode"))      ctrl.manual_mode      = jd.boolValue;
    if (json.get(jd, "actuator_command")) ctrl.actuator_command = jd.boolValue;
    if (json.get(jd, "setpoint"))         ctrl.setpoint         = jd.floatValue;
    ctrl.timestamp = millis();
    Serial.printf("✓ [JSON] Node %d: Mode=%s, Cmd=%s, SP=%.1f\n",
                  nodeId,
                  ctrl.manual_mode      ? "MANUAL" : "AUTO",
                  ctrl.actuator_command ? "ON"     : "OFF",
                  ctrl.setpoint);
  }
  // ── CASE 2: Update field tunggal — dashboard hanya update 1 key ──
  else {
    // Ambil nama field terakhir dari path
    // Contoh: /node_3/control/setpoint -> "setpoint"
    int lastSlash = path.lastIndexOf("/");
    String field  = path.substring(lastSlash + 1);

    Serial.printf("[STREAM] Field tunggal: '%s'\n", field.c_str());

    if (field == "setpoint" && (dataType == "float" || dataType == "int" || dataType == "double")) {
      ctrl.setpoint  = data.floatData();
      ctrl.timestamp = millis();
      Serial.printf("✓ [FIELD] Node %d Setpoint=%.1f\n", nodeId, ctrl.setpoint);
    }
    else if (field == "manual_mode" && dataType == "boolean") {
      ctrl.manual_mode = data.boolData();
      ctrl.timestamp   = millis();
      Serial.printf("✓ [FIELD] Node %d Mode=%s\n",
                    nodeId, ctrl.manual_mode ? "MANUAL" : "AUTO");
    }
    else if (field == "actuator_command" && dataType == "boolean") {
      ctrl.actuator_command = data.boolData();
      ctrl.timestamp        = millis();
      Serial.printf("✓ [FIELD] Node %d ActuatorCmd=%s\n",
                    nodeId, ctrl.actuator_command ? "ON" : "OFF");
    }
    else {
      Serial.printf("[STREAM] Field '%s' tidak dikenal / type '%s', skip\n",
                    field.c_str(), dataType.c_str());
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
  if (!firebaseReady) { Serial.println("[FIREBASE] Not ready, skip stream"); return; }
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
    if (Firebase.RTDB.getJSON(&fbdo_ctrl, path.c_str())) {
      if (fbdo.dataType() == "json") {
        FirebaseJson     json = fbdo.jsonObject();
        FirebaseJsonData jsonData;
        NodeControl& ctrl = controls[i - 1];
        if (json.get(jsonData, "manual_mode"))      ctrl.manual_mode      = jsonData.boolValue;
        if (json.get(jsonData, "actuator_command")) ctrl.actuator_command = jsonData.boolValue;
        if (json.get(jsonData, "setpoint"))         ctrl.setpoint         = jsonData.floatValue;
        ctrl.timestamp = millis();
        Serial.printf("  ✓ Node %d: Mode=%s, Cmd=%s, Setpoint=%.1f\n",
                      i,
                      ctrl.manual_mode      ? "MANUAL" : "AUTO",
                      ctrl.actuator_command ? "ON"     : "OFF",
                      ctrl.setpoint);
      } else {
        // Control node ini belum ada di Firebase - tulis default
        Serial.printf("  ⚠ Node %d: Tidak ada data control, tulis default\n", i);
        String writePath = "/nodes/node_" + String(i) + "/control";
        FirebaseJson defJson;
        defJson.set("manual_mode",      false);
        defJson.set("actuator_command", false);
        defJson.set("setpoint",         800.0f);
        defJson.set("timestamp",        (int)millis());
        Firebase.RTDB.setJSON(&fbdo_ctrl, writePath.c_str(), &defJson);
      }
    } else {
      Serial.printf("  ❌ Node %d sync gagal: %s\n", i, fbdo_ctrl.errorReason().c_str());
    }
    delay(100);
  }
  Serial.println("[FIREBASE] Sync complete!\n");
}

bool sendToFirebase(int node_id, const DynamicJsonDocument& sensorDoc) {
  if (!firebaseReady) { Serial.println("[FIREBASE] ⚠ Not connected"); return false; }
  int idx = node_id - 1;
  node_status[idx].last_seen = millis();
  node_status[idx].online    = true;

  if (sensorDoc.containsKey("temperature")) {
    node_status[idx].node_type      = 1;
    node_status[idx].temperature    = sensorDoc["temperature"];
    node_status[idx].humidity       = sensorDoc["humidity"];
    if (sensorDoc.containsKey("exhaust_status"))
      node_status[idx].exhaust_status = sensorDoc["exhaust_status"];
  } else if (sensorDoc.containsKey("ppm")) {
    node_status[idx].node_type   = 2;
    node_status[idx].current_ppm = round(sensorDoc["ppm"].as<float>() * 10) / 10.0f;
    node_status[idx].current_ph  = round(sensorDoc["ph"].as<float>()  * 10) / 10.0f;
    if (sensorDoc.containsKey("pump_active"))
      node_status[idx].pump_active = sensorDoc["pump_active"];
  }
  if (sensorDoc.containsKey("mode"))
    node_status[idx].mode = sensorDoc["mode"].as<String>();

  String dataPath   = "/nodes/node_" + String(node_id) + "/sensor_data";
  String statusPath = "/nodes/node_" + String(node_id) + "/status";

  FirebaseJson dataJson;
  dataJson.setJsonData(sensorDoc.as<String>());
  bool dataSuccess = Firebase.RTDB.pushJSON(&fbdo_ctrl, dataPath.c_str(), &dataJson);

  if (dataSuccess) {
    FirebaseJson statusJson;
    statusJson.set("online",    true);
    statusJson.set("last_seen", millis());
    statusJson.set("node_type", node_status[idx].node_type);
    if (node_status[idx].node_type == 1) {
      statusJson.set("temperature",    node_status[idx].temperature);
      statusJson.set("humidity",       node_status[idx].humidity);
      statusJson.set("exhaust_status", node_status[idx].exhaust_status);
    } else if (node_status[idx].node_type == 2) {
      statusJson.set("current_ppm", node_status[idx].current_ppm);
      statusJson.set("current_ph",  node_status[idx].current_ph);
      statusJson.set("pump_active", node_status[idx].pump_active);
    }
    statusJson.set("mode", node_status[idx].mode);
    Firebase.RTDB.setJSON(&fbdo_ctrl, statusPath.c_str(), &statusJson);
    Serial.printf("[FIREBASE] ✅ Data sent for Node %d\n", node_id);
    return true;
  } else {
    Serial.printf("[FIREBASE] ❌ Failed to send: %s\n", fbdo_ctrl.errorReason().c_str());
    return false;
  }
}

// ==================== HTTP HANDLERS ====================
void setCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}
void handleOptions() { setCORSHeaders(); server.send(200, "text/plain", ""); }

void handlePing() {
  setCORSHeaders();
  DynamicJsonDocument doc(512);
  doc["status"]    = "ok";
  doc["gateway"]   = "greenhouse-monitoring";
  doc["ap_ip"]     = ap_local_ip.toString();
  doc["sta_ip"]    = WiFi.localIP().toString();
  doc["internet"]  = internetConnected ? "connected" : "disconnected";
  doc["uptime"]    = millis() / 1000;
  doc["firebase"]  = firebaseReady ? "connected" : "disconnected";
  doc["stream"]    = streamReady   ? "active"    : "inactive";
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
  Serial.println("[PING] ✓ Received");
}

void handleNodeSync() {
  setCORSHeaders();
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method not allowed\"}"); return;
  }

  String body = server.arg("plain");
  Serial.println("\n========== NODE SYNC ==========");
  if (body.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"Empty body\"}"); return;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    Serial.printf("JSON Error: %s\n", error.c_str());
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  if (!doc.containsKey("node_id")) {
    server.send(400, "application/json", "{\"error\":\"Missing node_id\"}"); return;
  }

  int node_id = doc["node_id"];
  if (node_id < 1 || node_id > NUM_NODES) {
    server.send(400, "application/json", "{\"error\":\"Invalid node_id\"}"); return;
  }

  // ── Latency ──
  unsigned long t_receive = millis();
  if (doc.containsKey("t_send") && doc.containsKey("sequence_no")) {
    LatencyData latency;
    latency.t_send      = doc["t_send"];
    latency.t_receive   = t_receive;
    latency.latency     = t_receive - latency.t_send;
    latency.sequence_no = doc["sequence_no"];
    latency.rssi        = doc.containsKey("rssi") ? doc["rssi"].as<int>() : -100;
    if      (latency.latency < 50)  latency.status = "Excellent";
    else if (latency.latency < 100) latency.status = "Good";
    else if (latency.latency < 200) latency.status = "Fair";
    else                            latency.status = "Poor";

    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║       LATENCY MEASUREMENT RESULT       ║");
    Serial.println("╠════════════════════════════════════════╣");
    Serial.printf( "║ Timestamp      : %-21lu║\n", millis());
    Serial.printf( "║ Node ID        : %-21d║\n", node_id);
    Serial.printf( "║ Sequence No    : %-21lu║\n", latency.sequence_no);
    Serial.printf( "║ T_send (ms)    : %-21lu║\n", latency.t_send);
    Serial.printf( "║ T_receive (ms) : %-21lu║\n", latency.t_receive);
    Serial.printf( "║ Latency (ms)   : %-21lu║\n", latency.latency);
    Serial.printf( "║ RSSI (dBm)     : %-21d║\n", latency.rssi);
    Serial.printf( "║ Status         : %-21s║\n", latency.status.c_str());
    Serial.println("╚════════════════════════════════════════╝\n");
    sendLatencyToFirebase(node_id, latency);
    yield(); // beri kesempatan loop() proses stream
  }

  // ── PDR ──
  bool packet_valid = (doc.containsKey("temperature") || doc.containsKey("ppm"));
  updatePDRStats(node_id, packet_valid);
  int idx = node_id - 1;
  if (pdr_stats[idx].packets_sent % 10 == 0) { sendPDRToFirebase(node_id); yield(); }

  doc["server_timestamp"] = millis();
  NodeControl& ctrl = controls[idx];

  // ── Fuzzy Exhaust (node 1/2) ──
  if (doc.containsKey("temperature") && doc.containsKey("humidity") && !ctrl.manual_mode) {
    float temp = doc["temperature"];
    float hum  = doc["humidity"];
    FuzzyResult fuzzyResult = fuzzyControlExhaust(temp, hum);
    ctrl.actuator_command = (fuzzyResult.exhaust_status > 0);

    doc["fuzzy_defuzzification"]["mu_temp_low"]    = fuzzyResult.mu_temp_low;
    doc["fuzzy_defuzzification"]["mu_temp_normal"]  = fuzzyResult.mu_temp_normal;
    doc["fuzzy_defuzzification"]["mu_temp_high"]    = fuzzyResult.mu_temp_high;
    doc["fuzzy_defuzzification"]["mu_hum_low"]      = fuzzyResult.mu_hum_low;
    doc["fuzzy_defuzzification"]["mu_hum_normal"]   = fuzzyResult.mu_hum_normal;
    doc["fuzzy_defuzzification"]["mu_hum_high"]     = fuzzyResult.mu_hum_high;
    doc["fuzzy_defuzzification"]["out_off"]         = fuzzyResult.out_off;
    doc["fuzzy_defuzzification"]["out_medium"]      = fuzzyResult.out_medium;
    doc["fuzzy_defuzzification"]["out_high"]        = fuzzyResult.out_high;
    doc["fuzzy_defuzzification"]["crisp_output"]    = fuzzyResult.crisp_output;
    doc["fuzzy_defuzzification"]["exhaust_status"]  = fuzzyResult.exhaust_status;

    Serial.printf("[AUTO-FUZZY] Node %d: Temp=%.1f°C, Hum=%.1f%% -> Exhaust=%d\n",
                  node_id, temp, hum, fuzzyResult.exhaust_status);

    // ── Kirim log fuzzy exhaust ke Firebase ──
    sendFuzzyExhaustToFirebase(node_id, temp, hum, fuzzyResult, fuzzyResult.exhaust_status);
  }

  // ── Fuzzy Log Nutrient (node 3/4) ──
  if (doc.containsKey("ppm") && doc.containsKey("fuzzy_detail"))
    sendFuzzyNutrientToFirebase(node_id, doc);
  yield(); // beri kesempatan stream sebelum sendToFirebase

  bool firebaseSuccess = sendToFirebase(node_id, doc);
  yield(); // setelah push sensor data, beri stream kesempatan jalan

  // ── Response ke node ──
  DynamicJsonDocument response(512);
  response["status"]           = "success";
  response["firebase"]         = firebaseSuccess ? "success" : "failed";
  response["internet"]         = internetConnected ? "connected" : "disconnected";
  response["mode"]             = ctrl.manual_mode ? "MANUAL" : "AUTO";
  response["actuator_command"] = ctrl.actuator_command;
  response["setpoint"]         = ctrl.setpoint;
  response["timestamp"]        = ctrl.timestamp;
  response["t_receive"]        = t_receive;
  response["pdr_stats"]["packets_sent"]     = pdr_stats[idx].packets_sent;
  response["pdr_stats"]["packets_received"] = pdr_stats[idx].packets_received;
  response["pdr_stats"]["pdr_percentage"]   = pdr_stats[idx].pdr_percentage;
  response["pdr_stats"]["status"]           = pdr_stats[idx].status;

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
    server.send(405, "application/json", "{\"error\":\"Method not allowed\"}"); return;
  }
  String body = server.arg("plain");
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  int node_id = doc["node_id"];
  if (node_id < 1 || node_id > NUM_NODES) {
    server.send(400, "application/json", "{\"error\":\"Invalid node_id\"}"); return;
  }
  int idx = node_id - 1;
  if (doc.containsKey("manual_mode"))      controls[idx].manual_mode      = doc["manual_mode"];
  if (doc.containsKey("actuator_command")) controls[idx].actuator_command = doc["actuator_command"];
  if (doc.containsKey("setpoint"))         controls[idx].setpoint         = doc["setpoint"];
  controls[idx].timestamp = millis();

  if (firebaseReady) {
    String path = "/nodes/node_" + String(node_id) + "/control";
    FirebaseJson json;
    json.set("manual_mode",      controls[idx].manual_mode);
    json.set("actuator_command", controls[idx].actuator_command);
    json.set("setpoint",         controls[idx].setpoint);
    json.set("timestamp",        controls[idx].timestamp);
    // Pakai fbdo_ctrl — fbdo mungkin sedang dipakai handleNodeSync
    bool fbOk = Firebase.RTDB.setJSON(&fbdo_ctrl, path.c_str(), &json);
    Serial.printf("[CONTROL] Firebase write: %s\n", fbOk ? "✅" : "❌");
  }

  DynamicJsonDocument response(256);
  response["status"]   = "success";
  response["node_id"]  = node_id;
  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);

  Serial.printf("[CONTROL] Node %d: Mode=%s, Command=%s, Setpoint=%.1f\n",
                node_id,
                controls[idx].manual_mode ? "MANUAL" : "AUTO",
                controls[idx].actuator_command ? "ON" : "OFF",
                controls[idx].setpoint);
}

void handleGetStatus() {
  setCORSHeaders();
  DynamicJsonDocument doc(3072);

  JsonObject network = doc.createNestedObject("network");
  network["ap_ssid"]         = ap_ssid;
  network["ap_ip"]           = ap_local_ip.toString();
  network["ap_clients"]      = WiFi.softAPgetStationNum();
  network["sta_ssid"]        = internetConnected ? sta_ssid : "";
  network["sta_ip"]          = WiFi.localIP().toString();
  network["internet"]        = internetConnected ? "connected" : "disconnected";
  network["firebase_ready"]  = firebaseReady;
  network["firebase_stream"] = streamReady;

  JsonArray nodes = doc.createNestedArray("nodes");
  unsigned long now = millis();
  for (int i = 0; i < NUM_NODES; i++) {
    JsonObject node = nodes.createNestedObject();
    node["node_id"]       = i + 1;
    node["online"]        = (now - node_status[i].last_seen < 30000);
    node["last_seen_sec"] = (now - node_status[i].last_seen) / 1000;
    node["node_type"]     = node_status[i].node_type;
    if (node_status[i].node_type == 1) {
      node["temperature"]    = node_status[i].temperature;
      node["humidity"]       = node_status[i].humidity;
      node["exhaust_status"] = node_status[i].exhaust_status;
    } else if (node_status[i].node_type == 2) {
      node["current_ppm"] = node_status[i].current_ppm;
      node["current_ph"]  = node_status[i].current_ph;
      node["pump_active"] = node_status[i].pump_active;
    }
    node["mode"]             = node_status[i].mode;
    node["manual_mode"]      = controls[i].manual_mode;
    node["actuator_command"] = controls[i].actuator_command;
    node["setpoint"]         = controls[i].setpoint;
    JsonObject pdr = node.createNestedObject("pdr");
    pdr["packets_sent"]     = pdr_stats[i].packets_sent;
    pdr["packets_received"] = pdr_stats[i].packets_received;
    pdr["packets_lost"]     = pdr_stats[i].packets_lost;
    pdr["pdr_percentage"]   = pdr_stats[i].pdr_percentage;
    pdr["status"]           = pdr_stats[i].status;
  }
  doc["uptime"] = millis() / 1000;
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleResetPDR() {
  setCORSHeaders();
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method not allowed\"}"); return;
  }
  String body = server.arg("plain");
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  int node_id = doc.containsKey("node_id") ? doc["node_id"].as<int>() : 0;
  if (node_id == 0) {
    for (int i = 0; i < NUM_NODES; i++)
      pdr_stats[i] = {0, 0, 0, 0.0f, "No Data", millis()};
    Serial.println("[PDR] ✓ Reset all nodes");
  } else if (node_id >= 1 && node_id <= NUM_NODES) {
    pdr_stats[node_id - 1] = {0, 0, 0, 0.0f, "No Data", millis()};
    Serial.printf("[PDR] ✓ Reset node %d\n", node_id);
  } else {
    server.send(400, "application/json", "{\"error\":\"Invalid node_id\"}"); return;
  }
  server.send(200, "application/json", "{\"status\":\"success\"}");
}

// ==================== WIFI DUAL MODE ====================
void setupWiFi() {
  Serial.println("\n[WIFI] Memulai mode AP+STA...");
  WiFi.mode(WIFI_AP_STA);

  // Setup AP dengan IP static
  WiFi.softAPConfig(ap_local_ip, ap_gateway, ap_subnet);
  WiFi.softAP(ap_ssid, ap_password);

  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║     ACCESS POINT GATEWAY AKTIF         ║");
  Serial.println("╠════════════════════════════════════════╣");
  Serial.printf( "║  SSID     : %-27s║\n", ap_ssid);
  Serial.printf( "║  Password : %-27s║\n", ap_password);
  Serial.printf( "║  IP (AP)  : %-27s║\n", ap_local_ip.toString().c_str());
  Serial.println("╠════════════════════════════════════════╣");
  Serial.println("║  ⚠ SET DI SEMUA NODE SENSOR:           ║");
  Serial.printf( "║  ssid       = \"%s\"        ║\n", ap_ssid);
  Serial.printf( "║  password   = \"%s\"           ║\n", ap_password);
  Serial.println("║  gatewayIP  = \"192.168.4.1\"           ║");
  Serial.println("╚════════════════════════════════════════╝\n");

  // Connect ke router untuk internet
  Serial.printf("[WIFI] Menghubungkan ke router: %s\n", sta_ssid);
  WiFi.begin(sta_ssid, sta_password);
  WiFi.setAutoReconnect(true);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); Serial.print("."); attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    internetConnected = true;
    Serial.println("[WIFI] ✅ Internet (router) terhubung!");
    Serial.printf( "[WIFI] STA IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    internetConnected = false;
    Serial.println("[WIFI] ⚠ Internet tidak tersedia - mode lokal saja");
    Serial.println("[WIFI] Node sensor tetap bisa komunikasi via AP");
  }
}

void checkInternetConnection() {
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected == internetConnected) return;  // tidak ada perubahan

  internetConnected = connected;
  if (connected) {
    Serial.println("[WIFI] ✅ Internet kembali terhubung");
    if (!firebaseReady) {
      Firebase.begin(&config, &auth);
      Firebase.reconnectWiFi(true);
      // Tunggu auth selesai
      int fbW = 0;
      Serial.print("[FIREBASE] Reconnecting");
      while (!Firebase.ready() && fbW < 20) {
        Serial.print(".");
        delay(500);
        fbW++;
      }
      Serial.println();
      if (Firebase.ready()) {
        firebaseReady = true;
        Serial.println("[FIREBASE] ✅ Reconnected!");
        syncControlFromFirebase();
        setupFirebaseStream();
      }
    }
  } else {
    Serial.println("[WIFI] ⚠ Internet terputus - mode lokal aktif");
    firebaseReady = false;
    streamReady   = false;
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║   ESP32 GREENHOUSE GATEWAY             ║");
  Serial.println("║   Mode: AP (lokal) + STA (internet)   ║");
  Serial.println("║   IP Node Sensor SELALU: 192.168.4.1  ║");
  Serial.println("║   Fuzzy Log: Nutrient Node (3/4) Only ║");
  Serial.println("╚════════════════════════════════════════╝\n");

  for (int i = 0; i < NUM_NODES; i++) {
    controls[i]    = {false, false, 800.0f, 0};
    node_status[i] = {0, false, 0, 0.0f, 0.0f, 0, 0.0f, 0.0f, false, "AUTO"};
    pdr_stats[i]   = {0, 0, 0, 0.0f, "No Data", 0};
  }

  setupWiFi();

  // Firebase hanya jika internet tersedia
  if (internetConnected) {
    config.database_url               = FIREBASE_HOST;
    config.signer.tokens.legacy_token = FIREBASE_AUTH;
    // WAJIB: token callback agar Firebase.ready() bisa jadi true
    config.token_status_callback      = tokenStatusCallback;
    // Timeout lebih longgar untuk SSL handshake ke server Firebase
    config.timeout.serverResponse     = 30 * 1000;
    config.timeout.socketConnection   = 30 * 1000;
    config.timeout.sslHandshake       = 30 * 1000;
    config.cert.data                  = NULL;
    config.cert.file                  = "";
    config.cert.file_storage          = mem_storage_type_flash;
    // SSL buffer: RX 16384 wajib untuk Firebase SSL handshake
    fbdo.setBSSLBufferSize(16384, 1024);
    fbdo_stream.setBSSLBufferSize(16384, 1024);
    fbdo_ctrl.setBSSLBufferSize(4096, 1024);  // ctrl hanya butuh kecil (write kecil)
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    // Tunggu Firebase ready — auth legacy token butuh beberapa detik
    Serial.print("[FIREBASE] Autentikasi");
    int fbWait = 0;
    while (!Firebase.ready() && fbWait < 30) {
      Serial.print(".");
      delay(500);
      fbWait++;
    }
    Serial.println();

    if (Firebase.ready()) {
      firebaseReady = true;
      Serial.println("[FIREBASE] ✅ Connected!");
      syncControlFromFirebase();
      setupFirebaseStream();
    } else {
      Serial.println("[FIREBASE] ❌ Gagal — cek internet & kredensial");
    }
  }

  server.on("/ping",          HTTP_GET,     handlePing);
  server.on("/ping",          HTTP_OPTIONS, handleOptions);
  server.on("/api/node/sync", HTTP_POST,    handleNodeSync);
  server.on("/api/node/sync", HTTP_OPTIONS, handleOptions);
  server.on("/api/control",   HTTP_POST,    handleControl);
  server.on("/api/control",   HTTP_OPTIONS, handleOptions);
  server.on("/api/status",    HTTP_GET,     handleGetStatus);
  server.on("/api/status",    HTTP_OPTIONS, handleOptions);
  server.on("/api/pdr/reset", HTTP_POST,    handleResetPDR);
  server.on("/api/pdr/reset", HTTP_OPTIONS, handleOptions);
  server.onNotFound([]() {
    setCORSHeaders();
    server.send(404, "application/json", "{\"error\":\"Not found\"}");
  });
  server.begin();

  Serial.println("\n✅ SERVER AKTIF!");
  Serial.println("Firebase Paths:");
  Serial.println("  /fuzzy_log/node_X  <- Fuzzy detail NUTRIENT node saja");
  Serial.println("  /testing/latency/  <- Latency log semua node");
  Serial.println("  /testing/pdr/      <- PDR log semua node");
  Serial.println("  /nodes/node_X/     <- Sensor data semua node");
  Serial.printf( "Gateway lokal  : http://192.168.4.1:8080\n");
  Serial.printf( "Gateway router : http://%s:8080\n\n", WiFi.localIP().toString().c_str());
}

// ==================== LOOP ====================

// ── POLLING BACKUP: cek /control tiap 3 detik ──
// Memastikan update setpoint/mode dari dashboard selalu sampai ke gateway
// meskipun stream sedang lag atau miss event.
// Hanya baca jika ada perubahan (bandingkan timestamp).
void pollControlFromFirebase() {
  if (!firebaseReady) return;
  for (int i = 1; i <= NUM_NODES; i++) {
    String path = "/nodes/node_" + String(i) + "/control/timestamp";
    // Cek timestamp dulu — lebih ringan dari GET seluruh object
    if (Firebase.RTDB.getInt(&fbdo_ctrl, path.c_str())) {
      unsigned long fbTs = (unsigned long)fbdo_ctrl.intData();
      // Jika timestamp Firebase lebih baru dari yang kita simpan → ada update baru
      if (fbTs > controls[i - 1].timestamp) {
        Serial.printf("[POLL] Node %d: timestamp baru (%lu > %lu), ambil control...\n",
                      i, fbTs, controls[i - 1].timestamp);
        // Ambil seluruh control object
        String ctrlPath = "/nodes/node_" + String(i) + "/control";
        if (Firebase.RTDB.getJSON(&fbdo_ctrl, ctrlPath.c_str())) {
          if (fbdo_ctrl.dataType() == "json") {
            FirebaseJson     json = fbdo_ctrl.jsonObject();
            FirebaseJsonData jd;
            NodeControl& ctrl = controls[i - 1];
            bool changed = false;
            if (json.get(jd, "manual_mode") && ctrl.manual_mode != jd.boolValue) {
              ctrl.manual_mode = jd.boolValue; changed = true;
            }
            if (json.get(jd, "actuator_command") && ctrl.actuator_command != jd.boolValue) {
              ctrl.actuator_command = jd.boolValue; changed = true;
            }
            if (json.get(jd, "setpoint") && abs(ctrl.setpoint - jd.floatValue) > 0.1f) {
              ctrl.setpoint = jd.floatValue; changed = true;
            }
            if (json.get(jd, "timestamp")) ctrl.timestamp = (unsigned long)jd.intValue;
            if (changed) {
              Serial.printf("[POLL] ✅ Node %d updated: Mode=%s, Cmd=%s, SP=%.1f\n",
                            i,
                            ctrl.manual_mode      ? "MANUAL" : "AUTO",
                            ctrl.actuator_command ? "ON"     : "OFF",
                            ctrl.setpoint);
            }
          }
        }
        delay(50); // jeda antar node agar tidak spam Firebase
      }
    }
  }
}

void loop() {
  // readStream HARUS dipanggil sesering mungkin agar update control dari dashboard
  // segera diterima. Letakkan SEBELUM handleClient agar tidak diblock HTTP handler.
  if (firebaseReady && streamReady) Firebase.RTDB.readStream(&fbdo_stream);
  server.handleClient();

  // Cek koneksi internet tiap 10 detik
  if (millis() - lastInternetCheck > 10000) {
    lastInternetCheck = millis();
    checkInternetConnection();
  }

  // Reconnect stream kalau mati, cek tiap 5 detik (dipercepat dari 30s)
  if (firebaseReady && !streamReady && millis() - lastStreamCheck > 5000) {
    lastStreamCheck = millis();
    Serial.println("[STREAM] Reconnecting...");
    setupFirebaseStream();
  }

  // ── POLLING BACKUP: GET /control tiap 3 detik ──
  // Ini memastikan update dari dashboard selalu diterima
  // meskipun stream sedang lag, timeout, atau ada push sensor yang memenuhi antrian
  if (firebaseReady && millis() - lastControlPoll > 3000) {
    lastControlPoll = millis();
    pollControlFromFirebase();
  }

  delay(10);
}
