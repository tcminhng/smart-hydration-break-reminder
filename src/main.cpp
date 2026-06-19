#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_BME280.h>
#include <Keypad.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_NeoPixel.h>
#include <Firebase_ESP_Client.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"
#include "secrets.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ===== Core Firebase Global Objects =====
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ===== Keypad Configuration =====
const byte KP_ROWS = 4, KP_COLS = 3;
char kpKeys[KP_ROWS][KP_COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte kpRowPins[KP_ROWS] = {18, 17, 16, 15}; // GPIO Pins for Rows
byte kpColPins[KP_COLS] = {7, 8, 3};       // GPIO Pins for Columns
Keypad keypad = Keypad(makeKeymap(kpKeys), kpRowPins, kpColPins, KP_ROWS, KP_COLS);

// ===== Buzzer =====
#define BUZZER_PIN 4
const int BUZZ_CH = 3;

// ===== WS2812 RGB LED Strip =====
#define LED_PIN   20
#define LED_COUNT 8
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ===== Ultrasonic (JSN-SR04T) =====
#define US_TRIG 6
#define US_ECHO 5
static const float ULTRA_MIN_CM = 21.0f;
static const float HALF_MAX_CM  = 25.0f;

// ===== Global Struct and Tracking Variables =====
const float HYDRATION_BOOST = 1.15f; // +15% daily water when active
static bool exPendingResetBreak = false;
static bool exPendingResetWater = false;
static bool exerciseNow  = false;
static bool exercisePrev = false;
static uint8_t exRiseCount = 0;
static const uint8_t EX_RISES_FOR_RECALC = 5;
static bool exTriggeredThisWindow = false;

bool inActive = false;
bool blockCounted = false;
uint32_t activeStart = 0;
float dynFilt = 0.0f;
const int MAX_BLOCKS = 20;
uint32_t blockTimes[MAX_BLOCKS];
int blockHead = 0, blockCount = 0;
bool boostActive = false;

// ===== Display Objects =====
SPIClass spi(FSPI);
Adafruit_ILI9341 tft = Adafruit_ILI9341(&spi, 2, 10, 9);

// ===== State Management Structures =====
enum EventKind  { EV_NONE, EV_WATER, EV_BREAK };
enum RunState   { ST_IDLE, ST_ALARM, ST_DRINK_GRACE, ST_IN_BREAK };

static uint32_t min_u32(uint32_t a, uint32_t b) { return (a < b) ? a : b; }
static int32_t remainingMs(uint32_t now, uint32_t due) { return (int32_t)(due - now); }

struct UserProfile {
  int gender; // 1 = Male, 2 = Female
  int age;
  int sleepHours;
  int exerciseMin;
  int workMin;
  int hours;
};
UserProfile u;

struct Scheduler {
  int breakIntervalMin;
  int breakLenMin;
  int remainingBreakMin;
  uint32_t nextWaterDueMs;
  uint32_t nextBreakDueMs;
  uint32_t stateStartMs;
  RunState state;
};
Scheduler sched;

static volatile uint16_t timeDiv = 60; // 60 = 1 minute behaves like 1 second

static inline uint32_t minutesToMs(float minutes) {
  const float ms = minutes * 60000.0f;
  return (uint32_t)(ms / (timeDiv < 1 ? 1 : timeDiv));
}

// ===== Utility and Display Functions =====
String mmss(uint32_t ms) {
  uint32_t totalSec = ms / 1000;
  uint32_t m = totalSec / 60;
  uint32_t s = totalSec % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
  return String(buf);
}

int trueSecLeft(uint32_t now, uint32_t due) {
  int32_t rem = remainingMs(now, due);
  if (rem < 0) return 0;
  return (rem + 999) / 1000;
}

int smoothStepDown(int trueSec, int prevDisp) {
  if (prevDisp < 0) return trueSec; 
  if (trueSec >= prevDisp) return trueSec; 
  return prevDisp - 1; 
}

static void drawTextLine(int16_t x, int16_t y, const char* text, uint16_t color) {
  tft.fillRect(x, y, 300, 18, ILI9341_BLACK);
  tft.setCursor(x, y);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.print(text);
}

enum LevelState { LVL_FULL, LVL_HALF, LVL_EMPTY };
LevelState shownLevel = LVL_EMPTY;

LevelState getWaterLevelState(float cm) {
  if (cm <= ULTRA_MIN_CM) return LVL_FULL;
  else if (cm <= HALF_MAX_CM) return LVL_HALF;
  else return LVL_EMPTY;
}

uint32_t ledColor(uint8_t r, uint8_t g, uint8_t b) {
  return leds.Color(r, g, b);
}

static void applyLevelToLeds(LevelState lvl, bool blinkState, bool forceWrite = false) {
  if (!forceWrite && lvl == shownLevel && !(lvl == LVL_EMPTY)) return;
  leds.clear();
  switch (lvl) {
    case LVL_FULL:
      for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, ledColor(0,150,0));
      break;
    case LVL_HALF:
      for (int i = 0; i < 4 && i < LED_COUNT; i++) leds.setPixelColor(i, ledColor(150,150,0));
      break;
    case LVL_EMPTY:
      if (blinkState) {
        leds.setPixelColor(0, ledColor(150,0,0));
      }
      break;
  }
  leds.show();
  shownLevel = lvl;
}

void clearScreen() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(0, 0);
}

void showMessage(const String& msg) {
  clearScreen();
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.println(msg);
}

String getKeypadInput() {
  String input = "";
  tft.print("> ");
  while (true) {
    char key = keypad.getKey();
    if (key) {
      if (key == '#') break; 
      else if (key == '*') {   
        if (input.length() > 0) {
          input.remove(input.length() - 1);
          tft.fillRect(10, 120, 200, 40, ILI9341_BLACK);
          tft.setCursor(10, 120);
          tft.print("> ");
          tft.print(input);
        }
      } else {
        input += key;
        tft.print(key);
      }
    }
    delay(10);
  }
  return input;
}

int askIntBounded(const String& q, int minV, int maxV) {
  while (true) {
    clearScreen();
    tft.println(q + " (#=OK,*=DEL)");
    String s = getKeypadInput();
    long v = s.toInt();
    if (s.length() > 0 && v >= minV && v <= maxV) return (int)v;
    clearScreen();
    tft.setTextColor(ILI9341_RED); tft.setTextSize(2);
    tft.println("Invalid. Range:");
    tft.printf("%d .. %d\n", minV, maxV);
    delay(900);
  }
}

int askQuestion(const String& q) {
  return askIntBounded(q, 1, 2);
}

int clampi(int val, int low, int high) {
  if (val < low) return low;
  if (val > high) return high;
  return val;
}

float clampf(float val, float low, float high) {
  if (val < low) return low;
  if (val > high) return high;
  return val;
}

struct CalculatedParameters {
  float dailyWaterTarget;
  float dailyWaterAdj;
  int totalBreakMin;
  int breakIntervalMin;
};

CalculatedParameters runDynamicCalculations(UserProfile u, float tempC, float humidity) {
  CalculatedParameters p;
  float baseWater = (u.gender == 1) ? 3000.0f : 2200.0f;
  float ageFactor = (u.age > 50) ? -200.0f : (u.age < 18 ? -400.0f : 0.0f);
  float dailyWaterBase = baseWater + ageFactor;
  
  float activityAdj = u.exerciseMin * 7.5f; 
  float tempAdj = (tempC > 27.0f) ? (tempC - 27.0f) * 60.0f : 0.0f;
  float humAdj = (humidity < 40.0f) ? (40.0f - humidity) * 8.0f : 0.0f;
  float dailyWaterAdj = activityAdj + tempAdj + humAdj;
  
  if (boostActive) dailyWaterAdj += 400.0f;
  p.dailyWaterTarget = dailyWaterBase + dailyWaterAdj;
  p.dailyWaterAdj = dailyWaterAdj;
  
  int baseRest = 5 * clampi(u.hours, 1, 16);
  int heatAdd = (int)clampf((tempC - 24.0f), 0.0f, 10.0f);
  int total = baseRest + heatAdd;
  if (boostActive) total = (int)(total * 1.20f);
  
  p.totalBreakMin = clampi(total, 10, 120);
  int interval = (boostActive || tempC >= 28.0f) ? 45 : 60;
  p.breakIntervalMin = interval;
  
  return p;
}

float calculateTargetVolume(float base, float activityAdj, float temp, float hum) {
  float tempAdj = 0.0f;
  float humAdj = 0.0f;
  if (temp > 30)      tempAdj = 500.0f;
  else if (temp > 20) tempAdj = 250.0f;
  if (hum < 40)        humAdj = 200.0f;
  
  float total = base + activityAdj + tempAdj + humAdj;
  if (u.gender == 1) total *= 1.05; 
  return total;
}

bool loadUserFromFirebase(String userID, UserProfile &u) {
  String base = "/devices/esp32-01/users/" + userID;
  bool success = true;
  if (Firebase.RTDB.getInt(&fbdo, base + "/age")) u.age = fbdo.intData(); else success = false;
  if (Firebase.RTDB.getString(&fbdo, base + "/gender")) {
    String g = fbdo.stringData();
    u.gender = (g.equalsIgnoreCase("Male") ? 1 : 2);
  } else success = false;
  if (Firebase.RTDB.getInt(&fbdo, base + "/hours")) u.hours = fbdo.intData(); else success = false;
  return success;
}

void setup() {
  Serial.begin(115200);
  
  spi.begin(14, 12, 13, 10);
  tft.begin(spi);
  tft.setRotation(1);
  
  ledcSetup(BUZZ_CH, 3500, 8);
  ledcAttachPin(BUZZER_PIN, BUZZ_CH);
  
  leds.begin();
  leds.setBrightness(40);
  leds.clear();
  leds.show();
  
  pinMode(US_TRIG, OUTPUT);
  pinMode(US_ECHO, INPUT_PULLDOWN);
  digitalWrite(US_TRIG, LOW);
  
  showMessage("Connecting WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }
  Serial.println("WiFi connected!");
  
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.windows.com");
  setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();
  
  int genderVal = 0;
  while (true) {
    genderVal = askQuestion("Gender (1-M,2-F):");
    if (genderVal == 1 || genderVal == 2) break;
    clearScreen();
    showMessage("Invalid input!\nUse 1=Male, 2=Female");
    delay(1200);
  }
  u.gender = genderVal;
  u.age = askIntBounded("Age:", 1, 119);
  u.hours = askIntBounded("Hours per day (Work/Study):", 1, 16);
  
  clearScreen();
  tft.setTextColor(ILI9341_GREEN);
  tft.println("Initialization Done!");
  delay(1000);
  clearScreen();

  // Establish Initial Schedule Periods
  uint32_t baseMs = millis();
  sched.breakIntervalMin = 60;
  sched.breakLenMin = 5;
  sched.nextWaterDueMs = baseMs + minutesToMs(30);
  sched.nextBreakDueMs = baseMs + minutesToMs(sched.breakIntervalMin);
  sched.state = ST_IDLE;
}

static int prevBreakMin = -1;
static int dispSecW = -1;
static int dispSecB = -1;
static RunState prevState = ST_IDLE;

void loop() {
  uint32_t now = millis();
  
  // --- Check State Transitions and UI Changes ---
  if (sched.state != prevState) {
    clearScreen();
    prevState = sched.state;
    dispSecW = -1; 
    dispSecB = -1;
  }

  // --- Main Operational States ---
  if (sched.state == ST_IN_BREAK) {
    uint32_t elapsed = now - sched.stateStartMs;
    uint32_t totalBreakMs = minutesToMs(sched.breakLenMin);
    
    if (elapsed >= totalBreakMs) {
      sched.state = ST_IDLE;
      sched.nextBreakDueMs = now + minutesToMs(sched.breakIntervalMin);
    } else {
      uint32_t left = totalBreakMs - elapsed;
      sched.remainingBreakMin = (left + 59999) / 60000;
      
      if (prevBreakMin != sched.remainingBreakMin) {
        char buf[48]; 
        snprintf(buf, sizeof(buf), "Break left:  %d min", sched.remainingBreakMin);
        drawTextLine(10, 40, buf, ILI9341_YELLOW);
        prevBreakMin = sched.remainingBreakMin;
      }
    }
  } else {
    // Smooth Countdown logic for Hydration timers
    int secW_true = trueSecLeft(now, sched.nextWaterDueMs);
    int secW_disp = smoothStepDown(secW_true, dispSecW);
    if (secW_disp != dispSecW) {
      char buf[48];
      int m = secW_disp / 60, s = secW_disp % 60;
      snprintf(buf, sizeof(buf), "Next Water:  %02d:%02d", m, s);
      drawTextLine(10, 70, buf, ILI9341_CYAN);
      dispSecW = secW_disp;
    }

    // Smooth Countdown logic for Break timers
    int secB_true = trueSecLeft(now, sched.nextBreakDueMs);
    int secB_disp = smoothStepDown(secB_true, dispSecB);
    if (secB_disp != dispSecB) {
      char buf[48];
      int m = secB_disp / 60, s = secB_disp % 60;
      snprintf(buf, sizeof(buf), "Next Break:  %02d:%02d", m, s);
      drawTextLine(10, 100, buf, ILI9341_GREEN);
      dispSecB = secB_disp;
    }

    // Trigger alarms on milestones
    if (now >= sched.nextWaterDueMs && sched.state == ST_IDLE) {
      sched.state = ST_ALARM;
      ledcWriteNote(BUZZ_CH, NOTE_A, 4);
    }
    
    if (now >= sched.nextBreakDueMs && sched.state == ST_IDLE) {
      sched.state = ST_IN_BREAK;
      sched.stateStartMs = now;
      ledcWrite(BUZZ_CH, 0);
    }
  }

  // Keypad processing hooks
  char localKey = keypad.getKey();
  if (localKey == '0') {
    ledcWrite(BUZZ_CH, 0);
    if (sched.state == ST_ALARM) {
      sched.state = ST_IDLE;
      sched.nextWaterDueMs = now + minutesToMs(45);
    }
  }

  delay(30);
}
