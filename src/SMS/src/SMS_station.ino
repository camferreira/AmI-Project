// ============================================================
//  SMS – Station Management System  v3
//  Hardware : Arduino Uno R3 + NeoPixel strip + 3x HX711
//  Protocol : JSON over Serial (9600 baud, \n terminated)
//  Libs     : ArduinoJson v6 (output only), NeoPixel, HX711
//
//  RAM budget (Uno = 2048 bytes total):
//    rxBuf          256 B   serial receive buffer
//    Config struct   36 B   calibration + settings
//    Zone state      57 B   3x (r,g,b + 16 char label)
//    pixList         15 B   animation pixel order
//    globals         ~30 B  breath, timers, counters
//    libs + stack  ~800 B   NeoPixel, HX711, Arduino rt
//    headroom       ~850 B  safe margin
//
//  Parsing strategy: read directly from rxBuf with lightweight
//  helpers — zero heap allocation, zero ArduinoJson for input.
//  ArduinoJson used only for output (small stack-local docs).
// ============================================================

#include <Adafruit_NeoPixel.h>
#include <HX711.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// ── NeoPixel ─────────────────────────────────────────────────
#define STRIP_PIN   8
#define NUM_PIXELS  19
Adafruit_NeoPixel strip(NUM_PIXELS, STRIP_PIN, NEO_GRB + NEO_KHZ800);

const uint8_t ZONE_START[3] = { 1,  7, 13 };
const uint8_t ZONE_END[3]   = { 5, 11, 17 };

// ── HX711 ────────────────────────────────────────────────────
const uint8_t HX_DOUT[3] = { 2, 4, 6 };
const uint8_t HX_SCK[3]  = { 3, 5, 7 };
HX711 scale[3];

// ── EEPROM ───────────────────────────────────────────────────
#define EEPROM_MAGIC 0xAB
#define EEPROM_ADDR  0

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
    cfg.maxKg[i]       = 1.0f;
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

// ── Zone state ───────────────────────────────────────────────
struct Zone { uint8_t r, g, b; char label[16]; };
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
#define BUF_SIZE 256
char    rxBuf[BUF_SIZE];
uint8_t rxPos = 0;

unsigned long startMs;
float lastReportedKg[3] = {0, 0, 0};

// ── Breath ───────────────────────────────────────────────────
uint8_t       breathVal  = 0;
int8_t        breathDir  = 1;
unsigned long lastBreath = 0;
#define BREATH_B       180
#define BREATH_STEP_MS 12

// ── Float scratch ────────────────────────────────────────────
char fbuf[10];
#define FMT(f) dtostrf((f), 1, 2, fbuf)

// ============================================================
//  Lightweight JSON parsers
//  All operate directly on rxBuf — zero allocation.
//
//  jStr(buf, "key", out, len) — extract string value
//  jInt(buf, "key", out)      — extract integer value
//  jFlt(buf, "key", out)      — extract float value
//  jHas(buf, "key")           — check key exists
// ============================================================

// Find the value start position after "key": in a JSON buffer.
// Returns pointer to first char of the value, or nullptr.
static const char* jFind(const char* buf, const char* key) {
  const char* p = buf;
  uint8_t klen  = strlen(key);
  while (*p) {
    // Look for opening quote of key
    if (*p == '"') {
      if (strncmp(p + 1, key, klen) == 0 && *(p + 1 + klen) == '"') {
        // Found the key — skip past key, closing quote, colon, whitespace
        p = p + 1 + klen + 1; // past closing quote
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
  p++; // skip opening quote
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

bool jFlt(const char* buf, const char* key, float* out) {
  const char* p = jFind(buf, key);
  if (!p) return false;
  *out = atof(p);
  return true;
}

bool jHas(const char* buf, const char* key) {
  return jFind(buf, key) != nullptr;
}

// Parse one zone object starting at pos in buf.
// Fills z(0-based), r, g, b, label. Returns true on success.
bool jZone(const char* pos, uint8_t* z, uint8_t* r, uint8_t* g,
           uint8_t* b, char* label, uint8_t lblLen) {
  int zi = 0;
  if (!jInt(pos, "zone", &zi) || zi < 1 || zi > 3) return false;
  *z = (uint8_t)(zi - 1);
  int ri = 0, gi = 0, bi = 0;
  jInt(pos, "r", &ri); *r = (uint8_t)constrain(ri, 0, 255);
  jInt(pos, "g", &gi); *g = (uint8_t)constrain(gi, 0, 255);
  jInt(pos, "b", &bi); *b = (uint8_t)constrain(bi, 0, 255);
  if (!jStr(pos, "label", label, lblLen)) strncpy(label, "unknown", lblLen);
  return true;
}

// Walk through all {"zone":...} objects in the zones array of buf
// and apply them to the global zones[] state.
void parseAndApplyZones(const char* buf) {
  const char* p = buf;
  while ((p = strstr(p, "{\"zone\":")) != nullptr) {
    uint8_t z, r, g, b; char lbl[16];
    if (jZone(p, &z, &r, &g, &b, lbl, sizeof(lbl))) {
      zones[z].r = r; zones[z].g = g; zones[z].b = b;
      strncpy(zones[z].label, lbl, sizeof(zones[z].label) - 1);
    }
    p++;
  }
}

// ============================================================
//  Sensor helpers
// ============================================================

// Read raw ADC value once — single .read() call per reporting cycle.
// Derive kg and pct from raw — never call .read() twice on the same zone.
long readRaw(uint8_t z) {
  if (!scale[z].is_ready()) return 0L;
  return scale[z].read();
}

float rawToKg(uint8_t z, long raw) {
  if (raw <= 0) return 0.0f;
  return (float)raw / cfg.calibFactor[z];
}

uint8_t kgToPct(uint8_t z, float kg) {
  if (kg <= 0) return 0;
  return (uint8_t)constrain((int)((kg / cfg.maxKg[z]) * 100.0f), 0, 100);
}

// ============================================================
//  JSON output helpers  (ArduinoJson, stack-local docs only)
// ============================================================

void sendAck(const char* cmd, bool ok, const char* msg = nullptr) {
  StaticJsonDocument<96> doc;
  doc[F("type")]   = F("ACK");
  doc[F("cmd")]    = cmd;
  doc[F("status")] = ok ? F("ok") : F("error");
  if (msg) doc[F("msg")] = msg;
  serializeJson(doc, Serial); Serial.print('\n');
}

void sendEvent(const char* event, const char* msg = nullptr) {
  StaticJsonDocument<96> doc;
  doc[F("type")]  = F("EVENT");
  doc[F("event")] = event;
  doc[F("car")]   = cfg.carId;
  if (msg) doc[F("msg")] = msg;
  serializeJson(doc, Serial); Serial.print('\n');
}

void sendPong() {
  StaticJsonDocument<80> doc;
  doc[F("type")]      = F("PONG");
  doc[F("car")]       = cfg.carId;
  doc[F("uptime_ms")] = millis() - startMs;
  serializeJson(doc, Serial); Serial.print('\n');
}

void sendUptime() {
  StaticJsonDocument<96> doc;
  doc[F("type")]      = F("UPTIME");
  doc[F("car")]       = cfg.carId;
  doc[F("uptime_ms")] = millis() - startMs;
  doc[F("uptime_s")]  = (millis() - startMs) / 1000UL;
  serializeJson(doc, Serial); Serial.print('\n');
}

// Weight for one zone — build object into an existing array
void addWeightObj(JsonArray arr, uint8_t z) {
  long    raw = readRaw(z);           // single read
  float   kg  = rawToKg(z, raw);
  JsonObject o = arr.createNestedObject();
  o[F("zone")] = z + 1;
  o[F("raw")]  = raw;
  o[F("kg")]   = FMT(kg);
  o[F("pct")]  = kgToPct(z, kg);
}

void sendWeightAll() {
  StaticJsonDocument<320> doc;
  doc[F("type")] = F("WEIGHT");
  doc[F("car")]  = cfg.carId;
  JsonArray arr  = doc.createNestedArray(F("zones"));
  for (uint8_t z = 0; z < 3; z++) addWeightObj(arr, z);
  serializeJson(doc, Serial); Serial.print('\n');
}

void sendWeightOne(uint8_t z) {
  long    raw = readRaw(z);           // single read
  float   kg  = rawToKg(z, raw);
  StaticJsonDocument<96> doc;
  doc[F("type")] = F("WEIGHT");
  doc[F("car")]  = cfg.carId;
  doc[F("zone")] = z + 1;
  doc[F("raw")]  = raw;
  doc[F("kg")]   = FMT(kg);
  doc[F("pct")]  = kgToPct(z, kg);
  serializeJson(doc, Serial); Serial.print('\n');
}

void sendStatus() {
  StaticJsonDocument<384> doc;
  doc[F("type")]        = F("STATUS");
  doc[F("car")]         = cfg.carId;
  doc[F("train_state")] = trainStateStr();
  JsonArray arr = doc.createNestedArray(F("zones"));
  for (uint8_t z = 0; z < 3; z++) {
    long  raw = readRaw(z);           // single read
    float kg  = rawToKg(z, raw);
    JsonObject o = arr.createNestedObject();
    o[F("zone")]  = z + 1;
    o[F("r")]     = zones[z].r;
    o[F("g")]     = zones[z].g;
    o[F("b")]     = zones[z].b;
    o[F("label")] = zones[z].label;
    o[F("kg")]    = FMT(kg);
    o[F("pct")]   = kgToPct(z, kg);
  }
  serializeJson(doc, Serial); Serial.print('\n');
}

void sendConfig() {
  StaticJsonDocument<320> doc;
  doc[F("type")]             = F("CONFIG");
  doc[F("car")]              = cfg.carId;
  doc[F("brightness")]       = cfg.brightness;
  doc[F("push_interval_ms")] = cfg.pushIntervalMs;
  doc[F("weight_threshold")] = FMT(cfg.weightThreshold);
  JsonArray arr = doc.createNestedArray(F("zones"));
  for (uint8_t z = 0; z < 3; z++) {
    JsonObject o = arr.createNestedObject();
    o[F("zone")]   = z + 1;
    o[F("calib")]  = FMT(cfg.calibFactor[z]);
    o[F("max_kg")] = FMT(cfg.maxKg[z]);
  }
  serializeJson(doc, Serial); Serial.print('\n');
}

void sendRaw() {
  StaticJsonDocument<160> doc;
  doc[F("type")] = F("RAW");
  doc[F("car")]  = cfg.carId;
  JsonArray arr  = doc.createNestedArray(F("zones"));
  for (uint8_t z = 0; z < 3; z++) {
    JsonObject o = arr.createNestedObject();
    o[F("zone")] = z + 1;
    o[F("raw")]  = scale[z].is_ready() ? scale[z].read() : 0L;
  }
  serializeJson(doc, Serial); Serial.print('\n');
}

void sendWeightChangeEvent(uint8_t z, float kg) {
  StaticJsonDocument<96> doc;
  doc[F("type")]  = F("EVENT");
  doc[F("event")] = F("WEIGHT_CHANGE");
  doc[F("car")]   = cfg.carId;
  doc[F("zone")]  = z + 1;
  doc[F("kg")]    = FMT(kg);
  doc[F("pct")]   = kgToPct(z, kg);
  serializeJson(doc, Serial); Serial.print('\n');
}

// ============================================================
//  LED helpers
// ============================================================

bool isSep(uint8_t p) { return (p==0||p==6||p==12||p==18); }

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

uint8_t pixList[15];
uint8_t pixCount = 0;

void buildPixList(int8_t dir) {
  pixCount = 0;
  for (uint8_t z = 0; z < 3; z++)
    for (uint8_t p = ZONE_START[z]; p <= ZONE_END[z]; p++)
      pixList[pixCount++] = p;
  if (dir < 0)
    for (uint8_t i = 0; i < pixCount/2; i++) {
      uint8_t t=pixList[i];
      pixList[i]=pixList[pixCount-1-i];
      pixList[pixCount-1-i]=t;
    }
}

void animBoot() {
  for (uint8_t z = 0; z < 3; z++) {
    for (uint8_t p=ZONE_START[z]; p<=ZONE_END[z]; p++)
      strip.setPixelColor(p, strip.Color(0,0,180));
    strip.show(); delay(250);
    strip.clear(); strip.show(); delay(120);
  }
}

void animArriving(int8_t dir) {
  buildPixList(dir);
  strip.clear(); strip.show();
  for (uint8_t i=0; i<pixCount; i++) {
    strip.setPixelColor(pixList[i], strip.Color(255,255,255));
    strip.show(); delay(60);
  }
  delay(300);
  refreshStrip();
}

void animLeaving(int8_t dir) {
  buildPixList(dir);
  refreshStrip();
  for (uint8_t i=0; i<pixCount; i++) {
    strip.setPixelColor(pixList[i], 0);
    strip.show(); delay(60);
  }
  delay(200);
}

void animBoarding() {
  for (uint8_t pulse=0; pulse<2; pulse++) {
    for (int b=0;  b<=255; b+=5) { strip.setBrightness(b); strip.show(); delay(8); }
    for (int b=255; b>=60; b-=5) { strip.setBrightness(b); strip.show(); delay(8); }
  }
  strip.setBrightness(cfg.brightness); strip.show();
}

void animDoorsClosing() {
  for (uint8_t i=0; i<5; i++) {
    for (uint8_t p=0; p<NUM_PIXELS; p++)
      if (!isSep(p)) strip.setPixelColor(p, strip.Color(200,100,0));
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
  if (millis() - lastBreath < BREATH_STEP_MS) return;
  lastBreath = millis();
  breathVal += breathDir * 3;
  if (breathVal >= 252) { breathVal=255; breathDir=-1; }
  if (breathVal <=   3) { breathVal=0;   breathDir= 1; }
  for (uint8_t p=0; p<NUM_PIXELS; p++) {
    if (isSep(p)) { strip.setPixelColor(p,0); continue; }
    strip.setPixelColor(p, strip.Color(0, 0,
      (uint8_t)((BREATH_B * breathVal) / 255)
    ));
  }
  strip.show();
}

// ============================================================
//  Selftest  (own function — large enough to warrant isolation)
// ============================================================

void doSelftest() {
  // Flash each zone and report — output built piece by piece
  // to avoid a large JsonDocument for the nested array
  Serial.print(F("{\"type\":\"SELFTEST\",\"car\":\""));
  Serial.print(cfg.carId);
  Serial.print(F("\",\"zones\":["));
  for (uint8_t z=0; z<3; z++) {
    for (uint8_t p=ZONE_START[z]; p<=ZONE_END[z]; p++)
      strip.setPixelColor(p, strip.Color(255,255,255));
    strip.show(); delay(300);
    strip.clear(); strip.show(); delay(100);
    bool  ok  = scale[z].is_ready();
    long  raw = ok ? scale[z].read() : -1L;
    float kg  = ok ? (float)raw / cfg.calibFactor[z] : 0.0f;
    if (z) Serial.print(',');
    Serial.print(F("{\"zone\":"));   Serial.print(z+1);
    Serial.print(F(",\"sensor_ok\":")); Serial.print(ok ? F("true") : F("false"));
    Serial.print(F(",\"raw\":"));    Serial.print(raw);
    Serial.print(F(",\"kg\":"));     Serial.print(FMT(kg));
    Serial.print('}');
  }
  Serial.println(F("]}"));
  if (trainState == IN_SERVICE) refreshStrip();
}

// ============================================================
//  Command dispatcher
//  Parses rxBuf with lightweight jStr/jInt/jFlt helpers.
//  Zero heap allocation.
// ============================================================

void handleCommand(const char* buf) {
  char cmd[24] = "";
  if (!jStr(buf, "cmd", cmd, sizeof(cmd))) {
    Serial.println(F("{\"type\":\"ACK\",\"status\":\"error\",\"msg\":\"missing cmd\"}"));
    return;
  }

  // ── Diagnostics ───────────────────────────────────────
  if (strcmp_P(cmd, PSTR("PING"))       == 0) { sendPong();   return; }
  if (strcmp_P(cmd, PSTR("GET_UPTIME")) == 0) { sendUptime(); return; }
  if (strcmp_P(cmd, PSTR("GET_STATUS")) == 0) { sendStatus(); return; }
  if (strcmp_P(cmd, PSTR("GET_CONFIG")) == 0) { sendConfig(); return; }
  if (strcmp_P(cmd, PSTR("GET_RAW"))    == 0) { sendRaw();    return; }
  if (strcmp_P(cmd, PSTR("SELFTEST"))   == 0) { doSelftest(); return; }

  if (strcmp_P(cmd, PSTR("GET_WEIGHT")) == 0) {
    int z = 0;
    (jInt(buf,"zone",&z) && z>=1 && z<=3) ? sendWeightOne(z-1) : sendWeightAll();
    return;
  }

  // ── LED control ───────────────────────────────────────
  if (strcmp_P(cmd, PSTR("SET_ZONE")) == 0) {
    int z=0, r=0, g=0, b=0; char lbl[16]="unknown";
    if (!jInt(buf,"zone",&z)||z<1||z>3)               { sendAck(cmd,false,"invalid zone");  return; }
    if (!jInt(buf,"r",&r)||!jInt(buf,"g",&g)||!jInt(buf,"b",&b)) { sendAck(cmd,false,"missing r/g/b"); return; }
    z--;
    zones[z].r = constrain(r,0,255);
    zones[z].g = constrain(g,0,255);
    zones[z].b = constrain(b,0,255);
    jStr(buf,"label",lbl,sizeof(lbl));
    strncpy(zones[z].label, lbl, sizeof(zones[z].label)-1);
    if (trainState==IN_SERVICE||trainState==EXPECTED) { setZoneLeds(z); strip.show(); }
    sendAck(cmd, true);
    return;
  }

  if (strcmp_P(cmd, PSTR("SET_ALL")) == 0) {
    parseAndApplyZones(buf);
    if (trainState==IN_SERVICE||trainState==EXPECTED) refreshStrip();
    sendAck(cmd, true);
    return;
  }

  // ── Configuration ─────────────────────────────────────
  if (strcmp_P(cmd, PSTR("SET_BRIGHTNESS")) == 0) {
    int v=0;
    if (!jInt(buf,"value",&v))  { sendAck(cmd,false,"missing value"); return; }
    cfg.brightness = constrain(v,0,255);
    strip.setBrightness(cfg.brightness); strip.show();
    sendAck(cmd, true); return;
  }

  if (strcmp_P(cmd, PSTR("SET_CALIB")) == 0) {
    int z=0; float f=0;
    if (!jInt(buf,"zone",&z)||z<1||z>3) { sendAck(cmd,false,"invalid zone");   return; }
    if (!jFlt(buf,"factor",&f))         { sendAck(cmd,false,"missing factor"); return; }
    cfg.calibFactor[z-1]=f; scale[z-1].set_scale(f);
    sendAck(cmd, true); return;
  }

  if (strcmp_P(cmd, PSTR("SET_MAX_KG")) == 0) {
    int z=0; float v=0;
    if (!jInt(buf,"zone",&z)||z<1||z>3) { sendAck(cmd,false,"invalid zone"); return; }
    if (!jFlt(buf,"kg",&v))             { sendAck(cmd,false,"missing kg");   return; }
    cfg.maxKg[z-1]=v; sendAck(cmd, true); return;
  }

  if (strcmp_P(cmd, PSTR("SET_THRESHOLD")) == 0) {
    float v=0;
    if (!jFlt(buf,"kg",&v)) { sendAck(cmd,false,"missing kg"); return; }
    cfg.weightThreshold=v; sendAck(cmd, true); return;
  }

  if (strcmp_P(cmd, PSTR("SET_PUSH_INTERVAL")) == 0) {
    int v=0;
    if (!jInt(buf,"ms",&v)) { sendAck(cmd,false,"missing ms"); return; }
    cfg.pushIntervalMs=(uint16_t)constrain(v,0,60000);
    sendAck(cmd, true); return;
  }

  if (strcmp_P(cmd, PSTR("PUSH_ON"))  == 0) { cfg.pushIntervalMs=2000; sendAck(cmd,true); return; }
  if (strcmp_P(cmd, PSTR("PUSH_OFF")) == 0) { cfg.pushIntervalMs=0;    sendAck(cmd,true); return; }

  if (strcmp_P(cmd, PSTR("SET_CAR_ID")) == 0) {
    char id[8]="";
    if (!jStr(buf,"id",id,sizeof(id))||strlen(id)==0) { sendAck(cmd,false,"missing id"); return; }
    strncpy(cfg.carId,id,sizeof(cfg.carId)-1); sendAck(cmd,true); return;
  }

  if (strcmp_P(cmd, PSTR("TARE")) == 0) {
    int z=0;
    if (jInt(buf,"zone",&z)&&z>=1&&z<=3) { scale[z-1].tare(); lastReportedKg[z-1]=0; }
    else { for (uint8_t i=0;i<3;i++){scale[i].tare();lastReportedKg[i]=0;} }
    sendAck(cmd,true); return;
  }

  if (strcmp_P(cmd, PSTR("SAVE_CONFIG")) == 0) { saveConfig(); sendAck(cmd,true); return; }

  if (strcmp_P(cmd, PSTR("LOAD_CONFIG")) == 0) {
    loadConfig();
    for (uint8_t z=0;z<3;z++) scale[z].set_scale(cfg.calibFactor[z]);
    strip.setBrightness(cfg.brightness);
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

  if (strcmp_P(cmd, PSTR("TRAIN_EXPECTED_CLEAR")) == 0) {
    trainState=NO_TRAIN; breathVal=0; breathDir=1;
    strip.clear(); strip.show();
    sendAck(cmd,true); return;
  }

  if (strcmp_P(cmd, PSTR("TRAIN_ARRIVING")) == 0) {
    char dir[4]="ltr"; jStr(buf,"dir",dir,sizeof(dir));
    int8_t d=(strcmp(dir,"rtl")==0)?-1:1;
    trainState=ARRIVING; sendAck(cmd,true);
    animArriving(d);
    trainState=IN_SERVICE; sendEvent("TRAIN_IN_SERVICE");
    return;
  }

  if (strcmp_P(cmd, PSTR("TRAIN_BOARDING"))      == 0) { animBoarding();     sendAck(cmd,true); return; }
  if (strcmp_P(cmd, PSTR("TRAIN_DOORS_CLOSING")) == 0) { animDoorsClosing(); sendAck(cmd,true); return; }

  if (strcmp_P(cmd, PSTR("TRAIN_LEAVING")) == 0) {
    char dir[4]="ltr"; jStr(buf,"dir",dir,sizeof(dir));
    int8_t d=(strcmp(dir,"rtl")==0)?-1:1;
    trainState=LEAVING; sendAck(cmd,true);
    animLeaving(d);
    trainState=NO_TRAIN; breathVal=0; breathDir=1;
    sendEvent("TRAIN_GONE"); return;
  }

  sendAck(cmd, false, "unknown command");
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

  animBoot();
  trainState=NO_TRAIN; breathVal=0; breathDir=1;

  for (uint8_t z=0; z<3; z++) {
    scale[z].begin(HX_DOUT[z], HX_SCK[z]);
    scale[z].set_scale(cfg.calibFactor[z]);
    scale[z].tare();
  }

  sendEvent("BOOT", "SMS ready");
}

void loop() {
  // ── Serial receive ──────────────────────────────────
  while (Serial.available()) {
    char c=(char)Serial.read();
    if (c=='\n'||c=='\r') {
      if (rxPos>0) { rxBuf[rxPos]='\0'; handleCommand(rxBuf); rxPos=0; }
    } else if (rxPos < BUF_SIZE-1) {
      rxBuf[rxPos++]=c;
    }
  }

  // ── Idle breath ─────────────────────────────────────
  updateBreath();

  // ── Weight-change events ────────────────────────────
  static unsigned long lastChangeCheck=0;
  unsigned long now=millis();
  if (now-lastChangeCheck>500 && trainState==IN_SERVICE) {
    lastChangeCheck=now;
    for (uint8_t z=0; z<3; z++) {
      if (!scale[z].is_ready()) continue;
      long  raw=readRaw(z);          // single read
      float kg=rawToKg(z, raw);
      if (abs(kg-lastReportedKg[z]) >= cfg.weightThreshold) {
        lastReportedKg[z]=kg;
        sendWeightChangeEvent(z, kg);
      }
    }
  }

  // ── Periodic push ───────────────────────────────────
  static unsigned long lastPush=0;
  if (cfg.pushIntervalMs>0 && trainState==IN_SERVICE) {
    if (now-lastPush>=cfg.pushIntervalMs) { lastPush=now; sendWeightAll(); }
  }
}
