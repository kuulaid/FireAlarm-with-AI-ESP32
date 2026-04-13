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


// --- SENSOR & OUTPUT PIN DEFINITIONS ---
#define DHTPIN 4          
#define DHTTYPE DHT22     
#define FLAME_PIN 27      
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
const long  gmtOffset_sec = 28800;  
const int   daylightOffset_sec = 0; 

String getTimestamp() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm * tm = localtime(&tv.tv_sec); 
  char timeStringBuff[50];
  sprintf(timeStringBuff, "%04d-%02d-%02dT%02d:%02d:%02d.%03d+08:00",
          tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
          tm->tm_hour, tm->tm_min, tm->tm_sec, (int)(tv.tv_usec / 1000));
  return String(timeStringBuff);
}

void setup() {
  Serial.begin(115200);

  dht.begin();
  pinMode(FLAME_PIN, INPUT);
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

  // Sync Time with retry limit so it doesn't freeze
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
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

  if(WiFi.status() == WL_CONNECTED){
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    if (isnan(temp) || isnan(hum)) { temp = 0.0; hum = 0.0; }

    int mq2_val = analogRead(MQ2_PIN);
    int mq7_val = analogRead(MQ7_PIN);
    int mq135_val = analogRead(MQ135_PIN);
    bool flame_detected = (digitalRead(FLAME_PIN) == LOW);

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
    
    Serial.println("\n>>> SENDING JSON:");
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

  // --- 60 SECOND SMART DELAY FOR BUZZER ---
  unsigned long waitStart = millis();
  while (millis() - waitStart < 60000) {
    if (currentTriggerBuzzer) {
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