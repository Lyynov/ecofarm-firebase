#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// Konfigurasi Jaringan
const char* ssid = "RAFALEON";
const char* password = "11112222";

// Konfigurasi Firebase
#define FIREBASE_HOST "ecofarmiot-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "yIQkkdVgJesYWYjkmR7ssygvpD6sCrb1gGH47ZYJ"

// Firebase Objects
FirebaseData fbdo;
FirebaseData fbdo_stream; // Untuk real-time listening
FirebaseAuth auth;
FirebaseConfig config;

// Server
WebServer server(8080);

// Kontrol Commands (sinkronisasi dengan Firebase)
struct ControlCommand {
  bool manual_mode;
  bool relay_command;
  unsigned long timestamp;
};
ControlCommand controls[4] = {{false, false, 0}, {false, false, 0}, {false, false, 0}, {false, false, 0}};

// Status Firebase
bool firebaseReady = false;
bool streamReady = false;

// Fungsi callback untuk Firebase Stream
void streamCallback(FirebaseStream data) {
  Serial.println("========================================");
  Serial.println("[FIREBASE STREAM] Data changed!");
  Serial.printf("Path: %s\n", data.dataPath().c_str());
  Serial.printf("Type: %s\n", data.dataType().c_str());
  
  // Parse path untuk mendapatkan node_id
  String path = data.dataPath();
  if (path.startsWith("/nodes/node_")) {
    int nodeStart = path.indexOf("node_") + 5;
    int nodeEnd = path.indexOf("/", nodeStart);
    if (nodeEnd == -1) nodeEnd = path.length();
    
    String nodeStr = path.substring(nodeStart, nodeEnd);
    int nodeId = nodeStr.toInt();
    
    if (nodeId >= 1 && nodeId <= 4 && path.indexOf("/control") != -1) {
      Serial.printf("Control update for Node %d\n", nodeId);
      
      // Parse JSON data
      FirebaseJson json = data.jsonObject();
      FirebaseJsonData jsonData;
      
      bool manual_mode = false;
      bool relay_command = false;
      
      if (json.get(jsonData, "manual_mode")) {
        manual_mode = jsonData.boolValue;
      }
      if (json.get(jsonData, "relay_command")) {
        relay_command = jsonData.boolValue;
      }
      
      // Update local cache
      controls[nodeId - 1].manual_mode = manual_mode;
      controls[nodeId - 1].relay_command = relay_command;
      controls[nodeId - 1].timestamp = millis();
      
      Serial.printf("Updated cache for Node %d: Manual=%s, Relay=%s\n", 
                    nodeId, manual_mode ? "ON" : "OFF", relay_command ? "ON" : "OFF");
    }
  }
  Serial.println("========================================");
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("[FIREBASE STREAM] Timeout, reconnecting...");
    setupFirebaseStream();
  }
}

void setupFirebaseStream() {
  if (!firebaseReady) return;
  
  Serial.println("[FIREBASE] Setting up real-time stream...");
  
  // Setup stream untuk semua control nodes
  if (Firebase.RTDB.beginStream(&fbdo_stream, "/nodes")) {
    Firebase.RTDB.setStreamCallback(&fbdo_stream, streamCallback, streamTimeoutCallback);
    streamReady = true;
    Serial.println("[FIREBASE] ✅ Stream setup successful!");
  } else {
    Serial.println("[FIREBASE] ❌ Stream setup failed: " + fbdo_stream.errorReason());
    streamReady = false;
  }
}

// Fungsi untuk sync control data dari Firebase ke local cache
void syncControlFromFirebase() {
  if (!firebaseReady) return;
  
  Serial.println("[FIREBASE] Syncing control data from Firebase...");
  
  for (int i = 1; i <= 4; i++) {
    String path = "/nodes/node_" + String(i) + "/control";
    
    if (Firebase.RTDB.getJSON(&fbdo, path.c_str())) {
      if (fbdo.dataType() == "json") {
        FirebaseJson json = fbdo.jsonObject();
        FirebaseJsonData jsonData;
        
        bool manual_mode = false;
        bool relay_command = false;
        
        if (json.get(jsonData, "manual_mode")) {
          manual_mode = jsonData.boolValue;
        }
        if (json.get(jsonData, "relay_command")) {
          relay_command = jsonData.boolValue;
        }
        
        controls[i - 1].manual_mode = manual_mode;
        controls[i - 1].relay_command = relay_command;
        controls[i - 1].timestamp = millis();
        
        Serial.printf("Synced Node %d: Manual=%s, Relay=%s\n", 
                      i, manual_mode ? "ON" : "OFF", relay_command ? "ON" : "OFF");
      }
    }
  }
}

// Fungsi untuk mengirim data ke Firebase menggunakan Firebase ESP32
bool sendSensorDataToFirebase(int node_id, const DynamicJsonDocument& sensorDoc) {
  if (!firebaseReady) {
    Serial.println("[FIREBASE] Firebase belum ready");
    return false;
  }
  
  String path = "/nodes/node_" + String(node_id) + "/data";
  
  // Convert DynamicJsonDocument to JSON object
  FirebaseJson json;
  json.setJsonData(sensorDoc.as<String>());
  
  if (Firebase.RTDB.pushJSON(&fbdo, path.c_str(), &json)) {
    Serial.println("[FIREBASE] Data sensor berhasil dikirim");
    return true;
  } else {
    Serial.println("[FIREBASE] Gagal mengirim data sensor: " + fbdo.errorReason());
    return false;
  }
}

// Fungsi untuk mengirim control command ke Firebase
bool sendControlDataToFirebase(int node_id, const DynamicJsonDocument& controlDoc) {
  if (!firebaseReady) {
    Serial.println("[FIREBASE] Firebase belum ready");
    return false;
  }
  
  String path = "/nodes/node_" + String(node_id) + "/control";
  
  // Convert DynamicJsonDocument to JSON object
  FirebaseJson json;
  json.setJsonData(controlDoc.as<String>());
  
  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
    Serial.println("[FIREBASE] Control data berhasil dikirim");
    return true;
  } else {
    Serial.println("[FIREBASE] Gagal mengirim control data: " + fbdo.errorReason());
    return false;
  }
}

// Handler untuk CORS
void setCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

// Handler untuk OPTIONS request (CORS preflight)
void handleOptions() {
  setCORSHeaders();
  server.send(200, "text/plain", "");
}

// Handler untuk POST /sensor-data
void handleSensorData() {
  setCORSHeaders();
  
  Serial.println("========================================");
  Serial.println("[INCOMING] POST /sensor-data");
  
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method not allowed\"}");
    return;
  }
  
  String body = server.arg("plain");
  
  if (body.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"Tidak ada body\"}");
    return;
  }
  
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    server.send(400, "application/json", "{\"error\":\"JSON tidak valid\"}");
    return;
  }
  
  int node_id = doc["node_id"];
  String node_type = doc["node_type"];
  
  if (node_id >= 1 && node_id <= 4 && (node_type == "exhaust" || node_type == "nutrient")) {
    // Tambahkan server timestamp
    doc["server_timestamp"] = millis();
    
    // Kirim data ke Firebase
    bool firebaseSuccess = sendSensorDataToFirebase(node_id, doc);
    
    if (firebaseSuccess) {
      server.send(200, "application/json", "{\"status\":\"sukses\",\"firebase\":\"berhasil\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"sukses\",\"firebase\":\"gagal\"}");
    }
  } else {
    server.send(400, "application/json", "{\"error\":\"node_id atau node_type tidak valid\"}");
  }
}

// Handler untuk POST /api/control
void handleControl() {
  setCORSHeaders();
  
  Serial.println("========================================");
  Serial.println("[INCOMING] POST /api/control");
  
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method not allowed\"}");
    return;
  }
  
  String body = server.arg("plain");
  
  if (body.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"Tidak ada body\"}");
    return;
  }
  
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    server.send(400, "application/json", "{\"error\":\"JSON tidak valid\"}");
    return;
  }
  
  int node_id = doc["node_id"];
  bool manual_mode = doc["manual_mode"];
  bool relay_command = doc["relay_command"];
  
  Serial.printf("Control untuk Node %d: Manual=%s, Relay=%s\n", 
                node_id, manual_mode ? "ON" : "OFF", relay_command ? "ON" : "OFF");
  
  if (node_id >= 1 && node_id <= 4) {
    // Update local cache dulu
    controls[node_id - 1].manual_mode = manual_mode;
    controls[node_id - 1].relay_command = relay_command;
    controls[node_id - 1].timestamp = millis();
    
    // Tambahkan timestamp untuk Firebase
    doc["timestamp"] = millis();
    
    // Kirim control command ke Firebase
    bool firebaseSuccess = sendControlDataToFirebase(node_id, doc);
    
    if (firebaseSuccess) {
      server.send(200, "application/json", "{\"status\":\"sukses\",\"firebase\":\"berhasil\"}");
      Serial.println("[SUCCESS] Control berhasil dikirim ke Firebase");
    } else {
      server.send(200, "application/json", "{\"status\":\"sukses\",\"firebase\":\"gagal\"}");
      Serial.println("[WARNING] Control gagal dikirim ke Firebase");
    }
  } else {
    server.send(400, "application/json", "{\"error\":\"node_id tidak valid\"}");
  }
  Serial.println("========================================");
}

// Handler untuk GET /api/control/{nodeId}
void handleGetControl() {
  setCORSHeaders();
  
  String uri = server.uri();
  int lastSlash = uri.lastIndexOf('/');
  String nodeIdStr = uri.substring(lastSlash + 1);
  int node_id = nodeIdStr.toInt();
  
  Serial.printf("[REQUEST] GET /api/control/%d\n", node_id);
  
  if (node_id < 1 || node_id > 4) {
    server.send(400, "application/json", "{\"error\":\"node_id tidak valid\"}");
    return;
  }
  
  // Return control command dari local cache (yang sudah di-sync dengan Firebase)
  DynamicJsonDocument doc(512);
  doc["manual_mode"] = controls[node_id - 1].manual_mode;
  doc["relay_command"] = controls[node_id - 1].relay_command;
  doc["timestamp"] = controls[node_id - 1].timestamp;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
  
  Serial.printf("[RESPONSE] Node %d: Manual=%s, Relay=%s\n", 
                node_id, 
                controls[node_id - 1].manual_mode ? "ON" : "OFF", 
                controls[node_id - 1].relay_command ? "ON" : "OFF");
}

// Handler untuk GET /api/data
void handleGetData() {
  setCORSHeaders();
  String firebaseURL = "https://" + String(FIREBASE_HOST) + "/nodes.json";
  server.send(200, "application/json", "{\"message\":\"Data tersimpan di Firebase\",\"firebase_url\":\"" + firebaseURL + "\"}");
}

// Handler untuk GET /api/history/{nodeId}
void handleGetHistory() {
  setCORSHeaders();
  
  String uri = server.uri();
  int lastSlash = uri.lastIndexOf('/');
  String nodeIdStr = uri.substring(lastSlash + 1);
  int node_id = nodeIdStr.toInt();
  
  if (node_id < 1 || node_id > 4) {
    server.send(400, "application/json", "{\"error\":\"node_id tidak valid\"}");
    return;
  }
  
  String firebaseDataURL = "https://" + String(FIREBASE_HOST) + "/nodes/node_" + String(node_id) + "/data.json";
  server.send(200, "application/json", "{\"message\":\"Data history tersimpan di Firebase\",\"firebase_url\":\"" + firebaseDataURL + "\"}");
}

void setup() {
  Serial.begin(115200);
  Serial.println("========================================");
  Serial.println("ESP32 Firebase Gateway with Real-time Sync");
  Serial.println("========================================");

  // Koneksi WiFi
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Konfigurasi Firebase
  Serial.println("========================================");
  Serial.println("Configuring Firebase...");
  config.database_url = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  // Initialize Firebase
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Test Firebase connection
  Serial.println("Testing Firebase connection...");
  if (Firebase.ready()) {
    firebaseReady = true;
    Serial.println("[FIREBASE] ✅ Connection successful!");
    
    // Sync existing control data from Firebase
    syncControlFromFirebase();
    
    // Setup real-time stream
    setupFirebaseStream();
    
    // Test write
    FirebaseJson testJson;
    testJson.set("status", "online");
    testJson.set("timestamp", millis());
    
    if (Firebase.RTDB.setJSON(&fbdo, "/server_status", &testJson)) {
      Serial.println("[FIREBASE] ✅ Test write successful!");
    }
  } else {
    Serial.println("[FIREBASE] ❌ Connection failed!");
    firebaseReady = false;
  }

  // Setup server endpoints
  server.on("/sensor-data", HTTP_POST, handleSensorData);
  server.on("/sensor-data", HTTP_OPTIONS, handleOptions);
  server.on("/api/data", HTTP_GET, handleGetData);
  server.on("/api/data", HTTP_OPTIONS, handleOptions);
  server.on("/api/control", HTTP_POST, handleControl);
  server.on("/api/control", HTTP_OPTIONS, handleOptions);
  
  // Dynamic routes
  server.onNotFound([]() {
    String uri = server.uri();
    
    if (uri.startsWith("/api/history/")) {
      if (server.method() == HTTP_GET) {
        handleGetHistory();
        return;
      } else if (server.method() == HTTP_OPTIONS) {
        handleOptions();
        return;
      }
    }
    
    if (uri.startsWith("/api/control/")) {
      if (server.method() == HTTP_GET) {
        handleGetControl();
        return;
      } else if (server.method() == HTTP_OPTIONS) {
        handleOptions();
        return;
      }
    }
    
    setCORSHeaders();
    server.send(404, "application/json", "{\"error\":\"Endpoint tidak ditemukan\"}");
  });

  server.begin();
  Serial.println("========================================");
  Serial.println("HTTP Server Started on Port 8080");
  Serial.println("Firebase Real-time Sync: " + String(streamReady ? "✅ Active" : "❌ Inactive"));
  Serial.println("========================================");
}

void loop() {
  server.handleClient();
  
  // Maintain Firebase connection dan stream
  if (firebaseReady && WiFi.status() == WL_CONNECTED) {
    // Ini akan otomatis handle stream callback
    Firebase.ready();
    
    // Jika stream terputus, coba reconnect
    if (streamReady && !fbdo_stream.isStream()) {
      Serial.println("[FIREBASE] Stream disconnected, reconnecting...");
      setupFirebaseStream();
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    // Reconnect WiFi jika terputus
    Serial.println("[WIFI] Reconnecting...");
    WiFi.reconnect();
  }
  
  delay(10); // Small delay to prevent watchdog reset
}
