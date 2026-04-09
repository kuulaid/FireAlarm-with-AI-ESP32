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

// Initialize DHT sensor
DHT dht(DHTPIN, DHTTYPE);

// NTP Server Settings
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;      
const int   daylightOffset_sec = 0;

// Helper function to get current time
String getTimestamp() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm * tm = gmtime(&tv.tv_sec);
  
  char timeStringBuff[50];
  sprintf(timeStringBuff, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
          tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
          tm->tm_hour, tm->tm_min, tm->tm_sec, (int)(tv.tv_usec / 1000));
          
  return String(timeStringBuff);
}

void setup() {
  Serial.begin(115200);

  // Initialize Pins
  dht.begin();
  pinMode(FLAME_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); 

  // Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("SSD1306 allocation failed. Check OLED wiring (SDA=21, SCL=22)."));
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 10);
    display.println("Connecting to WiFi...");
    display.display();
  }

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");

  // Update OLED
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("WiFi Connected!");
  display.println("Syncing Time...");
  display.display();

  // Sync time via NTP
  Serial.println("Syncing time with NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo, 5000)) {
    Serial.println("Waiting for time sync...");
  }
  Serial.println("Time synchronized successfully!");
}

void loop() {
  // Variables to hold the AI's decision so our Buzzer Timer can use them later
  bool currentTriggerBuzzer = false;
  String currentDangerLevel = "SAFE";

  if(WiFi.status() == WL_CONNECTED){
    
    // --- 1. Read Actual Sensor Data ---
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    
    if (isnan(temp) || isnan(hum)) {
      Serial.println("Failed to read from DHT sensor!");
      temp = 0.0;
      hum = 0.0;
    }

    int mq2_val = analogRead(MQ2_PIN);
    int mq7_val = analogRead(MQ7_PIN);
    int mq135_val = analogRead(MQ135_PIN);
    bool flame_detected = (digitalRead(FLAME_PIN) == LOW);

    // --- 2. Setup Secure Client ---
    WiFiClientSecure client;
    client.setInsecure(); 
    HTTPClient http;
    http.begin(client, serverName); 
    http.setTimeout(30000); 
    http.addHeader("Content-Type", "application/json");

    // --- 3. Create JSON Payload ---
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
    
    // EXPLICIT DEBUGGING: Show exactly what is being sent
    Serial.println("\n========================================");
    Serial.println(">>> DATA SENT TO AI BACKEND:");
    Serial.println(requestBody);
    Serial.println("========================================");

    int httpResponseCode = http.POST(requestBody);

    if(httpResponseCode > 0){
      String response = http.getString();
      
      // EXPLICIT DEBUGGING: Show exactly what came back
      Serial.print("<<< HTTP Response code: ");
      Serial.println(httpResponseCode);
      Serial.println("<<< AI RESPONSE RECEIVED:");
      Serial.println(response);
      Serial.println("========================================\n");

      // --- 4. Parse Backend JSON Response ---
      DynamicJsonDocument responseDoc(1024);
      DeserializationError error = deserializeJson(responseDoc, response);

      if (!error) {
        // STRICT BOOLEAN & LEVEL LOGIC
        bool isDanger = responseDoc["danger"];
        currentTriggerBuzzer = responseDoc["trigger_buzzer"];
        currentDangerLevel = responseDoc["danger_level"].as<String>();
        currentDangerLevel.toUpperCase(); // Force uppercase for easy text matching

        // --- 5. Control the OLED Display ---
        display.clearDisplay();
        display.setCursor(0, 0);

        if (isDanger) {
          // --- FIRE ASCII ART ---
          display.setTextSize(2); 
          display.println("  ( )  ");
          display.println(" (___) ");
          
          display.setTextSize(1); 
          display.print("LVL: ");
          display.println(currentDangerLevel);
          
          // Dynamic text based on level
          if (currentDangerLevel == "CRITICAL") {
            display.println("EVACUATE NOW!");
          } else if (currentDangerLevel == "HIGH") {
            display.println("FIRE IMMINENT!");
          } else {
            display.println("WARNING! CHECK AREA");
          }
        } 
        else {
          // --- CHECKMARK ASCII ART ---
          display.setTextSize(2);
          display.println("      /");
          display.println(" \\   / ");
          display.println("  \\ /  ");
          
          display.setTextSize(1);
          display.println("Status: SAFE");
          display.print("T:"); display.print(temp, 1); display.print("C H:"); display.print(hum, 1); display.println("%");
        }
        
        display.display();

      } else {
        Serial.print("Failed to parse JSON response: ");
        Serial.println(error.c_str());
      }

    } else {
      Serial.print("Error connecting to Railway. Error code: ");
      Serial.println(httpResponseCode);
      
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(1);
      display.println("Network Error");
      display.print("Code: "); display.println(httpResponseCode);
      display.display();
    }
    
    http.end();
  } else {
    Serial.println("WiFi Disconnected. Checking connection...");
  }
  
  // --- 6. THE SMART TIMER & DYNAMIC BUZZER ---
  // Instead of delay(60000), we run a loop for 60 seconds.
  // This allows the ESP32 to constantly update the buzzer beeping speed.
  unsigned long waitStart = millis();
  while (millis() - waitStart < 60000) {
    
    if (currentTriggerBuzzer) {
      if (currentDangerLevel == "CRITICAL") {
        // Continuous tone
        digitalWrite(BUZZER_PIN, HIGH); 
      } 
      else if (currentDangerLevel == "HIGH") {
        // Fast Beep (Toggles every 100 milliseconds)
        if ((millis() / 100) % 2 == 0) digitalWrite(BUZZER_PIN, HIGH);
        else digitalWrite(BUZZER_PIN, LOW);
      } 
      else {
        // Moderate / Warning Beep (Toggles every 500 milliseconds)
        if ((millis() / 500) % 2 == 0) digitalWrite(BUZZER_PIN, HIGH);
        else digitalWrite(BUZZER_PIN, LOW);
      }
    } else {
      digitalWrite(BUZZER_PIN, LOW); // Safe state, ensure off
    }
    
    delay(10); // Tiny pause to prevent the ESP32 from crashing/overheating
  }

  // Ensure buzzer is totally quiet before starting the next sensor reading
  digitalWrite(BUZZER_PIN, LOW); 
}