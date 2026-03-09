// ============================================================
//  SMS – Station Management System  v2
//  Hardware : Arduino + Adafruit NeoPixel + 3× HX711 load cells
//  Protocol : JSON over Serial (9600 baud, Newline terminated)
// ============================================================

#include <Adafruit_NeoPixel.h>
#include <HX711.h>
#include <EEPROM.h>

// ── NeoPixel ────────────────────────────────────────────────
#define STRIP_PIN   8
#define NUM_PIXELS  19
Adafruit_NeoPixel strip(NUM_PIXELS, STRIP_PIN, NEO_GRB + NEO_KHZ800);

// Zone pixel ranges (inclusive). Pixels 0,6,12,18 are separators (always black).
const uint8_t ZONE_START[3] = { 1,  7, 13};
const uint8_t ZONE_END[3]   = { 5, 11, 17};

// ── HX711 ────────────────────────────────────────────────────
const uint8_t HX_DOUT[3] = {2, 4, 6};
const uint8_t HX_SCK[3]  = {3, 5, 7};
HX711 scale[3];

// ── EEPROM layout ────────────────────────────────────────────
#define EEPROM_MAGIC  0xAB
#define EEPROM_ADDR   0

struct Config {
  uint8_t  magic;
  char     carId[8];
  float    calibFactor[3];
  float    maxKg[3];
  float    weightThreshold;
  uint16_t pushIntervalMs;
  uint8_t  brightness;
};

Config cfg;

void configDefaults() {
  cfg.magic           = EEPROM_MAGIC;
  strncpy(cfg.carId, "CAR1", sizeof(cfg.carId));
  for (uint8_t i = 0; i < 3; i++) {
    cfg.calibFactor[i] = 420.0f;
    cfg.maxKg[i]       = 5.0f;
  }
  cfg.weightThreshold = 2.0f;
  cfg.pushIntervalMs  = 0;
  cfg.brightness      = 255;
}

void saveConfig() { EEPROM.put(EEPROM_ADDR, cfg); }

void loadConfig() {
  EEPROM.get(EEPROM_ADDR, cfg);
  if (cfg.magic != EEPROM_MAGIC) { configDefaults(); saveConfig(); }
}

// ── Zone runtime state ───────────────────────────────────────
struct Zone { uint8_t r, g, b; char label[16]; };
Zone zones[3] = {{0,120,0,"free"},{0,120,0,"free"},{0,120,0,"free"}};

// ── Train state machine ──────────────────────────────────────
enum TrainState { NO_TRAIN, ARRIVING, IN_SERVICE, LEAVING };
TrainState trainState = NO_TRAIN;

// ── Serial buffer ────────────────────────────────────────────
#define BUF_SIZE 256
char    rxBuf[BUF_SIZE];
uint8_t rxPos = 0;

unsigned long startMs;
float lastReportedKg[3] = {0, 0, 0};

// ── Idle breath state ────────────────────────────────────────
uint8_t       breathVal  = 0;
int8_t        breathDir  = 1;
unsigned long lastBreath = 0;
#define BREATH_R       0
#define BREATH_G       0
#define BREATH_B       180
#define BREATH_STEP_MS 12

// ============================================================
//  LED helpers
// ============================================================

bool isSeparator(uint8_t p) {
  return (p == 0 || p == 6 || p == 12 || p == 18);
}

void setZoneLeds(uint8_t z) {
  for (uint8_t p = ZONE_START[z]; p <= ZONE_END[z]; p++)
    strip.setPixelColor(p, strip.Color(zones[z].r, zones[z].g, zones[z].b));
}

void refreshStrip() {
  for (uint8_t z = 0; z < 3; z++) setZoneLeds(z);
  strip.show();
}

// ============================================================
//  Animations
// ============================================================

// Helper: ordered list of non-separator pixels, optionally reversed
uint8_t pixelList[15];
uint8_t pixelCount = 0;

void buildPixelList(int8_t dir) {
  pixelCount = 0;
  for (uint8_t z = 0; z < 3; z++)
    for (uint8_t p = ZONE_START[z]; p <= ZONE_END[z]; p++)
      pixelList[pixelCount++] = p;
  if (dir < 0) {
    for (uint8_t i = 0; i < pixelCount / 2; i++) {
      uint8_t tmp = pixelList[i];
      pixelList[i] = pixelList[pixelCount - 1 - i];
      pixelList[pixelCount - 1 - i] = tmp;
    }
  }
}

// Boot: each zone flashes blue in sequence to confirm wiring
void animBoot() {
  for (uint8_t z = 0; z < 3; z++) {
    for (uint8_t p = ZONE_START[z]; p <= ZONE_END[z]; p++)
      strip.setPixelColor(p, strip.Color(0, 0, 180));
    strip.show();  delay(250);
    strip.clear(); strip.show(); delay(120);
  }
}

// Arriving: pixels light up one by one (white sweep) then settle to zone colors
void animArriving(int8_t dir) {
  buildPixelList(dir);
  strip.clear(); strip.show();
  for (uint8_t i = 0; i < pixelCount; i++) {
    strip.setPixelColor(pixelList[i], strip.Color(255, 255, 255));
    strip.show();
    delay(60);
  }
  delay(300);
  refreshStrip();  // settle to occupancy colors
}

// Leaving: pixels turn off one by one in direction of travel
void animLeaving(int8_t dir) {
  buildPixelList(dir);
  refreshStrip();  // make sure we start from zone colors
  for (uint8_t i = 0; i < pixelCount; i++) {
    strip.setPixelColor(pixelList[i], 0);
    strip.show();
    delay(60);
  }
  delay(200);
}

// Boarding: slow double-pulse on zone LEDs (doors open)
void animBoarding() {
  for (uint8_t pulse = 0; pulse < 2; pulse++) {
    for (int b = 0; b <= 255; b += 5) { strip.setBrightness(b); strip.show(); delay(8); }
    for (int b = 255; b >= 60; b -= 5) { strip.setBrightness(b); strip.show(); delay(8); }
  }
  strip.setBrightness(cfg.brightness);
  strip.show();
}

// Doors closing: fast amber blink warning
void animDoorsClosing() {
  for (uint8_t i = 0; i < 5; i++) {
    for (uint8_t p = 1; p < NUM_PIXELS; p++)
      if (!isSeparator(p)) strip.setPixelColor(p, strip.Color(200, 100, 0));
    strip.show(); delay(120);
    strip.clear(); strip.show(); delay(80);
  }
  refreshStrip();
}

// ============================================================
//  Idle breath (non-blocking)
// ============================================================

void updateBreath() {
  if (trainState != NO_TRAIN) return;
  unsigned long now = millis();
  if (now - lastBreath < BREATH_STEP_MS) return;
  lastBreath = now;
  breathVal += breathDir * 3;
  if (breathVal >= 252) { breathVal = 255; breathDir = -1; }
  if (breathVal <= 3)   { breathVal = 0;   breathDir =  1; }
  for (uint8_t p = 0; p < NUM_PIXELS; p++) {
    if (isSeparator(p)) { strip.setPixelColor(p, 0); continue; }
    strip.setPixelColor(p, strip.Color(
      (BREATH_R * breathVal) / 255,
      (BREATH_G * breathVal) / 255,
      (BREATH_B * breathVal) / 255
    ));
  }
  strip.show();
}

// ============================================================
//  Minimal JSON helpers
// ============================================================

bool jsonGetInt(const char* json, const char* key, int* out) {
  const char* p = strstr(json, key);
  if (!p) return false;
  p += strlen(key);
  while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
  if (!*p) return false;
  *out = atoi(p);
  return true;
}

bool jsonGetFloat(const char* json, const char* key, float* out) {
  const char* p = strstr(json, key);
  if (!p) return false;
  p += strlen(key);
  while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
  if (!*p) return false;
  *out = atof(p);
  return true;
}

const char* jsonGetStr(const char* json, const char* key, char* outBuf, uint8_t bufLen) {
  const char* p = strstr(json, key);
  if (!p) return nullptr;
  p += strlen(key);
  while (*p && *p != '"') p++;
  if (*p != '"') return nullptr;
  p++;
  uint8_t i = 0;
  while (*p && *p != '"' && i < bufLen - 1) outBuf[i++] = *p++;
  outBuf[i] = '\0';
  return outBuf;
}

// ============================================================
//  Sensor helpers
// ============================================================

float readKg(uint8_t z) {
  if (!scale[z].is_ready()) return 0.0f;
  return (float)scale[z].read() / cfg.calibFactor[z];
}

uint8_t kgToPct(uint8_t z, float kg) {
  if (kg <= 0) return 0;
  return (uint8_t)constrain((int)((kg / cfg.maxKg[z]) * 100.0f), 0, 100);
}

// ============================================================
//  Serial output helpers
// ============================================================

void serialAck(const char* cmd, bool ok, const char* msg = nullptr) {
  Serial.print(F("{\"type\":\"ACK\",\"cmd\":\"")); Serial.print(cmd);
  Serial.print(ok ? F("\",\"status\":\"ok\"") : F("\",\"status\":\"error\""));
  if (msg) { Serial.print(F(",\"msg\":\"")); Serial.print(msg); Serial.print('"'); }
  Serial.println('}');
}

void serialWeightAll() {
  Serial.print(F("{\"type\":\"WEIGHT\",\"car\":\"")); Serial.print(cfg.carId);
  Serial.print(F("\",\"zones\":["));
  for (uint8_t z = 0; z < 3; z++) {
    float kg  = readKg(z);
    long  raw = scale[z].is_ready() ? scale[z].read() : 0;
    if (z) Serial.print(',');
    Serial.print(F("{\"zone\":")); Serial.print(z+1);
    Serial.print(F(",\"raw\":"));  Serial.print(raw);
    Serial.print(F(",\"kg\":"));   Serial.print(kg, 2);
    Serial.print(F(",\"pct\":")); Serial.print(kgToPct(z, kg));
    Serial.print('}');
  }
  Serial.println(F("]}"));
}

void serialWeightOne(uint8_t z) {
  float kg  = readKg(z);
  long  raw = scale[z].is_ready() ? scale[z].read() : 0;
  Serial.print(F("{\"type\":\"WEIGHT\",\"car\":\"")); Serial.print(cfg.carId);
  Serial.print(F("\",\"zone\":")); Serial.print(z+1);
  Serial.print(F(",\"raw\":"));    Serial.print(raw);
  Serial.print(F(",\"kg\":"));     Serial.print(kg, 2);
  Serial.print(F(",\"pct\":")); Serial.print(kgToPct(z, kg));
  Serial.println('}');
}

void serialStatus() {
  Serial.print(F("{\"type\":\"STATUS\",\"car\":\"")); Serial.print(cfg.carId);
  Serial.print(F("\",\"train_state\":\""));
  switch (trainState) {
    case NO_TRAIN:   Serial.print(F("no_train"));   break;
    case IN_SERVICE: Serial.print(F("in_service")); break;
    default:         Serial.print(F("transition")); break;
  }
  Serial.print(F("\",\"zones\":["));
  for (uint8_t z = 0; z < 3; z++) {
    float kg = readKg(z);
    if (z) Serial.print(',');
    Serial.print(F("{\"zone\":")); Serial.print(z+1);
    Serial.print(F(",\"r\":"));    Serial.print(zones[z].r);
    Serial.print(F(",\"g\":"));    Serial.print(zones[z].g);
    Serial.print(F(",\"b\":"));    Serial.print(zones[z].b);
    Serial.print(F(",\"label\":\"")); Serial.print(zones[z].label); Serial.print('"');
    Serial.print(F(",\"kg\":"));   Serial.print(kg, 2);
    Serial.print(F(",\"pct\":")); Serial.print(kgToPct(z, kg));
    Serial.print('}');
  }
  Serial.println(F("]}"));
}

void serialConfig() {
  Serial.print(F("{\"type\":\"CONFIG\",\"car\":\"")); Serial.print(cfg.carId);
  Serial.print(F("\",\"brightness\":")); Serial.print(cfg.brightness);
  Serial.print(F(",\"push_interval_ms\":")); Serial.print(cfg.pushIntervalMs);
  Serial.print(F(",\"weight_threshold\":")); Serial.print(cfg.weightThreshold, 2);
  Serial.print(F(",\"zones\":["));
  for (uint8_t z = 0; z < 3; z++) {
    if (z) Serial.print(',');
    Serial.print(F("{\"zone\":")); Serial.print(z+1);
    Serial.print(F(",\"calib\":")); Serial.print(cfg.calibFactor[z], 2);
    Serial.print(F(",\"max_kg\":")); Serial.print(cfg.maxKg[z], 2);
    Serial.print('}');
  }
  Serial.println(F("]}"));
}

// ============================================================
//  Command dispatcher
// ============================================================

void handleCommand(const char* json) {
  char cmd[32] = "";
  if (!jsonGetStr(json, "\"cmd\"", cmd, sizeof(cmd))) {
    Serial.println(F("{\"type\":\"ACK\",\"status\":\"error\",\"msg\":\"missing cmd\"}"));
    return;
  }

  if (strcmp(cmd, "PING") == 0) {
    Serial.print(F("{\"type\":\"PONG\",\"car\":\"")); Serial.print(cfg.carId);
    Serial.print(F("\",\"uptime_ms\":")); Serial.print(millis() - startMs);
    Serial.println('}');
    return;
  }

  if (strcmp(cmd, "GET_WEIGHT") == 0) {
    int z;
    if (jsonGetInt(json, "\"zone\"", &z) && z >= 1 && z <= 3) serialWeightOne(z-1);
    else serialWeightAll();
    return;
  }

  if (strcmp(cmd, "GET_STATUS") == 0)  { serialStatus();  return; }
  if (strcmp(cmd, "GET_CONFIG") == 0)  { serialConfig();  return; }

  if (strcmp(cmd, "GET_RAW") == 0) {
    Serial.print(F("{\"type\":\"RAW\",\"car\":\"")); Serial.print(cfg.carId);
    Serial.print(F("\",\"zones\":["));
    for (uint8_t z = 0; z < 3; z++) {
      long raw = scale[z].is_ready() ? scale[z].read() : 0;
      if (z) Serial.print(',');
      Serial.print(F("{\"zone\":")); Serial.print(z+1);
      Serial.print(F(",\"raw\":"));  Serial.print(raw);
      Serial.print('}');
    }
    Serial.println(F("]}"));
    return;
  }

  if (strcmp(cmd, "GET_UPTIME") == 0) {
    Serial.print(F("{\"type\":\"UPTIME\",\"car\":\"")); Serial.print(cfg.carId);
    Serial.print(F("\",\"uptime_ms\":")); Serial.print(millis() - startMs);
    Serial.print(F(",\"uptime_s\":")); Serial.print((millis()-startMs)/1000);
    Serial.println('}');
    return;
  }

  if (strcmp(cmd, "SET_ZONE") == 0) {
    int z, r, g, b;
    if (!jsonGetInt(json, "\"zone\"", &z) || z<1||z>3) { serialAck(cmd,false,"invalid zone"); return; }
    if (!jsonGetInt(json,"\"r\"",&r)||!jsonGetInt(json,"\"g\"",&g)||!jsonGetInt(json,"\"b\"",&b)) { serialAck(cmd,false,"missing r/g/b"); return; }
    z--;
    zones[z].r = constrain(r,0,255);
    zones[z].g = constrain(g,0,255);
    zones[z].b = constrain(b,0,255);
    char lbl[16]="unknown"; jsonGetStr(json,"\"label\"",lbl,sizeof(lbl));
    strncpy(zones[z].label, lbl, sizeof(zones[z].label)-1);
    if (trainState == IN_SERVICE) { setZoneLeds(z); strip.show(); }
    serialAck(cmd, true);
    return;
  }

  if (strcmp(cmd, "SET_ALL") == 0) {
    const char* p = json; uint8_t updated = 0;
    for (uint8_t i = 0; i < 3; i++) {
      p = strstr(p, "{\"zone\":");
      if (!p) break;
      int z = atoi(p+8);
      if (z<1||z>3) { p++; continue; }
      z--;
      int rv,gv,bv;
      if (jsonGetInt(p,"\"r\"",&rv)&&jsonGetInt(p,"\"g\"",&gv)&&jsonGetInt(p,"\"b\"",&bv)) {
        zones[z].r=constrain(rv,0,255); zones[z].g=constrain(gv,0,255); zones[z].b=constrain(bv,0,255);
        char lbl[16]="unknown"; jsonGetStr(p,"\"label\"",lbl,sizeof(lbl));
        strncpy(zones[z].label,lbl,sizeof(zones[z].label)-1);
        if (trainState==IN_SERVICE) setZoneLeds(z);
        updated++;
      }
      p++;
    }
    if (trainState==IN_SERVICE) strip.show();
    updated ? serialAck(cmd,true) : serialAck(cmd,false,"no valid zones");
    return;
  }

  if (strcmp(cmd, "SET_BRIGHTNESS") == 0) {
    int v; if (!jsonGetInt(json,"\"value\"",&v)) { serialAck(cmd,false,"missing value"); return; }
    cfg.brightness=constrain(v,0,255); strip.setBrightness(cfg.brightness); strip.show();
    serialAck(cmd,true); return;
  }

  if (strcmp(cmd, "SET_CALIB") == 0) {
    int z; float f;
    if (!jsonGetInt(json,"\"zone\"",&z)||z<1||z>3) { serialAck(cmd,false,"invalid zone"); return; }
    if (!jsonGetFloat(json,"\"factor\"",&f))        { serialAck(cmd,false,"missing factor"); return; }
    cfg.calibFactor[z-1]=f; scale[z-1].set_scale(f);
    serialAck(cmd,true); return;
  }

  if (strcmp(cmd, "SET_MAX_KG") == 0) {
    int z; float v;
    if (!jsonGetInt(json,"\"zone\"",&z)||z<1||z>3) { serialAck(cmd,false,"invalid zone"); return; }
    if (!jsonGetFloat(json,"\"kg\"",&v))            { serialAck(cmd,false,"missing kg"); return; }
    cfg.maxKg[z-1]=v; serialAck(cmd,true); return;
  }

  if (strcmp(cmd, "SET_THRESHOLD") == 0) {
    float v; if (!jsonGetFloat(json,"\"kg\"",&v)) { serialAck(cmd,false,"missing kg"); return; }
    cfg.weightThreshold=v; serialAck(cmd,true); return;
  }

  if (strcmp(cmd, "SET_PUSH_INTERVAL") == 0) {
    int v; if (!jsonGetInt(json,"\"ms\"",&v)) { serialAck(cmd,false,"missing ms"); return; }
    cfg.pushIntervalMs=(uint16_t)constrain(v,0,60000); serialAck(cmd,true); return;
  }

  if (strcmp(cmd, "PUSH_ON")  == 0) { cfg.pushIntervalMs=2000; serialAck(cmd,true); return; }
  if (strcmp(cmd, "PUSH_OFF") == 0) { cfg.pushIntervalMs=0;    serialAck(cmd,true); return; }

  if (strcmp(cmd, "SET_CAR_ID") == 0) {
    char id[8]; if (!jsonGetStr(json,"\"id\"",id,sizeof(id))) { serialAck(cmd,false,"missing id"); return; }
    strncpy(cfg.carId,id,sizeof(cfg.carId)-1); serialAck(cmd,true); return;
  }

  if (strcmp(cmd, "TARE") == 0) {
    int z;
    if (jsonGetInt(json,"\"zone\"",&z)&&z>=1&&z<=3) { scale[z-1].tare(); lastReportedKg[z-1]=0; }
    else { for (uint8_t i=0;i<3;i++){scale[i].tare();lastReportedKg[i]=0;} }
    serialAck(cmd,true); return;
  }

  if (strcmp(cmd, "SELFTEST") == 0) {
    Serial.print(F("{\"type\":\"SELFTEST\",\"car\":\"")); Serial.print(cfg.carId);
    Serial.print(F("\",\"zones\":["));
    for (uint8_t z=0;z<3;z++) {
      for (uint8_t p=ZONE_START[z];p<=ZONE_END[z];p++) strip.setPixelColor(p,strip.Color(255,255,255));
      strip.show(); delay(300); strip.clear(); strip.show(); delay(100);
      long raw=scale[z].is_ready()?scale[z].read():-1;
      float kg=raw>=0?(float)raw/cfg.calibFactor[z]:0.0f;
      if (z) Serial.print(',');
      Serial.print(F("{\"zone\":")); Serial.print(z+1);
      Serial.print(F(",\"sensor_ok\":")); Serial.print(scale[z].is_ready()?F("true"):F("false"));
      Serial.print(F(",\"raw\":"));  Serial.print(raw);
      Serial.print(F(",\"kg\":"));   Serial.print(kg,2);
      Serial.print('}');
    }
    Serial.println(F("]}"));
    if (trainState==IN_SERVICE) refreshStrip();
    return;
  }

  if (strcmp(cmd, "SAVE_CONFIG") == 0) { saveConfig(); serialAck(cmd,true); return; }

  if (strcmp(cmd, "LOAD_CONFIG") == 0) {
    loadConfig();
    for (uint8_t z=0;z<3;z++) scale[z].set_scale(cfg.calibFactor[z]);
    strip.setBrightness(cfg.brightness);
    serialAck(cmd,true); return;
  }

  if (strcmp(cmd, "RESET") == 0) {
    serialAck(cmd,true); delay(100);
    asm volatile ("jmp 0"); return;
  }

  if (strcmp(cmd, "TRAIN_ARRIVING") == 0) {
    char dirStr[8]="ltr"; jsonGetStr(json,"\"dir\"",dirStr,sizeof(dirStr));
    int8_t dir=(strcmp(dirStr,"rtl")==0)?-1:1;
    trainState=ARRIVING; serialAck(cmd,true);
    animArriving(dir);
    trainState=IN_SERVICE;
    Serial.print(F("{\"type\":\"EVENT\",\"event\":\"TRAIN_IN_SERVICE\",\"car\":\""));
    Serial.print(cfg.carId); Serial.println(F("\"}"));
    return;
  }

  if (strcmp(cmd, "TRAIN_LEAVING") == 0) {
    char dirStr[8]="ltr"; jsonGetStr(json,"\"dir\"",dirStr,sizeof(dirStr));
    int8_t dir=(strcmp(dirStr,"rtl")==0)?-1:1;
    trainState=LEAVING; serialAck(cmd,true);
    animLeaving(dir);
    trainState=NO_TRAIN;
    breathVal=0; breathDir=1;
    Serial.print(F("{\"type\":\"EVENT\",\"event\":\"TRAIN_GONE\",\"car\":\""));
    Serial.print(cfg.carId); Serial.println(F("\"}"));
    return;
  }

  if (strcmp(cmd, "TRAIN_BOARDING") == 0) {
    animBoarding(); serialAck(cmd,true); return;
  }

  if (strcmp(cmd, "TRAIN_DOORS_CLOSING") == 0) {
    animDoorsClosing(); serialAck(cmd,true); return;
  }

  serialAck(cmd, false, "unknown command");
}

// ============================================================
//  setup / loop
// ============================================================

void setup() {
  Serial.begin(9600);
  startMs = millis();

  loadConfig();
  strip.begin();
  strip.setBrightness(cfg.brightness);
  strip.clear(); strip.show();

  animBoot();            // zone-by-zone blue flash
  trainState = NO_TRAIN; // start idle breath
  breathVal = 0; breathDir = 1;

  for (uint8_t z = 0; z < 3; z++) {
    scale[z].begin(HX_DOUT[z], HX_SCK[z]);
    scale[z].set_scale(cfg.calibFactor[z]);
    scale[z].tare();
  }

  Serial.print(F("{\"type\":\"EVENT\",\"event\":\"BOOT\",\"car\":\""));
  Serial.print(cfg.carId);
  Serial.println(F("\",\"msg\":\"SMS ready\"}"));
}

void loop() {
  // ── Serial receive ──────────────────────────────────────
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxPos > 0) { rxBuf[rxPos]='\0'; handleCommand(rxBuf); rxPos=0; }
    } else if (rxPos < BUF_SIZE-1) {
      rxBuf[rxPos++] = c;
    }
  }

  // ── Idle breath ─────────────────────────────────────────
  updateBreath();

  // ── Weight-change events ────────────────────────────────
  static unsigned long lastChangeCheck = 0;
  unsigned long now = millis();
  if (now - lastChangeCheck > 500 && trainState == IN_SERVICE) {
    lastChangeCheck = now;
    for (uint8_t z = 0; z < 3; z++) {
      if (!scale[z].is_ready()) continue;
      float kg = readKg(z);
      if (abs(kg - lastReportedKg[z]) >= cfg.weightThreshold) {
        lastReportedKg[z] = kg;
        Serial.print(F("{\"type\":\"EVENT\",\"event\":\"WEIGHT_CHANGE\",\"car\":\""));
        Serial.print(cfg.carId);
        Serial.print(F("\",\"zone\":")); Serial.print(z+1);
        Serial.print(F(",\"kg\":"));    Serial.print(kg, 2);
        Serial.print(F(",\"pct\":")); Serial.print(kgToPct(z, kg));
        Serial.println('}');
      }
    }
  }

  // ── Periodic push ───────────────────────────────────────
  static unsigned long lastPush = 0;
  if (cfg.pushIntervalMs > 0 && trainState == IN_SERVICE) {
    if (now - lastPush >= cfg.pushIntervalMs) { lastPush=now; serialWeightAll(); }
  }
}
