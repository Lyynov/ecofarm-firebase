#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// Network Configuration
const char* ssid = "RAFALEON";
const char* password = "11112222";
const char* server_ip = "192.168.43.54"; // ESP32 Gateway IP
const int server_port = 8080;

// Hardware Configuration
#define DHT_PIN 4
#define RELAY_PIN 2
#define DHT_TYPE DHT22
#define NODE_ID 2 // Ubah untuk setiap node (1-4)
#define RELAY_ACTIVE_LOW true

// Sensor & Control
DHT dht(DHT_PIN, DHT_TYPE);
bool relay_state = false;
bool manual_mode = false;
bool last_manual_mode = false; // Untuk deteksi perubahan mode
float temp_threshold_on = 30.0; // Celsius (relay ON)
float temp_threshold_off = 28.0; // Celsius (relay OFF)

// Current sensor readings
float current_temperature = NAN;
float current_humidity = NAN;

// Timing - Polling lebih cepat untuk kontrol
unsigned long last_sensor_read = 0;
unsigned long last_data_send = 0;
unsigned long last_control_check = 0;
const unsigned long SENSOR_INTERVAL = 2000;      // Baca sensor setiap 2 detik
const unsigned long SEND_INTERVAL = 10000;       // Kirim data setiap 10 detik
const unsigned long CONTROL_INTERVAL = 2000;     // Cek kontrol setiap 2 detik (lebih cepat!)
const int MAX_RETRIES = 3;

// Control state tracking
unsigned long last_control_timestamp = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("========================================");
  Serial.println("ESP32 Node - Responsive Control Mode");
  Serial.printf("Node ID: %d\n", NODE_ID);
  Serial.println("========================================");
  
  // Initialize pins
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false); // Initialize relay to OFF
  
  // Initialize DHT
  dht.begin();
  
  // Connect WiFi
  connectWiFi();
  
  // Initial control check
  checkControlCommands();
}

void loop() {
  unsigned long current_time = millis();
  
  // Ensure WiFi is connected
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  
  // Read sensors
  if (current_time - last_sensor_read >= SENSOR_INTERVAL) {
    readSensors();
    last_sensor_read = current_time;
  }
  
  // Send data to server
  if (current_time - last_data_send >= SEND_INTERVAL) {
    sendSensorData();
    last_data_send = current_time;
  }
  
  // Check control commands - POLLING LEBIH CEPAT!
  if (current_time - last_control_check >= CONTROL_INTERVAL) {
    checkControlCommands();
    last_control_check = current_time;
  }
  
  // Auto control (if not in manual mode)
  if (!manual_mode) {
    autoControlExhaust();
  }
  
  delay(100); // Delay lebih kecil untuk responsivitas
}

void connectWiFi() {
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed. Retrying...");
  }
}

void readSensors() {
  current_temperature = dht.readTemperature();
  current_humidity = dht.readHumidity();
  int retries = 3;
  
  while ((isnan(current_temperature) || isnan(current_humidity)) && retries > 0) {
    delay(500);
    current_temperature = dht.readTemperature();
    current_humidity = dht.readHumidity();
    retries--;
  }
  
  if (!isnan(current_temperature) && !isnan(current_humidity)) {
    // Log sensor readings setiap 30 detik
    static unsigned long last_sensor_log = 0;
    if (millis() - last_sensor_log > 30000) {
      Serial.printf("Sensor -> Temp: %.2f°C, Humidity: %.2f%%\n", current_temperature, current_humidity);
      last_sensor_log = millis();
    }
  } else {
    Serial.println("Error: Unable to read DHT22 sensor");
    current_temperature = NAN;
    current_humidity = NAN;
  }
}

void sendSensorData() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(String("http://") + server_ip + ":" + server_port + "/sensor-data");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000); // 5 detik timeout
    
    // Baca sensor fresh untuk pengiriman data
    float temp_to_send = dht.readTemperature();
    float humid_to_send = dht.readHumidity();
    
    if (isnan(temp_to_send)) temp_to_send = current_temperature;
    if (isnan(humid_to_send)) humid_to_send = current_humidity;
    
    DynamicJsonDocument doc(1024);
    doc["node_id"] = NODE_ID;
    doc["node_type"] = "exhaust";
    doc["temperature"] = temp_to_send;
    doc["humidity"] = humid_to_send;
    doc["relay_state"] = relay_state;
    doc["manual_mode"] = manual_mode;
    doc["timestamp"] = millis();
    
    String json_string;
    serializeJson(doc, json_string);
    
    Serial.println("========================================");
    Serial.printf("[SEND] Data to Gateway (Node %d)\n", NODE_ID);
    Serial.printf("Temp: %.2f°C, Humidity: %.2f%%\n", temp_to_send, humid_to_send);
    Serial.printf("Relay: %s, Manual: %s\n", relay_state ? "ON" : "OFF", manual_mode ? "YES" : "NO");
    
    int response_code = http.POST(json_string);
    if (response_code > 0) {
      Serial.printf("[SUCCESS] Data sent: HTTP %d\n", response_code);
    } else {
      Serial.printf("[ERROR] Failed to send data: %d\n", response_code);
    }
    Serial.println("========================================");
    
    http.end();
  }
}

void checkControlCommands() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(String("http://") + server_ip + ":" + server_port + "/api/control/" + NODE_ID);
    http.setTimeout(3000); // Timeout lebih cepat untuk kontrol
    
    int response_code = http.GET();
    
    if (response_code == 200) {
      String payload = http.getString();
      
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);
      
      if (!error) {
        if (doc.containsKey("manual_mode") && doc.containsKey("relay_command")) {
          bool new_manual_mode = doc["manual_mode"];
          bool new_relay_command = doc["relay_command"];
          unsigned long server_timestamp = doc["timestamp"];
          
          // Cek apakah ini adalah command baru
          bool is_new_command = (server_timestamp > last_control_timestamp);
          
          if (is_new_command) {
            Serial.println("========================================");
            Serial.printf("[CONTROL] NEW COMMAND from Gateway (Node %d)\n", NODE_ID);
            Serial.printf("Manual: %s, Relay: %s\n", 
                         new_manual_mode ? "ON" : "OFF", 
                         new_relay_command ? "ON" : "OFF");
            
            // Update manual mode
            if (new_manual_mode != manual_mode) {
              manual_mode = new_manual_mode;
              Serial.printf("[UPDATE] Manual mode: %s -> %s\n", 
                           last_manual_mode ? "ON" : "OFF", 
                           manual_mode ? "ON" : "OFF");
              last_manual_mode = manual_mode;
            }
            
            // Apply relay command in manual mode
            if (manual_mode) {
              if (new_relay_command != relay_state) {
                setRelay(new_relay_command);
                Serial.printf("[UPDATE] Manual relay: %s -> %s\n", 
                             !new_relay_command ? "ON" : "OFF", 
                             new_relay_command ? "ON" : "OFF");
              }
            } else {
              Serial.println("[INFO] Switched to AUTO mode");
            }
            
            last_control_timestamp = server_timestamp;
            Serial.println("========================================");
          } else {
            // Log status setiap 30 detik jika tidak ada perubahan
            static unsigned long last_status_log = 0;
            if (millis() - last_status_log > 30000) {
              Serial.printf("[STATUS] Mode: %s, Relay: %s\n", 
                           manual_mode ? "MANUAL" : "AUTO", 
                           relay_state ? "ON" : "OFF");
              last_status_log = millis();
            }
          }
          
          // Update state
          manual_mode = new_manual_mode;
          if (manual_mode) {
            // Pastikan relay state sesuai dengan command di manual mode
            if (new_relay_command != relay_state) {
              setRelay(new_relay_command);
            }
          }
        }
      } else {
        Serial.println("[ERROR] Failed to parse control payload");
        // Jangan langsung switch ke auto mode jika parsing error
      }
    } else if (response_code == 404) {
      // No control commands available - stay in current mode
      static unsigned long last_404_log = 0;
      if (millis() - last_404_log > 60000) { // Log setiap 1 menit
        Serial.println("[INFO] No control commands available");
        last_404_log = millis();
      }
    } else {
      // Connection error - log tapi jangan ubah mode
      static unsigned long last_error_log = 0;
      if (millis() - last_error_log > 30000) {
        Serial.printf("[WARNING] Control check failed (HTTP %d)\n", response_code);
        last_error_log
