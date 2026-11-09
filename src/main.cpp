#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// Wi-Fi Credentials
const char* ssid = "psa1"; 
const char* password = "12345678";

WebServer server(80);
Preferences preferences;
Servo binServo;

// Initialize I2C LCD (Address 0x27, 16 columns, 2 rows)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Custom LCD Symbols (8x5 Pixel Matrix)
byte wifiIcon[8] = {  // Home Wi-Fi Connected Symbol
  B00000,
  B01110,
  B10001,
  B00100,
  B01010,
  B00000,
  B00100,
  B00000
};

byte apIcon[8] = {    // AP Mode / Connect-to-Rower Symbol
  B11111,
  B10001,
  B10101,
  B10001,
  B11111,
  B00100,
  B01110,
  B11111
};

byte manualIcon[8] = { // Manual Mode Symbol (Gamepad/Hand)
  B00000,
  B01110,
  B11111,
  B01110,
  B00100,
  B01110,
  B10001,
  B00000
};

byte autoIcon[8] = {   // Auto Mode Symbol (Robot/Gear)
  B01010,
  B11111,
  B10101,
  B01110,
  B01110,
  B10101,
  B11111,
  B01010
};

// Motor Driver Pin Definitions (L298N)
const int ENA = 32; 
const int IN1 = 26; 
const int IN2 = 27; 
const int IN3 = 14; 
const int IN4 = 12; 
const int ENB = 33; 

// Ultrasonic Sensor Pin Definitions (HC-SR04)
const int trigPin = 5;  
const int echoPin = 18; 

// IR Sensor Pin Definitions
const int irLeftPin = 34;   
const int irRightPin = 35;  

const int servoPin = 4;

#define SOUND_SPEED 0.0343f
#define ECHO_TIMEOUT_US 25000 

const int freq = 30000;
const int resolution = 8;
const int pwmChannelA = 0;
const int pwmChannelB = 1;

// Operating Modes & Speeds
String roverMode = "manual"; 
int fixedAutoSpeed = 180;    
bool obstacleAvoidanceEnabled = true;
unsigned long lastSensorCheck = 0;
bool isHomeConnected = false;

// Low-level motor stop helper
void stopMotors() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  ledcWrite(pwmChannelA, 0); ledcWrite(pwmChannelB, 0);
}

// Independent motor control for tank steering
void setLeftSide(int speed) {
  if (speed > 0) {
    digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
    ledcWrite(pwmChannelA, speed);
  } else if (speed < 0) {
    digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
    ledcWrite(pwmChannelA, abs(speed));
  } else {
    digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
    ledcWrite(pwmChannelA, 0);
  }
}

void setRightSide(int speed) {
  if (speed > 0) {
    digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
    ledcWrite(pwmChannelB, speed);
  } else if (speed < 0) {
    digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
    ledcWrite(pwmChannelB, abs(speed));
  } else {
    digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
    ledcWrite(pwmChannelB, 0);
  }
}

// Measure ultrasonic distance
float readDistanceCm() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long echo_us = pulseIn(echoPin, HIGH, ECHO_TIMEOUT_US);
  if (echo_us == 0) return -1.0;
  return (echo_us * SOUND_SPEED) / 2.0f;
}

// Update LCD using Custom Symbols instead of text labels
void updateLCD(String ipStr) {
  // Row 0: Connection Symbol + IP Address
  lcd.setCursor(0, 0);
  if (isHomeConnected) {
    lcd.write(byte(0)); // Home Wi-Fi Symbol
  } else {
    lcd.write(byte(1)); // AP Mode Symbol
  }
  lcd.print(" ");
  String line1 = ipStr;
  while (line1.length() < 14) line1 += " ";
  lcd.print(line1);

  // Row 1: Mode Symbol + Mode Text
  lcd.setCursor(0, 1);
  if (roverMode == "manual") {
    lcd.write(byte(2)); // Manual Mode Symbol
  } else {
    lcd.write(byte(3)); // Auto Mode Symbol
  }
  lcd.print(" ");
  String line2 = (roverMode == "manual") ? "MANUAL" : "AUTO";
  while (line2.length() < 14) line2 += " ";
  lcd.print(line2);
}

// Handle Mode Switching Endpoint
void handleSetMode() {
  if (server.hasArg("mode")) {
    roverMode = server.arg("mode");
    preferences.putString("roverMode", roverMode);
    Serial.println("Mode changed to: " + roverMode);
    
    stopMotors();
    updateLCD(isHomeConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
    server.send(200, "text/plain", "Mode set to " + roverMode);
  } else {
    server.send(400, "text/plain", "Missing mode parameter");
  }
}

// Handle Manual Slider Speed Controls
void handleManualDrive() {
  if (roverMode == "manual") {
    if (server.hasArg("left") && server.hasArg("right")) {
      int leftSpeed = server.arg("left").toInt();
      int rightSpeed = server.arg("right").toInt();
      setLeftSide(leftSpeed);
      setRightSide(rightSpeed);
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Ignored: Not in manual mode");
}

// Combined JSON endpoint for sensors and status
void handleSensors() {
  float distance = readDistanceCm();
  int irLeft = digitalRead(irLeftPin);
  int irRight = digitalRead(irRightPin);

  String json = "{";
  json += "\"dist\":" + (distance < 0 ? "\"Out of range\"" : String(distance, 1)) + ",";
  json += "\"irLeft\":" + String(irLeft) + ",";
  json += "\"irRight\":" + String(irRight) + ",";
  json += "\"mode\":\"" + roverMode + "\",";
  json += "\"avoidance\":" + String(obstacleAvoidanceEnabled ? "true" : "false");
  json += "}";

  server.send(200, "application/json", json);
}

void handleLidOpen() {
  binServo.write(90);
  server.send(200, "text/plain", "Lid Opened");
}

void handleLidClose() {
  binServo.write(0);
  server.send(200, "text/plain", "Lid Closed");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize I2C and LCD
  Wire.begin(21, 22); // SDA = GPIO 21, SCL = GPIO 22
  lcd.init();
  lcd.backlight();

  // Load Custom Symbols into LCD CGRAM
  lcd.createChar(0, wifiIcon);
  lcd.createChar(1, apIcon);
  lcd.createChar(2, manualIcon);
  lcd.createChar(3, autoIcon);

  preferences.begin("rover", false);
  roverMode = preferences.getString("roverMode", "manual");
  obstacleAvoidanceEnabled = preferences.getBool("avoidance", true);

  // Initialize Servo
  ESP32PWM::allocateTimer(0);
  binServo.setPeriodHertz(50);
  binServo.attach(servoPin, 500, 2400);
  binServo.write(0);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  digitalWrite(trigPin, LOW);

  pinMode(irLeftPin, INPUT);
  pinMode(irRightPin, INPUT);

  ledcSetup(pwmChannelA, freq, resolution);
  ledcSetup(pwmChannelB, freq, resolution);
  ledcAttachPin(ENA, pwmChannelA);
  ledcAttachPin(ENB, pwmChannelB);

  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    isHomeConnected = true;
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());
    updateLCD(WiFi.localIP().toString());
  } else {
    isHomeConnected = false;
    WiFi.disconnect(true);
    WiFi.softAP("ESP32_Dustbin_Rover", "password123");
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
    updateLCD(WiFi.softAPIP().toString());
  }

  server.on("/sensors", handleSensors);
  server.on("/set-mode", handleSetMode);
  server.on("/manual-drive", handleManualDrive);
  server.on("/lid-open", handleLidOpen);
  server.on("/lid-close", handleLidClose);

  server.begin();
  Serial.println("Web server started");
}

void loop() {
  server.handleClient();

  unsigned long currentMillis = millis();
  if (currentMillis - lastSensorCheck > 250) {
    lastSensorCheck = currentMillis;
    float dist = readDistanceCm();
    int irL = digitalRead(irLeftPin);
    int irR = digitalRead(irRightPin);

    // AUTO MODE BEHAVIOR
    if (roverMode == "auto") {
      if (obstacleAvoidanceEnabled && ((dist > 0 && dist < 15.0) || irL == 0 || irR == 0)) {
        stopMotors();
      } else {
        setLeftSide(fixedAutoSpeed);
        setRightSide(fixedAutoSpeed);
      }
    }
  }
}