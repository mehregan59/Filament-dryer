/*
 * SpacePi Dryer — custom ESP32 controller — Mod v01 Mike (firmware v3 OTA)
 *
 * OTA: Tools->Port->spacepi-dryer, password: dryer2024
 * Board: Elecrow 3.5" ESP32-WROOM-32 HMI (ILI9488 480x320, resistive touch)
 *
 * IO25 -> SSR input (+)   220V heater  |  IO32 -> IRLZ44N gate  12V fans
 * IO22 -> SHT40 SDA       |  IO21 -> SHT40 SCL   (connector labeled I2C)
 *
 * v2: flicker-free draws (padding + sprites), flame + dual 4-blade fan
 * animation, gradient UI, run-screensaver, on-screen WiFi setup keyboard.
 * SAFETY LOGIC UNCHANGED AND ACTIVE IN ALL SCREENS INCLUDING SCREENSAVER.
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
#define TOUCH_THRESHOLD  145       // lower = more sensitive (was 300)
#define IDLE_TIMEOUT_MS  30000UL   // home → idle clock after 30s no touch
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
              SCR_SAVER, SCR_WIFI, SCR_KBD };
Screen screen = SCR_HOME;

int   selPreset = 0, setTemp = 50, setHours = 6, setMinutes = 0;
float curTemp = 0, curHum = 0;          // raw sensor values (control uses these)
float dispTemp = 0, dispHum = 0;        // smoothed values shown on display
bool  heaterOn = false, fanOn = false, sensorOk = false;
bool  saverEnabled = true;              // toggled in Settings, stored in prefs

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
    bool drying = (screen == SCR_RUN || screen == SCR_SAVER);
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
  if (screen != SCR_RUN) return;   // saver no longer used during drying
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
  if (!want) rwArmed = false;
  if (rwArmed && heaterOn && (curTemp < setTemp - 5)) {
    if (millis() - rwStartMs > RUNAWAY_WINDOW_MS) {
      if (curTemp - rwStartTemp < RUNAWAY_MIN_RISE) { enterFault("E3 no heat rise"); return; }
      rwStartMs = millis(); rwStartTemp = curTemp;
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
void drawHome() {
  idleMode = false;   // full home screen disables idle overlay
  newScreen("SpacePi Dryer");
  drawPresets();
  tft.setTextColor(C_MUTED, C_HDR);  // labels sit on gradient; draw with own bg patches
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_MUTED, C_BG);
  // temp adjuster
  btn(12, 182, 56, 48, "-", C_CARD);
  tft.fillRoundRect(74, 182, 90, 48, 8, C_CARD);
  btn(170, 182, 56, 48, "+", C_CARD);
  drawTempVal();
  // time adjuster
  btn(254, 182, 56, 48, "-", C_CARD);
  tft.fillRoundRect(316, 182, 90, 48, 8, C_CARD);
  btn(412, 182, 56, 48, "+", C_CARD);
  drawTimeVal();
  // live readout card + start + settings
  tft.fillRoundRect(12, 238, 170, 24, 8, C_CARD);
  drawHomeLive();
  btn(12,  270, 230, 44, "START DRYING", C_GOOD, 0x0000);
  btn(250, 270, 110, 44, fanOn ? "FAN OFF" : "FAN ONLY", fanOn ? C_ACCENT : C_CARD, fanOn ? 0x0000 : C_TEXT);
  btn(368, 270, 100, 44, "Setup", C_CARD);
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
    // fan only — toggle fan without heating
    if (fanOn) {
      fanWrite(false);
    } else {
      fanWrite(true);
    }
    // redraw just the fan button
    btn(250, 270, 110, 44, fanOn ? "FAN OFF" : "FAN ONLY", fanOn ? C_ACCENT : C_CARD, fanOn ? 0x0000 : C_TEXT);
  }
  if (hit(tx, ty, 368, 270, 100, 44)) { screen = SCR_SETTINGS; drawSettings(); }
}

// ---------------------------------------------------------------
//                      RUN SCREEN
// ---------------------------------------------------------------
void drawRunStatic() {
  char t[32]; snprintf(t, sizeof(t), "%s  ->  %d C", presets[selPreset].name, setTemp);
  newScreen(t);
  tft.fillRoundRect(12,  44, 220, 104, 10, C_CARD);   // temp card
  tft.fillRoundRect(248, 44, 220, 104, 10, C_CARD);   // humidity card
  tft.fillRoundRect(12, 158, 456,  56, 10, C_CARD);   // timer card
  // status badge area (heater left, fan right) — redrawn dynamically
  tft.fillRoundRect(12,  228, 220, 28, 8, C_CARD);
  tft.fillRoundRect(248, 228, 220, 28, 8, C_CARD);
  btn(368, 262, 100, 50, "STOP", C_BAD);
}

void drawStatusBadges() {
  static bool lastHeaterOn = !heaterOn;   // force draw on first call
  static bool lastFanOn    = !fanOn;

  if (heaterOn != lastHeaterOn) {
    lastHeaterOn = heaterOn;
    uint16_t hbg = heaterOn ? C_HOT  : C_CARD;
    uint16_t hfg = heaterOn ? 0x0000 : C_MUTED;
    tft.fillRoundRect(12, 228, 220, 28, 8, hbg);
    tft.setTextColor(hfg, hbg); tft.setTextDatum(MC_DATUM);
    tft.drawString(heaterOn ? "~ HEATING ~" : "-- HOLDING --", 122, 242, 2);
  }

  if (fanOn != lastFanOn) {
    lastFanOn = fanOn;
    uint16_t fbg = fanOn ? C_ACCENT : C_CARD;
    uint16_t ffg = fanOn ? 0x0000   : C_MUTED;
    tft.fillRoundRect(248, 228, 220, 28, 8, fbg);
    tft.setTextColor(ffg, fbg); tft.setTextDatum(MC_DATUM);
    tft.drawString(fanOn ? "* FAN ON *" : "-- FAN OFF --", 358, 242, 2);
  }

  tft.setTextDatum(TL_DATUM);
}

void drawRunDynamic() {
  // DEBUG — remove after confirming fix
  Serial.printf("drawRunDynamic: sensorOk=%d dispTemp=%.1f dispHum=%.1f curTemp=%.1f curHum=%.1f\n",
                sensorOk, dispTemp, dispHum, curTemp, curHum);

  char v[16];
  snprintf(v, sizeof(v), "%.1f C", dispTemp);
  tft.setTextColor(C_HOT, C_CARD); tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(190);
  Serial.printf("  Drawing temp '%s' at x=122 y=88 color=0x%04X bg=0x%04X\n", v, (unsigned)C_HOT, (unsigned)C_CARD);
  tft.drawString(v, 122, 88, 6);
  tft.setTextColor(C_MUTED, C_CARD); tft.setTextPadding(0);
  tft.drawString("temperature", 122, 130, 2);
  snprintf(v, sizeof(v), "%.0f %%", dispHum);
  tft.setTextColor(C_ACCENT, C_CARD); tft.setTextPadding(190);
  tft.drawString(v, 358, 88, 6);
  tft.setTextColor(C_MUTED, C_CARD); tft.setTextPadding(0);
  tft.drawString("humidity", 358, 130, 2);
  char tl[16]; fmtTimeLeft(tl, sizeof(tl));
  tft.setTextColor(C_TEXT, C_CARD); tft.setTextPadding(250);
  tft.drawString(tl, 240, 184, 6);
  tft.setTextPadding(0);
  unsigned long total = runEndMs - runStartMs;
  int w = (int)(448.0f * min(1.0f, (float)(millis() - runStartMs) / (float)total));
  tft.fillRect(16, 220, 448, 4, C_CARD);
  tft.fillRect(16, 220, w, 4, C_GOOD);
  drawStatusBadges();
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
void enterSaver() {
  // snapshot current readings immediately into dedicated saver variables
  if (sensorOk) {
    saverTemp    = curTemp;
    saverHum     = curHum;
    saverHasData = true;
  }
  // also keep main display values in sync
  if (sensorOk) { dispTemp = curTemp; dispHum = curHum; }
  lastHumUpdateMs = millis();
  screen = SCR_SAVER;
  tft.fillScreen(TFT_BLACK);
  drawSaver();
}

void drawSaverAnimations() {
  // Full 480x320 sprite — redrawn every frame, nothing can overwrite it
  // This is the definitive fix: temp/hum live inside this sprite so they
  // are guaranteed to appear every 120ms regardless of any other draw order.
  sanim.fillSprite(TFT_BLACK);

  // --- TOP: clock + WiFi ---
  sanim.setTextColor(C_ACCENT, TFT_BLACK);
  sanim.setTextDatum(MC_DATUM);
  sanim.setTextPadding(320);
  sanim.drawString(clockStr, 230, 18, 4);
  sanim.setTextPadding(0);
  sanim.fillRoundRect(448, 6, 22, 14, 4, wifiUp ? C_GOOD : 0x3186);

  // --- CENTER: Temp left | Hum right — always from curTemp/curHum ---
  char v[20];
  snprintf(v, sizeof(v), "%.1f", curTemp);
  sanim.setTextColor(C_HOT, TFT_BLACK);
  sanim.setTextPadding(190);
  sanim.setTextDatum(MC_DATUM);
  sanim.drawString(v, 110, 90, 6);
  sanim.setTextPadding(0);
  sanim.setTextColor(C_MUTED, TFT_BLACK);
  sanim.drawString("C", 110, 138, 2);

  snprintf(v, sizeof(v), "%.0f", curHum);
  sanim.setTextColor(C_ACCENT, TFT_BLACK);
  sanim.setTextPadding(190);
  sanim.drawString(v, 350, 90, 6);
  sanim.setTextPadding(0);
  sanim.setTextColor(C_MUTED, TFT_BLACK);
  sanim.drawString("%", 350, 138, 2);

  // vertical divider
  sanim.drawFastVLine(240, 50, 110, 0x2945);

  // --- TIMER button ---
  char tl[16]; fmtTimeLeft(tl, sizeof(tl));
  sanim.fillRoundRect(80, 155, 320, 48, 10, C_CARD);
  sanim.setTextColor(C_TEXT, C_CARD);
  sanim.setTextPadding(300);
  sanim.drawString(tl, 240, 179, 4);
  sanim.setTextPadding(0);

  // --- HEATER animation (bottom-left) ---
  int hx = 100, hy = 270;
  if (heaterOn) {
    int fl = animFrame % 5;
    sanim.fillTriangle(hx-16, hy+26, hx+16, hy+26, hx,    hy-14-fl, C_HOT);
    sanim.fillEllipse(hx, hy+14, 16, 14, C_HOT);
    sanim.fillTriangle(hx-8,  hy+26, hx+8,  hy+26, hx,    hy-fl,    C_FLAME_Y);
    sanim.fillEllipse(hx, hy+16, 9, 9, C_FLAME_Y);
    if (animFrame % 2 == 0)
      sanim.fillTriangle(hx-3, hy+6, hx+3, hy+6, hx, hy-20-fl, C_TEXT);
    sanim.setTextColor(C_HOT, TFT_BLACK);
  } else {
    sanim.fillEllipse(hx, hy+14, 14, 8, 0x001F);
    sanim.fillTriangle(hx-8, hy+14, hx+8, hy+14, hx, hy-8, 0x033F);
    sanim.drawLine(hx-14, hy, hx+14, hy, 0x05FF);
    sanim.drawLine(hx, hy-8, hx, hy+14, 0x05FF);
    sanim.drawLine(hx-10, hy-4, hx+10, hy+10, 0x05FF);
    sanim.drawLine(hx+10, hy-4, hx-10, hy+10, 0x05FF);
    sanim.setTextColor(0x05FF, TFT_BLACK);
  }
  sanim.setTextDatum(MC_DATUM);
  sanim.drawString(heaterOn ? "HEAT" : "COLD", hx, hy+34, 2);

  // --- FAN animation (bottom-right) ---
  int fx = 370, fy = 272;
  sanim.drawCircle(fx, fy, 28, fanOn ? C_ACCENT : C_MUTED);
  if (fanOn) {
    float a0 = animFrame * 0.5f;
    for (int b = 0; b < 4; b++) {
      float a = a0 + b * 1.5708f;
      sanim.fillTriangle(fx+(int)(22*cosf(a)),       fy+(int)(22*sinf(a)),
                         fx+(int)(9 *cosf(a+0.6f)),  fy+(int)(9 *sinf(a+0.6f)),
                         fx+(int)(9 *cosf(a-0.6f)),  fy+(int)(9 *sinf(a-0.6f)), C_ACCENT);
    }
    sanim.fillCircle(fx, fy, 5, C_TEXT);
    sanim.setTextColor(C_ACCENT, TFT_BLACK);
  } else {
    for (int b = 0; b < 4; b++) {
      float a = 0.4f + b * 1.5708f;
      sanim.fillTriangle(fx+(int)(22*cosf(a)),       fy+(int)(22*sinf(a)),
                         fx+(int)(9 *cosf(a+0.6f)),  fy+(int)(9 *sinf(a+0.6f)),
                         fx+(int)(9 *cosf(a-0.6f)),  fy+(int)(9 *sinf(a-0.6f)), C_MUTED);
    }
    sanim.fillCircle(fx, fy, 5, 0x6B4D);
    sanim.drawLine(fx-10, fy-10, fx+10, fy+10, C_BAD);
    sanim.drawLine(fx+10, fy-10, fx-10, fy+10, C_BAD);
    sanim.setTextColor(C_MUTED, TFT_BLACK);
  }
  sanim.setTextDatum(MC_DATUM);
  sanim.drawString("FAN", fx, fy+34, 2);

  // hint
  sanim.setTextColor(0x1A2A3A, TFT_BLACK);
  sanim.drawString("touch to return", 240, 312, 2);

  sanim.pushSprite(0, 0);   // full screen push — covers everything cleanly
}

void drawSaver() {
  // --- TOP: date time + WiFi dot ---
  tft.setTextColor(C_ACCENT, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(320);
  tft.drawString(clockStr, 230, 18, 4);
  tft.setTextPadding(0);
  // WiFi indicator dot top-right
  tft.fillRect(448, 8, 14, 14, TFT_BLACK);
  tft.fillRoundRect(448, 8, 14, 14, 3, wifiUp ? C_GOOD : C_MUTED);

  // --- CENTER: Temp left | Hum right ---
  // Use saverTemp/saverHum — updated every 1s from readSensor, never 0.0
  char v[20];
  if (saverHasData) {
    snprintf(v, sizeof(v), "%.1f", saverTemp);
    tft.setTextColor(C_HOT, TFT_BLACK);
    tft.setTextPadding(190);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(v, 110, 90, 6);
    tft.setTextPadding(0);
    tft.setTextColor(C_MUTED, TFT_BLACK);
    tft.drawString("C", 110, 138, 2);

    snprintf(v, sizeof(v), "%.0f", saverHum);
    tft.setTextColor(C_ACCENT, TFT_BLACK);
    tft.setTextPadding(190);
    tft.drawString(v, 350, 90, 6);
    tft.setTextPadding(0);
    tft.setTextColor(C_MUTED, TFT_BLACK);
    tft.drawString("%", 350, 138, 2);
  } else {
    tft.setTextColor(C_MUTED, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("--", 110, 90, 6);
    tft.drawString("--", 350, 90, 6);
  }

  // divider
  tft.drawFastVLine(240, 50, 110, 0x2945);

  // --- TIMER BUTTON center --- (y=155, height=48 → bottom=203, safely above animation at y=220)
  char tl[16]; fmtTimeLeft(tl, sizeof(tl));
  tft.fillRoundRect(80, 155, 320, 48, 10, C_CARD);
  tft.setTextColor(C_TEXT, C_CARD);
  tft.setTextPadding(300);
  tft.drawString(tl, 240, 179, 4);
  tft.setTextPadding(0);

  // hint at very bottom (below animation sprite at y=220+90=310)
  tft.setTextColor(0x1A2A3A, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("touch anywhere to return", 240, 312, 2);
}
void exitSaver() {
  screen = SCR_RUN;
  tft.fillScreen(TFT_BLACK);  // clear saver
  drawRunStatic();
  drawRunDynamic();
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
  heaterWrite(false);
  historySaveRun(0);   // outcome 0 = completed
  runStartMs = 0;
  screen = SCR_DONE;
  drawDoneStatic();
}
void touchDone(int tx, int ty) {
  if (hit(tx, ty, 140, 254, 200, 50)) {
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
void drawSettings() {
  wifiUp = (WiFi.status() == WL_CONNECTED);   // always check live before drawing
  newScreen("Settings");
  tft.setTextDatum(TL_DATUM);
  tft.fillRoundRect(12, 44, 456, 96, 10, C_CARD);
  tft.setTextColor(C_MUTED, C_CARD);
  tft.drawString("WiFi:", 24, 54, 2);
  if (wifiUp) {
    tft.setTextColor(C_GOOD, C_CARD);
    char ip[52];
    snprintf(ip, sizeof(ip), "%s  %s (DHCP)", cfgSsid.c_str(), WiFi.localIP().toString().c_str());
    tft.drawString(ip, 70, 54, 2);
    tft.setTextColor(C_MUTED, C_CARD);
    tft.drawString("open the address in a phone browser", 24, 78, 2);
    tft.drawString("router device name: spacepi-dryer", 24, 98, 2);
  } else if (WiFi.getMode() & WIFI_AP) {
    tft.setTextColor(C_ACCENT, C_CARD);
    tft.drawString(AP_SSID, 70, 54, 2);
    tft.setTextColor(C_MUTED, C_CARD);
    tft.drawString("join this hotspot, open 192.168.4.1", 24, 78, 2);
  } else {
    tft.setTextColor(C_MUTED, C_CARD);
    tft.drawString("not configured", 70, 54, 2);
  }
  char v[48];
  snprintf(v, sizeof(v), "Time: %s %s", clockStr, timeSynced ? "" : "(needs WiFi)");
  tft.setTextColor(C_MUTED, C_CARD);
  tft.drawString(v, 24, 118, 2);
  btn(12,  160, 148, 50, "WIFI", C_ACCENT, 0x0000);
  btn(172, 160, 148, 50, "CALIBRATE", C_CARD);
  btn(332, 160, 136, 50, "BACK", C_CARD);
  // screensaver toggle
  btn(12, 220, 290, 44,
      saverEnabled ? "SCREENSAVER: ON" : "SCREENSAVER: OFF",
      saverEnabled ? C_GOOD : C_CARD, saverEnabled ? 0x0000 : C_TEXT);
  char s[56];
  snprintf(s, sizeof(s), "Safety: ceiling %.0fC, max set %dC (fixed)", MAX_TEMP_C, MAX_SET_TEMP);
  tft.fillRoundRect(12, 274, 456, 24, 8, C_CARD);
  tft.setTextColor(C_MUTED, C_CARD); tft.setTextDatum(TL_DATUM);
  tft.drawString(s, 24, 278, 2);
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
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>SpacePi Dryer</title>\n"
"<style>\n"
":root{--bg:#0a0e1a;--card:#1a2035;--border:#2a3a55;--text:#e8edf5;--muted:#6b7a99;--accent:#0af;--hot:#f80;--good:#0c6;--bad:#e33;--r:12px}\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{background:var(--bg);color:var(--text);font-family:system-ui,sans-serif;padding-bottom:70px}\n"
"nav{position:fixed;bottom:0;left:0;right:0;background:#111827;border-top:1px solid var(--border);display:flex;z-index:10}\n"
"nav button{flex:1;background:none;border:none;color:var(--muted);padding:12px 4px 8px;font-size:11px;cursor:pointer;display:flex;flex-direction:column;align-items:center;gap:3px}\n"
"nav button.on{color:var(--accent)}\n"
"nav button svg{width:22px;height:22px;stroke:currentColor;fill:none;stroke-width:1.8;stroke-linecap:round}\n"
".page{display:none;padding:16px}\n"
".page.on{display:block}\n"
".card{background:var(--card);border:1px solid var(--border);border-radius:var(--r);padding:16px;margin-bottom:14px}\n"
".lbl{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;margin-bottom:6px}\n"
".big{font-size:40px;font-weight:700;line-height:1}\n"
".hot{color:var(--hot)}.cool{color:var(--accent)}.ok{color:var(--good)}.err{color:var(--bad)}\n"
".row{display:flex;gap:10px;margin-bottom:14px}\n"
".row .card{flex:1;margin:0}\n"
".bar-wrap{background:#232d45;border-radius:4px;height:8px;overflow:hidden;margin:8px 0}\n"
".bar{height:100%;border-radius:4px;background:var(--accent);transition:width 1s linear}\n"
".badge{display:inline-flex;align-items:center;gap:5px;padding:3px 10px;border-radius:20px;font-size:12px;font-weight:600}\n"
".badge.heat{background:rgba(255,136,0,.15);color:var(--hot);border:1px solid rgba(255,136,0,.3)}\n"
".badge.cool-badge{background:rgba(107,122,153,.15);color:var(--muted);border:1px solid var(--border)}\n"
".badge.fan-on{background:rgba(0,170,255,.15);color:var(--accent);border:1px solid rgba(0,170,255,.3)}\n"
".dot{width:7px;height:7px;border-radius:50%;background:currentColor}\n"
"button.btn{display:inline-flex;align-items:center;justify-content:center;padding:11px 20px;border-radius:9px;font-size:14px;font-weight:600;cursor:pointer;border:none;margin:4px 4px 4px 0}\n"
".btn-go{background:var(--good);color:#000}\n"
".btn-stop{background:var(--bad);color:#fff}\n"
".btn-neu{background:#232d45;color:var(--text);border:1px solid var(--border)}\n"
".btn-acc{background:rgba(0,170,255,.2);color:var(--accent);border:1px solid rgba(0,170,255,.3)}\n"
"input[type=range]{width:100%;accent-color:var(--accent);margin:6px 0}\n"
"input[type=text]{width:100%;background:#232d45;border:1px solid var(--border);color:var(--text);padding:10px 12px;border-radius:9px;font-size:14px}\n"
"select{width:100%;background:#232d45;border:1px solid var(--border);color:var(--text);padding:10px 12px;border-radius:9px;font-size:14px;margin-bottom:10px}\n"
".preset-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:12px}\n"
".pb{background:#232d45;border:1px solid var(--border);color:var(--muted);padding:10px 4px;border-radius:9px;font-size:12px;cursor:pointer;text-align:center;line-height:1.5}\n"
".pb.on{border-color:var(--accent);background:rgba(0,170,255,.1);color:var(--accent)}\n"
".pb b{display:block;font-size:13px}\n"
"#conn-bar{height:3px;background:var(--bad);transition:background .3s}\n"
"#conn-bar.ok{background:var(--good)}\n"
"table{width:100%;border-collapse:collapse;font-size:12px}\n"
"td,th{padding:7px 4px;border-bottom:1px solid var(--border);text-align:center}\n"
"th{color:var(--muted);font-size:11px;text-transform:uppercase}\n"
"td:first-child,th:first-child{text-align:left}\n"
"canvas{width:100%;display:block}\n"
"#toast{position:fixed;bottom:76px;left:50%;transform:translateX(-50%);background:var(--card);border:1px solid var(--border);border-radius:9px;padding:10px 18px;font-size:13px;opacity:0;transition:opacity .3s;pointer-events:none;z-index:99;white-space:nowrap}\n"
"#toast.on{opacity:1}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div id=\"conn-bar\"></div>\n"
"\n"
"<!-- DASHBOARD -->\n"
"<div class=\"page on\" id=\"p-dash\">\n"
"  <div class=\"row\">\n"
"    <div class=\"card\"><div class=\"lbl\">Temperature</div><div class=\"big hot\" id=\"d-t\">--</div><div style=\"color:var(--muted);font-size:12px\"> CC</div></div>\n"
"    <div class=\"card\"><div class=\"lbl\">Humidity</div><div class=\"big cool\" id=\"d-h\">--</div><div style=\"color:var(--muted);font-size:12px\">%RH</div></div>\n"
"  </div>\n"
"  <div class=\"card\">\n"
"    <div class=\"lbl\">Time remaining</div>\n"
"    <div class=\"big\" id=\"d-timer\" style=\"font-size:30px;font-family:monospace\">--:--:--</div>\n"
"    <div class=\"bar-wrap\"><div class=\"bar\" id=\"d-bar\" style=\"width:0%\"></div></div>\n"
"    <div style=\"font-size:12px;color:var(--muted)\" id=\"d-prog\">idle</div>\n"
"  </div>\n"
"  <div class=\"row\">\n"
"    <div class=\"card\" style=\"display:flex;align-items:center;gap:10px\">\n"
"      <canvas id=\"c-flame\" width=\"44\" height=\"54\"></canvas>\n"
"      <div><div class=\"lbl\">Heater</div><span class=\"badge cool-badge\" id=\"d-hbadge\"><span class=\"dot\"></span>OFF</span></div>\n"
"    </div>\n"
"    <div class=\"card\" style=\"display:flex;align-items:center;gap:10px\">\n"
"      <canvas id=\"c-fan\" width=\"44\" height=\"44\"></canvas>\n"
"      <div><div class=\"lbl\">Fan</div><span class=\"badge cool-badge\" id=\"d-fbadge\"><span class=\"dot\"></span>OFF</span></div>\n"
"    </div>\n"
"  </div>\n"
"  <div class=\"card\" id=\"fault-box\" style=\"display:none;border-color:var(--bad)\">\n"
"    <div class=\"lbl err\">Fault</div>\n"
"    <div id=\"fault-msg\" style=\"margin:6px 0;font-size:14px\"></div>\n"
"    <button class=\"btn btn-neu\" onclick=\"api('action=reset')\">Reset (unlocks when cool)</button>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- CONTROL -->\n"
"<div class=\"page\" id=\"p-ctrl\">\n"
"  <div class=\"card\">\n"
"    <div class=\"lbl\">Preset</div>\n"
"    <div class=\"preset-grid\" id=\"presets\"></div>\n"
"    <div class=\"lbl\">Temperature</div>\n"
"    <div style=\"display:flex;align-items:center;gap:10px\">\n"
"      <input type=\"range\" id=\"sl-t\" min=\"35\" max=\"85\" step=\"5\" value=\"50\" oninput=\"document.getElementById('lbl-t').textContent=this.value+' CC'\">\n"
"      <span id=\"lbl-t\" style=\"min-width:44px;font-weight:600\">50 CC</span>\n"
"    </div>\n"
"    <div class=\"lbl\" style=\"margin-top:10px\">Duration</div>\n"
"    <div style=\"display:flex;align-items:center;gap:10px\">\n"
"      <input type=\"range\" id=\"sl-h\" min=\"0.5\" max=\"24\" step=\"0.5\" value=\"6\" oninput=\"updateDur()\">\n"
"      <span id=\"lbl-h\" style=\"min-width:44px;font-weight:600\">6:00</span>\n"
"    </div>\n"
"    <div style=\"margin-top:12px\">\n"
"      <button class=\"btn btn-go\" onclick=\"startDry()\">> Start</button>\n"
"      <button class=\"btn btn-stop\" onclick=\"api('action=stop')\">[..] Stop</button>\n"
"    </div>\n"
"  </div>\n"
"  <div class=\"card\">\n"
"    <div class=\"lbl\">Fault reset</div>\n"
"    <div style=\"font-size:13px;color:var(--muted);margin-bottom:10px\">Available after chamber cools below 50 CC</div>\n"
"    <button class=\"btn btn-acc\" onclick=\"api('action=reset')\">Reset fault</button>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- HISTORY -->\n"
"<div class=\"page\" id=\"p-hist\">\n"
"  <div class=\"row\" style=\"flex-wrap:wrap\">\n"
"    <div class=\"card\" style=\"min-width:80px\"><div class=\"lbl\">Total</div><div class=\"big\" style=\"font-size:28px\" id=\"hs-tot\">--</div></div>\n"
"    <div class=\"card\" style=\"min-width:80px\"><div class=\"lbl\">Done</div><div class=\"big ok\" style=\"font-size:28px\" id=\"hs-ok\">--</div></div>\n"
"    <div class=\"card\" style=\"min-width:80px\"><div class=\"lbl\">Stopped</div><div class=\"big hot\" style=\"font-size:28px\" id=\"hs-st\">--</div></div>\n"
"    <div class=\"card\" style=\"min-width:80px\"><div class=\"lbl\">Faults</div><div class=\"big err\" style=\"font-size:28px\" id=\"hs-er\">--</div></div>\n"
"  </div>\n"
"  <div class=\"card\">\n"
"    <div class=\"lbl\">Peak temp per run (last 30)</div>\n"
"    <canvas id=\"c-chart\" height=\"140\"></canvas>\n"
"  </div>\n"
"  <div class=\"card\" id=\"fault-log\" style=\"display:none\">\n"
"    <div class=\"lbl\">Fault log</div>\n"
"    <div id=\"fault-log-body\" style=\"font-size:13px;margin-top:8px\"></div>\n"
"  </div>\n"
"  <div class=\"card\">\n"
"    <div class=\"lbl\">Run log</div>\n"
"    <div style=\"overflow-x:auto\">\n"
"      <table>\n"
"        <thead><tr><th>Date</th><th>Preset</th><th>Peak</th><th>Hum</th><th>Result</th></tr></thead>\n"
"        <tbody id=\"runs-body\"></tbody>\n"
"      </table>\n"
"      <p id=\"runs-empty\" style=\"text-align:center;color:var(--muted);padding:20px;font-size:13px\">No runs yet</p>\n"
"    </div>\n"
"    <button class=\"btn btn-neu\" style=\"margin-top:10px\" onclick=\"clearHist()\">Clear history</button>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- SETTINGS -->\n"
"<div class=\"page\" id=\"p-set\">\n"
"  <div class=\"card\">\n"
"    <div class=\"lbl\">Connection</div>\n"
"    <div style=\"font-size:13px;color:var(--muted);margin-bottom:10px\">\n"
"      Open this page from the dryer's IP address and it connects automatically.<br>\n"
"      Or enter the IP manually below.\n"
"    </div>\n"
"    <input type=\"text\" id=\"ip-in\" placeholder=\"192.168.1.x\" style=\"margin-bottom:8px\">\n"
"    <button class=\"btn btn-acc\" onclick=\"saveIp()\">Connect</button>\n"
"    <div style=\"font-size:12px;color:var(--muted);margin-top:8px\" id=\"ip-status\">--</div>\n"
"  </div>\n"
"  <div class=\"card\">\n"
"    <div class=\"lbl\">About</div>\n"
"    <div style=\"font-size:13px;color:var(--muted);line-height:1.8\">\n"
"      SpacePi Dryer - Mod v01 Mike<br>\n"
"      Safety limits enforced on device only.<br>\n"
"      <a href=\"https://github.com/mehregan59/Filament-dryer\" style=\"color:var(--accent)\">GitHub</a><br><br>\n"
"<b style=\"color:var(--hot)\">Multiple devices:</b> polling auto-pauses when this tab is hidden or screen is off.\n"
"If another device is connected, close or background that browser tab first.\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<!-- BOTTOM NAV -->\n"
"<nav>\n"
"  <button class=\"on\" onclick=\"nav('dash',this)\">\n"
"    <svg viewBox=\"0 0 24 24\"><rect x=\"3\" y=\"3\" width=\"7\" height=\"7\" rx=\"1\"/><rect x=\"14\" y=\"3\" width=\"7\" height=\"7\" rx=\"1\"/><rect x=\"3\" y=\"14\" width=\"7\" height=\"7\" rx=\"1\"/><rect x=\"14\" y=\"14\" width=\"7\" height=\"7\" rx=\"1\"/></svg>\n"
"    Dashboard\n"
"  </button>\n"
"  <button onclick=\"nav('ctrl',this)\">\n"
"    <svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"3\"/><path d=\"M12 2v3M12 19v3M4.22 4.22l2.12 2.12M17.66 17.66l2.12 2.12M2 12h3M19 12h3M4.22 19.78l2.12-2.12M17.66 6.34l2.12-2.12\"/></svg>\n"
"    Control\n"
"  </button>\n"
"  <button onclick=\"nav('hist',this);loadHist()\">\n"
"    <svg viewBox=\"0 0 24 24\"><polyline points=\"22 12 18 12 15 21 9 3 6 12 2 12\"/></svg>\n"
"    History\n"
"  </button>\n"
"  <button onclick=\"nav('set',this)\">\n"
"    <svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"3\"/><path d=\"M19.4 15a1.65 1.65 0 00.33 1.82l.06.06a2 2 0 010 2.83 2 2 0 01-2.83 0l-.06-.06a1.65 1.65 0 00-1.82-.33 1.65 1.65 0 00-1 1.51V21a2 2 0 01-2 2 2 2 0 01-2-2v-.09A1.65 1.65 0 009 19.4a1.65 1.65 0 00-1.82.33l-.06.06a2 2 0 01-2.83 0 2 2 0 010-2.83l.06-.06A1.65 1.65 0 004.68 15a1.65 1.65 0 00-1.51-1H3a2 2 0 01-2-2 2 2 0 012-2h.09A1.65 1.65 0 004.6 9a1.65 1.65 0 00-.33-1.82l-.06-.06a2 2 0 010-2.83 2 2 0 012.83 0l.06.06A1.65 1.65 0 009 4.68a1.65 1.65 0 001-1.51V3a2 2 0 012-2 2 2 0 012 2v.09a1.65 1.65 0 001 1.51 1.65 1.65 0 001.82-.33l.06-.06a2 2 0 012.83 0 2 2 0 010 2.83l-.06.06A1.65 1.65 0 0019.4 9a1.65 1.65 0 001.51 1H21a2 2 0 012 2 2 2 0 01-2 2h-.09a1.65 1.65 0 00-1.51 1z\"/></svg>\n"
"    Settings\n"
"  </button>\n"
"</nav>\n"
"<div id=\"toast\"></div>\n"
"\n"
"<script>\n"
"// -- STATE ----------------------------------------------------------\n"
"const PRESETS=[{n:'PLA',t:50,h:6},{n:'PETG',t:60,h:6},{n:'ABS',t:70,h:8},{n:'ASA',t:70,h:8},{n:'PA/NY',t:80,h:12},{n:'PC',t:85,h:10}];\n"
"let ip='', poll_t=null, af=0, fa=0, sel=0, connected=false, dryer={}, totalMs=0, startMs=0;\n"
"\n"
"// -- IP DETECTION ---------------------------------------------------\n"
"// If served from ESP32 directly, use its IP - no CORS, no config needed\n"
"function detectIp() {\n"
"  const h = location.hostname;\n"
"  if (h && h !== 'localhost' && h !== '127.0.0.1' && location.protocol === 'http:') {\n"
"    ip = h;  // page served from ESP32 - use same host\n"
"    document.getElementById('ip-in').value = ip;\n"
"    document.getElementById('ip-status').textContent = 'Auto-connected to ' + ip;\n"
"    return true;\n"
"  }\n"
"  const stored = localStorage.getItem('dryer_ip');\n"
"  if (stored) { ip = stored; document.getElementById('ip-in').value = ip; return true; }\n"
"  return false;\n"
"}\n"
"\n"
"// -- FETCH - always relative when same host, absolute otherwise -----\n"
"function apiUrl(path) {\n"
"  if (ip && ip === location.hostname) return path;   // same origin - no CORS at all\n"
"  return 'http://' + ip + path;\n"
"}\n"
"\n"
"async function apiFetch(path, opts) {\n"
"  // When same-origin: no mode needed. When cross-origin: use cors.\n"
"  const same = ip === location.hostname;\n"
"  const defaults = { cache: 'no-store', signal: AbortSignal.timeout(4000) };\n"
"  if (!same) defaults.mode = 'cors';\n"
"  return fetch(apiUrl(path), Object.assign(defaults, opts));\n"
"}\n"
"\n"
"// -- POLLING --------------------------------------------------------\n"
"function startPoll() {\n"
"  if (poll_t) clearInterval(poll_t);\n"
"  doPoll();\n"
"  poll_t = setInterval(doPoll, 2000);\n"
"}\n"
"async function doPoll() {\n"
"  if (!ip) return;\n"
"  try {\n"
"    const r = await apiFetch('/api');\n"
"    if (!r.ok) throw new Error(r.status);\n"
"    dryer = await r.json();\n"
"    connected = true;\n"
"    document.getElementById('conn-bar').className = 'ok';\n"
"    document.getElementById('ip-status').textContent = 'Connected . ' + ip;\n"
"    render();\n"
"  } catch(e) {\n"
"    connected = false;\n"
"    document.getElementById('conn-bar').className = '';\n"
"    document.getElementById('ip-status').textContent = 'Cannot connect to ' + ip;\n"
"  }\n"
"}\n"
"\n"
"async function api(q) {\n"
"  if (!ip) return toast('Not connected');\n"
"  try {\n"
"    const r = await apiFetch('/cmd?' + q);\n"
"    toast(await r.text()); doPoll();\n"
"  } catch(e) { toast('Failed'); }\n"
"}\n"
"\n"
"// -- RENDER ---------------------------------------------------------\n"
"function render() {\n"
"  const d = dryer;\n"
"  document.getElementById('d-t').textContent = (+d.t).toFixed(1);\n"
"  document.getElementById('d-h').textContent = Math.round(+d.h);\n"
"  document.getElementById('d-timer').textContent = d.left || '--:--:--';\n"
"  const dry = d.state === 'drying';\n"
"  if (dry && d.left && d.left !== '--:--:--') {\n"
"    const [hh,mm,ss] = d.left.split(':').map(Number);\n"
"    const left = hh*3600+mm*60+ss;\n"
"    if (!startMs) { startMs = Date.now(); totalMs = left*1000; }\n"
"    const pct = totalMs ? Math.min(100,(totalMs-left*1000)/totalMs*100) : 0;\n"
"    document.getElementById('d-bar').style.width = pct.toFixed(1)+'%';\n"
"    document.getElementById('d-prog').textContent = pct.toFixed(0)+'% - '+d.left+' left';\n"
"  } else { startMs=0; document.getElementById('d-bar').style.width=d.state==='finished'?'100%':'0%'; document.getElementById('d-prog').textContent=d.state; }\n"
"  setBadge('d-hbadge', d.heater, 'heat', 'HEATING', 'cool-badge', 'HOLDING');\n"
"  setBadge('d-fbadge', d.fan,    'fan-on','FAN ON', 'cool-badge', 'OFF');\n"
"  const fb = document.getElementById('fault-box');\n"
"  const fault = d.state && (d.state.startsWith('E') || d.state.startsWith('e'));\n"
"  fb.style.display = fault ? 'block' : 'none';\n"
"  if (fault) document.getElementById('fault-msg').textContent = d.state;\n"
"}\n"
"function setBadge(id, val, cls1, lbl1, cls2, lbl2) {\n"
"  const el = document.getElementById(id);\n"
"  el.className = 'badge ' + (val ? cls1 : cls2);\n"
"  el.innerHTML = '<span class=\"dot\"></span>' + (val ? lbl1 : lbl2);\n"
"}\n"
"\n"
"// -- NAV ------------------------------------------------------------\n"
"function nav(id, btn) {\n"
"  document.querySelectorAll('.page').forEach(p=>p.classList.remove('on'));\n"
"  document.querySelectorAll('nav button').forEach(b=>b.classList.remove('on'));\n"
"  document.getElementById('p-'+id).classList.add('on');\n"
"  btn.classList.add('on');\n"
"}\n"
"\n"
"// -- PRESETS --------------------------------------------------------\n"
"function buildPresets() {\n"
"  document.getElementById('presets').innerHTML = PRESETS.map((p,i)=>\n"
"    `<button class=\"pb${i===0?' on':''}\" onclick=\"selPreset(${i})\"><b>${p.n}</b>${p.t} CC . ${p.h}h</button>`\n"
"  ).join('');\n"
"}\n"
"function selPreset(i) {\n"
"  sel=i;\n"
"  document.querySelectorAll('.pb').forEach((b,j)=>b.classList.toggle('on',j===i));\n"
"  document.getElementById('sl-t').value=PRESETS[i].t;\n"
"  document.getElementById('lbl-t').textContent=PRESETS[i].t+' CC';\n"
"  document.getElementById('sl-h').value=PRESETS[i].h;\n"
"  updateDur();\n"
"}\n"
"function updateDur() {\n"
"  const v=+document.getElementById('sl-h').value;\n"
"  document.getElementById('lbl-h').textContent=Math.floor(v)+':'+(v%1?.5*60<10?'0':''+(v%1?30:0)+'').padStart(2,'0');\n"
"}\n"
"function startDry() {\n"
"  const t=document.getElementById('sl-t').value;\n"
"  const m=Math.round(+document.getElementById('sl-h').value*60);\n"
"  api('action=start&preset='+sel+'&temp='+t+'&min='+m);\n"
"}\n"
"\n"
"// -- HISTORY --------------------------------------------------------\n"
"async function loadHist() {\n"
"  if (!ip) return;\n"
"  try {\n"
"    const r = await apiFetch('/history');\n"
"    const d = await r.json();\n"
"    document.getElementById('hs-tot').textContent=d.stats.total;\n"
"    document.getElementById('hs-ok').textContent=d.stats.completed;\n"
"    document.getElementById('hs-st').textContent=d.stats.stopped;\n"
"    document.getElementById('hs-er').textContent=d.stats.faulted;\n"
"    drawChart(d.runs.slice(-30));\n"
"    // fault log\n"
"    const fl=document.getElementById('fault-log'), fb=document.getElementById('fault-log-body');\n"
"    if(d.faults.length){fl.style.display='block';fb.innerHTML=d.faults.slice().reverse().map(f=>`<div style=\"padding:4px 0;border-bottom:1px solid var(--border);color:var(--bad)\">${f.code} <span style=\"color:var(--muted);float:right\">${f.ts?new Date(f.ts*1000).toLocaleString():'--'}</span></div>`).join('');}else{fl.style.display='none';}\n"
"    // table\n"
"    const tb=document.getElementById('runs-body'), em=document.getElementById('runs-empty');\n"
"    if(!d.runs.length){tb.innerHTML='';em.style.display='block';return;}\n"
"    em.style.display='none';\n"
"    const rc={completed:'<span style=\"color:var(--good)\">OK</span>',stopped:'<span style=\"color:var(--hot)\">[]</span>',faulted:'<span style=\"color:var(--bad)\">!</span>'};\n"
"    tb.innerHTML=d.runs.slice().reverse().map(r=>`<tr><td>${r.ts?new Date(r.ts*1000).toLocaleDateString():'--'}</td><td>${r.preset}</td><td style=\"color:var(--hot)\">${r.peak} CC</td><td>${r.startHum}->${r.endHum}%</td><td>${rc[r.outcome]||r.outcome}</td></tr>`).join('');\n"
"  } catch(e) { toast('History load failed'); }\n"
"}\n"
"function drawChart(runs) {\n"
"  const c=document.getElementById('c-chart'), ctx=c.getContext('2d');\n"
"  c.width=c.offsetWidth||360; c.height=140;\n"
"  ctx.clearRect(0,0,c.width,c.height);\n"
"  if(!runs.length){ctx.fillStyle='#3a4a6a';ctx.font='13px system-ui';ctx.textAlign='center';ctx.fillText('No runs yet',c.width/2,70);return;}\n"
"  const pad={l:28,r:8,t:8,b:20},cw=c.width-pad.l-pad.r,ch=c.height-pad.t-pad.b;\n"
"  const maxT=Math.max(...runs.map(r=>r.peak),50);\n"
"  const bw=Math.max(4,cw/runs.length*.7),gap=cw/runs.length;\n"
"  for(let i=0;i<=4;i++){const y=pad.t+i*(ch/4);ctx.fillStyle='#1e2a42';ctx.fillRect(pad.l,y,cw,.5);ctx.fillStyle='#4a6080';ctx.font='9px monospace';ctx.textAlign='right';ctx.fillText((maxT-i*maxT/4).toFixed(0),pad.l-3,y+3);}\n"
"  const oc={completed:'#0c6',stopped:'#f80',faulted:'#e33'};\n"
"  runs.forEach((r,i)=>{const x=pad.l+i*gap+gap/2-bw/2,h=(r.peak/maxT)*ch,y=pad.t+ch-h;ctx.fillStyle=oc[r.outcome]||'#4a6080';ctx.fillRect(x,y,bw,h);});\n"
"}\n"
"async function clearHist() {\n"
"  if(!confirm('Clear all history?'))return;\n"
"  await apiFetch('/history/clear'); toast('Cleared'); loadHist();\n"
"}\n"
"\n"
"// -- ANIMATIONS -----------------------------------------------------\n"
"function drawFlame(on) {\n"
"  const c=document.getElementById('c-flame'),ctx=c.getContext('2d');\n"
"  ctx.clearRect(0,0,44,54);\n"
"  if(on){const fl=af%5;ctx.fillStyle='#f80';ctx.beginPath();ctx.moveTo(22-11,50);ctx.quadraticCurveTo(4,32,22,14-fl);ctx.quadraticCurveTo(40,32,22+11,50);ctx.closePath();ctx.fill();ctx.fillStyle='#ffe040';ctx.beginPath();ctx.moveTo(22-6,50);ctx.quadraticCurveTo(13,38,22,26-fl);ctx.quadraticCurveTo(31,38,22+6,50);ctx.closePath();ctx.fill();}\n"
"  else{ctx.fillStyle='#1a3060';ctx.beginPath();ctx.ellipse(22,44,12,7,0,0,Math.PI*2);ctx.fill();ctx.fillStyle='#2244a0';ctx.beginPath();ctx.moveTo(15,44);ctx.lineTo(29,44);ctx.lineTo(22,26);ctx.closePath();ctx.fill();ctx.strokeStyle='#0af';ctx.lineWidth=1.5;[[8,36,36,36],[22,24,22,48],[13,29,31,43],[31,29,13,43]].forEach(([x1,y1,x2,y2])=>{ctx.beginPath();ctx.moveTo(x1,y1);ctx.lineTo(x2,y2);ctx.stroke();});}\n"
"}\n"
"function drawFan(on) {\n"
"  const c=document.getElementById('c-fan'),ctx=c.getContext('2d'),cx=22,cy=22;\n"
"  ctx.clearRect(0,0,44,44);\n"
"  ctx.strokeStyle=on?'#0af':'#334';ctx.lineWidth=1.5;ctx.beginPath();ctx.arc(cx,cy,18,0,Math.PI*2);ctx.stroke();\n"
"  for(let b=0;b<4;b++){const a=fa+b*Math.PI/2;ctx.fillStyle=on?'#0af':'#2a3a50';ctx.beginPath();ctx.moveTo(cx+15*Math.cos(a),cy+15*Math.sin(a));ctx.lineTo(cx+6*Math.cos(a+.6),cy+6*Math.sin(a+.6));ctx.lineTo(cx+6*Math.cos(a-.6),cy+6*Math.sin(a-.6));ctx.closePath();ctx.fill();}\n"
"  ctx.fillStyle='#eee';ctx.beginPath();ctx.arc(cx,cy,3.5,0,Math.PI*2);ctx.fill();\n"
"  if(!on){ctx.strokeStyle='#e33';ctx.beginPath();ctx.moveTo(13,13);ctx.lineTo(31,31);ctx.stroke();ctx.beginPath();ctx.moveTo(31,13);ctx.lineTo(13,31);ctx.stroke();}\n"
"}\n"
"function animLoop(){\n"
"  af++;if(dryer.fan)fa+=.1;\n"
"  drawFlame(dryer.heater);drawFan(dryer.fan);\n"
"  requestAnimationFrame(animLoop);\n"
"}\n"
"\n"
"// -- SETTINGS -------------------------------------------------------\n"
"function saveIp(){\n"
"  const v=document.getElementById('ip-in').value.trim();\n"
"  if(!v)return toast('Enter an IP');\n"
"  ip=v;localStorage.setItem('dryer_ip',ip);\n"
"  document.getElementById('ip-status').textContent='Connecting...';\n"
"  startPoll();\n"
"}\n"
"\n"
"// -- TOAST ----------------------------------------------------------\n"
"function toast(m){const t=document.getElementById('toast');t.textContent=m;t.className='on';setTimeout(()=>t.className='',2500);}\n"
"\n"
"// -- INIT -----------------------------------------------------------\n"
"buildPresets(); updateDur();\n"
"// Multi-client: pause polling if page hidden (phone screen off / different tab)\n"
"// This prevents laptop polling from blocking phone and vice versa\n"
"document.addEventListener('visibilitychange', function() {\n"
"  if (document.hidden) {\n"
"    if (poll_t) { clearInterval(poll_t); poll_t = null; }\n"
"  } else {\n"
"    if (ip) startPoll();\n"
"  }\n"
"});\n"
"if (detectIp()) startPoll();\n"
"animLoop();\n"
"</script>\n"
"</body>\n"
"</html>\n"
"\n"
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
  prefs.putInt("rcount", 0); prefs.putInt("fcount", 0);
  server.send(200, "text/plain", "cleared");
}
void handleApi()  {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char tl[16] = "--:--:--";
  if (screen == SCR_RUN) fmtTimeLeft(tl, sizeof(tl));
  const char* st = screen == SCR_RUN ? "drying" :
                   screen == SCR_DONE ? "finished" :
                   screen == SCR_FAULT ? faultMsg : "idle";
  char buf[200];
  snprintf(buf, sizeof(buf),
    "{\"t\":%.1f,\"h\":%.1f,\"left\":\"%s\",\"heater\":%d,\"fan\":%d,\"state\":\"%s\"}",
    curTemp, curHum, tl, heaterOn ? 1 : 0, fanOn ? 1 : 0, st);
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
  if (a == "stop") {
    heaterWrite(false);
    if (runStartMs > 0) { historySaveRun(1); runStartMs = 0; }
    if (screen == SCR_RUN) { screen = SCR_HOME; drawHome(); }
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
    if (screen != SCR_RUN) startRun();
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
void touchSettings(int tx, int ty) {
  if (hit(tx, ty, 12,  160, 148, 50)) { screen = SCR_WIFI; drawWifiList(); return; }
  if (hit(tx, ty, 172, 160, 148, 50)) { runTouchCalibration(); drawSettings(); return; }
  if (hit(tx, ty, 332, 160, 136, 50)) { screen = SCR_HOME; drawHome(); return; }
  if (hit(tx, ty, 12,  220, 290, 44)) {
    saverEnabled = !saverEnabled;
    prefs.putBool("saver", saverEnabled);
    drawSettings();
    return;
  }
}

// ---------------------------------------------------------------
//                      RUN START
// ---------------------------------------------------------------
void startRun() {
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
  sanim.createSprite(480, 320);  // screensaver — full screen sprite, nothing can overwrite

  prefs.begin("dryer", false);
  saverEnabled = prefs.getBool("saver", true);
  historyLoad();   // load run log and fault log from NVS
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

  wifiInit();
  otaInit();
  readSensor();
  dispTemp = curTemp;   // seed display values from first real reading
  dispHum  = curHum;
  lastHumUpdateMs = millis();
  lastTouchMs = millis();
  drawHome();
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

  // cooldown after run
  if (screen == SCR_DONE || (screen == SCR_HOME && fanOn)) {
    if (sensorOk && curTemp <= COOLDOWN_TEMP) fanWrite(false);
    else if (screen == SCR_DONE) fanWrite(true);
  }

  // Handle web clients and OTA FIRST — before any slow drawing
  server.handleClient();
  ArduinoOTA.handle();

  // idle clock mode on home screen after 30s no touch
  if (screen == SCR_HOME && !idleMode && now - lastTouchMs > IDLE_TIMEOUT_MS) {
    idleMode = true;
    drawIdleScreen(true);
    server.handleClient();
  }
  // only animate idle when actually on home screen
  if (idleMode && screen == SCR_HOME && now - lastIdleAnimMs >= 300) {
    lastIdleAnimMs = now;
    drawIdleScreen(false);
    server.handleClient();   // serve requests queued during slow draw
  }
  // if screen changed away from home while idle, cancel idle mode
  if (idleMode && screen != SCR_HOME) idleMode = false;

  // screensaver disabled during drying — run screen stays visible always

  // animation + dynamic redraws
  if (now - lastAnimMs >= 120) {
    lastAnimMs = now; animFrame++;
    switch (screen) {
      case SCR_RUN:
        drawAnimStrip();
        if (animFrame % 8 == 0) drawRunDynamic();
        break;
      case SCR_SAVER:
        drawSaverAnimations();
        break;
      case SCR_DONE:  drawDoneAnim(); break;
      case SCR_FAULT: if (animFrame % 8 == 0) faultTick(); break;
      case SCR_HOME:
        if (!idleMode && animFrame % 16 == 0) drawHomeLive();
        break;
      default: break;
    }
  }

  // touch
  uint16_t tx, ty;
  if (tft.getTouch(&tx, &ty, TOUCH_THRESHOLD)) {
    lastTouchMs = now;
    if (idleMode) { touchIdleScreen(tx, ty); delay(120); }
    else switch (screen) {
      case SCR_HOME:     touchHome(tx, ty);     break;
      case SCR_RUN:      touchRun(tx, ty);      break;
      case SCR_SAVER:    exitSaver();           break;
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
