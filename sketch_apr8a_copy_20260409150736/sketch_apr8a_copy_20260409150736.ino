#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#include <ArduinoJson.h>
#include <time.h>     
#include <sys/time.h> 
#include <DHT.h> 
#include <Wire.h>                
#include <Adafruit_GFX.h>        
#include <Adafruit_SSD1306.h>    

// --- Configuration ---
const char* ssid = "OPPOA16";        
const char* password = "12345678Kim"; 
const char* serverName = "https://firealarm-with-ai-production.up.railway.app/api/readings";

// --- NEW: Override Endpoint ---
const char* alarmServerName = "https://firealarm-with-ai-production.up.railway.app/api/alarm";
const char* device_id = "iot-device-001";

// --- SENSOR & OUTPUT PIN DEFINITIONS ---
#define DHTPIN 4          
#define DHTTYPE DHT22     
#define FLAME_PIN 27      // Connected to ADC-capable pin for Analog Read
#define MQ2_PIN 32        
#define MQ7_PIN 33        
#define MQ135_PIN 34      
#define BUZZER_PIN 25     

// --- OLED CONFIGURATION ---
#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 
#define OLED_RESET    -1 
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

DHT dht(DHTPIN, DHTTYPE);

const char* ntpServer = "pool.ntp.org";

String getTimestamp() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  
  // Use getLocalTime which strictly obeys the timezone configured in setup()
  struct tm timeinfo;
  getLocalTime(&timeinfo); 
  
  char timeStringBuff[50];
  sprintf(timeStringBuff, "%04d-%02d-%02dT%02d:%02d:%02d.%03d+08:00",
          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, (int)(tv.tv_usec / 1000));
  return String(timeStringBuff);
}

void setup() {
  Serial.begin(115200);

  dht.begin();
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); 

  // Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("OLED failed. Check wiring or I2C address."));
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 10);
    display.println("System Booting...");
    display.println("Connecting WiFi...");
    display.display();
  }

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  // Update OLED when connected
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("WiFi Connected!");
  display.println("Syncing Time...");
  display.display();

  // Configure Timezone to GMT+8 using POSIX string
  // Note: "UTC-8" in POSIX standard literally means 8 hours AHEAD of UTC.
  configTzTime("UTC-8", ntpServer);

  struct tm timeinfo;
  int timeRetries = 0;
  while (!getLocalTime(&timeinfo, 5000) && timeRetries < 3) { 
    Serial.println("Retrying time sync...");
    timeRetries++; 
  }

  Serial.println("\n--- SYSTEM FULLY POWERED ON ---");
}

void loop() {
  bool currentTriggerBuzzer = false;
  String currentDangerLevel = "SAFE";

  // 1. SENSOR SCANNING & AI POSTING
  if(WiFi.status() == WL_CONNECTED){
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    if (isnan(temp) || isnan(hum)) { temp = 0.0; hum = 0.0; }

    int mq2_val = analogRead(MQ2_PIN);
    int mq7_val = analogRead(MQ7_PIN);
    int mq135_val = analogRead(MQ135_PIN);

    // --- READ FLAME AS ANALOG ---
    int flame_val = analogRead(FLAME_PIN);
    
    // Convert analog to boolean to match FastAPI schema. 
    // Analog sensors usually drop resistance (lower value) when near fire.
    // Standard ESP32 ADC range is 0 - 4095. Tune this threshold!
    int flameThreshold = 2000; 
    bool flame_detected = (flame_val < flameThreshold);

    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Scanning Env...");
    display.println("AI Processing...");
    display.display();

    WiFiClientSecure client;
    client.setInsecure(); 
    HTTPClient http;
    http.begin(client, serverName); 
    http.setTimeout(30000); 
    http.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(512); 
    doc["device_id"] = device_id;
    doc["mq7"] = mq7_val;
    doc["mq135"] = mq135_val;
    doc["mq2"] = mq2_val;
    doc["dht22_temp"] = temp;
    doc["dht22_humidity"] = hum;
    doc["flame_detected"] = flame_detected;
    doc["timestamp"] = getTimestamp();

    String requestBody;
    serializeJson(doc, requestBody);
    
    Serial.println("\n>>> SENDING SENSORS TO AI:");
    Serial.println(requestBody);

    int httpResponseCode = http.POST(requestBody);
    if(httpResponseCode > 0){
      String response = http.getString();
      Serial.println("<<< AI RESPONSE:");
      Serial.println(response);
      
      DynamicJsonDocument responseDoc(1024);
      deserializeJson(responseDoc, response);

      bool isDanger = responseDoc["danger"];
      currentTriggerBuzzer = responseDoc["trigger_buzzer"];
      currentDangerLevel = responseDoc["danger_level"].as<String>();
      currentDangerLevel.toUpperCase(); 

      display.clearDisplay();
      display.setCursor(0, 0);
      if (isDanger) {
        display.setTextSize(2); 
        display.println("  ( )  ");
        display.println(" (_) ");
        display.setTextSize(1); 
        display.print("LVL: "); display.println(currentDangerLevel);
      } else {
        display.setTextSize(2);
        display.println("      /");
        display.println(" \\   / ");
        display.println("  \\ /  ");
        display.setTextSize(1);
        display.println("Status: SAFE");
      }
      display.display();
    }
    http.end();
  }

  // --- 60 SECOND SMART DELAY FOR BUZZER & OVERRIDE ---
  unsigned long waitStart = millis();
  unsigned long lastOverrideCheck = 0;
  bool manualOverrideActive = false;

  while (millis() - waitStart < 60000) {
    
    // --- CHECK MANUAL OVERRIDE EVERY 2 SECONDS ---
    if (millis() - lastOverrideCheck >= 2000) {
      lastOverrideCheck = millis();
      
      if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure overrideClient;
        overrideClient.setInsecure();
        HTTPClient overrideHttp;
        
        overrideHttp.begin(overrideClient, alarmServerName);
        int overrideCode = overrideHttp.GET();
        
        if (overrideCode == 200) {
          String payload = overrideHttp.getString();
          DynamicJsonDocument overrideDoc(256);
          if (!deserializeJson(overrideDoc, payload)) {
            manualOverrideActive = overrideDoc["is_active"];
          }
        }
        overrideHttp.end();
      }
    }

    // --- PRIORITY BUZZER LOGIC ---
    if (manualOverrideActive) {
      digitalWrite(BUZZER_PIN, HIGH);
      
    } else if (currentTriggerBuzzer) {
      if (currentDangerLevel == "CRITICAL") {
        digitalWrite(BUZZER_PIN, HIGH); 
      } else if (currentDangerLevel == "HIGH") {
        digitalWrite(BUZZER_PIN, (millis() / 150) % 2);
      } else {
        digitalWrite(BUZZER_PIN, (millis() / 600) % 2);
      }
      
    } else {
      digitalWrite(BUZZER_PIN, LOW); 
    }
    
    delay(20); 
  }
  
  digitalWrite(BUZZER_PIN, LOW); 
}