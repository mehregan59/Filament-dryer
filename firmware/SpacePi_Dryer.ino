/*
 * SpacePi Dryer — custom ESP32 controller — Mod v01 Mike (firmware v9 OTA — root-cause state separation)
 *
 * OTA: Tools->Port->spacepi-dryer, password: dryer2024
 * Board: Elecrow 3.5" ESP32-WROOM-32 HMI (ILI9488 480x320, resistive touch)
 *
 * IO25 -> SSR input (+)   220V heater  |  IO32 -> IRLZ44N gate  12V fans
 * IO22 -> SHT40 SDA       |  IO21 -> SHT40 SCL   (connector labeled I2C)
 *
 * v2: flicker-free draws (padding + sprites), flame + dual 4-blade fan
 * animation, gradient UI, run-screensaver, on-screen WiFi setup keyboard.
 * SAFETY: idle standby and active-cycle screensaver are separate states.
 * Heater control is impossible from standby.
 */

#include <Wire.h>
#include <TFT_eSPI.h>
#include <Adafruit_SHT4x.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoOTA.h>

// ================= USER CONFIG =================
const char* WIFI_SSID = "YOUR_WIFI_NAME";   // fallback if nothing configured on-screen
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* AP_SSID   = "SpacePi-Dryer";
const char* AP_PASS   = "dryer1234";
const char* TZ_INFO   = "CET-1CEST,M3.5.0,M10.5.0/3";  // Germany
const char* OTA_HOSTNAME = "spacepi-dryer";
const char* OTA_PASSWORD = "dryer2024";
#define TOUCH_THRESHOLD  145       // keep proven panel sensitivity; software filters false presses
#define ENABLE_IDLE_CLOCK 1         // 1 = show clock after timeout
#define IDLE_TIMEOUT_MS  30000UL   // used only when ENABLE_IDLE_CLOCK is 1
#define SAVER_TIMEOUT_MS 60000UL   // screensaver after 1 min untouched while drying

// ================= DISPLAY / CONTROL TUNING =================
#define TEMP_AVG_SAMPLES  180      // rolling average samples (1 per sec = 3 min window)
#define HYST_LOW   3.0f            // heater fires when this many deg C below target
#define HYST_HIGH  1.0f            // heater cuts when this many deg C above target
#define HUM_UPDATE_MS 180000UL     // humidity display updates every 3 minutes

// ================= PINS =================
#define PIN_HEATER  25
#define PIN_FAN     32
#define PIN_SDA     22
#define PIN_SCL     21
#define PIN_BL      27

// ================= SAFETY =================
#define MAX_TEMP_C          92.0f
#define MAX_SET_TEMP        85
#define MIN_SET_TEMP        35
#define RUNAWAY_WINDOW_MS   240000
#define RUNAWAY_MIN_RISE    2.0f
#define SENSOR_FAIL_LIMIT   8
#define COOLDOWN_TEMP       45.0f
#define FAULT_RESET_TEMP    50.0f
#define FAULT_RESET_TIMEOUT_MS 600000UL

// ================= PRESETS =================
struct Preset { const char* name; int tempC; int hours; uint16_t color; };
Preset presets[] = {
  {"PLA",   50,  6, 0x34BF}, {"PETG",  60,  6, 0x05FF}, {"ABS",   70,  8, 0xFD20},
  {"ASA",   70,  8, 0xFBE0}, {"PA/NY", 80, 12, 0xF81F}, {"PC",    85, 10, 0xF800},
};
const int N_PRESETS = 6;

// ================= STATE =================
enum Screen { SCR_HOME, SCR_RUN, SCR_DONE, SCR_SETTINGS, SCR_FAULT,
              SCR_SAVER,       // screensaver while a real drying cycle is active
              SCR_STANDBY,     // idle/home screensaver; NEVER runs heater control
              SCR_WIFI, SCR_KBD };
Screen screen = SCR_HOME;

int   selPreset = 0, setTemp = 50, setHours = 6, setMinutes = 0;
float curTemp = 0, curHum = 0;          // raw sensor values (control uses these)
float dispTemp = 0, dispHum = 0;        // smoothed values shown on display
bool  heaterOn = false, fanOn = false, sensorOk = false;
bool  manualFanMode = false;          // true only when user selected FAN ONLY
bool  saverEnabled = true;              // toggled in Settings, stored in prefs
uint8_t mainTheme = 0;                    // 0..3, selectable on LCD/web
uint8_t saverTheme = 0;                   // 0..3, selectable on LCD/web
bool antiFlicker = true;                  // redraw complete frames only when needed
uint8_t settingsPage = 0;                 // 0=appearance, 1=system

// rolling temperature average
float tempBuf[TEMP_AVG_SAMPLES];
int   tempBufIdx = 0;
bool  tempBufFull = false;
unsigned long lastHumUpdateMs = 0;
unsigned long runEndMs = 0, runStartMs = 0;
unsigned long lastSensorMs = 0, lastControlMs = 0, lastAnimMs = 0;
unsigned long lastTouchMs = 0, lastClockMs = 0;
int   sensorFails = 0;
char  faultMsg[40] = "";
bool  faultResetShown = false;
unsigned long faultAtMs = 0;
unsigned long rwStartMs = 0; float rwStartTemp = 0; bool rwArmed = false;
int   animFrame = 0;
char  clockStr[20] = "--:--";
bool  timeSynced = false;
uint16_t calData[5];
bool  wifiUp = false;
String cfgSsid, cfgPass;      // active WiFi credentials (prefs override constants)

// screensaver bounce
int svX = 60, svY = 80, svDX = 2, svDY = 2;

// screensaver sensor snapshot — updated every second, used directly by drawSaver()
float saverTemp = 0, saverHum = 0;
bool  saverHasData = false;

// wifi picker / keyboard
#define MAX_NETS 6
String netNames[MAX_NETS]; int netCount = 0;
String kbdSsid, kbdBuf; int kbdPage = 0;   // 0 lower, 1 upper, 2 symbols

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite anim  = TFT_eSprite(&tft);   // run screen animation strip (340x54)
TFT_eSprite sanim = TFT_eSprite(&tft);   // screensaver bottom animation bar (480x90)
TFT_eSprite saver = TFT_eSprite(&tft);   // unused placeholder kept for compatibility
Adafruit_SHT4x sht4;
WebServer server(80);
Preferences prefs;

// colors (RGB565)
#define C_BG      0x0904   // deep blue-black (gradient midpoint reference)
#define C_HDR     0x1928   // header band
#define C_CARD    0x2145
#define C_TEXT    0xFFFF
#define C_MUTED   0x8C71
#define C_ACCENT  0x05FF
#define C_HOT     0xFB40
#define C_FLAME_Y 0xFFE0
#define C_GOOD    0x07E8
#define C_BAD     0xF800

// ---------------------------------------------------------------
//                    SAFETY PRIMITIVES
// ---------------------------------------------------------------
//                    SAFETY PRIMITIVES
// ---------------------------------------------------------------
void heaterWrite(bool on) { heaterOn = on; digitalWrite(PIN_HEATER, on ? HIGH : LOW); }
void fanWrite(bool on)    { fanOn = on;    digitalWrite(PIN_FAN,    on ? HIGH : LOW); }

// ---------------------------------------------------------------
//                    RUN HISTORY & FAULT LOG
// ---------------------------------------------------------------
#define MAX_RUNS   100   // stored in NVS; oldest dropped when full
#define MAX_FAULTS 50

struct RunRecord {
  uint32_t startEpoch;    // unix timestamp at run start (0 if no NTP)
  uint16_t durationMin;   // planned duration in minutes
  uint16_t actualMin;     // actual minutes run before stop/fault/complete
  uint8_t  presetIdx;     // preset index (0-5)
  uint8_t  targetTemp;    // target °C
  int8_t   peakTemp;      // peak °C reached (int, saves space)
  uint8_t  startHum;      // humidity % at start
  uint8_t  endHum;        // humidity % at end
  uint8_t  outcome;       // 0=completed, 1=stopped, 2=faulted
};

struct FaultRecord {
  uint32_t epoch;
  char     code[8];       // E1/E2/E3
};

RunRecord   runLog[MAX_RUNS];
FaultRecord faultLog[MAX_FAULTS];
int runCount   = 0;
int faultCount = 0;

// tracking values for current run
float runPeakTemp  = 0;
float runStartHum  = 0;

void historyLoad() {
  runCount   = prefs.getInt("rcount", 0);
  faultCount = prefs.getInt("fcount", 0);
  if (runCount > 0)
    prefs.getBytes("runs",   runLog,   sizeof(RunRecord)   * min(runCount, MAX_RUNS));
  if (faultCount > 0)
    prefs.getBytes("faults", faultLog, sizeof(FaultRecord) * min(faultCount, MAX_FAULTS));
}

void historySaveRun(uint8_t outcome) {
  if (runCount >= MAX_RUNS) {
    memmove(runLog, runLog + 1, sizeof(RunRecord) * (MAX_RUNS - 1));
    runCount = MAX_RUNS - 1;
  }
  struct tm ti; uint32_t ep = 0;
  if (getLocalTime(&ti, 10)) ep = mktime(&ti);
  uint16_t actualMin = (runStartMs > 0) ? (uint16_t)((millis() - runStartMs) / 60000UL) : 0;
  runLog[runCount++] = {
    ep,
    (uint16_t)(setHours * 60 + setMinutes),
    actualMin,
    (uint8_t)selPreset,
    (uint8_t)setTemp,
    (int8_t)runPeakTemp,
    (uint8_t)runStartHum,
    (uint8_t)curHum,
    outcome
  };
  prefs.putInt("rcount", runCount);
  prefs.putBytes("runs", runLog, sizeof(RunRecord) * runCount);
}

void historySaveFault(const char* code) {
  if (faultCount >= MAX_FAULTS) {
    memmove(faultLog, faultLog + 1, sizeof(FaultRecord) * (MAX_FAULTS - 1));
    faultCount = MAX_FAULTS - 1;
  }
  struct tm ti; uint32_t ep = 0;
  if (getLocalTime(&ti, 10)) ep = mktime(&ti);
  faultLog[faultCount].epoch = ep;
  strncpy(faultLog[faultCount].code, code, 7);
  faultCount++;
  prefs.putInt("fcount", faultCount);
  prefs.putBytes("faults", faultLog, sizeof(FaultRecord) * faultCount);
}

// update peak temp during a run
void historyTrackRun() {
  if (screen == SCR_RUN && curTemp > runPeakTemp)
    runPeakTemp = curTemp;
}
void drawFault();  // fwd
void enterFault(const char* msg) {
  manualFanMode = false;
  heaterWrite(false);
  fanWrite(true);
  faultAtMs = millis();
  strncpy(faultMsg, msg, sizeof(faultMsg) - 1);
  historySaveFault(msg);
  if (runStartMs > 0) historySaveRun(2);   // outcome 2 = faulted
  runStartMs = 0;
  screen = SCR_FAULT;
  drawFault();
}

// ---------------------------------------------------------------
//                        SENSOR
// ---------------------------------------------------------------
void readSensor() {
  sensors_event_t h, t;
  if (sht4.getEvent(&h, &t)) {
    curTemp = t.temperature;
    curHum  = h.relative_humidity;

    // seed display values on first ever read so screensaver never shows 0.0
    static bool firstRead = true;
    if (firstRead) {
      dispTemp = curTemp;
      dispHum  = curHum;
      // fill entire rolling buffer with first reading so average is meaningful immediately
      for (int i = 0; i < TEMP_AVG_SAMPLES; i++) tempBuf[i] = curTemp;
      tempBufFull = true;
      firstRead = false;
    }

    sensorOk = true; sensorFails = 0;

    // keep saver snapshot current — updated on every good read
    saverTemp = curTemp;
    saverHum  = curHum;
    saverHasData = true;

    // rolling average for display temperature
    // but use raw curTemp directly until buffer has enough real samples
    tempBuf[tempBufIdx] = curTemp;
    tempBufIdx = (tempBufIdx + 1) % TEMP_AVG_SAMPLES;
    if (tempBufIdx == 0) tempBufFull = true;
    if (tempBufFull) {
      // full 3-min window available — use smoothed average
      float sum = 0;
      for (int i = 0; i < TEMP_AVG_SAMPLES; i++) sum += tempBuf[i];
      dispTemp = sum / TEMP_AVG_SAMPLES;
    } else {
      // buffer still filling — show raw value so screen is never blank
      dispTemp = curTemp;
    }

    // humidity display interval
    // during drying: update every 3 min once we have been running > 3 min
    // before that and in all other modes: update immediately
    unsigned long now2 = millis();
    bool drying = (screen == SCR_RUN || screen == SCR_SAVER);  // SCR_STANDBY is deliberately excluded
    unsigned long runningMs = (runStartMs > 0) ? (now2 - runStartMs) : 0;
    bool steadyDrying = drying && runningMs > HUM_UPDATE_MS;
    unsigned long humInterval = steadyDrying ? HUM_UPDATE_MS : 0UL;
    if (humInterval == 0 || now2 - lastHumUpdateMs >= humInterval) {
      dispHum = curHum;
      lastHumUpdateMs = now2;
    }
  } else if (++sensorFails >= SENSOR_FAIL_LIMIT) {
    sensorOk = false;
    if (screen == SCR_RUN) enterFault("E1 sensor lost");
  }
}

// ---------------------------------------------------------------
//                      CONTROL LOOP  (runs in RUN *and* SAVER)
// ---------------------------------------------------------------
void finishRun();  // fwd
void controlLoop() {
  // Root-cause fix: display state and machine state are separate.
  // Only SCR_RUN and SCR_SAVER belong to a real cycle. SCR_STANDBY can never
  // enter this control path, so selecting an idle screensaver cannot start outputs.
  if (screen != SCR_RUN && screen != SCR_SAVER) return;

  // A valid cycle must have both timestamps. This also protects against an
  // incomplete OTA/reboot state being interpreted as a finished or active run.
  if (runStartMs == 0 || runEndMs == 0) {
    heaterWrite(false);
    if (!manualFanMode && (!sensorOk || curTemp <= COOLDOWN_TEMP)) fanWrite(false);
    screen = SCR_HOME;
    drawHome();
    return;
  }

  if (sensorOk && curTemp >= MAX_TEMP_C) { enterFault("E2 over temperature"); return; }
  if ((long)(millis() - runEndMs) >= 0)  { finishRun(); return; }
  if (!sensorOk) return;

  bool want = heaterOn;
  // Adaptive hysteresis: wider band at high temps because PTC thermal mass is larger.
  // At 80-85C target the chamber overshoots more — cut later, fire earlier.
  float hystLow  = (setTemp >= 75) ? 4.0f : HYST_LOW;   // fire when N deg below target
  float hystHigh = (setTemp >= 75) ? 3.0f : HYST_HIGH;  // cut when N deg above target
  if (curTemp < (float)setTemp - hystLow)  want = true;
  if (curTemp > (float)setTemp + hystHigh) want = false;

  if (want && !heaterOn) { rwArmed = true; rwStartMs = millis(); rwStartTemp = curTemp; }
  if (!want) { rwArmed = false; }
  if (rwArmed && heaterOn) {
    // Thermal runaway window is longer at high targets — PTC heaters rise slowly above 70C
    unsigned long window = (setTemp >= 70) ? 480000UL : RUNAWAY_WINDOW_MS;  // 8 min vs 4 min
    // Minimum rise is smaller at high temps — 0.5C in 8 min is still real progress
    float minRise = (setTemp >= 70) ? 0.5f : RUNAWAY_MIN_RISE;
    if (millis() - rwStartMs > window) {
      if (curTemp - rwStartTemp < minRise) {
        enterFault("E3 no heat rise");
        return;
      }
      // reset window — keep checking each window
      rwStartMs = millis();
      rwStartTemp = curTemp;
    }
  }
  heaterWrite(want);
  fanWrite(true);
}

// ---------------------------------------------------------------
//                      CLOCK
// ---------------------------------------------------------------
void updateClock() {
  struct tm ti;
  if (getLocalTime(&ti, 50)) {
    timeSynced = true;
    strftime(clockStr, sizeof(clockStr), "%d.%m  %H:%M", &ti);
  }
}
void drawClock() {   // inside the header band
  tft.setTextColor(C_MUTED, C_HDR);
  tft.setTextDatum(TR_DATUM);
  tft.setTextPadding(120);
  tft.drawString(clockStr, 468, 9, 2);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------
//                      UI PRIMITIVES
// ---------------------------------------------------------------
void drawBackdrop() {          // vertical gradient, drawn once per screen change
  for (int y = 34; y < 320; y += 2) {
    float f = (y - 34) / 286.0f;
    uint8_t r = 10 - (uint8_t)(f * 8), g = 14 - (uint8_t)(f * 11), b = 34 - (uint8_t)(f * 26);
    uint16_t c = tft.color565(r, g, b);
    tft.drawFastHLine(0, y, 480, c);
    tft.drawFastHLine(0, y + 1, 480, c);
  }
}
void drawHeader(const char* title) {
  tft.fillRect(0, 0, 480, 34, C_HDR);
  tft.setTextColor(C_TEXT, C_HDR); tft.setTextDatum(TL_DATUM);
  tft.drawString(title, 12, 7, 4);
  tft.setTextColor(C_ACCENT, C_HDR);
  tft.drawString("Mod v01 Mike", 246, 13, 2);
  drawClock();
}
void newScreen(const char* title) { drawHeader(title); drawBackdrop(); }

void btn(int x, int y, int w, int h, const char* label, uint16_t col, uint16_t txt = C_TEXT) {
  tft.fillRoundRect(x, y, w, h, 8, col);
  tft.setTextColor(txt, col); tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2, 2);
}
bool hit(int tx, int ty, int x, int y, int w, int h) {
  return tx >= x && tx <= x + w && ty >= y && ty <= y + h;
}
void fmtTimeLeft(char* out, size_t n) {
  long s = ((long)(runEndMs - millis())) / 1000; if (s < 0) s = 0;
  snprintf(out, n, "%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
}

// ---------------------------------------------------------------
//                   IDLE CLOCK SCREEN (home when not drying)
// ---------------------------------------------------------------
#define N_PARTICLES 16
#define IDLE_BG 0x0204   // very dark blue-black
struct Particle { int x, y, dx, dy; uint16_t col; uint8_t r; };
Particle particles[N_PARTICLES];
bool particlesInited = false;
char idleClockStr[8]  = "--:--";
char idleDayStr[12]   = "---";
char idleDateStr[16]  = "--.--.-";
unsigned long lastIdleAnimMs = 0;
bool idleMode = false;

uint16_t randColor() {
  uint8_t w = random(0, 5);
  if (w == 0) return C_ACCENT;
  if (w == 1) return C_HOT;
  if (w == 2) return 0x539F;   // purple
  if (w == 3) return C_GOOD;
  return C_MUTED;
}

void initParticles() {
  for (int i = 0; i < N_PARTICLES; i++) {
    particles[i].x   = random(10, 470);
    particles[i].y   = random(10, 310);
    particles[i].dx  = random(0, 2) ? 1 : -1;
    particles[i].dy  = random(0, 2) ? 1 : -1;
    particles[i].col = randColor();
    particles[i].r   = random(2, 4);
  }
  particlesInited = true;
}

// Idle screen uses a full 480x320 sprite — zero trail artifacts
// idleSprite removed - uses direct TFT drawing to save RAM
bool idleSpriteReady = false;  // unused

// flip-clock: track previous time chars to animate only changed digits
char prevClockChars[6] = "     ";  // HH:MM\0
char prevDayStr2[12]   = "";
char prevDateStr2[16]  = "";

// draw one flip-clock digit with sliding animation
void drawFlipDigit(TFT_eSprite& spr, int x, int y, int w, int h,
                   char oldC, char newC, int flipFrame, int totalFrames) {
  uint16_t bg   = 0x0A18;   // dark card
  uint16_t fg   = C_TEXT;
  uint16_t fold = C_MUTED;
  spr.fillRoundRect(x, y, w, h, 6, bg);
  if (oldC != newC && flipFrame < totalFrames) {
    // top half shows old number sliding up, bottom half shows new number sliding in
    float p = (float)flipFrame / totalFrames;
    int mid = y + h / 2;
    // upper half: old char moving up (clip top portion)
    int oy = y + (int)(p * h / 2) - h / 4;
    spr.setTextColor(fold, bg); spr.setTextDatum(MC_DATUM);
    char s[2] = {oldC, 0};
    spr.drawString(s, x + w/2, oy + h/4, 6);
    // lower half: new char coming from bottom
    int ny = mid + (int)((1.0f - p) * h / 2);
    spr.setTextColor(fg, bg);
    s[0] = newC;
    spr.drawString(s, x + w/2, ny + h/4, 6);
    // centre fold line
    spr.drawFastHLine(x, mid, w, 0x1A3A);
  } else {
    char s[2] = {newC, 0};
    spr.setTextColor(fg, bg); spr.setTextDatum(MC_DATUM);
    spr.drawString(s, x + w/2, y + h/2, 6);
  }
}

// flip animation state
int  flipFrame = 0;
bool flipping  = false;
char flipFrom[6] = "00:00";
char flipTo[6]   = "00:00";
#define FLIP_FRAMES 8

void drawIdleScreen(bool full) {
  // Card zone: x=38..442, y=96..182 — particles excluded here entirely
  // Background: pure black throughout
  const int CARD_X1=38, CARD_X2=442, CARD_Y1=90, CARD_Y2=195;

  if (full) {
    tft.fillScreen(TFT_BLACK);
    flipping = false; flipFrame = 0;
    strncpy(flipFrom, "00:00", 5);
    strncpy(flipTo,   "00:00", 5);
    initParticles();
  }

  // --- PARTICLES: erase old, move, draw — skip card+text zone ---
  static int oldX[N_PARTICLES], oldY[N_PARTICLES];
  static bool firstDraw = true;

  if (!firstDraw) {
    for (int i = 0; i < N_PARTICLES; i++) {
      bool inZone = (oldY[i] >= 22 && oldY[i] <= CARD_Y2);
      if (!inZone) tft.fillCircle(oldX[i], oldY[i], particles[i].r + 1, TFT_BLACK);
    }
  }
  for (int i = 0; i < N_PARTICLES; i++) {
    particles[i].x += particles[i].dx;
    particles[i].y += particles[i].dy;
    if (particles[i].x < 4 || particles[i].x > 476) {
      particles[i].dx = -particles[i].dx;
      particles[i].x = constrain(particles[i].x, 4, 476);
    }
    if (particles[i].y < 4 || particles[i].y > 316) {
      particles[i].dy = -particles[i].dy;
      particles[i].y = constrain(particles[i].y, 4, 316);
    }
    oldX[i] = particles[i].x;
    oldY[i] = particles[i].y;
  }
  firstDraw = false;

  for (int i = 0; i < N_PARTICLES; i++) {
    // skip entire text+clock band (y 22 to CARD_Y2)
    if (particles[i].y >= 22 && particles[i].y <= CARD_Y2) continue;
    tft.fillCircle(particles[i].x, particles[i].y, particles[i].r, particles[i].col);
  }

  // --- TIME ---
  struct tm ti;
  if (getLocalTime(&ti, 10)) {
    strftime(idleClockStr, sizeof(idleClockStr), "%H:%M", &ti);
    strftime(idleDayStr,   sizeof(idleDayStr),   "%A",    &ti);
    strftime(idleDateStr,  sizeof(idleDateStr),  "%d . %m . %Y", &ti);
    if (strcmp(idleClockStr, flipTo) != 0 && !flipping) {
      if (flipTo[0] == '0' && flipTo[1] == '0') strncpy(flipFrom, idleClockStr, 5);
      else strncpy(flipFrom, flipTo, 5);
      strncpy(flipTo, idleClockStr, 5);
      if (strcmp(flipFrom, flipTo) != 0) { flipping = true; flipFrame = 0; }
    }
  }

  // --- TOP ROW: temp, hum, wifi ---
  if (sensorOk) {
    char v[16];
    snprintf(v, sizeof(v), "%.1f C", dispTemp);
    tft.setTextColor(C_HOT, TFT_BLACK); tft.setTextDatum(TL_DATUM);
    tft.setTextPadding(110); tft.drawString(v, 8, 6, 2);
    snprintf(v, sizeof(v), "%.0f %%", dispHum);
    tft.setTextColor(C_ACCENT, TFT_BLACK); tft.setTextDatum(TR_DATUM);
    tft.setTextPadding(90);  tft.drawString(v, 420, 6, 2);
    tft.setTextPadding(0);
  }
  tft.fillCircle(462, 12, 6, wifiUp ? C_GOOD : 0x2945);

  // --- DAY + DATE ---
  tft.setTextColor(C_TEXT, TFT_BLACK); tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(340); tft.drawString(idleDayStr,  240, 34, 4);
  tft.setTextColor(0x8C71, TFT_BLACK);
  tft.setTextPadding(360); tft.drawString(idleDateStr, 240, 66, 4);
  tft.setTextPadding(0);
  tft.drawFastHLine(80, 84, 320, 0x2124);

  // --- FLIP CLOCK ---
  const int digW=72, digH=84, gap=6, colonW=20;
  const int x0=(480-(4*digW+3*gap+colonW))/2, dy=96;
  const int xpos[4]={x0, x0+digW+gap, x0+digW*2+gap*2+colonW, x0+digW*3+gap*3+colonW};
  const uint16_t cardBg=0x0C1A2E;  // dark navy — distinct from black

  for (int d=0; d<4; d++) {
    int ci=(d<2)?d:d+1;
    char newC=(flipTo[ci]  && flipTo[ci]!=':') ? flipTo[ci]  : '0';
    char oldC=(flipFrom[ci] && flipFrom[ci]!=':') ? flipFrom[ci] : newC;

    // draw card background (always, covers any particle that snuck in)
    tft.fillRoundRect(xpos[d], dy, digW, digH, 8, cardBg);
    tft.drawFastHLine(xpos[d], dy+digH/2, digW, 0x1828);

    tft.setTextDatum(MC_DATUM);
    if (flipping && oldC != newC) {
      // DROP: new digit falls from top, old exits at bottom
      float p = (float)flipFrame / FLIP_FRAMES;
      int shift = (int)(p * digH);

      // new digit: starts above card, drops into center
      int newCY = dy - digH + shift + digH/2;
      if (newCY >= dy && newCY <= dy+digH) {
        tft.setTextColor(C_TEXT, cardBg);
        char s[2]={newC,0}; tft.drawString(s, xpos[d]+digW/2, newCY, 6);
      }
      // old digit: starts at center, falls below card
      int oldCY = dy + shift + digH/2;
      if (oldCY >= dy && oldCY <= dy+digH) {
        tft.setTextColor(0x506070, cardBg);
        char s[2]={oldC,0}; tft.drawString(s, xpos[d]+digW/2, oldCY, 6);
      }
    } else {
      tft.setTextColor(C_TEXT, cardBg);
      char s[2]={newC,0};
      tft.drawString(s, xpos[d]+digW/2, dy+digH/2, 6);
    }
  }

  // colon
  int cx=xpos[1]+digW+gap+colonW/2;
  tft.fillCircle(cx, dy+digH/3,   5, C_TEXT);
  tft.fillCircle(cx, dy+2*digH/3, 5, C_TEXT);

  if (flipping) {
    flipFrame++;
    if (flipFrame >= FLIP_FRAMES) { flipping=false; strncpy(flipFrom,flipTo,5); }
  }

  // hint
  tft.setTextColor(0x1828, TFT_BLACK); tft.setTextDatum(MC_DATUM);
  tft.drawString("touch to start", 240, 308, 2);
}
void touchIdleScreen(int tx, int ty) {
  idleMode = false;
  drawHome();
}
void drawPresets() {
  for (int i = 0; i < N_PRESETS; i++) {
    int x = 12 + (i % 3) * 156, y = 44 + (i / 3) * 52;
    uint16_t bg = (i == selPreset) ? presets[i].color : C_CARD;
    uint16_t fg = (i == selPreset) ? 0x0000 : C_TEXT;
    char lbl[24];
    snprintf(lbl, sizeof(lbl), "%s %dC %dh", presets[i].name, presets[i].tempC, presets[i].hours);
    btn(x, y, 148, 44, lbl, bg, fg);
  }
}
void drawTempVal() {
  char v[8]; snprintf(v, sizeof(v), "%d C", setTemp);
  tft.setTextColor(C_HOT, C_CARD); tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(84);
  tft.drawString(v, 119, 206, 4);
  tft.setTextPadding(0);
}
void drawTimeVal() {
  char v[8]; snprintf(v, sizeof(v), "%d:%02d", setHours, setMinutes);
  tft.setTextColor(C_ACCENT, C_CARD); tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(84);
  tft.drawString(v, 361, 206, 4);
  tft.setTextPadding(0);
}
void drawHomeLive() {
  char v[24];
  if (sensorOk) snprintf(v, sizeof(v), "%.1f C   %.0f %%", dispTemp, dispHum);
  else snprintf(v, sizeof(v), "SENSOR!");
  tft.setTextColor(sensorOk ? C_TEXT : C_BAD, C_CARD);
  tft.setTextDatum(ML_DATUM);
  tft.setTextPadding(150);
  tft.drawString(v, 22, 250, 2);
  tft.setTextPadding(0);
}
void drawHomeThemeHeader(const char* title) {
  newScreen(title);
  // compact live status in header, common to all four styles
  char live[32];
  if (sensorOk) snprintf(live, sizeof(live), "%.1fC  %.0f%%", dispTemp, dispHum);
  else snprintf(live, sizeof(live), "sensor --");
  tft.setTextColor(sensorOk ? C_ACCENT : C_BAD, C_HDR);
  tft.setTextDatum(MR_DATUM); tft.setTextPadding(150);
  tft.drawString(live, 468, 18, 2); tft.setTextPadding(0);
}

void drawHomeControlsCommon() {
  // Presets and all original controls remain in the same touch zones.
  drawPresets();
  btn(12, 182, 56, 48, "-", C_CARD);
  tft.fillRoundRect(74, 182, 90, 48, 8, C_CARD);
  btn(170, 182, 56, 48, "+", C_CARD);
  btn(254, 182, 56, 48, "-", C_CARD);
  tft.fillRoundRect(316, 182, 90, 48, 8, C_CARD);
  btn(412, 182, 56, 48, "+", C_CARD);
  drawTempVal(); drawTimeVal();
  tft.fillRoundRect(12, 238, 170, 24, 8, C_CARD);
  drawHomeLive();
  btn(12, 270, 230, 44, "START DRYING", C_GOOD, 0x0000);
  const char* fanLabel = manualFanMode ? "FAN OFF" : (fanOn ? "COOLING" : "FAN ONLY");
  btn(250, 270, 110, 44, fanLabel, fanOn ? C_ACCENT : C_CARD, fanOn ? 0x0000 : C_TEXT);
  btn(368, 270, 100, 44, "Setup", C_CARD);
}

void drawHome() {
  idleMode = false;
  switch (mainTheme % 4) {
    case 0: // Modern cards
      drawHomeThemeHeader("SpacePi Dryer");
      drawHomeControlsCommon();
      break;
    case 1: // Circular accents behind preset cards
      drawHomeThemeHeader("Circular Control");
      tft.drawCircle(82, 96, 47, 0x047F); tft.drawCircle(398, 96, 47, C_HOT);
      drawHomeControlsCommon();
      break;
    case 2: // Minimal industrial
      drawHomeThemeHeader("Dryer Control");
      tft.drawFastHLine(12, 174, 456, 0x2945);
      drawHomeControlsCommon();
      break;
    default: // Split-panel
      drawHomeThemeHeader("Live Dashboard");
      tft.fillRoundRect(8, 40, 464, 112, 12, 0x1108);
      drawHomeControlsCommon();
      break;
  }
}
void startRun();  // fwd
void drawSettings();
void touchHome(int tx, int ty) {
  for (int i = 0; i < N_PRESETS; i++) {
    int x = 12 + (i % 3) * 156, y = 44 + (i / 3) * 52;
    if (hit(tx, ty, x, y, 148, 44)) {
      selPreset = i; setTemp = presets[i].tempC;
      setHours = presets[i].hours; setMinutes = 0;
      drawPresets(); drawTempVal(); drawTimeVal(); return;
    }
  }
  if (hit(tx, ty, 12, 182, 56, 48))  { setTemp = max(MIN_SET_TEMP, setTemp - 5); drawTempVal(); }
  if (hit(tx, ty, 170, 182, 56, 48)) { setTemp = min(MAX_SET_TEMP, setTemp + 5); drawTempVal(); }
  if (hit(tx, ty, 254, 182, 56, 48)) {
    int t = setHours * 60 + setMinutes - 30; if (t < 30) t = 30;
    setHours = t / 60; setMinutes = t % 60; drawTimeVal();
  }
  if (hit(tx, ty, 412, 182, 56, 48)) {
    int t = setHours * 60 + setMinutes + 30; if (t > 24 * 60) t = 24 * 60;
    setHours = t / 60; setMinutes = t % 60; drawTimeVal();
  }
  if (hit(tx, ty, 12,  270, 230, 44)) startRun();
  if (hit(tx, ty, 250, 270, 110, 44)) {
    // Manual FAN ONLY mode. Automatic cooling cannot be switched off while hot.
    if (manualFanMode) {
      manualFanMode = false;
      if (!sensorOk || curTemp <= COOLDOWN_TEMP) fanWrite(false);
    } else if (!(fanOn && sensorOk && curTemp > COOLDOWN_TEMP)) {
      manualFanMode = true;
      fanWrite(true);
    }
    const char* fanLabel = manualFanMode ? "FAN OFF" : (fanOn ? "COOLING" : "FAN ONLY");
    btn(250, 270, 110, 44, fanLabel, fanOn ? C_ACCENT : C_CARD, fanOn ? 0x0000 : C_TEXT);
  }
  if (hit(tx, ty, 368, 270, 100, 44)) { screen = SCR_SETTINGS; drawSettings(); }
}

// ---------------------------------------------------------------
//                      RUN SCREEN
// ---------------------------------------------------------------
void drawRunStatic() {
  char title[32];
  snprintf(title, sizeof(title), "%s  target %d C", presets[selPreset].name, setTemp);
  newScreen(title);
  switch (mainTheme % 4) {
    case 0: // Modern Cards
      tft.fillRoundRect(12,44,220,104,12,C_CARD);
      tft.fillRoundRect(248,44,220,104,12,C_CARD);
      tft.fillRoundRect(12,158,456,60,12,C_CARD);
      tft.setTextDatum(MC_DATUM); tft.setTextColor(C_MUTED,C_CARD);
      tft.drawString("TEMPERATURE",122,58,2); tft.drawString("HUMIDITY",358,58,2);
      tft.drawString("TIME REMAINING",240,168,2);
      break;
    case 1: // Circular Dial
      tft.fillRoundRect(12,72,112,104,12,C_CARD);
      tft.fillRoundRect(356,72,112,104,12,C_CARD);
      tft.fillCircle(240,145,76,C_BG);
      tft.drawCircle(240,145,91,0x047F); tft.drawCircle(240,145,82,0x025F);
      tft.drawArc(240,145,91,86,210,330,C_ACCENT,C_BG);
      tft.drawArc(240,145,91,86,30,145,C_HOT,C_BG);
      tft.setTextDatum(MC_DATUM); tft.setTextColor(C_MUTED,C_CARD);
      tft.drawString("TEMP",68,82,2); tft.drawString("HUM",412,82,2);
      tft.setTextColor(C_MUTED,C_BG); tft.drawString("TIME REMAINING",240,108,2);
      break;
    case 2: // Minimal Industrial
      tft.fillRoundRect(12,72,456,146,10,0x080F);
      tft.setTextDatum(TL_DATUM); tft.setTextColor(C_MUTED,0x080F); tft.drawString("TEMPERATURE",18,80,2);
      tft.setTextDatum(TR_DATUM); tft.drawString("HUMIDITY",462,80,2);
      tft.setTextDatum(MC_DATUM); tft.drawString("TIME REMAINING",240,124,2);
      break;
    default: // Split Dashboard
      tft.fillRoundRect(12,44,220,82,12,C_CARD);
      tft.fillRoundRect(248,44,220,82,12,C_CARD);
      tft.fillRoundRect(12,136,456,82,12,0x1108);
      tft.setTextDatum(MC_DATUM); tft.setTextColor(C_MUTED,C_CARD);
      tft.drawString("TEMPERATURE",122,58,2); tft.drawString("HUMIDITY",358,58,2);
      tft.setTextColor(C_MUTED,0x1108); tft.drawString("TIME REMAINING",240,150,2);
      break;
  }
  tft.fillRoundRect(12,228,338,28,8,C_CARD);
  btn(368,262,100,50,"STOP",C_BAD);
  tft.setTextDatum(TL_DATUM);
}

void drawStatusBadges() {
  tft.fillRoundRect(12,228,338,28,8,C_CARD);
  char info[48];
  snprintf(info,sizeof(info),"Target %d C   Fan %s",setTemp,fanOn?"ON":"OFF");
  tft.setTextDatum(MC_DATUM); tft.setTextColor(C_MUTED,C_CARD);
  tft.drawString(info,181,242,2); tft.setTextDatum(TL_DATUM);
}

void drawRunDynamic() {
  char temp[18], hum[18], tl[18];
  snprintf(temp,sizeof(temp),"%.1f C",dispTemp);
  snprintf(hum,sizeof(hum),"%.0f %%RH",dispHum);
  fmtTimeLeft(tl,sizeof(tl));
  unsigned long total=max(1UL,runEndMs-runStartMs);
  int progress=(int)(100.0f*min(1.0f,(float)(millis()-runStartMs)/(float)total));
  tft.setTextDatum(MC_DATUM);
  switch(mainTheme%4){
    case 0:
      tft.fillRect(20,72,204,62,C_CARD); tft.setTextColor(C_HOT,C_CARD); tft.drawString(temp,122,101,4);
      tft.fillRect(256,72,204,62,C_CARD); tft.setTextColor(C_ACCENT,C_CARD); tft.drawString(hum,358,101,4);
      tft.fillRect(24,180,432,28,C_CARD); tft.setTextColor(C_TEXT,C_CARD); tft.drawString(tl,240,194,4);
      tft.fillRect(18,210,444,5,0x2945); tft.fillRect(18,210,444*progress/100,5,C_ACCENT); break;
    case 1:
      tft.fillRect(14,96,108,60,C_CARD); tft.setTextColor(C_HOT,C_CARD); tft.drawString(temp,68,126,2);
      tft.fillRect(358,96,108,60,C_CARD); tft.setTextColor(C_ACCENT,C_CARD); tft.drawString(hum,412,126,2);
      tft.fillCircle(240,151,70,C_BG); tft.setTextColor(C_TEXT,C_BG); tft.drawString(tl,240,151,4);
      {char pc[12];snprintf(pc,sizeof(pc),"%d %%",progress);tft.setTextColor(C_MUTED,C_BG);tft.drawString(pc,240,186,2);} break;
    case 2:
      tft.fillRect(18,98,180,36,0x080F); tft.setTextDatum(TL_DATUM); tft.setTextColor(C_HOT,0x080F); tft.drawString(temp,18,101,4);
      tft.fillRect(282,98,180,36,0x080F); tft.setTextDatum(TR_DATUM); tft.setTextColor(C_ACCENT,0x080F); tft.drawString(hum,462,101,4);
      tft.setTextDatum(MC_DATUM); tft.fillRect(70,145,340,50,0x080F); tft.setTextColor(C_TEXT,0x080F); tft.drawString(tl,240,170,4);
      tft.fillRect(18,206,444,7,0x2945); tft.fillRect(18,206,444*progress/100,7,C_ACCENT); break;
    default:
      tft.fillRect(20,76,204,38,C_CARD); tft.setTextColor(C_HOT,C_CARD); tft.drawString(temp,122,94,4);
      tft.fillRect(256,76,204,38,C_CARD); tft.setTextColor(C_ACCENT,C_CARD); tft.drawString(hum,358,94,4);
      tft.fillRect(40,166,400,36,0x1108); tft.setTextColor(C_TEXT,0x1108); tft.drawString(tl,240,183,4);
      tft.fillRect(24,205,432,5,0x2945); tft.fillRect(24,205,432*progress/100,5,C_ACCENT); break;
  }
  tft.setTextDatum(TL_DATUM); drawStatusBadges();
}

// animation strip: two 4-blade fans + flame, sprite = no flicker
void drawAnimStrip() {
  anim.fillSprite(C_BG);

  // --- two fans ---
  for (int f = 0; f < 2; f++) {
    int cx = 28 + f * 56, cy = 26;
    if (fanOn) {
      // spinning blades — bright blue
      float a0 = animFrame * 0.5f + f * 0.9f;
      for (int b = 0; b < 4; b++) {
        float a = a0 + b * 1.5708f;
        anim.fillTriangle(cx + (int)(18 * cosf(a)),       cy + (int)(18 * sinf(a)),
                          cx + (int)(7  * cosf(a + 0.6f)),cy + (int)(7  * sinf(a + 0.6f)),
                          cx + (int)(7  * cosf(a - 0.6f)),cy + (int)(7  * sinf(a - 0.6f)),
                          C_ACCENT);
      }
      anim.fillCircle(cx, cy, 4, C_TEXT);
    } else {
      // stopped — draw blades grey at fixed angle
      for (int b = 0; b < 4; b++) {
        float a = 0.4f + b * 1.5708f;
        anim.fillTriangle(cx + (int)(18 * cosf(a)),       cy + (int)(18 * sinf(a)),
                          cx + (int)(7  * cosf(a + 0.6f)),cy + (int)(7  * sinf(a + 0.6f)),
                          cx + (int)(7  * cosf(a - 0.6f)),cy + (int)(7  * sinf(a - 0.6f)),
                          C_MUTED);
      }
      anim.fillCircle(cx, cy, 4, 0x8C71);
      // cross mark over stopped fan
      anim.drawLine(cx - 8, cy - 8, cx + 8, cy + 8, C_BAD);
      anim.drawLine(cx + 8, cy - 8, cx - 8, cy + 8, C_BAD);
    }
  }
  anim.setTextColor(fanOn ? C_ACCENT : C_MUTED, C_BG);
  anim.setTextDatum(TL_DATUM);
  anim.drawString(fanOn ? "fans" : "fans off", 114, 18, 2);

  // --- flame / heater ---
  int fx = 200, fy = 26;
  if (heaterOn) {
    // animated layered flame — orange body, yellow core, white tip flicker
    int fl = (animFrame % 5);
    // outer flame (orange)
    anim.fillTriangle(fx - 13, fy + 22, fx + 13, fy + 22, fx,     fy - 10 - fl, C_HOT);
    anim.fillEllipse(fx, fy + 14, 13, 10, C_HOT);
    // inner flame (yellow)
    anim.fillTriangle(fx - 7,  fy + 22, fx + 7,  fy + 22, fx,     fy + 2  - fl, C_FLAME_Y);
    anim.fillEllipse(fx, fy + 16, 7, 7, C_FLAME_Y);
    // tip flicker (white on every other frame)
    if (animFrame % 2 == 0) {
      anim.fillTriangle(fx - 3, fy + 10, fx + 3, fy + 10, fx, fy - 14 - fl, C_TEXT);
    }
    anim.setTextColor(C_HOT, C_BG);
    anim.drawString("heating", 218, 18, 2);
  } else {
    // cold / off — grey ash pile
    anim.fillEllipse(fx, fy + 18, 14, 6, C_MUTED);                  // ash base
    anim.fillTriangle(fx - 6, fy + 18, fx + 6, fy + 18, fx, fy + 4, 0x6B4D); // cold grey
    // dashed cross — heater off indicator
    anim.drawLine(fx - 10, fy + 4, fx + 10, fy + 4, C_BAD);
    anim.setTextColor(C_MUTED, C_BG);
    anim.drawString("off", 218, 18, 2);
  }

  anim.pushSprite(12, 262);
}
void touchRun(int tx, int ty) {
  if (hit(tx, ty, 368, 262, 100, 50)) {
    heaterWrite(false);
    if (runStartMs > 0) { historySaveRun(1); runStartMs = 0; }  // outcome 1 = stopped
    screen = SCR_HOME; drawHome();
  }
}

// ---------------------------------------------------------------
//                      SCREENSAVER — matches user sketch
// ---------------------------------------------------------------
// forward declarations — defined later in file
void drawSaver();
void drawSaverStatic();
void saveThemePrefs();

void enterSaver(bool fromHome = false) {
  if (sensorOk) {
    saverTemp = curTemp;
    saverHum  = curHum;
    saverHasData = true;
    dispTemp = curTemp;
    dispHum  = curHum;
  }
  lastHumUpdateMs = millis();
  screen = fromHome ? SCR_STANDBY : SCR_SAVER;
  if (fromHome) idleMode = true;   // prevent drawHomeLive from firing over standby
  tft.fillScreen(TFT_BLACK);       // clear previous screen ONCE on entry — not on every redraw
  drawSaverStatic();               // draw all static elements once — never redrawn
  drawSaver();
}

void drawSaverStatic() {
  // Called ONCE on saver entry — draws all static background elements
  // Subsequent redraws only update text with setTextPadding to erase in place
  tft.setTextDatum(MC_DATUM);
  switch (saverTheme % 4) {
    case 0: // Minimal Glow — just a line
      tft.drawFastHLine(90,180,300,0x18C5);
      tft.setTextColor(C_MUTED,TFT_BLACK); tft.setTextPadding(200);
      tft.drawString("touch to wake",240,292,2);
      tft.setTextPadding(0);
      break;
    case 1: // Data Cards — draw card backgrounds once
      tft.fillRoundRect(18,18,210,82,12,C_CARD);
      tft.fillRoundRect(252,18,210,82,12,C_CARD);
      tft.setTextColor(C_MUTED,C_CARD);
      tft.drawString("TEMPERATURE",123,38,2);
      tft.drawString("HUMIDITY",357,38,2);
      tft.fillRoundRect(75,252,330,36,10,C_CARD);
      tft.setTextColor(C_MUTED,TFT_BLACK); tft.setTextPadding(200);
      tft.drawString("touch to wake",240,292,2);
      tft.setTextPadding(0);
      break;
    case 2: // Orbit Dial — draw circles and arcs once
      { int cx=240,cy=155;
        tft.drawCircle(cx,cy,105,0x047F);
        tft.drawCircle(cx,cy,96,0x025F);
        tft.drawArc(cx,cy,105,99,210,330,C_ACCENT,TFT_BLACK);
        tft.drawArc(cx,cy,105,99,30,145,C_HOT,TFT_BLACK);
        tft.setTextColor(C_MUTED,TFT_BLACK); tft.setTextPadding(200);
        tft.drawString("touch to wake",cx,286,2);
        tft.setTextPadding(0);
      }
      break;
    default: // Modern Flip — draw cards once
      { const int x[4]={55,155,275,375};
        for(int i=0;i<4;i++) {
          tft.fillRoundRect(x[i]-42,72,84,126,10,C_CARD);
          tft.drawFastHLine(x[i]-40,135,80,0x3148);
        }
        tft.fillCircle(215,107,5,C_TEXT);
        tft.fillCircle(215,163,5,C_TEXT);
        tft.setTextColor(C_MUTED,TFT_BLACK); tft.setTextPadding(200);
        tft.drawString("touch to wake",240,292,2);
        tft.setTextPadding(0);
      }
      break;
  }
  tft.fillRoundRect(452,8,16,12,3,wifiUp?C_GOOD:C_MUTED);
}

void drawSaverFrame() {
  // Only redraws CHANGING text — static elements drawn once in drawSaverStatic()
  // setTextPadding erases old text in place — no fillScreen, no card redraws, no flicker
  tft.setTextDatum(MC_DATUM);
  char tl[16];
  if (screen == SCR_STANDBY) strcpy(tl, "STANDBY"); else fmtTimeLeft(tl, sizeof(tl));
  char temp[16], hum[16];
  if (sensorOk) { snprintf(temp,sizeof(temp),"%.1f C",curTemp); snprintf(hum,sizeof(hum),"%.0f %%RH",curHum); }
  else { strcpy(temp,"--.- C"); strcpy(hum,"-- %RH"); }

  switch (saverTheme % 4) {
    case 0: { // Minimal Glow
      tft.setTextColor(C_MUTED,TFT_BLACK); tft.setTextPadding(200);
      tft.drawString(screen==SCR_STANDBY?"STANDBY":"DRYING",240,34,2);
      tft.setTextColor(C_ACCENT,TFT_BLACK); tft.setTextPadding(360);
      tft.drawString(clockStr,240,112,6);
      tft.setTextColor(C_HOT,TFT_BLACK); tft.setTextPadding(150);
      tft.drawString(temp,90,22,2);
      tft.setTextColor(C_ACCENT,TFT_BLACK); tft.setTextPadding(150);
      tft.drawString(hum,390,22,2);
      tft.setTextPadding(0);
      break;
    }
    case 1: { // Data Cards — text only, cards already drawn
      tft.setTextColor(C_HOT,C_CARD); tft.setTextPadding(200);
      tft.drawString(temp,123,72,4);
      tft.setTextColor(C_ACCENT,C_CARD); tft.setTextPadding(200);
      tft.drawString(hum,357,72,4);
      tft.setTextColor(C_ACCENT,TFT_BLACK); tft.setTextPadding(360);
      tft.drawString(clockStr,240,150,6);
      tft.setTextColor(C_TEXT,TFT_BLACK); tft.setTextPadding(260);
      tft.drawString(tl,240,218,4);
      // heater/fan status — erase full card width then redraw
      tft.setTextColor(C_MUTED,C_CARD); tft.setTextPadding(320);
      tft.drawString(screen==SCR_STANDBY?"Standby":"Drying",123,270,2);
      tft.setTextColor(heaterOn?C_HOT:C_MUTED,C_CARD); tft.setTextPadding(60);
      tft.drawString(heaterOn?"HEAT":"COLD",210,270,2);
      tft.setTextColor(fanOn?C_ACCENT:C_MUTED,C_CARD); tft.setTextPadding(80);
      tft.drawString(fanOn?"FAN ON":"FAN OFF",320,270,2);
      tft.setTextPadding(0);
      break;
    }
    case 2: { // Orbit Dial — text only, circles already drawn
      int cx=240,cy=155;
      tft.setTextColor(C_MUTED,TFT_BLACK); tft.setTextPadding(160);
      tft.drawString(screen==SCR_STANDBY?"IDLE":"DRYING",cx,92,2);
      tft.setTextColor(C_TEXT,TFT_BLACK); tft.setTextPadding(320);
      tft.drawString(clockStr,cx,145,6);
      tft.setTextColor(C_HOT,TFT_BLACK); tft.setTextPadding(120);
      tft.drawString(temp,78,200,2);
      tft.setTextColor(C_ACCENT,TFT_BLACK); tft.setTextPadding(120);
      tft.drawString(hum,402,200,2);
      tft.setTextPadding(0);
      break;
    }
    default: { // Modern Flip — digits only, cards already drawn
      tft.setTextColor(C_HOT,TFT_BLACK); tft.setTextPadding(150);
      tft.drawString(temp,90,28,2);
      tft.setTextColor(C_ACCENT,TFT_BLACK); tft.setTextPadding(150);
      tft.drawString(hum,390,28,2);
      const int x[4]={55,155,275,375};
      char digits[5]="0000"; int di=0;
      for(int i=0;clockStr[i]&&di<4;i++)
        if(clockStr[i]>='0'&&clockStr[i]<='9') digits[di++]=clockStr[i];
      for(int i=0;i<4;i++) {
        char z[2]={digits[i],0};
        tft.setTextColor(C_ACCENT,C_CARD); tft.setTextPadding(78);
        tft.drawString(z,x[i],135,6);
      }
      tft.setTextColor(C_TEXT,TFT_BLACK); tft.setTextPadding(260);
      tft.drawString(tl,240,230,4);
      tft.setTextPadding(0);
      break;
    }
  }
}
void drawSaverAnimations() { drawSaverFrame(); }
void drawSaver() { drawSaverFrame(); }
void exitSaver() {
  bool wasStandby = (screen == SCR_STANDBY);
  idleMode = false;   // clear standby flag
  tft.fillScreen(TFT_BLACK);
  if (wasStandby) {
    screen = SCR_HOME;
    drawHome();
  } else {
    screen = SCR_RUN;
    drawRunStatic();
    drawRunDynamic();
  }
  lastTouchMs = millis();
}

// ---------------------------------------------------------------
//                      DONE SCREEN
// ---------------------------------------------------------------
void drawDoneStatic() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_GOOD, C_BG); tft.setTextDatum(MC_DATUM);
  tft.drawString("Drying complete", 240, 60, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("fans run until cool", 240, 92, 2);
  btn(140, 254, 200, 50, "DONE", C_CARD);
}
void drawDoneAnim() {
  int cx = 240, cy = 170;
  int r = 26 + (animFrame % 12) * 2;
  tft.fillCircle(cx, cy, 52, C_BG);
  tft.drawCircle(cx, cy, r, C_GOOD);
  tft.fillCircle(cx, cy, 26, C_GOOD);
  tft.drawLine(cx - 10, cy, cx - 3, cy + 8, 0x0000);
  tft.drawLine(cx - 3, cy + 8, cx + 11, cy - 8, 0x0000);
  tft.drawLine(cx - 10, cy + 1, cx - 3, cy + 9, 0x0000);
  tft.drawLine(cx - 3, cy + 9, cx + 11, cy - 7, 0x0000);
  char v[24]; snprintf(v, sizeof(v), "%.1f C  %.0f %%", dispTemp, dispHum);
  tft.setTextColor(C_MUTED, C_BG); tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(200);
  tft.drawString(v, 240, 232, 2);
  tft.setTextPadding(0);
}
void finishRun() {
  manualFanMode = false;
  heaterWrite(false);
  historySaveRun(0);   // outcome 0 = completed
  runStartMs = 0;
  screen = SCR_DONE;
  drawDoneStatic();
}
void touchDone(int tx, int ty) {
  if (hit(tx, ty, 140, 254, 200, 50)) {
    manualFanMode = false;
    fanWrite(sensorOk && curTemp > COOLDOWN_TEMP);
    screen = SCR_HOME; drawHome();
  }
}

// ---------------------------------------------------------------
//                      FAULT
// ---------------------------------------------------------------
bool faultUnlocked() {
  if (sensorOk && curTemp < FAULT_RESET_TEMP) return true;
  if (!sensorOk && millis() - faultAtMs > FAULT_RESET_TIMEOUT_MS) return true;
  return false;
}
void drawFault() {
  faultResetShown = false;
  tft.fillScreen(0x4000);
  tft.setTextColor(C_TEXT, 0x4000); tft.setTextDatum(MC_DATUM);
  tft.drawString("FAULT", 240, 90, 4);
  tft.drawString(faultMsg, 240, 140, 4);
  tft.setTextColor(0xFDB8, 0x4000);
  tft.drawString("heater off, fans on", 240, 190, 2);
  tft.drawString(sensorOk ? "reset unlocks below 50 C"
                          : "reset unlocks after 10 min cooling", 240, 214, 2);
}
void faultTick() {
  if (!faultResetShown && faultUnlocked()) {
    faultResetShown = true;
    btn(140, 244, 200, 54, "RESET", C_CARD);
  }
  if (faultResetShown) return;
  char v[16];
  if (sensorOk) snprintf(v, sizeof(v), "%.1f C", curTemp);
  else {
    long left = (long)(FAULT_RESET_TIMEOUT_MS - (millis() - faultAtMs)) / 1000;
    snprintf(v, sizeof(v), "%ld:%02ld", max(0L, left) / 60, max(0L, left) % 60);
  }
  tft.setTextColor(C_TEXT, 0x4000); tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(110);
  tft.drawString(v, 240, 234, 2);
  tft.setTextPadding(0);
}
void resetFault() {
  faultMsg[0] = 0;
  screen = SCR_HOME;
  fanWrite(sensorOk && curTemp > COOLDOWN_TEMP);
  drawHome();
}
void touchFault(int tx, int ty) {
  if (faultResetShown && hit(tx, ty, 140, 244, 200, 54)) resetFault();
}

// ---------------------------------------------------------------
//                      SETTINGS + WIFI SETUP
// ---------------------------------------------------------------
void themeTile(int x,int y,int w,int h,const char* label,bool selected,uint8_t kind,bool saverKind) {
  uint16_t bg=selected?0x1230:C_CARD, edge=selected?C_ACCENT:0x3148;
  tft.fillRoundRect(x,y,w,h,8,bg); tft.drawRoundRect(x,y,w,h,8,edge);
  tft.setTextDatum(MC_DATUM);
  if (saverKind) {
    tft.setTextColor(kind==2?C_TEXT:C_ACCENT,bg);
    if(kind==1) tft.drawCircle(x+w/2,y+24,15,C_ACCENT);
    else if(kind==2){ for(int i=0;i<4;i++) tft.drawFastHLine(x+12,y+14+i*6,w-24,0x047F+i*0x20); }
    else if(kind==3){ for(int i=0;i<10;i++) tft.fillCircle(x+10+(i*23)%(w-20),y+10+(i*17)%32,1,C_ACCENT); }
    tft.drawString("20:06",x+w/2,y+28,2);
  } else {
    tft.setTextColor(C_HOT,bg); tft.drawString("38",x+w/3,y+24,2);
    tft.setTextColor(C_ACCENT,bg); tft.drawString("32",x+2*w/3,y+24,2);
    if(kind==1) tft.drawCircle(x+w/2,y+24,20,C_ACCENT);
    if(kind==2) tft.drawFastHLine(x+10,y+42,w-20,C_MUTED);
    if(kind==3) tft.drawRoundRect(x+6,y+7,w-12,34,5,C_ACCENT);
  }
  tft.setTextColor(selected?C_ACCENT:C_TEXT,bg); tft.drawString(label,x+w/2,y+h-10,2);
  if(selected){ tft.fillCircle(x+w-10,y+10,7,C_ACCENT); tft.setTextColor(0,bg); tft.drawString("+",x+w-10,y+10,2); }
}

void drawSettings() {
  wifiUp=(WiFi.status()==WL_CONNECTED);
  newScreen(settingsPage==0?"Settings: Appearance":"Settings: System");
  btn(12,42,146,36,"APPEARANCE",settingsPage==0?C_ACCENT:C_CARD,settingsPage==0?0:C_TEXT);
  btn(166,42,146,36,"SYSTEM",settingsPage==1?C_ACCENT:C_CARD,settingsPage==1?0:C_TEXT);
  btn(320,42,148,36,"BACK",C_CARD);
  if(settingsPage==0){
    tft.setTextDatum(TL_DATUM); tft.setTextColor(C_MUTED,C_BG); tft.drawString("Main display style",16,84,2);
    { const char* mn[]={"Modern","Circular","Minimal","Split"};
      for(int i=0;i<4;i++) themeTile(12+i*116,104,108,62,mn[i],mainTheme==i,i,false); }
    tft.setTextColor(C_MUTED,C_BG); tft.drawString("Screensaver style",16,174,2);
    { const char* sn[]={"Glow","Cards","Orbit","Flip"};
      for(int i=0;i<4;i++) themeTile(12+i*116,194,108,62,sn[i],saverTheme==i,i,true); }
    btn(12,270,144,38,saverEnabled?"SAVER ON":"SAVER OFF",saverEnabled?C_GOOD:C_CARD,saverEnabled?0:C_TEXT);
    btn(168,270,144,38,antiFlicker?"ANTI-FLICKER ON":"ANTI-FLICKER OFF",antiFlicker?C_ACCENT:C_CARD,antiFlicker?0:C_TEXT);
    btn(324,270,144,38,"PREVIEW",C_CARD);
  } else {
    tft.fillRoundRect(12,88,456,82,10,C_CARD); tft.setTextDatum(TL_DATUM);
    tft.setTextColor(wifiUp?C_GOOD:C_MUTED,C_CARD);
    String net=wifiUp?(cfgSsid+"  "+WiFi.localIP().toString()):String("not connected / AP 192.168.4.1");
    tft.drawString(net,24,100,2); tft.setTextColor(C_MUTED,C_CARD);
    tft.drawString("Time: "+String(clockStr)+(timeSynced?"":" (needs WiFi)"),24,126,2);
    btn(12,184,140,48,"WIFI",C_ACCENT,0); btn(164,184,140,48,"CALIBRATE",C_CARD); btn(316,184,152,48,"HOME",C_CARD);
    char safe[64]; snprintf(safe,sizeof(safe),"Safety ceiling %.0fC | maximum set %dC",MAX_TEMP_C,MAX_SET_TEMP);
    tft.fillRoundRect(12,248,456,48,8,C_CARD); tft.setTextColor(C_MUTED,C_CARD); tft.drawString(safe,24,262,2);
  }
}
void runTouchCalibration() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_TEXT, C_BG); tft.setTextDatum(MC_DATUM);
  tft.drawString("Touch the corner arrows", 240, 140, 4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("use the stylus, press firmly", 240, 176, 2);
  tft.calibrateTouch(calData, TFT_GREEN, C_BG, 15);
  prefs.putBytes("cal", calData, sizeof(calData));
  tft.setTouch(calData);
}

// --- WiFi network list ---
void drawWifiList() {
  newScreen("Choose network");
  tft.setTextColor(C_MUTED, C_BG); tft.setTextDatum(MC_DATUM);
  tft.drawString("scanning...", 240, 160, 4);
  int n = WiFi.scanNetworks();
  netCount = min(n, MAX_NETS);
  drawBackdrop();
  for (int i = 0; i < netCount; i++) {
    netNames[i] = WiFi.SSID(i);
    char lbl[40];
    snprintf(lbl, sizeof(lbl), "%s  (%d)", netNames[i].c_str(), WiFi.RSSI(i));
    btn(40, 44 + i * 40, 400, 34, lbl, C_CARD);
  }
  if (netCount == 0) {
    tft.setTextColor(C_MUTED, C_BG); tft.setTextDatum(MC_DATUM);
    tft.drawString("no networks found", 240, 140, 4);
  }
  btn(140, 44 + netCount * 40 + 8, 200, 40, "CANCEL", C_CARD);
  WiFi.scanDelete();
}
void kbdDraw();
void touchWifiList(int tx, int ty) {
  for (int i = 0; i < netCount; i++) {
    if (hit(tx, ty, 40, 44 + i * 40, 400, 34)) {
      kbdSsid = netNames[i]; kbdBuf = ""; kbdPage = 0;
      screen = SCR_KBD; kbdDraw(); return;
    }
  }
  if (hit(tx, ty, 140, 44 + netCount * 40 + 8, 200, 40)) { screen = SCR_SETTINGS; drawSettings(); }
}

// --- on-screen keyboard ---
const char* KBD_ROWS[3][4] = {
  {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"},
  {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
  {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "`~ "},
};
void kbdDrawField() {
  tft.fillRoundRect(12, 40, 456, 34, 8, C_CARD);
  tft.setTextColor(C_TEXT, C_CARD); tft.setTextDatum(ML_DATUM);
  String shown = kbdBuf;
  if (shown.length() > 30) shown = shown.substring(shown.length() - 30);
  tft.setTextPadding(430);
  tft.drawString(shown.c_str(), 24, 57, 2);
  tft.setTextPadding(0);
}
void kbdDraw() {
  char t[48]; snprintf(t, sizeof(t), "Password: %s", kbdSsid.c_str());
  newScreen(t);
  kbdDrawField();
  for (int r = 0; r < 4; r++) {
    const char* row = KBD_ROWS[kbdPage][r];
    int len = strlen(row);
    int keyw = 44, x0 = (480 - len * (keyw + 2)) / 2;
    for (int i = 0; i < len; i++) {
      char k[2] = { row[i], 0 };
      btn(x0 + i * (keyw + 2), 80 + r * 42, keyw, 38, k, C_CARD);
    }
  }
  btn(12,  252, 88, 40, kbdPage == 2 ? "abc" : (kbdPage == 1 ? "sym" : "ABC"), C_CARD);
  btn(108, 252, 120, 40, "SPACE", C_CARD);
  btn(236, 252, 70, 40, "DEL", C_CARD);
  btn(314, 252, 74, 40, "OK", C_GOOD, 0x0000);
  btn(396, 252, 72, 40, "X", C_BAD);
}
void wifiApplyNew();
void touchKbd(int tx, int ty) {
  for (int r = 0; r < 4; r++) {
    const char* row = KBD_ROWS[kbdPage][r];
    int len = strlen(row);
    int keyw = 44, x0 = (480 - len * (keyw + 2)) / 2;
    for (int i = 0; i < len; i++) {
      if (hit(tx, ty, x0 + i * (keyw + 2), 80 + r * 42, keyw, 38)) {
        kbdBuf += row[i]; kbdDrawField(); return;
      }
    }
  }
  if (hit(tx, ty, 12, 252, 88, 40))  { kbdPage = (kbdPage + 1) % 3; kbdDraw(); return; }
  if (hit(tx, ty, 108, 252, 120, 40)) { kbdBuf += ' '; kbdDrawField(); return; }
  if (hit(tx, ty, 236, 252, 70, 40)) {
    if (kbdBuf.length()) kbdBuf.remove(kbdBuf.length() - 1);
    kbdDrawField(); return;
  }
  if (hit(tx, ty, 314, 252, 74, 40)) { wifiApplyNew(); return; }
  if (hit(tx, ty, 396, 252, 72, 40)) { screen = SCR_SETTINGS; drawSettings(); return; }
}

// ---------------------------------------------------------------
//                      WEB
// ---------------------------------------------------------------
const char PAGE[] PROGMEM =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>SpacePi Dryer</title><style>\n"
":root{--bg:#070b14;--panel:#10192a;--panel2:#15223a;--line:#263a5d;--text:#f3f7ff;--muted:#8494b3;--blue:#1597ff;--orange:#ff8a1f;--green:#32d583;--red:#ff4d5f;--shadow:0 18px 50px #0008}*{box-sizing:border-box}html,body{margin:0;min-height:100%;background:radial-gradient(circle at 20% 0,#10213f 0,#070b14 42%);color:var(--text);font:15px/1.45 Inter,system-ui,sans-serif}button,input,select{font:inherit}.app{min-height:100vh}.side{position:fixed;inset:auto 0 0 0;height:72px;background:#0b1220ee;border-top:1px solid var(--line);display:flex;z-index:20;backdrop-filter:blur(14px)}.brand{display:none}.nav{display:flex;width:100%}.nav button{flex:1;border:0;background:transparent;color:var(--muted);padding:9px 4px;font-size:11px}.nav button b{display:block;font-size:20px;line-height:1}.nav button.on{color:var(--blue)}main{padding:16px 16px 92px;max-width:1480px;margin:auto}.page{display:none}.page.on{display:block}.top{display:flex;justify-content:space-between;align-items:center;margin:2px 0 16px}.top h1{font-size:23px;margin:0}.connected{color:var(--green);font-size:13px}.grid{display:grid;gap:14px}.card{background:linear-gradient(145deg,var(--panel2),var(--panel));border:1px solid var(--line);border-radius:18px;padding:18px;box-shadow:var(--shadow)}.hero{display:grid;gap:14px}.clock{font:700 clamp(52px,13vw,112px)/1 ui-monospace,monospace;letter-spacing:-.08em;color:#fff}.eyebrow{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.12em}.stats{display:grid;grid-template-columns:1fr 1fr;gap:12px}.value{font-size:36px;font-weight:800}.hot{color:var(--orange)}.cool{color:var(--blue)}.status{display:flex;align-items:center;gap:9px}.dot{width:9px;height:9px;border-radius:50%;background:currentColor}.actions{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}.btn{border:1px solid var(--line);background:#172640;color:var(--text);border-radius:13px;padding:13px;cursor:pointer;font-weight:700}.btn.primary{background:linear-gradient(135deg,#0878e9,#19a7ff);border:0}.btn.danger{border-color:#7b2631;color:#ff7784;background:#28141b}.btn.orange{color:var(--orange)}.bar{height:9px;background:#21304a;border-radius:99px;overflow:hidden}.bar i{display:block;height:100%;width:0;background:linear-gradient(90deg,#0878e9,#26b6ff);transition:width .4s}.quick{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}.quick button{min-height:78px}.quick span{display:block;font-size:22px}.charts{grid-template-columns:1fr}.chart{height:150px;width:100%}.controls{display:grid;gap:12px}.control-row{display:grid;grid-template-columns:1fr auto;align-items:center;gap:12px;padding:13px;border:1px solid var(--line);border-radius:14px;background:#0c1526}.switch{min-width:92px}.themes{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}.theme{border:1px solid var(--line);background:#0b1424;color:var(--muted);padding:9px;border-radius:13px;cursor:pointer}.theme.on{border-color:var(--blue);color:var(--blue);box-shadow:0 0 0 1px #1597ff55}.preview{height:74px;border-radius:9px;background:#030711;display:flex;align-items:center;justify-content:center;font:700 22px ui-monospace;margin-bottom:7px;overflow:hidden}.preview.ring:before{content:\"20:06\";display:grid;place-items:center;width:58px;height:58px;border:4px solid #168fff;border-radius:50%}.preview.split{gap:5px}.preview.split i{font-style:normal;padding:13px 8px;border-radius:7px;background:#132543}.preview.cards{gap:7px}.preview.cards i{font-style:normal;border:1px solid #294a75;padding:12px 8px;border-radius:8px}.settings-grid{display:grid;gap:14px}.range{width:100%;accent-color:var(--blue)}select,input[type=number]{background:#0c1526;color:var(--text);border:1px solid var(--line);border-radius:10px;padding:10px}.presets{display:grid;grid-template-columns:repeat(3,1fr);gap:9px}.preset{border:1px solid var(--line);background:#0c1526;color:var(--muted);padding:11px;border-radius:11px}.preset.on{color:var(--blue);border-color:var(--blue)}table{width:100%;border-collapse:collapse;font-size:13px}th,td{padding:10px 6px;border-bottom:1px solid var(--line);text-align:left}th{color:var(--muted)}.notice{color:var(--muted);font-size:13px}.toast{position:fixed;left:50%;bottom:88px;transform:translateX(-50%);background:#15223a;border:1px solid var(--line);padding:10px 18px;border-radius:12px;opacity:0;pointer-events:none;transition:.2s;z-index:50}.toast.on{opacity:1}\n"
"@media(min-width:900px){.app{display:grid;grid-template-columns:250px 1fr}.side{inset:0 auto 0 0;width:250px;height:auto;display:block;border:0;border-right:1px solid var(--line);padding:24px 16px}.brand{display:block;font-size:22px;font-weight:800;margin:4px 10px 30px}.brand small{display:block;color:var(--muted);font-size:12px;font-weight:400;margin-top:4px}.nav{display:grid;gap:6px}.nav button{display:flex;gap:12px;align-items:center;text-align:left;border-radius:12px;padding:13px 14px;font-size:14px}.nav button b{font-size:18px}.nav button.on{background:#142747}.content{grid-column:2}main{padding:28px 30px 40px}.dashboard{grid-template-columns:minmax(0,1.45fr) minmax(320px,.75fr)}.hero{grid-column:1}.rightcol{grid-column:2;grid-row:1/3}.charts{grid-template-columns:1fr 1fr}.settings-grid{grid-template-columns:1fr 1fr}.themes{grid-template-columns:repeat(4,1fr)}.toast{bottom:28px}.clock{font-size:84px}.actions{grid-template-columns:1fr 1fr}.quick{grid-template-columns:repeat(4,1fr)}}\n"
"</style></head><body><div class=\"app\"><aside class=\"side\"><div class=\"brand\">SpacePi Dryer<small id=\"device-ip\">Local controller</small></div><div class=\"nav\"><button class=\"on\" data-page=\"dash\"><b>\u25a6</b>Dashboard</button><button data-page=\"control\"><b>\u2637</b>Control</button><button data-page=\"history\"><b>\u2301</b>History</button><button data-page=\"settings\"><b>\u2699</b>Settings</button></div></aside><div class=\"content\"><main>\n"
"<section class=\"page on\" id=\"dash\"><div class=\"top\"><h1>Dashboard</h1><span class=\"connected\" id=\"online\">\u25cf Connecting</span></div><div class=\"grid dashboard\"><div class=\"hero\"><div class=\"card\"><div class=\"eyebrow\" id=\"date\">SpacePi Dryer</div><div class=\"clock\" id=\"clock\">--:--</div><div class=\"status notice\"><span class=\"dot\" id=\"state-dot\"></span><span id=\"state\">Waiting for device</span></div></div><div class=\"stats\"><div class=\"card\"><div class=\"eyebrow\">Temperature</div><div class=\"value hot\"><span id=\"temp\">--</span>\u00b0C</div></div><div class=\"card\"><div class=\"eyebrow\">Humidity</div><div class=\"value cool\"><span id=\"hum\">--</span>%</div></div></div><div class=\"card\"><div class=\"eyebrow\">Time remaining</div><div class=\"value\" id=\"left\">--:--:--</div><div class=\"bar\"><i id=\"progress\"></i></div><div class=\"notice\" id=\"progress-label\">Idle</div></div><div class=\"quick\"><button class=\"btn\" onclick=\"startDry()\"><span>\u25b6</span>Start</button><button class=\"btn danger\" onclick=\"cmd('stop')\"><span>\u25a0</span>Stop</button><button class=\"btn\" onclick=\"cmd('fan_on')\"><span>\u2723</span>Fan only</button><button class=\"btn\" onclick=\"showPage('settings')\"><span>\u2699</span>Settings</button></div><div class=\"grid charts\"><div class=\"card\"><div class=\"eyebrow hot\">Temperature \u00b7 last hour</div><canvas id=\"tempChart\" class=\"chart\"></canvas></div><div class=\"card\"><div class=\"eyebrow cool\">Humidity \u00b7 last hour</div><canvas id=\"humChart\" class=\"chart\"></canvas></div></div></div><div class=\"rightcol grid\"><div class=\"card\"><div class=\"eyebrow\">System status</div><div class=\"control-row\"><span>Heater</span><strong class=\"hot\" id=\"heater\">OFF</strong></div><div class=\"control-row\"><span>Fan</span><strong class=\"cool\" id=\"fan\">OFF</strong></div><div class=\"control-row\"><span>Controller</span><strong id=\"health\" style=\"color:var(--green)\">Normal</strong></div></div><div class=\"card\"><div class=\"eyebrow\">Quick controls</div><div class=\"actions\"><button class=\"btn primary\" onclick=\"startDry()\">Start cycle</button><button class=\"btn danger\" onclick=\"cmd('stop')\">Stop</button><button class=\"btn\" onclick=\"cmd('fan_on')\">Fan ON</button><button class=\"btn\" onclick=\"cmd('fan_off')\">Fan OFF</button></div></div></div></div></section>\n"
"<section class=\"page\" id=\"control\"><div class=\"top\"><h1>Control</h1></div><div class=\"settings-grid\"><div class=\"card\"><div class=\"eyebrow\">Drying preset</div><div class=\"presets\" id=\"presets\"></div><p>Target temperature: <strong class=\"hot\" id=\"targetLabel\">50\u00b0C</strong></p><input class=\"range\" id=\"target\" type=\"range\" min=\"35\" max=\"85\" step=\"5\" value=\"50\" oninput=\"targetLabel.textContent=this.value+'\u00b0C'\"><p>Duration: <strong id=\"durationLabel\">6:00</strong></p><input class=\"range\" id=\"duration\" type=\"range\" min=\"30\" max=\"1440\" step=\"30\" value=\"360\" oninput=\"durationLabel.textContent=fmtMin(+this.value)\"><div class=\"actions\"><button class=\"btn primary\" onclick=\"startDry()\">Start drying</button><button class=\"btn danger\" onclick=\"cmd('stop')\">Stop</button></div></div><div class=\"card\"><div class=\"eyebrow\">Manual fan</div><p class=\"notice\">The fan can run without the heater. It cannot be switched off during drying or while cooling is required.</p><div class=\"actions\"><button class=\"btn\" onclick=\"cmd('fan_on')\">Fan ON</button><button class=\"btn\" onclick=\"cmd('fan_off')\">Fan OFF</button></div></div></div></section>\n"
"<section class=\"page\" id=\"history\"><div class=\"top\"><h1>History</h1><div style=\"display:flex;gap:10px\"><button class=\"btn\" onclick=\"loadHistory()\">Refresh</button><button class=\"btn danger\" onclick=\"clearHistory()\">Clear history</button></div></div><div class=\"card\"><table><thead><tr><th>Date</th><th>Preset</th><th>Peak</th><th>Humidity</th><th>Result</th></tr></thead><tbody id=\"historyBody\"><tr><td colspan=\"5\" class=\"notice\">No data loaded</td></tr></tbody></table></div></section>\n"
"<section class=\"page\" id=\"settings\"><div class=\"top\"><h1>Settings & appearance</h1></div><div class=\"settings-grid\"><div class=\"card\"><div class=\"eyebrow\">Main display theme</div><p class=\"notice\">Choose one of the four LCD home-screen layouts.</p><div class=\"themes\" id=\"mainThemes\"></div></div><div class=\"card\"><div class=\"eyebrow\">Screensaver theme</div><p class=\"notice\">Choose independently from all four standby designs.</p><div class=\"themes\" id=\"saverThemes\"></div></div><div class=\"card\"><div class=\"eyebrow\">Display behavior</div><div class=\"control-row\"><span>Screensaver</span><button class=\"btn switch\" id=\"saverFlag\" onclick=\"cmd('saver_toggle')\">\u2014</button></div><div class=\"control-row\"><span>Anti-flicker</span><button class=\"btn switch\" id=\"flickerFlag\" onclick=\"cmd('antiflick_toggle')\">\u2014</button></div><p class=\"notice\">Anti-flicker redraws a complete frame only when needed, preventing old digit fragments.</p></div><div class=\"card\"><div class=\"eyebrow\">Device connection</div><div class=\"control-row\"><span>Address</span><strong id=\"address\">\u2014</strong></div><div class=\"control-row\"><span>Last update</span><strong id=\"updated\">\u2014</strong></div></div></div></section>\n"
"</main></div></div><div class=\"toast\" id=\"toast\"></div><script>\n"
"const P=[['PLA',50,360],['PETG',60,360],['ABS',70,480],['ASA',70,480],['PA/NY',80,720],['PC',85,600]];let selected=0,state={mainTheme:0,saverTheme:0},tempData=[],humData=[];const $=id=>document.getElementById(id);function base(){return location.protocol.startsWith('http')?location.origin:'http://192.168.178.143'}async function get(path){const r=await fetch(base()+path,{cache:'no-store'});if(!r.ok)throw Error(await r.text());return r}async function cmd(a){try{await get('/cmd?action='+a);toast('Updated');await poll()}catch(e){toast(e.message)}}function toast(s){$('toast').textContent=s;$('toast').classList.add('on');setTimeout(()=>$('toast').classList.remove('on'),1800)}function showPage(p){document.querySelectorAll('.page').forEach(x=>x.classList.toggle('on',x.id===p));document.querySelectorAll('.nav button').forEach(x=>x.classList.toggle('on',x.dataset.page===p));if(p==='history')loadHistory()}document.querySelectorAll('.nav button').forEach(b=>b.onclick=()=>showPage(b.dataset.page));function fmtMin(m){return Math.floor(m/60)+':'+String(m%60).padStart(2,'0')}function buildPresets(){$('presets').innerHTML=P.map((p,i)=>`<button class=\"preset ${i===selected?'on':''}\" onclick=\"pick(${i})\"><b>${p[0]}</b><br>${p[1]}\u00b0C \u00b7 ${fmtMin(p[2])}</button>`).join('')}function pick(i){selected=i;$('target').value=P[i][1];$('duration').value=P[i][2];$('targetLabel').textContent=P[i][1]+'\u00b0C';$('durationLabel').textContent=fmtMin(P[i][2]);buildPresets()}function startDry(){cmd(`start&preset=${selected}&temp=${$('target').value}&min=${$('duration').value}`)}function themePreview(i,saver){if(i===0)return `<div class=\"preview\">20:06</div>`;if(i===1)return `<div class=\"preview ring\"></div>`;if(i===2)return `<div class=\"preview cards\"><i>${saver?'20:06':'38\u00b0'}</i><i>${saver?'02.08':'32%'}</i></div>`;return `<div class=\"preview split\"><i>20</i><i>06</i></div>`}function buildThemes(){const mn=['Modern Cards','Circular Dial','Minimal Industrial','Split Dashboard'],sn=['Minimal Glow','Data Cards','Orbit Dial','Modern Flip'];$('mainThemes').innerHTML=mn.map((n,i)=>`<button class=\"theme ${state.mainTheme===i?'on':''}\" onclick=\"setTheme('main',${i})\">${themePreview(i,false)}${n}</button>`).join('');$('saverThemes').innerHTML=sn.map((n,i)=>`<button class=\"theme ${state.saverTheme===i?'on':''}\" onclick=\"setTheme('saver',${i})\">${themePreview(i,true)}${n}</button>`).join('')}async function setTheme(k,i){await cmd(`theme&kind=${k}&value=${i}`)}function drawChart(id,data,color){const c=$(id),dpr=devicePixelRatio||1,w=c.clientWidth||400,h=c.clientHeight||150;c.width=w*dpr;c.height=h*dpr;const x=c.getContext('2d');x.scale(dpr,dpr);x.clearRect(0,0,w,h);x.strokeStyle='#263a5d';x.beginPath();for(let j=1;j<4;j++){x.moveTo(0,j*h/4);x.lineTo(w,j*h/4)}x.stroke();if(data.length<2)return;let min=Math.min(...data),max=Math.max(...data);if(max-min<1){min-=.5;max+=.5}x.strokeStyle=color;x.lineWidth=2;x.beginPath();data.forEach((v,i)=>{let px=i*w/(data.length-1),py=h-8-(v-min)*(h-16)/(max-min);i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke()}async function poll(){try{let d=await(await get('/api')).json();state=d;$('online').textContent='\u25cf Connected';$('online').style.color='var(--green)';$('temp').textContent=d.t.toFixed(1);$('hum').textContent=d.h.toFixed(0);$('left').textContent=d.left;$('state').textContent=d.state;$('heater').textContent=d.heater?'ON':'OFF';$('fan').textContent=d.fan?'ON':'OFF';$('health').textContent=d.state.includes('E')?'Fault':'Normal';$('saverFlag').textContent=d.saverEnabled?'ON':'OFF';$('flickerFlag').textContent=d.antiFlicker?'ON':'OFF';$('updated').textContent=new Date().toLocaleTimeString();$('address').textContent=location.host;$('device-ip').textContent=location.host;let now=new Date();$('clock').textContent=now.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});$('date').textContent=now.toLocaleDateString([],{weekday:'long',day:'2-digit',month:'short',year:'numeric'});tempData.push(d.t);humData.push(d.h);if(tempData.length>60){tempData.shift();humData.shift()}drawChart('tempChart',tempData,'#ff8a1f');drawChart('humChart',humData,'#1597ff');buildThemes()}catch(e){$('online').textContent='\u25cf Disconnected';$('online').style.color='var(--red)'}}async function loadHistory(){try{let d=await(await get('/history')).json();$('historyBody').innerHTML=d.runs.length?d.runs.slice().reverse().map(r=>`<tr><td>${r.ts?new Date(r.ts*1000).toLocaleDateString():'\u2014'}</td><td>${r.preset}</td><td>${r.peak}\u00b0C</td><td>${r.startHum}\u2192${r.endHum}%</td><td>${r.outcome}</td></tr>`).join(''):'<tr><td colspan=\"5\" class=\"notice\">No runs yet</td></tr>'}catch(e){toast('History unavailable')}}async function clearHistory(){if(!confirm('Clear all run and fault history? This cannot be undone.'))return;try{await get('/history/clear');$('historyBody').innerHTML='<tr><td colspan=\"5\" class=\"notice\">History cleared</td></tr>';toast('History cleared')}catch(e){toast('Could not clear history')}}buildPresets();buildThemes();poll();setInterval(poll,2000);setInterval(()=>{$('clock').textContent=new Date().toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'})},1000);\n"
"</script></body></html>\n"
;

void handleRoot() {
  // Send PAGE in chunks — required for >8KB PROGMEM strings on ESP32 WebServer
  size_t pageLen = strlen_P(PAGE);
  server.setContentLength(pageLen);
  server.send(200, "text/html; charset=utf-8", "");
  const char* p = PAGE;
  size_t rem = pageLen;
  char ch[512];
  while (rem > 0) {
    size_t n = rem > 511 ? 511 : rem;
    memcpy_P(ch, p, n); ch[n] = 0;
    server.sendContent(ch);
    p += n; rem -= n;
    yield();
  }
  server.sendContent("");
}

void handleHistory() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String json = "{\"runs\":[";
  int completed = 0, stopped = 0, faulted = 0;
  const char* outcomeStr[] = {"completed","stopped","faulted"};
  const char* presetNames[] = {"PLA","PETG","ABS","ASA","PA/NY","PC"};
  for (int i = 0; i < runCount; i++) {
    if (i > 0) json += ",";
    RunRecord& r = runLog[i];
    if (r.outcome == 0) completed++;
    else if (r.outcome == 1) stopped++;
    else faulted++;
    char buf[200];
    snprintf(buf, sizeof(buf),
      "{\"ts\":%lu,\"preset\":\"%s\",\"target\":%d,\"peak\":%d,"
      "\"startHum\":%d,\"endHum\":%d,\"planned\":%d,\"actual\":%d,\"outcome\":\"%s\"}",
      (unsigned long)r.startEpoch,
      (r.presetIdx < 6) ? presetNames[r.presetIdx] : "?",
      r.targetTemp, r.peakTemp,
      r.startHum, r.endHum,
      r.durationMin, r.actualMin,
      outcomeStr[min((int)r.outcome, 2)]);
    json += buf;
  }
  json += "],\"faults\":[";
  for (int i = 0; i < faultCount; i++) {
    if (i > 0) json += ",";
    char buf[60];
    snprintf(buf, sizeof(buf), "{\"ts\":%lu,\"code\":\"%s\"}",
      (unsigned long)faultLog[i].epoch, faultLog[i].code);
    json += buf;
  }
  char stats[120];
  snprintf(stats, sizeof(stats),
    "],\"stats\":{\"total\":%d,\"completed\":%d,\"stopped\":%d,\"faulted\":%d}}",
    runCount, completed, stopped, faulted);
  json += stats;
  server.send(200, "application/json", json);
}

void handleClearHistory() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  runCount = 0; faultCount = 0;
  memset(runLog, 0, sizeof(runLog));
  memset(faultLog, 0, sizeof(faultLog));
  prefs.putInt("rcount", 0); prefs.putInt("fcount", 0);
  prefs.remove("runs");
  prefs.remove("faults");
  server.send(200, "text/plain", "cleared");
}
void handleApi()  {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char tl[16] = "--:--:--";
  if (screen == SCR_RUN || screen == SCR_SAVER) fmtTimeLeft(tl, sizeof(tl));
  const char* st = (screen == SCR_RUN || screen == SCR_SAVER) ? "drying" :
                   screen == SCR_DONE ? "finished" :
                   screen == SCR_FAULT ? faultMsg : "idle";
  char buf[200];
  snprintf(buf, sizeof(buf),
    "{\"t\":%.1f,\"h\":%.1f,\"left\":\"%s\",\"heater\":%d,\"fan\":%d,\"state\":\"%s\",\"mainTheme\":%u,\"saverTheme\":%u,\"saverEnabled\":%d,\"antiFlicker\":%d}",
    curTemp, curHum, tl, heaterOn ? 1 : 0, fanOn ? 1 : 0, st, mainTheme, saverTheme, saverEnabled?1:0, antiFlicker?1:0);
  server.send(200, "application/json", buf);
}
void handleCmd() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String a = server.arg("action");
  if (a == "reset") {
    if (screen == SCR_FAULT && faultUnlocked()) { resetFault(); server.send(200, "text/plain", "ok"); }
    else server.send(409, "text/plain", "not resettable yet");
    return;
  }
  if (screen == SCR_FAULT) { server.send(409, "text/plain", "fault: reset blocked until cool"); return; }
  if (a == "theme") {
    String kind=server.arg("kind"); int v=constrain(server.arg("value").toInt(),0,3);
    if(kind=="main") mainTheme=v; else if(kind=="saver") saverTheme=v; else {server.send(400,"text/plain","bad theme kind");return;}
    saveThemePrefs();
    if(screen==SCR_HOME) drawHome();
    else if(screen==SCR_SAVER || screen==SCR_STANDBY) {
      tft.fillScreen(TFT_BLACK);
      drawSaverStatic();
      drawSaverFrame();
    }
    server.send(200,"text/plain","theme saved"); return;
  }
  if (a == "saver_toggle") { saverEnabled=!saverEnabled; saveThemePrefs(); server.send(200,"text/plain",saverEnabled?"screensaver on":"screensaver off"); return; }
  if (a == "antiflick_toggle") { antiFlicker=!antiFlicker; saveThemePrefs(); server.send(200,"text/plain",antiFlicker?"anti-flicker on":"anti-flicker off"); return; }
  if (a == "fan_on") {
    manualFanMode = true;
    fanWrite(true);
    if (screen == SCR_HOME) drawHome();
    server.send(200, "text/plain", "fan on"); return;
  }
  if (a == "fan_off") {
    if (screen == SCR_RUN || screen == SCR_SAVER ||
        (sensorOk && curTemp > COOLDOWN_TEMP)) {
      server.send(409, "text/plain", "fan must remain on while drying or cooling"); return;
    }
    manualFanMode = false;
    fanWrite(false);
    if (screen == SCR_HOME) drawHome();
    server.send(200, "text/plain", "fan off"); return;
  }
  if (a == "stop") {
    manualFanMode = false;
    heaterWrite(false);
    if (runStartMs > 0) { historySaveRun(1); runStartMs = 0; }
    if (screen == SCR_RUN || screen == SCR_SAVER || screen == SCR_STANDBY) { screen = SCR_HOME; drawHome(); }
    server.send(200, "text/plain", "ok"); return;
  }
  if (a == "start") {
    int p = server.arg("preset").toInt();
    if (p >= 0 && p < N_PRESETS) selPreset = p;
    int t = server.arg("temp").toInt();
    int m = server.arg("min").toInt();
    if (t) setTemp = constrain(t, MIN_SET_TEMP, MAX_SET_TEMP);
    if (m) { m = constrain(m, 30, 24 * 60); setHours = m / 60; setMinutes = m % 60; }
    lastTouchMs = millis();   // prevent screensaver from triggering immediately after web start
    if (screen != SCR_RUN && screen != SCR_SAVER) { startRun(); }
    server.send(200, "text/plain", "ok"); return;
  }
  server.send(400, "text/plain", "unknown action");
}

// ---------------------------------------------------------------
//                      WIFI
// ---------------------------------------------------------------

// ---------------------------------------------------------------
//                      OTA UPDATE
// ---------------------------------------------------------------
void drawOtaScreen(const char* msg, int pct) {
  static bool drawn = false;
  if (!drawn) {
    tft.fillScreen(0x0208);
    tft.setTextColor(C_ACCENT, 0x0208); tft.setTextDatum(MC_DATUM);
    tft.drawString("OTA Update", 240, 80, 4);
    tft.setTextColor(C_MUTED, 0x0208);
    tft.drawString("do not power off", 240, 108, 2);
    tft.fillRoundRect(40, 150, 400, 28, 6, C_CARD);
    drawn = true;
  }
  if (pct >= 0) {
    int w = (int)(396.0f * pct / 100.0f);
    tft.fillRoundRect(42, 152, max(1,w), 24, 4, C_ACCENT);
    tft.setTextColor(C_TEXT, 0x0208); tft.setTextDatum(MC_DATUM);
    char buf[16]; snprintf(buf, sizeof(buf), "%d %%", pct);
    tft.setTextPadding(100); tft.drawString(buf, 240, 196, 4); tft.setTextPadding(0);
  }
  if (msg[0]) {
    tft.setTextColor(C_MUTED, 0x0208); tft.setTextDatum(MC_DATUM);
    tft.setTextPadding(400); tft.drawString(msg, 240, 230, 2); tft.setTextPadding(0);
  }
}

void otaInit() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    digitalWrite(PIN_HEATER, LOW); digitalWrite(PIN_FAN, LOW);
    heaterOn = false; fanOn = false;
    tft.fillScreen(0x0208);
    drawOtaScreen("starting...", 0);
  });
  ArduinoOTA.onEnd([]() {
    drawOtaScreen("complete - rebooting", 100); delay(800);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    drawOtaScreen("", (int)(100.0f * done / total));
  });
  ArduinoOTA.onError([](ota_error_t err) {
    const char* r = "unknown";
    if (err==OTA_AUTH_ERROR) r="auth failed";
    else if (err==OTA_BEGIN_ERROR) r="begin failed";
    else if (err==OTA_CONNECT_ERROR) r="connect failed";
    else if (err==OTA_RECEIVE_ERROR) r="receive failed";
    else if (err==OTA_END_ERROR) r="end failed";
    drawOtaScreen(r, -1); delay(3000); drawHome();
  });
  ArduinoOTA.begin();
}

void startAp() { WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_SSID, AP_PASS); }
void wifiConnect() {
  WiFi.setHostname("spacepi-dryer");
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
  wifiUp = (WiFi.status() == WL_CONNECTED);
}
// forward declarations for web handlers (defined later in file)
void handleRoot();
void handleApi();
void handleCmd();
void handleHistory();
void handleClearHistory();

void wifiInit() {
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  if (cfgSsid.length() == 0 && strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0) {
    cfgSsid = WIFI_SSID; cfgPass = WIFI_PASS;
  }
  if (cfgSsid.length()) { WiFi.mode(WIFI_STA); wifiConnect(); }
  if (!wifiUp) startAp();
  if (wifiUp) { configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com"); updateClock(); }
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api", HTTP_GET, handleApi);
  server.on("/cmd", HTTP_GET, handleCmd);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/history/clear", HTTP_GET, handleClearHistory);
  server.onNotFound([]() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (server.method() == HTTP_OPTIONS) server.send(204);
    else server.send(404, "text/plain", "not found");
  });
  server.begin();
}
void wifiApplyNew() {   // from keyboard OK
  tft.fillScreen(C_BG);
  tft.setTextColor(C_TEXT, C_BG); tft.setTextDatum(MC_DATUM);
  tft.drawString("connecting...", 240, 150, 4);
  cfgSsid = kbdSsid; cfgPass = kbdBuf;
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  WiFi.disconnect(true, true);
  delay(300);
  WiFi.mode(WIFI_STA);
  wifiConnect();
  if (!wifiUp) startAp();
  else configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  screen = SCR_SETTINGS;
  drawSettings();
}
void saveThemePrefs() {
  prefs.putUChar("mainTheme",mainTheme); prefs.putUChar("saverTheme",saverTheme);
  prefs.putBool("antiFlick",antiFlicker); prefs.putBool("saver",saverEnabled);
}
void touchSettings(int tx,int ty) {
  if(hit(tx,ty,12,42,146,36)){settingsPage=0;drawSettings();return;}
  if(hit(tx,ty,166,42,146,36)){settingsPage=1;drawSettings();return;}
  if(hit(tx,ty,320,42,148,36)){screen=SCR_HOME;drawHome();return;}
  if(settingsPage==0){
    for(int i=0;i<4;i++) if(hit(tx,ty,12+i*116,104,108,62)){mainTheme=i;saveThemePrefs();drawSettings();return;}
    for(int i=0;i<4;i++) if(hit(tx,ty,12+i*116,194,108,62)){saverTheme=i;saveThemePrefs();drawSettings();return;}
    if(hit(tx,ty,12,270,144,38)){saverEnabled=!saverEnabled;saveThemePrefs();drawSettings();return;}
    if(hit(tx,ty,168,270,144,38)){antiFlicker=!antiFlicker;saveThemePrefs();drawSettings();return;}
    if(hit(tx,ty,324,270,144,38)){ tft.fillScreen(TFT_BLACK); drawSaverStatic(); drawSaverFrame(); delay(2500); drawSettings(); return; }
  } else {
    if(hit(tx,ty,12,184,140,48)){screen=SCR_WIFI;drawWifiList();return;}
    if(hit(tx,ty,164,184,140,48)){runTouchCalibration();drawSettings();return;}
    if(hit(tx,ty,316,184,152,48)){screen=SCR_HOME;drawHome();return;}
  }
}

// ---------------------------------------------------------------
//                      RUN START
// ---------------------------------------------------------------
void startRun() {
  manualFanMode = false;
  if (!sensorOk) { enterFault("E1 sensor lost"); return; }
  idleMode = false;   // cancel idle screensaver if active
  dispTemp = curTemp; dispHum = curHum;
  lastHumUpdateMs = millis();
  runPeakTemp = curTemp;
  runStartHum = curHum;
  runStartMs = millis();
  runEndMs   = millis() + ((unsigned long)setHours * 3600UL + setMinutes * 60UL) * 1000UL;
  rwArmed = false;
  screen = SCR_RUN;
  fanWrite(true);
  lastTouchMs = millis();
  drawRunStatic();
  drawRunDynamic();
}

// ---------------------------------------------------------------
//                      SETUP / LOOP
// ---------------------------------------------------------------
void setup() {
  pinMode(PIN_HEATER, OUTPUT); digitalWrite(PIN_HEATER, LOW);
  pinMode(PIN_FAN, OUTPUT);    digitalWrite(PIN_FAN, LOW);

  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);

  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);
  tft.init();
  tft.setRotation(3);   // confirmed correct for this mounting
  tft.fillScreen(C_BG);

  anim.createSprite(340, 54);    // run screen animation strip
  // sanim sprite removed — drawSaverFrame draws directly to TFT (480x320 sprite exceeds ESP32 RAM)

  prefs.begin("dryer", false);
  saverEnabled = prefs.getBool("saver", true);
  mainTheme = prefs.getUChar("mainTheme", 0) % 4;
  saverTheme = prefs.getUChar("saverTheme", 0) % 4;
  antiFlicker = prefs.getBool("antiFlick", true);
  historyLoad();   // load run log and fault log from NVS

  // Runs are not resumed after reboot. Always boot safely to the home screen.
  runStartMs = 0;
  runEndMs = 0;
  screen = SCR_HOME;
  heaterWrite(false);
  fanWrite(false);

  memset(tempBuf, 0, sizeof(tempBuf));
  if (prefs.getBytes("cal", calData, sizeof(calData)) == sizeof(calData)) {
    tft.setTouch(calData);
  } else {
    runTouchCalibration();
    tft.fillScreen(C_BG);
  }

  if (sht4.begin(&Wire)) {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);
  }

  // Draw the main display before Wi-Fi connection attempts. This guarantees
  // visible local UI even when the router is unavailable or connection is slow.
  readSensor();
  dispTemp = curTemp;
  dispHum  = curHum;
  lastHumUpdateMs = millis();
  lastTouchMs = millis();
  drawHome();

  wifiInit();
  otaInit();
  // Refresh once after Wi-Fi/NTP initialization to update the header status.
  if (screen == SCR_HOME) drawHome();
}

bool touchIsMeaningfulForCurrentScreen(uint16_t x,uint16_t y){
  if(screen==SCR_RUN) return hit(x,y,360,252,120,68);
  if(screen==SCR_HOME){
    for(int i=0;i<N_PRESETS;i++){int bx=12+(i%3)*156,by=44+(i/3)*52;if(hit(x,y,bx,by,148,44))return true;}
    return hit(x,y,12,178,456,56)||hit(x,y,12,266,456,54);
  }
  return true;
}

void loop() {
  unsigned long now = millis();

  if (now - lastSensorMs >= 1000) { lastSensorMs = now; readSensor(); historyTrackRun(); }
  if (now - lastControlMs >= 500) { lastControlMs = now; controlLoop(); }

  // WiFi live state + late NTP
  static bool wasUp = false;
  bool isUp = (WiFi.status() == WL_CONNECTED);
  if (isUp && !wasUp) configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  if (isUp != wasUp && screen == SCR_SETTINGS) drawSettings();
  wifiUp = isUp; wasUp = isUp;

  if (wifiUp && now - lastClockMs >= 20000) {
    lastClockMs = now; updateClock();
    if (screen == SCR_HOME || screen == SCR_RUN) drawClock();
  }

  // Automatic cooldown after a run. Manual FAN ONLY mode stays on until the user turns it off.
  if ((screen == SCR_DONE && !manualFanMode) ||
      (screen == SCR_HOME && fanOn && !manualFanMode)) {
    if (sensorOk && curTemp <= COOLDOWN_TEMP) fanWrite(false);
    else if (screen == SCR_DONE) fanWrite(true);
  }

  // Handle web clients and OTA FIRST — before any slow drawing
  server.handleClient();
  ArduinoOTA.handle();

  // Home standby uses the selected screensaver theme, exactly like the drying saver.
  if (ENABLE_IDLE_CLOCK && saverEnabled && screen == SCR_HOME && now - lastTouchMs > IDLE_TIMEOUT_MS) {
    enterSaver(true);
    server.handleClient();
  }

  // During drying, enter the selected full-screen saver after the configured timeout.
  if (saverEnabled && screen == SCR_RUN && now - lastTouchMs > SAVER_TIMEOUT_MS) {
    enterSaver();
    server.handleClient();
  }

  // animation + dynamic redraws
  if (now - lastAnimMs >= 120) {
    lastAnimMs = now; animFrame++;
    switch (screen) {
      case SCR_RUN: {
        drawAnimStrip();   // fans + flame animation every 120ms
        static unsigned long lastRunDraw=0;
        if(now-lastRunDraw>=1000UL){lastRunDraw=now;drawRunDynamic();}
        break;
      }
      case SCR_SAVER:
      case SCR_STANDBY: {
        static unsigned long lastSaverDraw = 0;
        unsigned long saverInterval = antiFlicker ? 1000UL : 250UL;
        if (now - lastSaverDraw >= saverInterval) { lastSaverDraw = now; drawSaverAnimations(); }
        break;
      }
      case SCR_DONE:  drawDoneAnim(); break;
      case SCR_FAULT: if (animFrame % 8 == 0) faultTick(); break;
      case SCR_HOME:
        if (!idleMode && animFrame % 16 == 0) drawHomeLive();
        break;
      default: break;
    }
  }

  // touch: require two close readings; electrical noise must not reset standby timer
  static bool pendingTouch=false; static uint16_t px=0,py=0; static unsigned long pendingAt=0;
  uint16_t tx,ty; bool rawTouch=tft.getTouch(&tx,&ty,TOUCH_THRESHOLD);
  bool confirmed=false;
  if(rawTouch){
    if(pendingTouch && now-pendingAt>=35 && now-pendingAt<=350 && abs((int)tx-(int)px)<24 && abs((int)ty-(int)py)<24){confirmed=true;pendingTouch=false;}
    else if(!pendingTouch){pendingTouch=true;px=tx;py=ty;pendingAt=now;}
  } else if(pendingTouch && now-pendingAt>350) pendingTouch=false;
  if (confirmed) {
    if(touchIsMeaningfulForCurrentScreen(tx,ty)) lastTouchMs=now;
    switch (screen) {
      case SCR_HOME:     touchHome(tx, ty);     break;
      case SCR_RUN:      touchRun(tx, ty);      break;
      case SCR_SAVER:
      case SCR_STANDBY:  exitSaver();           break;
      case SCR_DONE:     touchDone(tx, ty);     break;
      case SCR_SETTINGS: touchSettings(tx, ty); break;
      case SCR_WIFI:     touchWifiList(tx, ty); break;
      case SCR_KBD:      touchKbd(tx, ty);      break;
      case SCR_FAULT:    touchFault(tx, ty);    break;
    }
    delay(screen == SCR_KBD ? 200 : 120);
  }

  server.handleClient();   // second call — catches anything queued during drawing
}
