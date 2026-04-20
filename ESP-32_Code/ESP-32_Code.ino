#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// Helper libraries for Firebase
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- 1. WIFI & FIREBASE CONFIG ---
#define WIFI_SSID "Yash"
#define WIFI_PASSWORD "11111111"
#define API_KEY "AIzaSyBnQXfDLt_DHWpRLcuX6qYGa7QyOBD6Ug4"
#define DATABASE_URL "https://smart-water-system-e5126-default-rtdb.firebaseio.com"

// --- 2. USER CALIBRATION & PINS ---
const int LOW_LEVEL_CM = 40;  
const int HIGH_LEVEL_CM = 25; 
const int SOAP_PULSE = 150;   

const int relaySoap = 32;    
const int relayWater = 33;   
const int relayRefill = 25; 
const int irTrigger = 14;    
const int irChoice = 27;     
const int pwmPin = 34;       
const int flowSensorPin = 19; 

// --- 3. VARIABLES ---
volatile int pulseCount = 0;
volatile unsigned long lastPulseTime = 0;
unsigned long totalMilliLitres = 0;
unsigned long oldTime = 0;
int lastDist = 0;

// Firebase Objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;
bool isOnline = false; 
unsigned long firebaseUpdatePrevMillis = 0;

LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- 4. INTERRUPT SERVICE ROUTINE ---
void IRAM_ATTR pulseCounter() {
  unsigned long currentTime = micros();
  if (currentTime - lastPulseTime > 500) { 
    pulseCount++;
    lastPulseTime = currentTime;
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(relaySoap, OUTPUT);
  pinMode(relayWater, OUTPUT);
  pinMode(relayRefill, OUTPUT);
  
  digitalWrite(relaySoap, HIGH);
  digitalWrite(relayWater, HIGH);
  digitalWrite(relayRefill, HIGH);

  pinMode(irTrigger, INPUT);
  pinMode(irChoice, INPUT);
  pinMode(pwmPin, INPUT);
  pinMode(flowSensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);

  lcd.init();
  lcd.backlight();
  lcd.print("SYSTEM STARTING");
  delay(1000);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 25000) {
    delay(500);
    Serial.print(".");
    lcd.setCursor(0, 1);
    lcd.print("WAITING: "); 
    lcd.print((25000 - (millis() - startAttemptTime)) / 1000);
    lcd.print("s  ");
  }

  if (WiFi.status() == WL_CONNECTED) {
    isOnline = true;
    Serial.println("\nWi-Fi Connected!");
    lcd.clear();
    lcd.print("WIFI: ONLINE");

    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    if (Firebase.signUp(&config, &auth, "", "")) signupOK = true;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
  } else {
    isOnline = false;
    Serial.println("\nWi-Fi Failed. Offline Mode.");
    lcd.clear();
    lcd.print("WIFI: FAILED");
    lcd.setCursor(0, 1);
    lcd.print("OFFLINE MODE");
  }

  delay(2000);
  lcd.clear();
}

void loop() {
  updateRefillLogic();
  handleLCDRotation();

  if (digitalRead(irTrigger) == LOW) {
    runWashingCycle();
  }

  if (isOnline) {
    syncToFirebase();
  }
}

void runWashingCycle() {
  lcd.clear();
  lcd.print("WETTING HANDS...");
  digitalWrite(relayWater, LOW);
  delay(4000);
     if (isOnline) {
    Firebase.RTDB.setTimestamp(&fbdo, "smartwash/station_1/lastSeen");
  }
  digitalWrite(relayWater, HIGH);

  bool wantsSoap = false;
  lcd.clear();
  lcd.print("WANT SOAP?");
  unsigned long timer = millis();
  while (millis() - timer < 4000) {
    lcd.setCursor(0, 1);
    lcd.print("Wave Now: "); lcd.print(4 - (millis()-timer)/1000); lcd.print("s");
    if (digitalRead(irChoice) == LOW) { 
      wantsSoap = true; 
      break; }
    delay(50);
  }

  if (wantsSoap) {
    digitalWrite(relaySoap, LOW);
    delay(SOAP_PULSE); 
    digitalWrite(relaySoap, HIGH);
    
    for (int i = 20; i > 0; i--) {
      lcd.setCursor(0,0); lcd.print("SCRUBBING...    ");
      lcd.setCursor(0,1); lcd.print("TIME: "); lcd.print(i); lcd.print("s   ");
      
     if (isOnline && (i == 15 || i == 10 || i == 5 || i == 1)) {
    Firebase.RTDB.setTimestamp(&fbdo, "smartwash/station_1/lastSeen");
  }
      delay(1000);
    }

    lcd.clear();
    lcd.print("FINAL RINSING...");
    digitalWrite(relayWater, LOW);
      if (isOnline) {
    Firebase.RTDB.setTimestamp(&fbdo, "smartwash/station_1/lastSeen");
  }
    delay(5000); 
    digitalWrite(relayWater, HIGH);
  } else {
    lcd.clear();
    lcd.print("NO SOAP NEEDED");
    delay(1000);
  }
  
  if (isOnline && signupOK) {
      FirebaseJson json;
      static unsigned long lastTotal = 0;
      unsigned long sessionML = totalMilliLitres - lastTotal;
      lastTotal = totalMilliLitres;

      int waterPercent;
      if (lastDist <= 25) waterPercent = 100;
      else if (lastDist >= 40) waterPercent = 0;
      else waterPercent = map(lastDist, 40, 25, 0, 100);

      json.add("mlUsed", sessionML);
      json.add("tankLevel", waterPercent);
      json.add("timestamp", "SERVER_TIME"); 

      if (Firebase.RTDB.pushJSON(&fbdo, "smartwash/history", &json)) {
          Serial.println("Session Logged to History!");
      }
  }

  lcd.clear();
  lcd.print("PROCESS DONE!");
  delay(2000);
  lcd.clear();
}

void updateRefillLogic() {
  if ((millis() - oldTime) > 1000) {
    totalMilliLitres += pulseCount * 2.22;
    pulseCount = 0;
    oldTime = millis();
  }

  long duration = 0;

  // 🔁 RETRY FIX
  for (int i = 0; i < 3; i++) {
    duration = pulseIn(pwmPin, HIGH, 30000);
    if (duration > 0) break;
    delay(10);
  }

  // 🔍 DEBUG
  Serial.println("\n------ ULTRASONIC DEBUG ------");
  Serial.print("Pulse Duration (us): ");
  Serial.println(duration);

  if (duration > 0) {
    lastDist = duration / 58;

    Serial.print("Distance (cm): ");
    Serial.println(lastDist);

    if (lastDist > LOW_LEVEL_CM) {
      Serial.println("Status: LOW LEVEL → REFILL ON");
      digitalWrite(relayRefill, LOW); 
    } else if (lastDist < HIGH_LEVEL_CM) {
      Serial.println("Status: HIGH LEVEL → REFILL OFF");
      digitalWrite(relayRefill, HIGH); 
    } else {
      Serial.println("Status: MID LEVEL → NO CHANGE");
    }

  } else {
    Serial.println("⚠️ ERROR: No echo (timing issue / interference)");
  }

  Serial.println("-------------------------------");
}

void handleLCDRotation() {
  int waterPercent;
  if (lastDist <= 25) waterPercent = 100;
  else if (lastDist >= 40) waterPercent = 0;
  else waterPercent = map(lastDist, 40, 25, 0, 100);

  lcd.setCursor(0, 0);
  lcd.print("Water: "); lcd.print(waterPercent); lcd.print("%   ");
  lcd.setCursor(0, 1);
  lcd.print("Used: "); lcd.print(totalMilliLitres); lcd.print("mL   ");
}

void syncToFirebase() {
  if (Firebase.ready() && signupOK) {
    if (Firebase.RTDB.getInt(&fbdo, "smartwash/station_1/triggerRefill")) {
      if (fbdo.intData() == 1) {
        lcd.clear(); lcd.print("REMOTE REFILL");
        digitalWrite(relayRefill, LOW); 
        delay(3000); 
        digitalWrite(relayRefill, HIGH); 
        Firebase.RTDB.setInt(&fbdo, "smartwash/station_1/triggerRefill", 0);
        lcd.clear();
      }
    }

    if (millis() - firebaseUpdatePrevMillis > 5000) {
      firebaseUpdatePrevMillis = millis();
      
      Firebase.RTDB.setTimestamp(&fbdo, "smartwash/station_1/lastSeen");

      int waterPercent;
      if (lastDist <= 25) waterPercent = 100;
      else if (lastDist >= 40) waterPercent = 0;
      else waterPercent = map(lastDist, 40, 25, 0, 100);

      Firebase.RTDB.setInt(&fbdo, "smartwash/station_1/waterPercent", waterPercent);
      Firebase.RTDB.setInt(&fbdo, "smartwash/station_1/totalMilliLitres", totalMilliLitres);
    }
  }
}