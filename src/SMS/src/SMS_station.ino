// ============================================================
//  SMS – Station Management System  v4  [SIMULATION BUILD]
//  Hardware : Arduino Uno R3 + WS2812B (75 LEDs) + 4x buttons
//  Protocol : JSON over Serial (9600 baud, \n terminated)
//
//  LED layout (75 total):
//    Zone 1 : 0  – 24  (25 LEDs)
//    Zone 2 : 25 – 49  (25 LEDs)
//    Zone 3 : 50 – 74  (25 LEDs)
// ============================================================

#include <FastLED.h>
#include <EEPROM.h>

// ── FastLED ──────────────────────────────────────────────────
#define LED_PIN     2
#define NUM_LEDS    75
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

CRGB leds[NUM_LEDS];

const uint8_t ZONE_START[3] = { 1, 26, 51 };
const uint8_t ZONE_END[3]   = { 23, 48, 74 };

// ── Button simulation ─────────────────────────────────────────
const uint8_t BTN_ZONE[3] = { 9, 10, 11 };
const uint8_t BTN_MODE    = 12;
#define PERSON_WEIGHT 70.0f

float simWeight[3] = { 0, 0, 0 };
bool  lastBtn[3]   = { HIGH, HIGH, HIGH };
bool  lastMode     = HIGH;
bool  subtractMode = false;

// ── EEPROM ───────────────────────────────────────────────────
#define EEPROM_MAGIC 0xAC
#define EEPROM_ADDR  0

struct Config {
  uint8_t magic;
  char    carId[6];
  float   maxKg[3];
  uint8_t brightness;
  uint8_t pctMedium;
  uint8_t pctFull;
};
Config cfg;

void configDefaults() {
  cfg.magic     = EEPROM_MAGIC;
  strncpy(cfg.carId, "CAR1", sizeof(cfg.carId));
  for (uint8_t i = 0; i < 3; i++) cfg.maxKg[i] = 1050.0f;
  cfg.brightness = 128;
  cfg.pctMedium  = 40;
  cfg.pctFull    = 80;
}
void saveConfig() { EEPROM.put(EEPROM_ADDR, cfg); }
void loadConfig() {
  EEPROM.get(EEPROM_ADDR, cfg);
  if (cfg.magic != EEPROM_MAGIC) { configDefaults(); saveConfig(); }
}

// ── Zone state ───────────────────────────────────────────────
struct Zone { uint8_t r, g, b; char label[8]; };
Zone zones[3] = {
  {0, 120, 0, "free"},
  {0, 120, 0, "free"},
  {0, 120, 0, "free"},
};

// ── Train state ──────────────────────────────────────────────
enum TrainState { NO_TRAIN, EXPECTED, ARRIVING, IN_SERVICE, LEAVING };
TrainState trainState = NO_TRAIN;

const char* trainStateStr() {
  switch (trainState) {
    case NO_TRAIN:   return "no_train";
    case EXPECTED:   return "expected";
    case ARRIVING:   return "arriving";
    case IN_SERVICE: return "in_service";
    case LEAVING:    return "leaving";
    default:         return "unknown";
  }
}

// ── Serial buffer ────────────────────────────────────────────
#define BUF_SIZE 192
char    rxBuf[BUF_SIZE];
uint8_t rxPos = 0;

// ── Float scratch ────────────────────────────────────────────
char fbuf[10];
#define FMT(f) dtostrf((f), 1, 2, fbuf)

// ============================================================
// JSON parsers
// ============================================================

static const char* jFind(const char* buf, const char* key) {
  const char* p = buf;
  uint8_t klen  = strlen(key);
  while (*p) {
    if (*p == '"') {
      if (strncmp(p + 1, key, klen) == 0 && *(p + 1 + klen) == '"') {
        p = p + 1 + klen + 1;
        while (*p == ':' || *p == ' ') p++;
        return p;
      }
    }
    p++;
  }
  return nullptr;
}

bool jStr(const char* buf, const char* key, char* out, uint8_t outLen) {
  const char* p = jFind(buf, key);
  if (!p || *p != '"') return false;
  p++;
  uint8_t i = 0;
  while (*p && *p != '"' && i < outLen - 1) out[i++] = *p++;
  out[i] = '\0';
  return true;
}

bool jInt(const char* buf, const char* key, int* out) {
  const char* p = jFind(buf, key);
  if (!p) return false;
  *out = atoi(p);
  return true;
}

void parseAndApplyZones(const char* buf) {
  const char* p = buf;
  while ((p = strstr(p, "{\"zone\":")) != nullptr) {
    int zi = 0;
    if (jInt(p, "zone", &zi) && zi >= 1 && zi <= 3) {
      uint8_t z = zi - 1;
      int r = 0, g = 0, b = 0;
      jInt(p, "r", &r);
      jInt(p, "g", &g);
      jInt(p, "b", &b);
      zones[z].r = constrain(r, 0, 255);
      zones[z].g = constrain(g, 0, 255);
      zones[z].b = constrain(b, 0, 255);
      char lbl[8] = "unknown";
      jStr(p, "label", lbl, sizeof(lbl));
      strncpy(zones[z].label, lbl, sizeof(zones[z].label) - 1);
    }
    p++;
  }
}

// ============================================================
// Sensor helpers
// ============================================================

uint8_t kgToPct(uint8_t z, float kg) {
  if (kg <= 0) return 0;
  return (uint8_t)constrain((int)((kg / cfg.maxKg[z]) * 100.0f), 0, 100);
}

bool applyOccupancyBand(uint8_t z, uint8_t pct) {
  uint8_t r, g, b;
  const char* lbl;
  if (pct >= cfg.pctFull) {
    r=120; g=0;   b=0;  lbl="full";
  } else if (pct >= cfg.pctMedium) {
    r=120; g=60;  b=0;  lbl="medium";
  } else {
    r=0;   g=120; b=0;  lbl="free";
  }
  if (zones[z].r==r && zones[z].g==g && zones[z].b==b) return false;
  zones[z].r=r; zones[z].g=g; zones[z].b=b;
  strncpy(zones[z].label, lbl, sizeof(zones[z].label)-1);
  return true;
}

// ============================================================
// LED helpers
// ============================================================

void setZoneLeds(uint8_t z) {
  CRGB color = CRGB(zones[z].r, zones[z].g, zones[z].b);
  for (uint8_t p = ZONE_START[z]; p <= ZONE_END[z]; p++)
    leds[p] = color;
}

void refreshStrip() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  for (uint8_t z = 0; z < 3; z++) setZoneLeds(z);
  FastLED.show();
}

// ============================================================
// Animations (fixed reverse loop bug)
// ============================================================

void animWalk(CRGB color, int8_t dir, bool clearFirst) {
  if (clearFirst) { fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show(); }
  else refreshStrip();

  if (dir >= 0) {
    for (uint8_t z = 0; z < 3; z++)
      for (uint8_t p = ZONE_START[z]; p <= ZONE_END[z]; p++) {
        leds[p] = color; FastLED.show(); delay(10);
      }
  } else {
    for (int8_t z = 2; z >= 0; z--)
      for (int p = ZONE_END[z]; p >= (int)ZONE_START[z]; p--) {
        leds[p] = color; FastLED.show(); delay(10);
      }
  }
}

// ============================================================
//  Serial output helpers
// ============================================================

void sendAck(const char* cmd, bool ok, const char* msg = nullptr) {
  Serial.print(F("{\"type\":\"ACK\",\"cmd\":\""));
  Serial.print(cmd);
  Serial.print(F("\",\"status\":\""));
  Serial.print(ok ? F("ok") : F("error"));
  Serial.print('"');
  if (msg) {
    Serial.print(F(",\"msg\":\""));
    Serial.print(msg);
    Serial.print('"');
  }
  Serial.println('}');
}

void sendEvent(const char* event) {
  Serial.print(F("{\"type\":\"EVENT\",\"event\":\""));
  Serial.print(event);
  Serial.print(F("\",\"car\":\""));
  Serial.print(cfg.carId);
  Serial.println(F("\"}"));
}

void sendPong() {
  Serial.print(F("{\"type\":\"PONG\",\"car\":\""));
  Serial.print(cfg.carId);
  Serial.print(F("\",\"uptime_ms\":"));
  Serial.print(millis());
  Serial.println('}');
}

void sendStatus() {
  Serial.print(F("{\"type\":\"STATUS\",\"car\":\""));
  Serial.print(cfg.carId);
  Serial.print(F("\",\"train_state\":\""));
  Serial.print(trainStateStr());
  Serial.print(F("\",\"zones\":["));
  for (uint8_t z = 0; z < 3; z++) {
    float   kg  = simWeight[z];
    uint8_t pct = kgToPct(z, kg);
    if (z) Serial.print(',');
    Serial.print(F("{\"zone\":"));     Serial.print(z + 1);
    Serial.print(F(",\"r\":"));        Serial.print(zones[z].r);
    Serial.print(F(",\"g\":"));        Serial.print(zones[z].g);
    Serial.print(F(",\"b\":"));        Serial.print(zones[z].b);
    Serial.print(F(",\"label\":\""));  Serial.print(zones[z].label);
    Serial.print(F("\",\"kg\":"));     Serial.print(FMT(kg));
    Serial.print(F(",\"pct\":"));      Serial.print(pct);
    Serial.print('}');
  }
  Serial.println(F("]}"));
}

// ============================================================
//  Animations
// ============================================================

void animBoot() {
  for (uint8_t z = 0; z < 3; z++) {
    for (uint8_t p = ZONE_START[z]; p <= ZONE_END[z]; p++)
      leds[p] = CRGB(0, 0, 180);
    FastLED.show(); delay(250);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show(); delay(120);
  }
}

void animArriving(int8_t dir) {
  animWalk(CRGB::White, dir, true);
  delay(300);
  refreshStrip();
}

void animLeaving(int8_t dir) {
  animWalk(CRGB::Black, dir, false);
  delay(200);
}

void animDoorsClosing() {
  for (uint8_t i = 0; i < 5; i++) {
    for (uint8_t z = 0; z < 3; z++)
      for (uint8_t p = ZONE_START[z]; p <= ZONE_END[z]; p++)
        leds[p] = CRGB(200, 100, 0);
    FastLED.show(); delay(120);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show(); delay(80);
  }
  refreshStrip();
}

// ============================================================
//  Selftest
// ============================================================

void doSelftest() {
  Serial.print(F("{\"type\":\"SELFTEST\",\"car\":\""));
  Serial.print(cfg.carId);
  Serial.print(F("\",\"zones\":["));
  for (uint8_t z = 0; z < 3; z++) {
    for (uint8_t p = ZONE_START[z]; p <= ZONE_END[z]; p++)
      leds[p] = CRGB::White;
    FastLED.show(); delay(300);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show(); delay(100);
    if (z) Serial.print(',');
    Serial.print(F("{\"zone\":"));      Serial.print(z + 1);
    Serial.print(F(",\"sensor_ok\":true,\"kg\":"));
    Serial.print(FMT(simWeight[z]));
    Serial.print(F(",\"pct\":"));       Serial.print(kgToPct(z, simWeight[z]));
    Serial.print('}');
  }
  Serial.println(F("]}"));
  if (trainState == IN_SERVICE) refreshStrip();
}

// ============================================================
//  Command dispatcher
// ============================================================

void handleCommand(const char* buf) {
  char cmd[22] = "";  // longest cmd: TRAIN_DOORS_CLOSING = 19 chars
  if (!jStr(buf, "cmd", cmd, sizeof(cmd))) {
    Serial.println(F("{\"type\":\"ACK\",\"status\":\"error\",\"msg\":\"missing cmd\"}"));
    return;
  }

  // ── Diagnostics ───────────────────────────────────────
  if (strcmp_P(cmd, PSTR("PING"))       == 0) { sendPong();   return; }
  if (strcmp_P(cmd, PSTR("GET_STATUS")) == 0) { sendStatus(); return; }
  if (strcmp_P(cmd, PSTR("SELFTEST"))   == 0) { doSelftest(); return; }

  // ── LED control ───────────────────────────────────────
  if (strcmp_P(cmd, PSTR("SET_ZONE")) == 0) {
    int z=0, r=0, g=0, b=0; char lbl[8]="unknown";
    if (!jInt(buf,"zone",&z)||z<1||z>3)                          { sendAck(cmd,false,"invalid zone");  return; }
    if (!jInt(buf,"r",&r)||!jInt(buf,"g",&g)||!jInt(buf,"b",&b)) { sendAck(cmd,false,"missing r/g/b"); return; }
    z--;
    zones[z].r = constrain(r,0,255);
    zones[z].g = constrain(g,0,255);
    zones[z].b = constrain(b,0,255);
    jStr(buf,"label",lbl,sizeof(lbl));
    strncpy(zones[z].label, lbl, sizeof(zones[z].label)-1);
    if (trainState==IN_SERVICE||trainState==EXPECTED) { setZoneLeds(z); FastLED.show(); }
    sendAck(cmd, true);
    return;
  }

  if (strcmp_P(cmd, PSTR("SET_ALL")) == 0) {
    parseAndApplyZones(buf);
    if (trainState==IN_SERVICE||trainState==EXPECTED) refreshStrip();
    sendAck(cmd, true);
    return;
  }

  // ── Config ────────────────────────────────────────────
  if (strcmp_P(cmd, PSTR("SET_BANDS")) == 0) {
    int medium=0, full=0;
    if (!jInt(buf,"medium",&medium)) { sendAck(cmd,false,"missing medium"); return; }
    if (!jInt(buf,"full",&full))     { sendAck(cmd,false,"missing full");   return; }
    cfg.pctMedium = (uint8_t)constrain(medium, 1, 99);
    cfg.pctFull   = (uint8_t)constrain(full,   1, 99);
    sendAck(cmd, true); return;
  }

  if (strcmp_P(cmd, PSTR("SAVE_CONFIG")) == 0) { saveConfig(); sendAck(cmd,true); return; }

  if (strcmp_P(cmd, PSTR("LOAD_CONFIG")) == 0) {
    loadConfig();
    FastLED.setBrightness(cfg.brightness);
    sendAck(cmd,true); return;
  }

  if (strcmp_P(cmd, PSTR("RESET")) == 0) {
    sendAck(cmd,true); delay(100);
    asm volatile("jmp 0"); return;
  }

  // ── Train lifecycle ───────────────────────────────────
  if (strcmp_P(cmd, PSTR("TRAIN_EXPECTED")) == 0) {
    parseAndApplyZones(buf);
    trainState = EXPECTED;
    refreshStrip();
    sendAck(cmd, true); return;
  }

  if (strcmp_P(cmd, PSTR("TRAIN_ARRIVING")) == 0) {
    char dir[4]="ltr"; jStr(buf,"dir",dir,sizeof(dir));
    int8_t d = (strcmp(dir,"rtl")==0) ? -1 : 1;
    trainState = ARRIVING; sendAck(cmd, true);
    animArriving(d);
    trainState = IN_SERVICE; sendEvent("TRAIN_IN_SERVICE");
    return;
  }

  if (strcmp_P(cmd, PSTR("TRAIN_DOORS_CLOSING")) == 0) {
    animDoorsClosing(); sendAck(cmd,true); return;
  }

  if (strcmp_P(cmd, PSTR("TRAIN_LEAVING")) == 0) {
    char dir[4]="ltr"; jStr(buf,"dir",dir,sizeof(dir));
    int8_t d = (strcmp(dir,"rtl")==0) ? -1 : 1;
    trainState = LEAVING; sendAck(cmd, true);
    animLeaving(d);
    trainState = NO_TRAIN;
    sendEvent("TRAIN_GONE"); return;
  }

  sendAck(cmd, false, "unknown command");
}

// ============================================================
//  setup / loop
// ============================================================

void setup() {
  Serial.begin(9600);

  loadConfig();

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(cfg.brightness);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  for (uint8_t i = 0; i < 3; i++) pinMode(BTN_ZONE[i], INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);

  animBoot();
  trainState = NO_TRAIN;

  sendEvent("BOOT");
}

void loop() {
  // ── Serial receive ──────────────────────────────────
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxPos > 0) {
        rxBuf[rxPos] = '\0';
        handleCommand(rxBuf);
        memset(rxBuf, 0, rxPos);
        rxPos = 0;
      }
    } else if (rxPos < BUF_SIZE - 1) {
      rxBuf[rxPos++] = c;
    }
  }

  // ── Button simulation ────────────────────────────────
  bool modeNow = digitalRead(BTN_MODE);
  if (modeNow == LOW && lastMode == HIGH) subtractMode = !subtractMode;
  lastMode = modeNow;

  for (uint8_t i = 0; i < 3; i++) {
    bool btnNow = digitalRead(BTN_ZONE[i]);
    if (btnNow == LOW && lastBtn[i] == HIGH) {
      if (subtractMode)
        simWeight[i] = max(0.0f, simWeight[i] - PERSON_WEIGHT);
      else
        simWeight[i] += PERSON_WEIGHT;
      if (trainState == IN_SERVICE) {
        uint8_t pct = kgToPct(i, simWeight[i]);
        if (applyOccupancyBand(i, pct)) { setZoneLeds(i); FastLED.show(); }
      }
      Serial.print(F("{\"type\":\"EVENT\",\"event\":\"WEIGHT_CHANGE\",\"car\":\""));
      Serial.print(cfg.carId);
      Serial.print(F("\",\"zone\":"));  Serial.print(i + 1);
      Serial.print(F(",\"kg\":"));      Serial.print(FMT(simWeight[i]));
      Serial.print(F(",\"pct\":"));     Serial.print(kgToPct(i, simWeight[i]));
      Serial.println('}');
    }
    lastBtn[i] = btnNow;
  }

  // ── Weight-change detection ──────────────────────────
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck > 500 && trainState == IN_SERVICE) {
    lastCheck = now;
    for (uint8_t z = 0; z < 3; z++) {
      uint8_t pct = kgToPct(z, simWeight[z]);
      if (applyOccupancyBand(z, pct)) {
        setZoneLeds(z);
        FastLED.show();
      }
    }
  }
}
