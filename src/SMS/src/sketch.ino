// ============================================================
//  SMS – Station Management System
//  Hardware : Arduino + Adafruit NeoPixel + 3× HX711 load cells
//  Protocol : JSON over Serial (9600 baud)
// ============================================================

#include <Adafruit_NeoPixel.h>
#include <HX711.h>

// ── NeoPixel ────────────────────────────────────────────────
#define STRIP_PIN    8
#define NUM_PIXELS   19
Adafruit_NeoPixel strip(NUM_PIXELS, STRIP_PIN, NEO_GRB + NEO_KHZ800);

// Zone pixel ranges (inclusive). Index 0,6,12,18 are separators.
const uint8_t ZONE_START[3] = {1,  7,  13};
const uint8_t ZONE_END[3]   = {5, 11,  17};

// ── HX711 pin assignments ────────────────────────────────────
//  Zone 1 → HX711 #0,  Zone 2 → HX711 #1,  Zone 3 → HX711 #2
const uint8_t HX_DOUT[3] = {2, 4, 6};
const uint8_t HX_SCK[3]  = {3, 5, 7};
HX711 scale[3];

// ── Zone state ───────────────────────────────────────────────
struct Zone {
  uint8_t r, g, b;
  char    label[16];   // e.g. "free", "medium", "full"
  float   calibFactor; // grams per raw unit – tune per load cell
  float   maxKg;       // 100 % reference weight
};

Zone zones[3] = {
  {0, 120, 0, "free",   420.0f, 75.0f},
  {0, 120, 0, "free",   420.0f, 75.0f},
  {0, 120, 0, "free",   420.0f, 75.0f},
};

// ── Serial / JSON buffer ─────────────────────────────────────
#define BUF_SIZE 256
char    rxBuf[BUF_SIZE];
uint8_t rxPos = 0;

// ── Weight-change event thresholds ──────────────────────────
#define WEIGHT_CHANGE_KG 2.0f   // fire EVENT if weight changes by this much
float lastReportedKg[3] = {0, 0, 0};

// ── Timing ───────────────────────────────────────────────────
unsigned long startMs;

// ============================================================
//  Helpers
// ============================================================

void setZoneLeds(uint8_t z) {
  for (uint8_t p = ZONE_START[z]; p <= ZONE_END[z]; p++)
    strip.setPixelColor(p, strip.Color(zones[z].r, zones[z].g, zones[z].b));
}

void refreshStrip() {
  for (uint8_t z = 0; z < 3; z++) setZoneLeds(z);
  strip.show();
}

float readKg(uint8_t z) {
  if (!scale[z].is_ready()) return 0.0f;
  long raw = scale[z].get_value(3);          // average of 3 readings
  return (float)raw / zones[z].calibFactor;
}

uint8_t kgToPct(uint8_t z, float kg) {
  if (kg <= 0) return 0;
  int pct = (int)((kg / zones[z].maxKg) * 100.0f);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return (uint8_t)pct;
}

// ── Minimal JSON helpers (no library needed) ─────────────────

// Find numeric value after key in a flat JSON string
// Returns true and fills *out on success.
bool jsonGetInt(const char* json, const char* key, int* out) {
  const char* p = strstr(json, key);
  if (!p) return false;
  p += strlen(key);
  while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
  if (!*p) return false;
  *out = atoi(p);
  return true;
}

// Returns pointer into json right after "key":" (for string values)
const char* jsonGetStr(const char* json, const char* key, char* outBuf, uint8_t bufLen) {
  const char* p = strstr(json, key);
  if (!p) return nullptr;
  p += strlen(key);
  while (*p && *p != '"') p++;   // find opening quote of value
  if (*p != '"') return nullptr;
  p++;                           // skip opening quote
  uint8_t i = 0;
  while (*p && *p != '"' && i < bufLen - 1) outBuf[i++] = *p++;
  outBuf[i] = '\0';
  return outBuf;
}

// ── Serial output helpers ─────────────────────────────────────

void serialWeightAll() {
  Serial.print(F("{\"type\":\"WEIGHT\",\"zones\":["));
  for (uint8_t z = 0; z < 3; z++) {
    float kg  = readKg(z);
    long  raw = scale[z].is_ready() ? scale[z].get_value(1) : 0;
    uint8_t pct = kgToPct(z, kg);
    if (z) Serial.print(',');
    Serial.print(F("{\"zone\":"));  Serial.print(z + 1);
    Serial.print(F(",\"raw\":"));   Serial.print(raw);
    Serial.print(F(",\"kg\":"));    Serial.print(kg, 2);
    Serial.print(F(",\"pct\":"));   Serial.print(pct);
    Serial.print('}');
  }
  Serial.println(F("]}"));
}

void serialWeightOne(uint8_t z) {
  float   kg  = readKg(z);
  long    raw = scale[z].is_ready() ? scale[z].get_value(1) : 0;
  uint8_t pct = kgToPct(z, kg);
  Serial.print(F("{\"type\":\"WEIGHT\",\"zone\":")); Serial.print(z + 1);
  Serial.print(F(",\"raw\":"));  Serial.print(raw);
  Serial.print(F(",\"kg\":"));   Serial.print(kg, 2);
  Serial.print(F(",\"pct\":")); Serial.print(pct);
  Serial.println('}');
}

void serialStatus() {
  Serial.print(F("{\"type\":\"STATUS\",\"zones\":["));
  for (uint8_t z = 0; z < 3; z++) {
    float   kg  = readKg(z);
    uint8_t pct = kgToPct(z, kg);
    if (z) Serial.print(',');
    Serial.print(F("{\"zone\":")); Serial.print(z + 1);
    Serial.print(F(",\"r\":"));    Serial.print(zones[z].r);
    Serial.print(F(",\"g\":"));    Serial.print(zones[z].g);
    Serial.print(F(",\"b\":"));    Serial.print(zones[z].b);
    Serial.print(F(",\"label\":\"")); Serial.print(zones[z].label); Serial.print('"');
    Serial.print(F(",\"kg\":"));   Serial.print(kg, 2);
    Serial.print(F(",\"pct\":")); Serial.print(pct);
    Serial.print('}');
  }
  Serial.println(F("]}"));
}

void serialAck(const char* cmd, bool ok, const char* msg = nullptr) {
  Serial.print(F("{\"type\":\"ACK\",\"cmd\":\""));
  Serial.print(cmd);
  Serial.print(ok ? F("\",\"status\":\"ok\"") : F("\",\"status\":\"error\""));
  if (msg) { Serial.print(F(",\"msg\":\"")); Serial.print(msg); Serial.print('"'); }
  Serial.println('}');
}

// ── Animations ───────────────────────────────────────────────

void animateArriving() {
  // Blue sweep left→right, then restore
  for (uint8_t p = 0; p < NUM_PIXELS; p++) {
    strip.setPixelColor(p, strip.Color(0, 0, 150));
    strip.show();
    delay(40);
  }
  delay(400);
  refreshStrip();
}

void animateLeaving() {
  // Fade out all zones to black
  for (int b = 255; b >= 0; b -= 5) {
    strip.setBrightness(b);
    strip.show();
    delay(15);
  }
  strip.clear();
  strip.setBrightness(255);
  strip.show();
  // Restore (train gone → LEDs off, CMS will re-set after boarding)
}

// ============================================================
//  Command dispatcher
// ============================================================

void handleCommand(const char* json) {
  char cmd[32];
  if (!jsonGetStr(json, "\"cmd\"", cmd, sizeof(cmd))) {
    Serial.println(F("{\"type\":\"ACK\",\"status\":\"error\",\"msg\":\"missing cmd\"}"));
    return;
  }

  // ── PING ──────────────────────────────────────────────────
  if (strcmp(cmd, "PING") == 0) {
    Serial.print(F("{\"type\":\"PONG\",\"uptime_ms\":"));
    Serial.print(millis() - startMs);
    Serial.println('}');
    return;
  }

  // ── GET_WEIGHT ────────────────────────────────────────────
  if (strcmp(cmd, "GET_WEIGHT") == 0) {
    int z;
    if (jsonGetInt(json, "\"zone\"", &z) && z >= 1 && z <= 3)
      serialWeightOne(z - 1);
    else
      serialWeightAll();
    return;
  }

  // ── GET_STATUS ────────────────────────────────────────────
  if (strcmp(cmd, "GET_STATUS") == 0) {
    serialStatus();
    return;
  }

  // ── SET_ZONE ──────────────────────────────────────────────
  if (strcmp(cmd, "SET_ZONE") == 0) {
    int z, r, g, b;
    if (!jsonGetInt(json, "\"zone\"", &z) || z < 1 || z > 3) {
      serialAck(cmd, false, "invalid zone"); return;
    }
    if (!jsonGetInt(json, "\"r\"", &r) ||
        !jsonGetInt(json, "\"g\"", &g) ||
        !jsonGetInt(json, "\"b\"", &b)) {
      serialAck(cmd, false, "missing r/g/b"); return;
    }
    z--;  // 0-indexed
    zones[z].r = constrain(r, 0, 255);
    zones[z].g = constrain(g, 0, 255);
    zones[z].b = constrain(b, 0, 255);
    char lbl[16] = "unknown";
    jsonGetStr(json, "\"label\"", lbl, sizeof(lbl));
    strncpy(zones[z].label, lbl, sizeof(zones[z].label) - 1);
    setZoneLeds(z);
    strip.show();
    serialAck(cmd, true);
    return;
  }

  // ── SET_ALL ───────────────────────────────────────────────
  if (strcmp(cmd, "SET_ALL") == 0) {
    // Minimal parse: find each "zone": block manually
    // Expected: {"cmd":"SET_ALL","zones":[{"zone":1,"r":0,"g":120,"b":0,"label":"free"}, ...]}
    const char* p = json;
    uint8_t updated = 0;
    for (uint8_t i = 0; i < 3; i++) {
      p = strstr(p, "{\"zone\":");
      if (!p) break;
      int z, r, g, b;
      z = atoi(p + 8);
      if (z < 1 || z > 3) { p++; continue; }
      z--;
      int rv, gv, bv;
      if (jsonGetInt(p, "\"r\"", &rv) &&
          jsonGetInt(p, "\"g\"", &gv) &&
          jsonGetInt(p, "\"b\"", &bv)) {
        zones[z].r = constrain(rv, 0, 255);
        zones[z].g = constrain(gv, 0, 255);
        zones[z].b = constrain(bv, 0, 255);
        char lbl[16] = "unknown";
        jsonGetStr(p, "\"label\"", lbl, sizeof(lbl));
        strncpy(zones[z].label, lbl, sizeof(zones[z].label) - 1);
        setZoneLeds(z);
        updated++;
      }
      p++;
    }
    strip.show();
    if (updated) serialAck(cmd, true);
    else         serialAck(cmd, false, "no valid zones parsed");
    return;
  }

  // ── TARE ──────────────────────────────────────────────────
  if (strcmp(cmd, "TARE") == 0) {
    int z;
    if (jsonGetInt(json, "\"zone\"", &z) && z >= 1 && z <= 3) {
      scale[z - 1].tare();
      lastReportedKg[z - 1] = 0;
    } else {
      for (uint8_t i = 0; i < 3; i++) { scale[i].tare(); lastReportedKg[i] = 0; }
    }
    serialAck(cmd, true);
    return;
  }

  // ── TRAIN_ARRIVING ────────────────────────────────────────
  if (strcmp(cmd, "TRAIN_ARRIVING") == 0) {
    serialAck(cmd, true);
    animateArriving();
    return;
  }

  // ── TRAIN_LEAVING ─────────────────────────────────────────
  if (strcmp(cmd, "TRAIN_LEAVING") == 0) {
    serialAck(cmd, true);
    animateLeaving();
    return;
  }

  // ── Unknown ───────────────────────────────────────────────
  serialAck(cmd, false, "unknown command");
}

// ============================================================
//  setup / loop
// ============================================================

void setup() {
  Serial.begin(9600);
  startMs = millis();

  // NeoPixel init – separators stay black
  strip.begin();
  strip.clear();
  refreshStrip();

  // HX711 init
  for (uint8_t z = 0; z < 3; z++) {
    scale[z].begin(HX_DOUT[z], HX_SCK[z]);
    scale[z].set_scale(zones[z].calibFactor);
    scale[z].tare();
  }

  Serial.println(F("{\"type\":\"EVENT\",\"event\":\"BOOT\",\"msg\":\"SMS ready\"}"));
}

void loop() {
  // ── Read incoming serial, one char at a time ──────────────
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxPos > 0) {
        rxBuf[rxPos] = '\0';
        handleCommand(rxBuf);
        rxPos = 0;
      }
    } else if (rxPos < BUF_SIZE - 1) {
      rxBuf[rxPos++] = c;
    }
  }

// Replace the spontaneous weight check in loop() with this:
static unsigned long lastWeightCheck = 0;
if (millis() - lastWeightCheck > 500) {
    lastWeightCheck = millis();
    for (uint8_t z = 0; z < 3; z++) {
        // Only read if HX711 is ready RIGHT NOW, never block
        if (!scale[z].is_ready()) continue;
        long raw = scale[z].read(); // single non-blocking read
        float kg = (float)raw / zones[z].calibFactor;
        if (abs(kg - lastReportedKg[z]) >= WEIGHT_CHANGE_KG) {
            lastReportedKg[z] = kg;
            Serial.print(F("{\"type\":\"EVENT\",\"event\":\"WEIGHT_CHANGE\",\"zone\":"));
            Serial.print(z + 1);
            Serial.print(F(",\"kg\":"));  Serial.print(kg, 2);
            Serial.print(F(",\"pct\":")); Serial.print(kgToPct(z, kg));
            Serial.println('}');
        }
    }
}
}
