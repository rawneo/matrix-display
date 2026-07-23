// Matrix Display — ESP8266 (NodeMCU v2) port of the Raspberry Pi app.py
// 7x MAX7219 FC-16 chain: DIN=D7(GPIO13), CLK=D5(GPIO14), CS=D8(GPIO15)
// DHT11 on D2 (GPIO4). Web UI + live canvas replica on http://<ip>/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <math.h>
#include <SPI.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include "config.h"

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define CS_PIN 15  // D8
#define PANEL_W (MAX_DEVICES * 8)

MD_Parola parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);
MD_MAX72XX *mx;
ESP8266WebServer server(80);
DHTesp dht;

// ---------------------------------------------------------------- modes ----
enum Mode {
  M_CLOCK, M_TEXT, M_WEATHER, M_DHT, M_CHARSET, M_PACMAN, M_DANCER,
  M_KITT, M_EQ, M_RAIN, M_FIREWORKS, M_STARFIELD, M_ECG, M_IP
};
Mode mode = M_IP;
uint32_t lastFrame = 0;   // millis of last animation frame
uint32_t frameNo = 0;     // per-mode frame counter

char textMsg[160];        // current scroll message (text/weather/dht/ip)
char timeOverride[8] = "";   // test pattern for the clock face (e.g. "88:88")

// ------------------------------------------------------------- sprites ----
const uint8_t RUNNER_FRAMES[4][8] PROGMEM = {
  {0x18,0x18,0x5A,0x3C,0x18,0x18,0x24,0x42},
  {0x18,0x18,0x3C,0x18,0x18,0x18,0x18,0x3C},
  {0x18,0x18,0x5A,0x3C,0x18,0x18,0x42,0x24},
  {0x18,0x18,0x3C,0x18,0x18,0x18,0x18,0x3C},
};
const uint8_t PACMAN_LEFT[2][8] PROGMEM = {
  {0x3C,0x7E,0x1F,0x0F,0x0F,0x1F,0x7E,0x3C},
  {0x3C,0x7E,0xFF,0xFF,0xFF,0xFF,0x7E,0x3C},
};
const uint8_t PACMAN_RIGHT[2][8] PROGMEM = {
  {0x3C,0x7E,0xF8,0xF0,0xF0,0xF8,0x7E,0x3C},
  {0x3C,0x7E,0xFF,0xFF,0xFF,0xFF,0x7E,0x3C},
};
const uint8_t GHOST_FRAMES[2][8] PROGMEM = {
  {0x3C,0x7E,0xDB,0xFF,0xFF,0xFF,0xFF,0xAA},
  {0x3C,0x7E,0xDB,0xFF,0xFF,0xFF,0xFF,0x55},
};
const uint8_t DANCER_FRAMES[4][8] PROGMEM = {
  {0x00,0x18,0x18,0x7E,0x18,0x18,0x18,0x3C},   // arms-out bop
  {0x00,0x18,0x18,0xF8,0x18,0x18,0x1E,0x10},   // kick right / arms left
  {0x00,0x18,0x5A,0x18,0x18,0x18,0x18,0x3C},   // arms up
  {0x00,0x18,0x18,0x1F,0x18,0x18,0x78,0x08},   // kick left / arms right
};
const uint8_t SHIP[8] PROGMEM = {0x00,0x00,0x0C,0x1E,0x7F,0x1E,0x0C,0x00};

// ECG: one heartbeat (PQRST), row values, baseline 5
const uint8_t ECG_BEAT[26] PROGMEM = {5,5,5,5,5,5,4,4,5,5,5,6,0,7,5,5,5,4,3,4,5,5,5,5,5,5};

// ------------------------------------------------------- pixel helpers ----
static inline void px(int x, int y, bool on = true) {
  if (x < 0 || x >= PANEL_W || y < 0 || y >= 8) return;
#if ROTATE_180
  x = PANEL_W - 1 - x;
  y = 7 - y;
#endif
#if FLIP_X
  mx->setPoint(y, PANEL_W - 1 - x, on);
#else
  mx->setPoint(y, x, on);
#endif
}

static inline bool getPx(int x, int y) {
#if ROTATE_180
  x = PANEL_W - 1 - x;
  y = 7 - y;
#endif
#if FLIP_X
  return mx->getPoint(y, PANEL_W - 1 - x);
#else
  return mx->getPoint(y, x);
#endif
}

void blit(const uint8_t *sprite /*PROGMEM*/, int x0, int y0 = 0) {
  for (int r = 0; r < 8; r++) {
    uint8_t bits = pgm_read_byte(sprite + r);
    for (int c = 0; c < 8; c++)
      if (bits & (1 << (7 - c))) px(x0 + c, y0 + r);
  }
}

void frameBegin() { mx->clear(); }
void frameEnd()   { mx->update(); }

// ------------------------------------------------------------- weather ----
const char *wmoText(int code) {
  switch (code) {
    case 0: case 1: return "Clear";
    case 2: return "Partly Cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Icy Drizzle";
    case 61: case 63: return "Rain";
    case 65: return "Heavy Rain";
    case 66: case 67: return "Freezing Rain";
    case 71: case 73: case 77: return "Snow";
    case 75: return "Heavy Snow";
    case 80: case 81: return "Showers";
    case 82: return "Heavy Showers";
    case 85: case 86: return "Snow Showers";
    case 95: case 96: case 99: return "Thunderstorm";
    default: return "";
  }
}

bool fetchWeather(char *out, size_t outlen) {
  if (WiFi.status() != WL_CONNECTED) return false;
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  client->setBufferSizes(1024, 512);
  HTTPClient https;
  String url = F("https://api.open-meteo.com/v1/forecast?latitude=" WEATHER_LAT
                 "&longitude=" WEATHER_LON
                 "&current=temperature_2m,relative_humidity_2m,weather_code&timezone=auto");
  if (!https.begin(*client, url)) return false;
  int rc = https.GET();
  if (rc != HTTP_CODE_OK) { https.end(); return false; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, https.getString());
  https.end();
  if (err) return false;
  float t = doc["current"]["temperature_2m"] | NAN;
  int   h = doc["current"]["relative_humidity_2m"] | -1;
  int   c = doc["current"]["weather_code"] | -1;
  if (isnan(t)) return false;
  const char *desc = wmoText(c);
  if (h >= 0 && desc[0])
    snprintf(out, outlen, "%s  %.0fC  %s  Hum %d%%", WEATHER_CITY, t, desc, h);
  else
    snprintf(out, outlen, "%s  %.0fC", WEATHER_CITY, t);
  return true;
}

bool readDht(char *out, size_t outlen) {
  TempAndHumidity th = dht.getTempAndHumidity();
  if (dht.getStatus() != DHTesp::ERROR_NONE || isnan(th.temperature)) return false;
  snprintf(out, outlen, "Room Temp %.0fC  Hum %.0f%%", th.temperature, th.humidity);
  return true;
}

// -------------------------------------------------------- mode control ----
void startScroll(const char *msg) {
  parola.displayClear();
#if ROTATE_180
  // Panel is upside down: logical right-scroll reads as left-scroll.
  parola.displayScroll(msg, PA_RIGHT, PA_SCROLL_RIGHT, 50);
#else
  parola.displayScroll(msg, PA_LEFT, PA_SCROLL_LEFT, 50);
#endif
}

void switchMode(Mode m) {
  mode = m;
  frameNo = 0;
  lastFrame = 0;
  parola.setIntensity(DEFAULT_INTENSITY);
  parola.displayClear();
  switch (m) {
    case M_CLOCK:
      timeOverride[0] = 0;   // leaving/re-entering clock clears any test pattern
      break;
    case M_TEXT:
    case M_IP:
      startScroll(textMsg);
      break;
    case M_WEATHER:
      if (!fetchWeather(textMsg, sizeof(textMsg)))
        strlcpy(textMsg, "WEATHER N/A", sizeof(textMsg));
      startScroll(textMsg);
      break;
    case M_DHT:
      if (!readDht(textMsg, sizeof(textMsg)))
        strlcpy(textMsg, "DHT11 no data", sizeof(textMsg));
      startScroll(textMsg);
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------ M_CLOCK -----
// Render a string into font-column bytes (1 blank col between chars).
// Returns the number of columns used.
int buildCols(const char *s, uint8_t *cols, int maxCols) {
  int n = 0;
  uint8_t cbuf[8];
  for (const char *p = s; *p && n < maxCols - 7; p++) {
    uint8_t w = mx->getChar((uint8_t)*p, sizeof(cbuf), cbuf);
    for (uint8_t i = 0; i < w; i++) cols[n++] = cbuf[i];
    cols[n++] = 0;
  }
  return n;
}

static inline void putCol(int x, uint8_t bits) {
  for (int r = 0; r < 8; r++)
    if (bits & (1 << r)) px(x, r);
}

// Like buildCols, but double-strikes each glyph (1px smear) for a bold face.
int buildColsBold(const char *s, uint8_t *cols, int maxCols) {
  int n = 0;
  uint8_t cbuf[8];
  for (const char *p = s; *p && n < maxCols - 9; p++) {
    if (*p == ':') { cols[n++] = 0x24; cols[n++] = 0; continue; }  // small colon dots
    if (*p == ' ') { cols[n++] = 0x00; cols[n++] = 0; continue; }  // blink-off, same width
    uint8_t w = mx->getChar((uint8_t)*p, sizeof(cbuf), cbuf);
    uint8_t prev = 0;
    for (uint8_t i = 0; i < w; i++) { cols[n++] = cbuf[i] | prev; prev = cbuf[i]; }
    cols[n++] = prev;   // finish the trailing smear column
    cols[n++] = 0;      // spacing
  }
  return n;
}

uint8_t dateCols[200];
int dateNCols = 0, dateScroll = 0, lastDay = -1;
#define DATE_GAP 10   // blank cols between scroll repeats
#define TIME_REGION 24   // modules 1-3 (brighter vendor): time; 4-7: date scroller

// LCD_FONT digits from the original Pi build (luma.core legacy LCD_FONT,
// proportional-trimmed; bit0 = top row). Layout mirrors the Pi's clock face:
// hours at x=0, TINY_FONT colon at x=11, minutes at x=13 -> 24px total.
const uint8_t LCD_DIGITS[11][5] PROGMEM = {
  {0x3E, 0x51, 0x49, 0x45, 0x3E},  // 0
  {0x42, 0x7F, 0x40, 0x00, 0x00},  // 1 (3 cols wide)
  {0x42, 0x61, 0x51, 0x49, 0x46},  // 2
  {0x21, 0x41, 0x45, 0x4B, 0x31},  // 3
  {0x18, 0x14, 0x12, 0x7F, 0x10},  // 4
  {0x27, 0x45, 0x45, 0x45, 0x39},  // 5
  {0x3C, 0x4A, 0x49, 0x49, 0x30},  // 6
  {0x01, 0x71, 0x09, 0x05, 0x03},  // 7
  {0x36, 0x49, 0x49, 0x49, 0x36},  // 8
  {0x06, 0x49, 0x49, 0x29, 0x1E},  // 9
  {0x08, 0x08, 0x08, 0x08, 0x08},  // -
};
const uint8_t LCD_W[11] = {5, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static int glyphIdx(char c) { return (c == '-') ? 10 : c - '0'; }

int drawTimeGlyph(int x, char c) {
  int idx = glyphIdx(c);
  for (int i = 0; i < LCD_W[idx]; i++)
    putCol(x + i, pgm_read_byte(&LCD_DIGITS[idx][i]));
  return LCD_W[idx];
}

// s is "HH:MM" (':' swapped for ' ' during blink-off)
void drawTimeLCD(const char *s) {
  int x = 0;
  x += drawTimeGlyph(x, s[0]) + 1;
  drawTimeGlyph(x, s[1]);
  if (s[2] == ':') putCol(11, 0x14);   // TINY_FONT colon, same as the Pi
  x = 13;
  x += drawTimeGlyph(x, s[3]) + 1;
  drawTimeGlyph(x, s[4]);
}

void tickClock() {
  if (millis() - lastFrame < 50) return;   // 50ms: smooth date scroll
  lastFrame = millis();
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  char tbuf[8];
  bool synced = t.tm_year >= 100;
  if (synced) {
    bool colon = (millis() / 500) & 1;
    snprintf(tbuf, sizeof(tbuf), "%02d%c%02d", t.tm_hour, colon ? ':' : ' ', t.tm_min);
    parola.setIntensity((t.tm_hour < 6 || t.tm_hour >= 21) ? 0 : DEFAULT_INTENSITY);
    if (t.tm_mday != lastDay) {            // rebuild "Thursday July 23" on day change
      lastDay = t.tm_mday;
      char dbuf[40];
      strftime(dbuf, sizeof(dbuf), "%A %B %d", &t);
      dateNCols = buildCols(dbuf, dateCols, sizeof(dateCols));
      dateScroll = 0;
    }
  } else {
    strlcpy(tbuf, "--:--", sizeof(tbuf));
  }

  if (timeOverride[0]) strlcpy(tbuf, timeOverride, sizeof(tbuf));   // test mode

  frameBegin();
  drawTimeLCD(tbuf);                     // original Pi clock face, modules 1-3
  if (synced && dateNCols > 0) {
    int x0 = TIME_REGION;                                     // modules 4-7
    int cycle = dateNCols + DATE_GAP;
    for (int i = 0; x0 + i < PANEL_W; i++) {
      int ci = (dateScroll + i) % cycle;
      putCol(x0 + i, ci < dateNCols ? dateCols[ci] : 0);
    }
    if (frameNo & 1) dateScroll = (dateScroll + 1) % cycle;   // 100ms/col scroll
  }
  frameEnd();
  frameNo++;
}

// ---------------------------------------------------------- M_CHARSET -----
void tickCharset() {
  if (millis() - lastFrame < 100) return;
  lastFrame = millis();
  frameNo++;
  if (frameNo > 255) { switchMode(M_CLOCK); return; }
  char buf[MAX_DEVICES + 1];
  memset(buf, (char)frameNo, MAX_DEVICES);
  buf[MAX_DEVICES] = 0;
  parola.displayText(buf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  parola.displayAnimate();
}

// ----------------------------------------------------------- M_PACMAN -----
int pmDir, pmX;
void tickPacman() {
  if (millis() - lastFrame < 120) return;
  lastFrame = millis();
  if (frameNo == 0) { pmDir = random(2) ? 1 : -1; pmX = (pmDir < 0) ? PANEL_W : -8; }
  const int gap = 11, gap2 = 22;
  int pacX = pmX - pmDir * gap;
  int ghostX = pmX - pmDir * gap2;
  int front = (pmDir < 0) ? pacX : pacX + 7;
  frameBegin();
  for (int p = 2; p < PANEL_W; p += 5) {
    bool ahead = (pmDir < 0) ? (p < front) : (p > front);
    if (ahead) px(p, 3);
  }
  blit(GHOST_FRAMES[frameNo % 2], ghostX);
  blit((pmDir < 0 ? PACMAN_LEFT : PACMAN_RIGHT)[frameNo % 2], pacX);
  blit(RUNNER_FRAMES[frameNo % 4], pmX);
  frameEnd();
  pmX += pmDir * 2;
  if ((pmDir < 0 && pmX < -8 - gap2) || (pmDir > 0 && pmX > PANEL_W + gap2)) {
    pmDir = random(2) ? 1 : -1;
    pmX = (pmDir < 0) ? PANEL_W : -8;
  }
  frameNo++;
}

// ----------------------------------------------------------- M_DANCER -----
void tickDancer() {
  if (millis() - lastFrame < 180) return;
  lastFrame = millis();
  const int posn[3] = {8, 24, 40};
  frameBegin();
  for (int i = 0; i < 3; i++) {
    int bob = ((frameNo + i) % 2 == 0) ? -1 : 0;
    blit(DANCER_FRAMES[(frameNo + i) % 4], posn[i], bob);
  }
  frameEnd();
  frameNo++;
}

// ------------------------------------------------------------- M_KITT -----
int kittPos = 0, kittStep = 1;
void tickKitt() {
  if (millis() - lastFrame < 30) return;
  lastFrame = millis();
  const uint8_t tail[4] = {8, 6, 4, 2};
  frameBegin();
  for (int i = 0; i < 4; i++) {
    int bx = kittPos - kittStep * i;
    if (bx < 0 || bx >= PANEL_W) continue;
    int y0 = (8 - tail[i]) / 2;
    for (int y = y0; y < y0 + tail[i]; y++) px(bx, y);
  }
  frameEnd();
  kittPos += kittStep;
  if (kittPos >= PANEL_W - 1) kittStep = -1;
  else if (kittPos <= 0) kittStep = 1;
}

// --------------------------------------------------------------- M_EQ -----
#define EQ_BARS 14
float eqPhase[EQ_BARS], eqSpeed[EQ_BARS];
void tickEq() {
  if (millis() - lastFrame < 50) return;
  lastFrame = millis();
  if (frameNo == 0)
    for (int i = 0; i < EQ_BARS; i++) {
      eqPhase[i] = random(628) / 100.0f;
      eqSpeed[i] = 0.15f + random(30) / 100.0f;
    }
  frameBegin();
  for (int i = 0; i < EQ_BARS; i++) {
    float level = sinf(eqPhase[i] + frameNo * eqSpeed[i]) * 0.5f + 0.5f;
    int h = max(1, (int)(level * 8));
    int x0 = i * 4;
    for (int y = 8 - h; y < 8; y++)
      for (int x = x0; x < x0 + 3 && x < PANEL_W; x++) px(x, y);
  }
  frameEnd();
  frameNo++;
}

// ------------------------------------------------------------- M_RAIN -----
int8_t rainHead[PANEL_W];
uint8_t rainSpeed[PANEL_W];
void tickRain() {
  if (millis() - lastFrame < 70) return;
  lastFrame = millis();
  if (frameNo == 0)
    for (int c = 0; c < PANEL_W; c++) {
      rainHead[c] = (random(100) < 35) ? 0 : -1;
      rainSpeed[c] = random(3) ? 1 : 2;
    }
  frameBegin();
  for (int c = 0; c < PANEL_W; c++) {
    if (rainHead[c] < 0) continue;
    for (int seg = 0; seg < 3; seg++) {
      int y = rainHead[c] - seg;
      if (y >= 0 && y < 8) px(c, y);
    }
  }
  frameEnd();
  for (int c = 0; c < PANEL_W; c++) {
    if (rainHead[c] < 0) {
      if (random(100) < 5) { rainHead[c] = 0; rainSpeed[c] = random(3) ? 1 : 2; }
    } else {
      rainHead[c] += rainSpeed[c];
      if (rainHead[c] - 3 >= 8) rainHead[c] = -1;
    }
  }
  frameNo++;
}

// -------------------------------------------------------- M_FIREWORKS -----
// phase 0 = shell rising, phase 1 = ring expanding & fading
int fwPhase, fwBx, fwPeak, fwY, fwRing;
void tickFireworks() {
  uint32_t interval = (fwPhase == 0) ? 40 : 90;
  if (millis() - lastFrame < interval) return;
  lastFrame = millis();
  if (frameNo == 0) { fwPhase = 0; fwBx = random(7, PANEL_W - 7); fwPeak = random(0, 3); fwY = 7; }
  if (fwPhase == 0) {
    parola.setIntensity(0);                     // faint rising trail
    frameBegin(); px(fwBx, fwY); frameEnd();
    if (--fwY <= fwPeak) { fwPhase = 1; fwRing = 0; }
  } else {
    const int RINGS = 12;                       // expands until the next blast
    int r = fwRing + 1;
    int inten = (int)(15.0f * (1.0f - (float)fwRing / (RINGS - 1)));
    parola.setIntensity(max(0, inten));
    frameBegin();
    for (int ang = 0; ang < 360; ang += 45) {
      float rad = ang * 3.14159f / 180.0f;
      px((int)roundf(fwBx + r * cosf(rad)), (int)roundf(fwPeak + r * sinf(rad)));
    }
    frameEnd();
    if (++fwRing >= RINGS) {                    // next blast
      fwPhase = 0; fwBx = random(7, PANEL_W - 7); fwPeak = random(0, 3); fwY = 7;
    }
  }
  frameNo++;
}

// -------------------------------------------------------- M_STARFIELD -----
#define N_STARS 18
float starX[N_STARS], starSp[N_STARS];
uint8_t starY[N_STARS];
float shipX;
void tickStarfield() {
  if (millis() - lastFrame < 50) return;
  lastFrame = millis();
  if (frameNo == 0) {
    for (int i = 0; i < N_STARS; i++) {
      starX[i] = random(PANEL_W);
      starY[i] = random(8);
      starSp[i] = 0.3f + random(130) / 100.0f;
    }
    shipX = PANEL_W;
  }
  frameBegin();
  for (int i = 0; i < N_STARS; i++) px((int)starX[i], starY[i]);
  blit(SHIP, (int)shipX);
  frameEnd();
  for (int i = 0; i < N_STARS; i++) {
    starX[i] -= starSp[i];
    if (starX[i] < 0) { starX[i] = PANEL_W - 1; starY[i] = random(8); starSp[i] = 0.3f + random(130) / 100.0f; }
  }
  shipX -= 0.8f;
  if (shipX < -8) shipX = PANEL_W;
  frameNo++;
}

// -------------------------------------------------------------- M_ECG -----
void tickEcg() {
  if (millis() - lastFrame < 40) return;
  lastFrame = millis();
  frameBegin();
  int prev = pgm_read_byte(ECG_BEAT + (frameNo % 26));
  for (int x = 0; x < PANEL_W; x++) {
    int y = pgm_read_byte(ECG_BEAT + ((frameNo + x) % 26));
    int lo = min(prev, y), hi = max(prev, y);
    for (int yy = lo; yy <= hi; yy++) px(x, yy);
    prev = y;
  }
  frameEnd();
  frameNo++;
}

// -------------------------------------------------------- scroll modes ----
void tickScroll() {
  if (parola.displayAnimate()) {
    if (mode == M_IP) { switchMode(M_CLOCK); return; }
    if (mode == M_DHT && readDht(textMsg, sizeof(textMsg))) startScroll(textMsg);
    else if (mode == M_WEATHER && (frameNo % 5 == 4) && fetchWeather(textMsg, sizeof(textMsg))) startScroll(textMsg);
    else parola.displayReset();
    frameNo++;
  }
}

// ------------------------------------------------------------ web page ----
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en" data-bs-theme="dark">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>MATRIX DISPLAY</title>
<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css" rel="stylesheet">
</head>
<body>
<div class="container text-center" style="max-width:900px">
  <div class="row my-4"><h1>MATRIX DISPLAY</h1></div>
  <canvas id="led" width="560" height="80" style="max-width:100%;background:#111;border-radius:8px"></canvas>
  <div class="row g-2 my-3"><div class="col-12">
    <div class="d-flex gap-2 flex-wrap flex-sm-nowrap">
      <form action="/ledtext" method="post" class="d-flex gap-2 flex-grow-1 m-0">
        <input type="text" class="form-control flex-grow-1" name="ledtext" placeholder="Enter Text">
        <input type="submit" value="Send" class="btn btn-primary flex-shrink-0">
      </form>
      <form action="/stopledtext" method="post" class="m-0 flex-shrink-0">
        <input type="submit" value="Stop" class="btn btn-danger">
      </form>
    </div>
  </div></div>
  <div class="row g-3 my-2 justify-content-center">
    <div class="col-6 col-md-4 col-lg"><form action="/startclock" method="post"><input type="submit" value="Clock" class="btn btn-success w-100"></form></div>
    <div class="col-6 col-md-4 col-lg"><form action="/weather" method="post"><input type="submit" value="Weather" class="btn btn-info w-100"></form></div>
    <div class="col-6 col-md-4 col-lg"><form action="/dht" method="post"><input type="submit" value="Room Temp" class="btn btn-secondary w-100"></form></div>
    <div class="col-6 col-md-4 col-lg"><form action="/charset" method="post"><input type="submit" value="Charset" class="btn btn-warning w-100"></form></div>
    <div class="col-6 col-md-4 col-lg"><form action="/pacman" method="post"><input type="submit" value="Pacman" class="btn btn-primary w-100"></form></div>
    <div class="col-6 col-md-4 col-lg"><form action="/dancer" method="post"><input type="submit" value="The Dancer" class="btn btn-primary w-100"></form></div>
  </div>
  <div class="row g-3 my-2 justify-content-center">
    <div class="col-6 col-md-4 col-lg"><form action="/kitt" method="post"><input type="submit" value="KITT" class="btn btn-danger w-100"></form></div>
    <div class="col-6 col-md-4 col-lg"><form action="/equalizer" method="post"><input type="submit" value="Equalizer" class="btn btn-success w-100"></form></div>
    <div class="col-6 col-md-4 col-lg"><form action="/rain" method="post"><input type="submit" value="Rain" class="btn btn-info w-100"></form></div>
    <div class="col-6 col-md-4 col-lg"><form action="/fireworks" method="post"><input type="submit" value="Fireworks" class="btn btn-warning w-100"></form></div>
    <div class="col-6 col-md-4 col-lg"><form action="/starfield" method="post"><input type="submit" value="Starfield" class="btn btn-secondary w-100"></form></div>
    <div class="col-6 col-md-4 col-lg"><form action="/ecg" method="post"><input type="submit" value="ECG &#10084;" class="btn btn-danger w-100"></form></div>
  </div>
</div>
<script>
const cv=document.getElementById('led'),cx=cv.getContext('2d');
async function poll(){
  try{
    const t=await (await fetch('/frame')).text();
    const p=t.split(' '),w=+p[0],h=+p[1],bits=p[2];
    cx.fillStyle='#111';cx.fillRect(0,0,cv.width,cv.height);
    const cw=cv.width/w,ch=cv.height/h;
    cx.fillStyle='#f33';
    for(let y=0;y<h;y++)for(let x=0;x<w;x++)
      if(bits[y*w+x]==='1'){cx.beginPath();cx.arc(x*cw+cw/2,y*ch+ch/2,cw*0.38,0,7);cx.fill();}
  }catch(e){}
  setTimeout(poll,300);
}
poll();
</script>
</body>
</html>)HTML";

// --------------------------------------------------------------- routes ----
void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void handleFrame() {
  char buf[PANEL_W * 8 + 16];
  int n = snprintf(buf, sizeof(buf), "%d 8 ", PANEL_W);
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < PANEL_W; x++)
      buf[n++] = getPx(x, y) ? '1' : '0';
  buf[n] = 0;
  server.send(200, "text/plain", buf);
}

void setupRoutes() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/frame", HTTP_GET, handleFrame);
  server.on("/health", HTTP_GET, []() { server.send(200, "application/json", "{\"status\":\"ok\"}"); });
  server.on("/testtime", HTTP_POST, []() {
    String v = server.arg("t");
    if (!v.length()) v = "88:88";
    switchMode(M_CLOCK);
    strlcpy(timeOverride, v.c_str(), sizeof(timeOverride));
    server.send(200, "text/plain", timeOverride);
  });
  server.on("/dhtpin", HTTP_GET, []() {
    pinMode(DHT_PIN, INPUT);
    delay(5);
    int floatLevel = digitalRead(DHT_PIN);
    pinMode(DHT_PIN, INPUT_PULLUP);
    delay(5);
    int pullupLevel = digitalRead(DHT_PIN);
    dht.setup(DHT_PIN, DHTesp::DHT11);   // restore driver state
    char b[80];
    snprintf(b, sizeof(b), "{\"floating\":%d,\"with_pullup\":%d}", floatLevel, pullupLevel);
    server.send(200, "application/json", b);
  });
  server.on("/dhtraw", HTTP_GET, []() {
    TempAndHumidity th = dht.getTempAndHumidity();
    char b[96];
    snprintf(b, sizeof(b), "{\"status\":\"%s\",\"temp\":%.1f,\"hum\":%.1f}",
             dht.getStatusString(), th.temperature, th.humidity);
    server.send(200, "application/json", b);
  });
  server.on("/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "rebooting");
    delay(200);
    ESP.restart();
  });
  server.on("/startclock", HTTP_POST, []() { switchMode(M_CLOCK); redirectHome(); });
  server.on("/stopledtext", HTTP_POST, []() { switchMode(M_CLOCK); redirectHome(); });
  server.on("/ledtext", HTTP_POST, []() {
    String m = server.arg("ledtext");
    m.trim();
    if (m.length()) { strlcpy(textMsg, m.c_str(), sizeof(textMsg)); switchMode(M_TEXT); }
    redirectHome();
  });
  server.on("/weather", HTTP_POST, []() { switchMode(M_WEATHER); redirectHome(); });
  server.on("/dht", HTTP_POST, []() { switchMode(M_DHT); redirectHome(); });
  server.on("/charset", HTTP_POST, []() { switchMode(M_CHARSET); redirectHome(); });
  server.on("/pacman", HTTP_POST, []() { switchMode(M_PACMAN); redirectHome(); });
  server.on("/dancer", HTTP_POST, []() { switchMode(M_DANCER); redirectHome(); });
  server.on("/kitt", HTTP_POST, []() { switchMode(M_KITT); redirectHome(); });
  server.on("/equalizer", HTTP_POST, []() { switchMode(M_EQ); redirectHome(); });
  server.on("/rain", HTTP_POST, []() { switchMode(M_RAIN); redirectHome(); });
  server.on("/fireworks", HTTP_POST, []() { switchMode(M_FIREWORKS); redirectHome(); });
  server.on("/starfield", HTTP_POST, []() { switchMode(M_STARFIELD); redirectHome(); });
  server.on("/ecg", HTTP_POST, []() { switchMode(M_ECG); redirectHome(); });
  server.onNotFound([]() { server.send(404, "text/plain", "Not Found"); });
}

// ---------------------------------------------------------------- setup ----
void setup() {
  Serial.begin(115200);
  randomSeed(ESP.getCycleCount());

  parola.begin();
#if ROTATE_180
  parola.setZoneEffect(0, true, PA_FLIP_UD);
  parola.setZoneEffect(0, true, PA_FLIP_LR);
#endif
  parola.setIntensity(DEFAULT_INTENSITY);
  parola.displayClear();
  mx = parola.getGraphicObject();

  dht.setup(DHT_PIN, DHTesp::DHT11);

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
#ifdef STATIC_IP
  {
    IPAddress ip(STATIC_IP), gw(STATIC_GW), mask(STATIC_MASK), dns1(STATIC_DNS), dns2(8, 8, 8, 8);
    WiFi.config(ip, gw, mask, dns1, dns2);
  }
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);

  configTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.begin();

  setupRoutes();
  server.begin();

  switchMode(M_CLOCK);   // straight to the clock, no boot messages
}

// ----------------------------------------------------------------- loop ----
void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  switch (mode) {
    case M_CLOCK:     tickClock();     break;
    case M_TEXT:
    case M_WEATHER:
    case M_DHT:
    case M_IP:        tickScroll();    break;
    case M_CHARSET:   tickCharset();   break;
    case M_PACMAN:    tickPacman();    break;
    case M_DANCER:    tickDancer();    break;
    case M_KITT:      tickKitt();      break;
    case M_EQ:        tickEq();        break;
    case M_RAIN:      tickRain();      break;
    case M_FIREWORKS: tickFireworks(); break;
    case M_STARFIELD: tickStarfield(); break;
    case M_ECG:       tickEcg();       break;
  }
}
