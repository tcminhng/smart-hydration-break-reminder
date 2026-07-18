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
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
//#define TIME_SCALE 0.0167f   // 1 min behaves like ~1 sec
static int dispSecW = -1;  // displayed seconds for water
static int dispSecB = -1;  // displayed seconds for break
static volatile bool skipNextRequested = false;
static SemaphoreHandle_t fbMutex = nullptr;
// ---- Motion seconds in last 1 hour (1-sec bins) ----
static const uint32_t WINDOW_SEC = 3600;   // 1 hour
static const uint16_t BIN_MS     = 1000;   // 1 second bins
static const uint16_t MOVE_SEC_THRESHOLD = 10; // >10 seconds of motion triggers change
static uint8_t  moveSec[WINDOW_SEC]; // ring buffer of 0/1 per second (zero-init)
static uint16_t moveIdx = 0;         // write index
static uint16_t moveCount = 0;       // sum of last 3600 seconds
static uint16_t moveFilled = 0;      // how many bins have been filled (<=3600)
static uint32_t binStartMs = 0;      // start of current 1-sec bin
static bool     motionInBin = false; // has motion occurred in current bin?
// ===== TFT (ESP32-S3 FSPI) =====
#define TFT_SCK   12
#define TFT_MOSI  11
#define TFT_MISO  13
#define TFT_CS    9
#define TFT_DC    10
#define TFT_RST   14
//BME280 
#define I2C_SDA   2
#define I2C_SCL   1
//Accelerometer
#define MPU_SDA   41
#define MPU_SCL   40
// ===== Keypad =====
const byte KP_ROWS = 4, KP_COLS = 3;
char kpKeys[KP_ROWS][KP_COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte kpRowPins[KP_ROWS] = {18, 17, 16, 15};
byte kpColPins[KP_COLS] = {7, 8, 3};
Keypad keypad = Keypad(makeKeymap(kpKeys), kpRowPins, kpColPins, KP_ROWS, KP_COLS);
// ===== Buzzer =====
#define BUZZER_PIN 4
const int BUZZ_CH = 3;
// ===== WS2812 (solid colors) =====
#define LED_PIN   20
#define LED_COUNT 8
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
// ===== Ultrasonic (JSN-SR04T) =====
#define US_TRIG 6
#define US_ECHO 5
// Simple, bucketed interpretation (cm)
static const float ULTRA_MIN_CM   = 21.0f;  // blind/“min” → treat as FULL
static const float HALF_MAX_CM    = 25.0f;  // >20 and ≤25 → HALF
static const float EMPTY_MIN_CM   = 25.0f;  // ≥27 → EMPTY
// Pause/resume primary I2C around ultrasonic reads (safe even if not shared)
static inline void i2cPause(bool off) {
  if (off) {
    Wire.end();                     // release I2C peripheral
    pinMode(I2C_SCL, INPUT);        // float SCL so ECHO has clean control if shared
    delayMicroseconds(20);
  } else {
    Wire.begin(I2C_SDA, I2C_SCL);   // restore I2C
    Wire.setClock(100000);
    Wire.setTimeOut(50);
  }
}
enum LevelState : uint8_t { LVL_UNKNOWN=0, LVL_FULL, LVL_HALF, LVL_EMPTY };
static LevelState waterLevel   = LVL_UNKNOWN;   // latest measured level
static LevelState shownLevel   = LVL_UNKNOWN;   // what LEDs are currently showing
static uint32_t   lastBlinkMs  = 0;             // for EMPTY blink
static bool       blinkOn      = false;
// === Ultrasonic deferred-measure control ===
static uint32_t ultraDueMs = 0;
static const uint32_t ULTRA_DELAY_MS = 3000;   // “couple of seconds” before measuring
// ---- ACTIVITY DETECTION (Stage 3) ----
TwoWire WireMPU(1);          // second I2C controller for MPU6050
// thresholds/timing (tune if needed)
const float ACTIVITY_THRESH = 1.0f;      // m/s^2 above gravity (dynamic accel)
const uint32_t ACTIVE_MIN_MS = 1000;     // ≥ 3 seconds continuous to count as a block
const uint32_t WINDOW_MS     = 30UL * 60UL * 1000UL; // 30 minutes rolling window
const float HYDRATION_BOOST  = 1.15f;    // +15% daily water when active
static bool exPendingResetBreak = false;  // set true when a break completes
static bool exPendingResetWater = false;  // set true when a drink completes
// --- Simple exercise edge counter (retimes next-break only) ---
static bool     exerciseNow  = false;
static bool     exercisePrev = false;
static uint8_t  exRiseCount  = 0;
static const uint8_t EX_RISES_FOR_RECALC = 5; // No→Yes edges needed before next break
static bool exTriggeredThisWindow = false;  // allow exactly one retime per break window
// activity state
bool inActive = false;
bool blockCounted = false;
uint32_t activeStart = 0;
float dynFilt = 0.0f;        // simple exponential smoothing of dynamic accel
// tiny ring buffer of timestamps for recent active blocks
const int MAX_BLOCKS = 20;
uint32_t blockTimes[MAX_BLOCKS];
int blockHead = 0, blockCount = 0;
bool boostActive = false;    // whether hydration boost is currently applied
// ===== Objects =====
SPIClass spi(FSPI);
Adafruit_ILI9341 tft = Adafruit_ILI9341(&spi, TFT_CS, TFT_DC, TFT_RST);
Adafruit_BME280 bme;
Adafruit_MPU6050 mpu;
WiFiMulti wifiMulti;
// State
bool bmeOk = false, mpuOk = false;
float cTemp=NAN, cHum=NAN, cDist=NAN;
uint32_t lastRead=0, lastUsRead=0, lastFbPush=0, lastSeen=0;
String basePath;
// ---------- User struct ----------
struct User {
  String id;
  int age;
  int gender;
  int hours;
  float weight;
  float height;
}; User currentUser;
// ---- FreeRTOS + logging globals (place BEFORE serviceScheduler) ----
enum LogKind : uint8_t { LOG_NONE=0, LOG_HYDRATION=1, LOG_BREAK=2 };
struct LogItem {
  LogKind   kind;
  uint32_t  ts_ms;         // millis() when event happened
  int       amount_ml;     // hydration only
  int       remaining_ml;  // hydration: remaining water; break: remaining minutes
  int       duration_min;  // break only
};
static QueueHandle_t logQueue = nullptr;
static volatile bool dirtyTotals = false;      // set true when remaining_* changed
static volatile bool recalcRequested = false;  // set by Firebase task, handled in loop()
// YYYY-MM-DD for per-day log buckets
static String todayStr() {
  time_t t = time(nullptr);
  struct tm* ti = localtime(&t);
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", ti);
  return String(buf);
}
// ===== Demo/test timing control =====
static volatile uint16_t timeDiv = 60; 
// 1 = real minutes; 60 = 1 minute behaves like 1 second; 120 = 1 minute ≈ 0.5 s
// Replace your minutesToMs() with this version:
// (If your current function is already named minutesToMs, REPLACE it.)
static inline uint32_t minutesToMs(float minutes) {
  // compress minutes into milliseconds by a divisor (demo mode)
  const float ms = minutes * 60000.0f;
  return (uint32_t)(ms / (timeDiv < 1 ? 1 : timeDiv));
}
// ===== SCHEDULER CORE =====
enum EventKind  { EV_NONE, EV_WATER, EV_BREAK };
enum RunState   { ST_IDLE, ST_ALARM, ST_DRINK_GRACE, ST_IN_BREAK };
static uint32_t min_u32(uint32_t a, uint32_t b){ return (a<b)?a:b; }
static int32_t  remainingMs(uint32_t now, uint32_t due){ return (int32_t)(due - now); }
struct Scheduler {
  // plan
  int   breakIntervalMin;    // spacing between events (we use same spacing for both)
  int   breakLenMin;         // per-break duration
  float mlPerBreak;          // drink target per event
  // remaining daily totals
  float remainingWaterMl;
  int   remainingBreakMin;
  // timers
  uint32_t nextWaterDueMs;
  uint32_t nextBreakDueMs;
  // runtime
  EventKind currentDue;
  RunState  state;
  uint32_t  stateStartMs;
  // buzzer pattern
  uint32_t buzzOnMs = 250, buzzOffMs = 250;
  bool     buzzing = false;
  uint32_t lastBuzzToggleMs = 0;
};
Scheduler sched;   // global
// ---------- Helper Functions ----------
// one-slot queue so we never block during alarms
/*struct PendingLog {
  String path;
  FirebaseJson json;
  bool pending = false;
};
static PendingLog logQ;
static bool dirtyTotals = false;  // we changed remaining_*; flush on next sync
*/
void clearScreen() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(10, 100);
  tft.setTextColor(ILI9341_GREEN);
  tft.setTextSize(2);
}
// true seconds remaining based on due time
static inline int trueSecLeft(uint32_t now, uint32_t dueMs) {
  if ((int32_t)(dueMs - now) <= 0) return 0;
  return (int)((dueMs - now + 999) / 1000);
}
// smooth the displayed seconds so they never jump by >1 per frame
static inline int smoothStepDown(int trueSec, int prevDisp) {
  if (prevDisp < 0) return trueSec;          // first paint
  if (trueSec >= prevDisp) return trueSec;   // caught up or extended
  // step down by exactly 1 per UI frame to avoid big visual jumps
  return prevDisp - 1;
}
static void drawTextLine(int16_t x, int16_t y, const char* text, uint16_t color) {
  tft.fillRect(x, y, 300, 18, ILI9341_BLACK);  // clear just that line
  tft.setCursor(x, y);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.print(text);
}
void showMessage(String msg) {
  tft.println(msg);
}
void buzzerStart() {
  if (!sched.buzzing) {
    ledcWriteTone(BUZZ_CH, 3500);
    sched.buzzing = true;
    sched.lastBuzzToggleMs = millis();
  }
}
void buzzerStop() {
  if (sched.buzzing) {
    ledcWriteTone(BUZZ_CH, 0);
    sched.buzzing = false;
  }
}
// Optional: make it beep-beep (on/off pattern) without blocking
void buzzerService() {
  if (!sched.buzzing) return;
  uint32_t now = millis();
  uint32_t dt  = now - sched.lastBuzzToggleMs;
  if (dt >= sched.buzzOnMs) {
    // toggle off phase
    ledcWriteTone(BUZZ_CH, 0);
    // wait off for buzzOffMs, then restart tone
    ledcWriteTone(BUZZ_CH, 2200);
    sched.lastBuzzToggleMs = millis();
  }
}
void beepShort() {
  ledcWriteTone(BUZZ_CH, 2000);   // Start tone
  delay(150);                     // Play 150 ms
  ledcWriteTone(BUZZ_CH, 0);      // Stop tone
}
void recordActiveBlock(uint32_t nowMs) {
  // push timestamp
  blockTimes[blockHead] = nowMs;
  blockHead = (blockHead + 1) % MAX_BLOCKS;
  if (blockCount < MAX_BLOCKS) blockCount++;
  // prune old entries outside the rolling window
  int valid = 0;
  for (int i = 0; i < blockCount; i++) {
    int idx = (blockHead - 1 - i + MAX_BLOCKS) % MAX_BLOCKS;
    if ((nowMs - blockTimes[idx]) <= WINDOW_MS) valid++;
    else break; // older ones are packed behind; stop
  }
  blockCount = valid;
}
int blocksInWindow(uint32_t nowMs) {
  int n = 0;
  for (int i = 0; i < blockCount; i++) {
    int idx = (blockHead - 1 - i + MAX_BLOCKS) % MAX_BLOCKS;
    if ((nowMs - blockTimes[idx]) <= WINDOW_MS) n++;
    else break;
  }
  return n;
}
static inline uint32_t ledColor(uint8_t r, uint8_t g, uint8_t b) {
  return leds.Color(r, g, b);
}
static float readUltrasonicCmOnce() {
  // 1) Ensure ECHO is low before triggering (up to ~2 ms)
  uint32_t t0 = micros();
  while (digitalRead(US_ECHO) == HIGH && (micros() - t0) < 2000UL) {}
  // 2) 10 µs TRIG pulse
  digitalWrite(US_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(US_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(US_TRIG, LOW);
  // 3) Read echo with ~30 ms timeout (≈ ~5 m max)
  unsigned long dur = pulseIn(US_ECHO, HIGH, 30000UL);
  if (dur == 0) return NAN;  // no echo → treat later as FULL
  // 4) Temperature-compensated speed of sound (uses your BME280 cTemp if valid)
  float T = isfinite(cTemp) ? cTemp : 20.0f;  // °C
  float v_m_per_s = 331.3f + 0.606f * T;
  // Convert µs to cm: distance = (dur * v) / 2 ; v in cm/µs
  float cm = (dur * (v_m_per_s * 100.0f)) / 2000000.0f;
  // 5) Sanity window (ignore garbage)
  if (cm < 0.5f || cm > 500.0f) return NAN;
  return cm;
}
static float readUltrasonicCm() {
  // median-of-3 for stability; ignore NANs
  float v[3]; int n=0;
  for (int i=0;i<3;i++){
    float x = readUltrasonicCmOnce();
    if (isfinite(x)) v[n++] = x;
    delay(20);
  }
  if (n==0) return NAN;
  if (n==1) return v[0];
  if (n==2) return (v[0]+v[1])*0.5f;
  // median of 3
  if (v[0] > v[1]) { float t=v[0]; v[0]=v[1]; v[1]=t; }
  if (v[1] > v[2]) { float t=v[1]; v[1]=v[2]; v[2]=t; }
  if (v[0] > v[1]) { float t=v[0]; v[0]=v[1]; v[1]=t; }
  return v[1];
}
static LevelState classifyLevel(float cm) {
  // If the sensor can't pick up a value (too close / blind / timeout),
  // treat as FULL (water up top under the lid).
  if (!isfinite(cm)) return LVL_FULL; 
  if (cm < ULTRA_MIN_CM)        return LVL_FULL;  // <21 cm → FULL (8 green)
  else if (cm <= HALF_MAX_CM)     return LVL_HALF;  // 21–25 cm → HALF (4 yellow)
  else                              return LVL_EMPTY; // >25 cm → EMPTY (1 blinking red)
}
// Forceable LED writer: refresh even if the level didn't change when forceWrite=true
static void applyLevelToLeds(LevelState lvl, bool blinkState, bool forceWrite = false) {
  if (!forceWrite && lvl == shownLevel && !(lvl == LVL_EMPTY)) return;  // skip unless forced
  leds.clear();
  switch (lvl) {
    case LVL_FULL:  // 8 green
      for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, ledColor(0,150,0));
      break;
    case LVL_HALF:  // 4 yellow
      for (int i = 0; i < 4 && i < LED_COUNT; i++) leds.setPixelColor(i, ledColor(150,150,0));
      break;
    case LVL_EMPTY: // 1 blinking red
      if (blinkState) leds.setPixelColor(0, ledColor(150,0,0));
      break;
    default: break;
  }
  leds.show();
  shownLevel = lvl;
}
static void ultraMeasureAndUpdate(bool /*force*/) {
  // Take a snapshot reading
  // AFTER — pause I2C while measuring, then restore it
  i2cPause(true);
  float cm = readUltrasonicCm();   // median-of-3
  i2cPause(false);
  LevelState lvl = classifyLevel(cm);
  // Show the measured distance for testing (TFT + Serial)
  cDist = cm;  // keep the last raw median
  char line[48];
  if (isfinite(cDist)) {
    snprintf(line, sizeof(line), "Ultrasonic: %.1f cm", cDist);
    Serial.printf("[US] %.1f cm -> lvl %d\n", cDist, (int)lvl);
  } else {
    snprintf(line, sizeof(line), "Ultrasonic: --.- cm");
    Serial.println("[US] NaN -> keeping last level classification");
  }
//drawTextLine(10, 130, line, ILI9341_WHITE);
  // Always refresh LEDs on each measurement (forceWrite = true)
  lastBlinkMs = millis();
  blinkOn = true;
  waterLevel = lvl;
  applyLevelToLeds(waterLevel, blinkOn, /*forceWrite=*/true);
}
static void animateLevelLEDs(uint32_t now) {
  if (waterLevel != LVL_EMPTY) return;
  if (now - lastBlinkMs >= 400) {          // ~2.5 Hz blink
    lastBlinkMs = now;
    blinkOn = !blinkOn;
    applyLevelToLeds(waterLevel, blinkOn); // only pixel 0 toggles
  }
}
String getKeypadInput() {
  String input = "";
  tft.print("> "); // show input line
  while (true) {
    char key = keypad.getKey();
    if (key) {
      if (key == '#') break;          // confirm
      else if (key == '*') {          // backspace
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
    delay(100);
  }
  return input;
}
int askQuestion(String q) {
  clearScreen();
  tft.println(q);
  String ans = getKeypadInput();
  return ans.toInt();
}
bool userExists(String userID) {
  String base = "/devices/esp32-01/users/" + userID;
  // 1) Preferred: explicit marker
  if (Firebase.RTDB.getBool(&fbdo, base + "/exists") && fbdo.dataType() == "boolean") {
    return fbdo.boolData();
  }
  // 2) Fallbacks: if any core field exists, treat as existing
  if (Firebase.RTDB.getInt(&fbdo,     base + "/age"))    return true;
  if (Firebase.RTDB.getString(&fbdo, base + "/gender")) return true;
  if (Firebase.RTDB.getInt(&fbdo,    base + "/hours"))  return true;
  return false;
}
void saveUserToFirebase(User u) {
  String base = "/devices/esp32-01/users/" + u.id;
  bool ok1 = Firebase.RTDB.setInt(&fbdo, base + "/age", u.age);
  String genderStr = (u.gender == 1) ? "Male" : "Female";
  bool ok2 = Firebase.RTDB.setString(&fbdo, base + "/gender", genderStr);
  bool ok3 = Firebase.RTDB.setInt(&fbdo, base + "/hours", u.hours);
  bool ok4 = Firebase.RTDB.setFloat(&fbdo, base + "/weight", u.weight);
  bool ok5 = Firebase.RTDB.setFloat(&fbdo, base + "/height", u.height);
  // NEW: mark this user as existing + createdAt
  Firebase.RTDB.setBool(&fbdo, base + "/exists", true);
  Firebase.RTDB.setTimestamp(&fbdo, base + "/createdAt");
  Serial.printf("Write age:%s gender:%s hours:%s weight:%s height:%s\n",
                ok1 ? "OK" : fbdo.errorReason().c_str(),
                ok2 ? "OK" : fbdo.errorReason().c_str(),
                ok3 ? "OK" : fbdo.errorReason().c_str(),
                ok4 ? "OK" : fbdo.errorReason().c_str(),
                ok5 ? "OK" : fbdo.errorReason().c_str());
}
bool loadUserFromFirebase(String userID, User &u) {
  String base = "/devices/esp32-01/users/" + userID;
  bool success = true;
  if (Firebase.RTDB.getInt(&fbdo, base + "/age"))    u.age = fbdo.intData(); else success = false;
  if (Firebase.RTDB.getString(&fbdo, base + "/gender")) {
    String g = fbdo.stringData();
    u.gender = (g.equalsIgnoreCase("Male") ? 1 : 2);
  } else success = false;
  if (Firebase.RTDB.getInt(&fbdo, base + "/hours"))  u.hours  = fbdo.intData();   else success = false;
  if (Firebase.RTDB.getFloat(&fbdo, base + "/weight")) u.weight = fbdo.floatData(); else success = false;
  if (Firebase.RTDB.getFloat(&fbdo, base + "/height")) u.height = fbdo.floatData(); else success = false;
  u.id = userID;
  return success;
}
struct DayPlan {
  float dailyWaterBase;      // mL
  float dailyWaterAdj;       // mL (after activity boost, same as Stage 3 adjusted)
  int   totalBreakMin;       // total rest time for the day
  int   breakIntervalMin;    // minutes between breaks (suggested)
  int   breaksPerDay;        // count
  int   breakLenMin;         // minutes per break (rounded)
  float mlPerBreak;          // mL to drink each break
};
// --- Stage 5 scheduler flags ---
bool schedulerReady = false;   // becomes true once we seed the timers
DayPlan todaysPlan;            // keep the chosen plan for the day
static int clampi(int v, int lo, int hi){ return v<lo?lo:(v>hi?hi:v); }
static float clampf(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v); }
// Compute total daily rest & a simple plan
DayPlan makeDailyPlan(const User& u, float tempC, float humRH,
                      float dailyWaterBase, float dailyWaterAdj,
                      bool boostActive)
{
  DayPlan p{};
  p.dailyWaterBase = dailyWaterBase;
  p.dailyWaterAdj  = dailyWaterAdj;
  // --- Total rest minutes ---
  int baseRest = 5 * clampi(u.hours, 1, 16);               // 5 min per planned hour
  int heatAdd  = (int)clampf((tempC - 24.0f), 0.0f, 10.0f); // +1 min/°C above 24, cap +10
  int total    = baseRest + heatAdd;
  if (boostActive) total = (int)(total * 1.20f);            // +20% when very active
  p.totalBreakMin = clampi(total, 10, 120);                 // keep within sane bounds
  // --- Spacing between breaks ---
  int interval = (boostActive || tempC >= 28.0f) ? 45 : 60; // 45 min if hot/active, else 60
  p.breakIntervalMin = interval;
  // planned work window (minutes)
  int dayWindowMin = clampi(u.hours, 1, 16) * 60;
  p.breaksPerDay   = clampi((dayWindowMin + interval - 1) / interval, 1, 24);
  // per-break length: split total rest across the number of breaks
  p.breakLenMin = clampi((p.totalBreakMin + p.breaksPerDay/2) / p.breaksPerDay, 2, 15);
  // water per break: split the adjusted daily water evenly
  p.mlPerBreak = p.dailyWaterAdj / (float)p.breaksPerDay;
  return p;
}
void schedulerInitFromPlan(const DayPlan& p, uint32_t nowMs){
  sched.breakIntervalMin  = p.breakIntervalMin;
  sched.breakLenMin       = p.breakLenMin;
  sched.mlPerBreak        = p.mlPerBreak;
  sched.remainingWaterMl  = p.dailyWaterAdj;  // start of day
  sched.remainingBreakMin = p.totalBreakMin;
  uint32_t firstWater = nowMs + minutesToMs(p.breakIntervalMin);
  uint32_t firstBreak = nowMs + minutesToMs(max(1, p.breakIntervalMin/2)); // half-interval offset
  sched.nextWaterDueMs = firstWater;
  sched.nextBreakDueMs = firstBreak;
  exRiseCount = 0;   // start a new window for edge counting
  exTriggeredThisWindow = false;
  exPendingResetBreak = false;
  exPendingResetWater = false;
  sched.currentDue   = EV_NONE;
  sched.state        = ST_IDLE;
  sched.stateStartMs = nowMs;
  sched.buzzing      = false;
}
// ========== Hydration Formula ==========
float calculateDailyWater(User u, float temp, float hum) {
  float base = 35.0f * u.weight;         // 35 mL per kg
  float activityAdj = u.hours * 150.0f;  // 150 mL per active hour
  float tempAdj = 0.0f;
  float humAdj = 0.0f;
  if (temp > 30)       tempAdj = 500.0f;
  else if (temp > 20)  tempAdj = 250.0f;
  if (hum < 40)        humAdj = 200.0f;
  float total = base + activityAdj + tempAdj + humAdj;
  if (u.gender == 1) total *= 1.05;      // male boost
  return total;
}
void updateActivityAndHydration(float &dailyWaterAdjusted, float dailyWaterBase) {
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);
  // Adafruit returns accel in m/s^2. Compute dynamic accel above gravity.
  float mag = sqrtf(a.acceleration.x*a.acceleration.x +
                    a.acceleration.y*a.acceleration.y +
                    a.acceleration.z*a.acceleration.z);
  const float g0 = 9.80665f;
  float dynamicAcc = fabsf(mag - g0);
  // discard impossible / noisy samples
  if (!isfinite(dynamicAcc) || dynamicAcc < 0.02f || dynamicAcc > 50.0f) return;
  // exponential smoothing to reduce jitter
  const float alpha = 0.25f;
  dynFilt = alpha * dynamicAcc + (1.0f - alpha) * dynFilt;
  uint32_t nowMs = millis();
  // state machine for a continuous "active block" (≥ 3s)
  if (dynFilt > ACTIVITY_THRESH) {
    if (!inActive) { inActive = true; activeStart = nowMs; blockCounted = false; }
    if (!blockCounted && (nowMs - activeStart) >= ACTIVE_MIN_MS) {
      recordActiveBlock(nowMs);
      blockCounted = true; // count only once per continuous streak
      // push to Firebase for visibility
      // (moved to background task): activity/active_blocks
      // will be pushed from the FreeRTOS firebaseTask
    }
  } else {
    inActive = false;
    // do not reset activeStart; it marks the last streak start for logic
  }
// --- Debounced rising-edge counter (No->Yes), stops at 10 and retimes once ---
  static uint32_t movingSinceMs = 0;
  const bool movingRaw = (dynFilt > ACTIVITY_THRESH);
  // debounce: require ~250 ms of continuous "moving" to count as a rise
  bool movingStable = false;
  if (movingRaw) {
    if (movingSinceMs == 0) movingSinceMs = nowMs;
    if (nowMs - movingSinceMs >= 250) movingStable = true;
  } else {
    movingSinceMs = 0;
  }
  // we only show the count now; keep exerciseNow for any internal use if needed
  exercisePrev = exerciseNow;
  exerciseNow  = movingStable;
  // If we already triggered this window, stop counting further
  if (!exTriggeredThisWindow) {
    // true rising edge: No -> Yes (with debounce)
    if (!exercisePrev && exerciseNow) {
      if (exRiseCount < EX_RISES_FOR_RECALC) exRiseCount++;   // count up to 10
      if (exRiseCount >= EX_RISES_FOR_RECALC) {
        // One-shot retime: pull the *remaining* time for the next break to half (>=60 s)
        exTriggeredThisWindow = true;
        exPendingResetBreak = false;
        exPendingResetWater = false;
        const uint32_t TARGET_MIN     = 10;                                 // show ~10:00 remaining
        const uint32_t MIN_LEFT_MIN   = 1;                                  // but never below ~1:00
        const uint32_t nowMs          = millis();
        auto retimeToTarget = [&](uint32_t &due){
          int32_t rem = (int32_t)(due - nowMs);
          if (rem > 0) {
            uint32_t target  = minutesToMs(TARGET_MIN);     // respects timeDiv
            uint32_t minLeft = minutesToMs(MIN_LEFT_MIN);
            uint32_t newLeft = (uint32_t)rem;
            if ((uint32_t)rem > target) newLeft = target;   // jump down to target if larger
            if (newLeft < minLeft)       newLeft = minLeft; // keep at least ~1:00
            due = nowMs + newLeft;
          }
        };
        retimeToTarget(sched.nextBreakDueMs);
        retimeToTarget(sched.nextWaterDueMs);
        // force the LCD countdowns to repaint to the new values (disables smoothing once)
        dispSecB = -1;
        dispSecW = -1;
        // freeze the display at 10 (stop counting beyond)
        exRiseCount = EX_RISES_FOR_RECALC;
      }
    }
  }
  // Do NOT change hydration totals here
  boostActive = false;
  dailyWaterAdjusted = dailyWaterBase;
}
// ----- Scheduler state machine -----
String mmss(int32_t ms){
  if (ms < 0) ms = 0;
  int s = (ms + 500)/1000;
  int m = s/60; s %= 60;
  char buf[16]; sprintf(buf, "%02d:%02d", m, s);
  return String(buf);
}
void serviceScheduler() {
  uint32_t now = millis();
  char k = keypad.getKey();
  switch (sched.state) {
    case ST_IDLE: {
      uint32_t nextDue = min_u32(sched.nextWaterDueMs, sched.nextBreakDueMs);
      if ((int32_t)(nextDue - now) <= 0) {
        sched.currentDue = (sched.nextWaterDueMs <= sched.nextBreakDueMs) ? EV_WATER : EV_BREAK;
        sched.state = ST_ALARM;
        sched.stateStartMs = now;
        buzzerStart();
      }
    } break;
    case ST_ALARM: {
      // steady tone; keep simple (no blocking beeps)
      if (k == '*') {
        buzzerStop();
        sched.stateStartMs = now;
        sched.state = (sched.currentDue == EV_WATER) ? ST_DRINK_GRACE : ST_IN_BREAK;
      }
    } break;
    case ST_DRINK_GRACE: {
      const uint32_t GRACE_MS = 3000;
      uint32_t elapsed = now - sched.stateStartMs;
      if (k == '#') {
        sched.remainingWaterMl = max(0.0f, sched.remainingWaterMl - sched.mlPerBreak);
        sched.nextWaterDueMs = now + minutesToMs(sched.breakIntervalMin);
        sched.state = ST_IDLE;
        sched.currentDue = EV_NONE;
        if (Firebase.ready()) {
          dirtyTotals = true;
          LogItem li{};
          li.kind = LOG_HYDRATION;
          li.ts_ms = millis();
          li.amount_ml = (int)roundf(sched.mlPerBreak);
          li.remaining_ml = (int)roundf(sched.remainingWaterMl);
          li.duration_min = 0;
          if (logQueue) xQueueSend(logQueue, &li, 0);
        }
        if (exTriggeredThisWindow) {
          exPendingResetWater = true;
          if (exPendingResetWater && exPendingResetBreak) {
            exTriggeredThisWindow = false;
            exRiseCount = 0;                         // <-- reset counter only after BOTH done
            exPendingResetWater = false;             // <-- clear flags for the next window
            exPendingResetBreak = false;
        }
        exercisePrev = false;}
        // Defer re-measure so the user can close the lid
        ultraDueMs = millis() + ULTRA_DELAY_MS;
      } 
      else if (elapsed >= GRACE_MS) {
        // re-alarm
        sched.state = ST_ALARM;
        sched.stateStartMs = now;
        buzzerStart();
      }
    } break;
    case ST_IN_BREAK: {
      
      uint32_t durMs   = minutesToMs(sched.breakLenMin);   // respects TIME_SCALE
      uint32_t elapsed = now - sched.stateStartMs;
      if (elapsed >= durMs || k == '#') {
        // minutes completed, respecting TIME_SCALE
        // minutes completed, respecting demo scaling (minutes→ms via minutesToMs)
        uint32_t oneMinMs = minutesToMs(1);             // scaled size of 1 "planned minute"
        int doneMin = (oneMinMs > 0) ? (int)(elapsed / oneMinMs) : 0;
        if (doneMin > sched.breakLenMin) doneMin = sched.breakLenMin;
        if (doneMin < 0) doneMin = 0;
        sched.remainingBreakMin = max(0, sched.remainingBreakMin - doneMin);
        sched.nextBreakDueMs = now + minutesToMs(sched.breakIntervalMin);
        // DO NOT reset here unconditionally.
        // Mark break completed; reset only if water was also completed.
        if (exTriggeredThisWindow) {
          exPendingResetBreak = true;
          if (exPendingResetWater && exPendingResetBreak) {
            exTriggeredThisWindow = false;
            exRiseCount = 0;                         // <-- reset counter only after BOTH done
            exPendingResetWater = false;             // <-- clear flags for the next window
            exPendingResetBreak = false;
          }
        }
        sched.state = ST_IDLE;
        sched.currentDue = EV_NONE;
        if (Firebase.ready() && !currentUser.id.isEmpty()) {
          dirtyTotals = true;
          LogItem li{};
          li.kind = LOG_BREAK;
          li.ts_ms = millis();
          li.amount_ml = 0;
          li.remaining_ml = sched.remainingBreakMin;
          li.duration_min = sched.breakLenMin;
          if (logQueue) xQueueSend(logQueue, &li, 0);
        }
        exercisePrev = false;
      }
    } break;
  }
}
// Uses your existing drawTextLine(x,y,text,color)
void paintStatus(){
  static float prevWater = -1;
  static int   prevBreakMin = -1;
  static uint32_t prevNextWater = 0, prevNextBreak = 0;
  static RunState prevState = (RunState)999;
  uint32_t now = millis();
  int32_t msW = (int32_t)(sched.nextWaterDueMs - now);
  int32_t msB = (int32_t)(sched.nextBreakDueMs - now);
  // Only clear once on first call
  static bool first = true;
  if (first) { clearScreen(); first = false; }
  // Line 1: remaining water
  if (prevWater < 0 || fabsf(prevWater - sched.remainingWaterMl) >= 1.0f) {
    char buf[48]; snprintf(buf, sizeof(buf), "Water left:  %.0f mL", sched.remainingWaterMl);
    drawTextLine(10, 20, buf, ILI9341_GREEN);
    prevWater = sched.remainingWaterMl;
  }
  // Line 2: remaining break minutes
  if (prevBreakMin != sched.remainingBreakMin) {
    char buf[48]; snprintf(buf, sizeof(buf), "Break left:  %d min", sched.remainingBreakMin);
    drawTextLine(10, 40, buf, ILI9341_YELLOW);
    prevBreakMin = sched.remainingBreakMin;
  }
  // --- Next water countdown (smooth) ---
  int secW_true = trueSecLeft(now, sched.nextWaterDueMs);
  int secW_disp = smoothStepDown(secW_true, dispSecW);
  if (secW_disp != dispSecW) {
    char buf[48];
    // mm:ss from smoothed seconds
    int m = secW_disp / 60, s = secW_disp % 60;
    snprintf(buf, sizeof(buf), "Next water:  %02d:%02d", m, s);
    drawTextLine(10, 60, buf, ILI9341_CYAN);
    dispSecW = secW_disp;
  }
  // --- Next break countdown (smooth) ---
  int secB_true = trueSecLeft(now, sched.nextBreakDueMs);
  int secB_disp = smoothStepDown(secB_true, dispSecB);
  if (secB_disp != dispSecB) {
    char buf[48];
    int m = secB_disp / 60, s = secB_disp % 60;
    snprintf(buf, sizeof(buf), "Next break:  %02d:%02d", m, s);
    drawTextLine(10, 80, buf, ILI9341_CYAN);
    dispSecB = secB_disp;
  }
  // Line: Exercise: Yes/No + rising-edge count (redraw on state or count change)
  static uint8_t prevExCount  = 255;
  if (prevExCount != exRiseCount) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Exercise count: %u", (unsigned)exRiseCount);
    drawTextLine(10, 150, buf, ILI9341_WHITE);
    prevExCount = exRiseCount;
  }
  // Line 5: state line
  if (prevState != sched.state) {
    switch (sched.state) {
      case ST_ALARM:       drawTextLine(10, 110, "ALARM! Press *", ILI9341_RED); break;
      case ST_DRINK_GRACE: drawTextLine(10, 110, "Drink now (3s). Press #", ILI9341_MAGENTA); break;
      case ST_IN_BREAK: {
        uint32_t now = millis();
        uint32_t leftMs = minutesToMs(sched.breakLenMin) - (now - sched.stateStartMs);
        if ((int32_t)leftMs < 0) leftMs = 0;
        int sec = (leftMs + 999) / 1000;
        char buf[48];
        snprintf(buf, sizeof(buf), "On break... %02d:%02d left", sec/60, sec%60);
        drawTextLine(10, 110, buf, ILI9341_ORANGE);
      } break;
      default:             drawTextLine(10, 110, "Waiting...", ILI9341_WHITE); break;
    }
    prevState = sched.state;
  }
  // If in break, update the “left” every second without clearing everything
  if (sched.state == ST_IN_BREAK && (now % 1000) < 50) {
    uint32_t left = minutesToMs(sched.breakLenMin) - (now - sched.stateStartMs);
    char buf[48]; snprintf(buf, sizeof(buf), "On break... %s left", mmss(left).c_str());
    drawTextLine(10, 110, buf, ILI9341_ORANGE);
  }
}
// ---- Bounded keypad input helpers ----
int askIntBounded(const String& q, int minV, int maxV) {
  while (true) {
    clearScreen();
    tft.println(q + " (#=OK,*=DEL)");
    String s = getKeypadInput();   // your existing helper
    long v = s.toInt();
    if (s.length() > 0 && v >= minV && v <= maxV) return (int)v;
    clearScreen();
    tft.setTextColor(ILI9341_RED); tft.setTextSize(2);
    tft.println("Invalid. Range:");
    tft.printf("%d .. %d\n", minV, maxV);
    delay(900);
  }
}
float askFloatBounded(const String& q, float minV, float maxV) {
  while (true) {
    clearScreen();
    tft.println(q + " (#=OK,*=DEL)");
    String s = getKeypadInput();
    float v = s.toFloat();
    if (s.length() > 0 && v >= minV && v <= maxV) return v;
    clearScreen();
    tft.setTextColor(ILI9341_RED); tft.setTextSize(2);
    tft.println("Invalid. Range:");
    tft.printf("%.0f .. %.0f\n", minV, maxV);
    delay(900);
  }
}
static void ledFill(uint8_t r, uint8_t g, uint8_t b) {
  for (int i=0; i<LED_COUNT; ++i) leds.setPixelColor(i, leds.Color(r,g,b));
  leds.show();
}
static void ledSelfTest() {
  Serial.println("[LED] Self-test start");
  uint8_t oldB = 40; // default you use later
  // If your library has getBrightness(), you can read it; else we just restore to 40 later.
  leds.setBrightness(100);  // brighter for the test
  ledFill(150, 0,   0  ); delay(200); // red
  ledFill(0,   150, 0  ); delay(200); // green
  ledFill(0,   0,   150); delay(200); // blue
  ledFill(120, 120, 120); delay(200); // white
  ledFill(0,   0,   0  );             // off
  leds.setBrightness(oldB);
  Serial.println("[LED] Self-test done");
}
// ---------- Setup ----------
void setup() {
  
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  tft.begin();
  tft.setRotation(1);
  ledcSetup(BUZZ_CH, 3500, 8);          // Channel, frequency, resolution (8-bit)
  ledcAttachPin(BUZZER_PIN, BUZZ_CH);   // Attach the buzzer pin to the channel
  // LED strip + Ultrasonic I/O
  leds.begin();
  leds.setBrightness(40);
  leds.clear();
  leds.show();
//ledSelfTest();
  pinMode(US_TRIG, OUTPUT);
  pinMode(US_ECHO, INPUT_PULLDOWN);
  digitalWrite(US_TRIG, LOW);
  showMessage("Connecting WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }
  Serial.println("WiFi connected!");
  Serial.println(WiFi.localIP());
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.windows.com");
  setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();
  struct tm tmnow;
  for (int i = 0; i < 50; i++) {
    if (getLocalTime(&tmnow, 100)) break;  // 100 ms per try
    delay(100);
  }
  config.timeout.serverResponse = 10000;     // 10 s
  config.timeout.socketConnection = 10000;   // 10 s
  WiFi.setSleep(false);  // reduce Wi-Fi power-save stalls during SSL
  delay(1000);  
  showMessage("\nConnecting Firebase...");
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  bool fb_ready = false;
  for (int i = 0; i < 3; i++) {
    Serial.printf("Connecting to Firebase (attempt %d)...\n", i + 1);
    Firebase.begin(&config, &auth);
    if (Firebase.ready()) {
      fb_ready = true;
      break;
    }
  }
  if (!fb_ready) {
    Serial.println("Firebase failed after 3 tries!");
    clearScreen();
    showMessage("Firebase failed!");
    delay(1000);
  } else {
    Serial.println("Firebase connected successfully.");
    clearScreen();
    showMessage("Firebase OK!");
    delay(1000);
  }
  Firebase.reconnectWiFi(true);
  clearScreen();
  tft.println("Enter User ID (#=OK,*=DEL):");
  String userID = getKeypadInput();
  userID.trim(); 
  if (userExists(userID)) {
    // ---------------- Existing User ----------------
    clearScreen();
    showMessage("Syncing user...");
    if (loadUserFromFirebase(userID, currentUser)) {
      clearScreen();
      showMessage("Welcome back!");
      beepShort();
    } else {
      clearScreen();
      showMessage("Error loading user data!");
      delay(2000);
    }
  } else {
    // ---------------- New User Registration ----------------
    User u;
    u.id = userID;
    int genderVal = 0;
    while (true) {
      genderVal = askQuestion("Gender (1-M,2-F):");
      if (genderVal == 1 || genderVal == 2) break;      // valid input
      clearScreen();
      showMessage("Invalid input!\nUse 1=Male, 2=Female");
      delay(1200);
    }
    u.gender = genderVal;
    // gender is already validated (1 or 2) — keep that loop you added
    u.age    = askIntBounded  ("Age:",                1, 119);  // <120
    u.hours  = askIntBounded  ("Hours per day (Work/Study):",      0, 23);   // <24
    u.weight = askFloatBounded("Weight (kg):",        1, 399);  // <400
    u.height = askFloatBounded("Height (cm):",        30, 299); // <300
    saveUserToFirebase(u);
    
    // Immediately make this the active user
    currentUser = u;
    delay(1000);
    clearScreen();
    drawTextLine(10, 100, "User created!", ILI9341_GREEN);
    beepShort();
  }
  Firebase.RTDB.setString(&fbdo, "/devices/esp32-01/users/currentId", currentUser.id);
  delay(3000);
  // Start BME280
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  bmeOk = bme.begin(0x76, &Wire) || bme.begin(0x77, &Wire);
  if (!bmeOk) {
    drawTextLine(10, 100, "BME not found", ILI9341_GREEN);
    while (1) delay(1000);
  }
  drawTextLine(10, 100, "Reading Sensors....", ILI9341_GREEN);
  delay(800);
  // ---- MPU6050 init (Stage 3) ----
  WireMPU.begin(MPU_SDA, MPU_SCL);  // pins from your defines (41,40), 100 kHz
  WireMPU.setClock(100000);
  WireMPU.setTimeOut(50); 
  if (!mpu.begin(0x68, &WireMPU)) {         // typical address 0x68
    clearScreen();
    drawTextLine(10, 100, "MPU6050 not found!", ILI9341_RED);
    while (1) delay(100);
  }
  // Set reasonable ranges and filtering
  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  mpu.setGyroRange(MPU6050_RANGE_250_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  drawTextLine(10, 140, "MPU6050 OK!", ILI9341_CYAN);
  delay(500);
  // Read environment once to seed the plan
  float temp0 = bme.readTemperature();
  float hum0  = bme.readHumidity();
  // Base hydration (Stage 2)
  float dailyBase0 = calculateDailyWater(currentUser, temp0, hum0);
  // Adjusted hydration (Stage 3). At boot boostActive is likely false; that's fine.
  float dailyAdj0  = dailyBase0;
  // Build daily plan (Stage 4)
  todaysPlan = makeDailyPlan(currentUser, temp0, hum0, dailyBase0, dailyAdj0, /*boostActive=*/false);
  // Seed the scheduler (Stage 5) ONE TIME
  schedulerInitFromPlan(todaysPlan, millis());
  schedulerReady = true;
  dispSecW = -1;
  dispSecB = -1;
  // Optional UI line
  clearScreen();
  drawTextLine(10, 100, "Schedule ready", ILI9341_GREEN);
  delay(600);
  // Seed the scheduler (Stage 5)
  schedulerInitFromPlan(todaysPlan, millis());
  schedulerReady = true;
  dispSecW = -1;
  dispSecB = -1;
  // DEFER the very first ultrasonic snapshot (lid time)
  ultraDueMs = millis() + ULTRA_DELAY_MS;
  // === Create log queue and start Firebase background task ===
  logQueue = xQueueCreate(10, sizeof(LogItem)); // holds up to 10 log items
  fbMutex = xSemaphoreCreateMutex();
  
}
static void serviceFirebaseSync(uint32_t now) {
  static uint32_t lastSync = 0;
  static uint32_t nextTryMs = 0;
  static uint8_t  failCount = 0;
  const uint32_t SYNC_MS   = 30000;
  if (now < nextTryMs) return;
  if (now - lastSync < SYNC_MS) return;
  lastSync = now;
  // Back off if Wi-Fi is down
  if (WiFi.status() != WL_CONNECTED) {
    failCount = (failCount < 10) ? (failCount + 1) : 10;
    nextTryMs = now + 1000 * failCount; // 1s..10s
    return;
  }
  if (!Firebase.ready()) return;
  bool ok = true;
  // 1) Heartbeat
  ok &= Firebase.RTDB.setBool(&fbdo, "/devices/esp32-01/status/online", true);
  delay(1);
  ok &= Firebase.RTDB.setTimestamp(&fbdo, "/devices/esp32-01/status/lastSeen");
  delay(1);
  // 2) Demo speed (minutes→seconds) — auto-reanalyse if changed
  {
    uint16_t newDiv = timeDiv;
    if (Firebase.RTDB.getInt(&fbdo, "/devices/esp32-01/settings/time_divisor")) {
      int v = fbdo.intData(); if (v < 1) v = 1; if (v > 3600) v = 3600;
      newDiv = (uint16_t)v;
    }
    if (newDiv != timeDiv) {
      timeDiv = newDiv;
      recalcRequested = true;       // rebuild due times with new scale
      dispSecW = -1; dispSecB = -1; // instant repaint on TFT
    }
  }
  delay(1);
  if (!currentUser.id.isEmpty()) {
    const String u = "/devices/esp32-01/users/" + currentUser.id;
    // 3) Flush totals if changed
    if (dirtyTotals) {
      ok &= Firebase.RTDB.setFloat(&fbdo, u + "/hydration/remaining_ml", sched.remainingWaterMl);
      delay(1);
      ok &= Firebase.RTDB.setInt   (&fbdo, u + "/breaks/remaining_min",   sched.remainingBreakMin);
      delay(1);
      ok &= Firebase.RTDB.setInt   (&fbdo, u + "/events/ts",  (int)(millis()/1000));
      delay(1);
      dirtyTotals = false;
    }
    // 4) Drain up to 4 queued logs
    LogItem li;
    for (int i = 0; i < 4; i++) {
      if (xQueueReceive(logQueue, &li, 0) != pdTRUE) break;
      const String day = todayStr();
      String base = u + "/history/" + day;
      FirebaseJson j;
      j.set("ts", (int)(li.ts_ms/1000));
      if (li.kind == LOG_HYDRATION) {
        base += "/hydration";
        j.set("amount_ml",    li.amount_ml);
        j.set("remaining_ml", li.remaining_ml);
      } else if (li.kind == LOG_BREAK) {
        base += "/breaks";
        j.set("duration_min", li.duration_min);
        j.set("remaining_min",li.remaining_ml);
      } else continue;
      ok &= Firebase.RTDB.pushJSON(&fbdo, base.c_str(), &j);
      delay(1);
    }
    // 5) Commands
    if (Firebase.RTDB.getBool(&fbdo, "/devices/esp32-01/commands/reanalyse") && fbdo.boolData()) {
      recalcRequested = true;
      Firebase.RTDB.setBool(&fbdo, "/devices/esp32-01/commands/reanalyse", false);
      delay(1);
    }
    if (Firebase.RTDB.getBool(&fbdo, "/devices/esp32-01/commands/skipNext") && fbdo.boolData()) {
      skipNextRequested = true;
      Firebase.RTDB.setBool(&fbdo, "/devices/esp32-01/commands/skipNext", false);
      delay(1);
    }
    // 6) Lightweight activity metrics
    Firebase.RTDB.setInt (&fbdo, u + "/activity/active_blocks", blocksInWindow(millis()));
    delay(1);
    Firebase.RTDB.setBool(&fbdo, u + "/activity/boost_active",  boostActive);
    delay(1);
  }
  // Circuit breaker
  if (!ok) {
    failCount = (failCount < 8) ? (failCount + 1) : 8;
    nextTryMs = now + (1000U << (failCount - 1)); // 1s..~128s
  } else {
    failCount = 0;
    nextTryMs = now; // allow normal cadence
  }
}
 
void loop() {
  static uint32_t lastEnvMs = 0;
  static float     tempC_cached = NAN, humRH_cached = NAN;
  const uint32_t ENV_PERIOD_MS = 60000;   // read BME every 60 s
  const uint32_t UI_PERIOD_MS  = 200;     // UI refresh cadence
  static uint32_t lastUiMs     = 0;
  uint32_t now = millis();
  // 1) Environment: poll BME at low cadence and cache results
  if (now - lastEnvMs >= ENV_PERIOD_MS) {
    lastEnvMs = now;
    float t = bme.readTemperature();
    float h = bme.readHumidity();
    if (!isnan(t)) tempC_cached = t;
    if (!isnan(h)) humRH_cached = h;
    // Push env to Firebase only when we actually polled
    if (Firebase.ready() && !currentUser.id.isEmpty()) {
      String base = "/devices/esp32-01/users/" + currentUser.id + "/env";
      if (fbMutex && xSemaphoreTake(fbMutex, pdMS_TO_TICKS(8000))) {
        Firebase.RTDB.setFloat(&fbdo, base + "/tempC", tempC_cached);
        Firebase.RTDB.setFloat(&fbdo, base + "/humRH", humRH_cached);
        xSemaphoreGive(fbMutex);
      }
    }
  }
  // 2) Hydration totals (use cached env; fall back to reasonable defaults)
  float tempUse = isfinite(tempC_cached) ? tempC_cached : 25.0f;
  float humUse  = isfinite(humRH_cached) ? humRH_cached : 50.0f;
  float dailyBase = calculateDailyWater(currentUser, tempUse, humUse);
  // Stage 3 activity adjustment (also updates global boostActive)
  float dailyAdj = dailyBase;
  updateActivityAndHydration(dailyAdj, dailyBase);
  // 3) Live tweaks for scheduler (do NOT re-initialize the scheduler here)
  //    - tighten interval when boostActive, else use plan's interval
  sched.breakIntervalMin = boostActive ? 45 : todaysPlan.breakIntervalMin;
  //    - recompute future mL per break from today's adjusted total
  int plannedBreaksPerDay = todaysPlan.breaksPerDay > 0 ? todaysPlan.breaksPerDay : 1;
  sched.mlPerBreak = dailyAdj / (float)plannedBreaksPerDay;
  // 4) Service the non-blocking scheduler + UI
  if (schedulerReady) {
    serviceScheduler();
  }
  // Handle "reanalyse" command signalled by the Firebase task
  if (recalcRequested) {
    recalcRequested = false;
    // reuse cached env if available to avoid blocking reads
    float t = isfinite(tempC_cached) ? tempC_cached : bme.readTemperature();
    float h = isfinite(humRH_cached) ? humRH_cached : bme.readHumidity();
    float base = calculateDailyWater(currentUser, t, h);
    float adj  = base * (boostActive ? 1.15f : 1.0f);
    todaysPlan = makeDailyPlan(currentUser, t, h, base, adj, boostActive);
    schedulerInitFromPlan(todaysPlan, millis());
    schedulerReady = true;
    // ADD THIS so both countdowns repaint immediately after rescale/reanalyse
    dispSecW = -1;
    dispSecB = -1;
    // tell the background task to push new totals on its next tick
    dirtyTotals = true;
  }
  // Fast test: jump to the next alert in ~2 seconds
  if (skipNextRequested) {
    skipNextRequested = false;
    uint32_t nowMs = millis();
    if (sched.nextWaterDueMs <= sched.nextBreakDueMs) {
      sched.nextWaterDueMs = nowMs + 2000;   // Water will fire in ~2s
    } else {
      sched.nextBreakDueMs = nowMs + 2000;   // Break will fire in ~2s
    }
  }
  if (now - lastUiMs >= UI_PERIOD_MS) {
    lastUiMs = now;
    paintStatus();  // draws: remaining water/breaks, next timers, current state
  }
  // --- deferred ultrasonic snapshot + empty LED blink ---
  if (ultraDueMs && (int32_t)(now - ultraDueMs) >= 0) {
    ultraDueMs = 0;
    ultraMeasureAndUpdate(/*force=*/true);
  }
  animateLevelLEDs(now);
// ---- Periodic Firebase sync (deferred during interactive states) ----
  delay(5); // snappier key polling
  bool interactive = (sched.state == ST_ALARM ||
                      sched.state == ST_DRINK_GRACE ||
                      sched.state == ST_IN_BREAK);
  if (!interactive) {
    serviceFirebaseSync(now);   // <— no goto, no label, no compile error
  }
}
