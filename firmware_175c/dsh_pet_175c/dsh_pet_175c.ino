// dsh-pet for Waveshare ESP32-S3-Touch-AMOLED-1.75C
//
// Same animation blob and serial protocol as the M5Stack Dial build, but the
// panel is a 466x466 CO5300 over QSPI and the only input is the CST9217 touch
// panel, so gestures replace the knob:
//   tap            -> next animation
//   double tap     -> previous animation
//   swipe left/right -> next / previous
//   long press     -> sleep / wake

#define LGFX_USE_QSPI
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_CO5300.hpp>
#include <JPEGDEC.h>
#include <Wire.h>
#include <esp_partition.h>

static const int SCR = 466;

// ---------------------------------------------------------------- panel
class Panel_175C : public lgfx::Panel_CO5300 {
 public:
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
        0xFE, 1, 0x20,
        0x19, 1, 0x10,
        0x1C, 1, 0xA0,
        0xFE, 1, 0x00,
        0xC4, 1, 0x80,
        0x3A, 1, 0x55,   // 16bpp
        0x35, 1, 0x00,
        0x53, 1, 0x20,
        0x51, 1, 0xFF,
        0x63, 1, 0xFF,
        0x2A, 4, 0x00, 0x06, 0x01, 0xD7,
        0x2B, 4, 0x00, 0x00, 0x01, 0xD1,
        0x11, 0, 120,
        0x29, 0, 0,
        0xFF, 0xFF,
    };
    return (listno == 0) ? list0 : nullptr;
  }
};

class LGFX_175C : public lgfx::LGFX_Device {
  Panel_175C _panel;
  lgfx::Bus_SPI _bus;
 public:
  LGFX_175C() {
    auto b = _bus.config();
    b.spi_host = SPI2_HOST;
    b.spi_mode = 0;
    b.freq_write = 80000000;
    b.freq_read = 16000000;
    b.spi_3wire = false;
    b.use_lock = true;
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = 38;
    b.pin_io0 = 4;
    b.pin_io1 = 5;
    b.pin_io2 = 6;
    b.pin_io3 = 7;
    _bus.config(b);
    _panel.setBus(&_bus);

    auto p = _panel.config();
    p.pin_cs = 12;
    p.pin_rst = 1;
    p.pin_busy = -1;
    p.panel_width = 466;
    p.panel_height = 466;
    p.memory_width = 480;
    p.memory_height = 466;
    p.offset_x = 6;
    p.offset_y = 0;
    p.readable = false;
    p.invert = false;
    p.rgb_order = false;
    _panel.config(p);
    setPanel(&_panel);
  }
};

static LGFX_175C lcd;
static LGFX_Sprite canvas(&lcd);
static JPEGDEC jpeg;

// ---------------------------------------------------------------- asset format
#pragma pack(push, 1)
struct PetHeader {
  char magic[4];
  uint16_t version, animCount;
  uint32_t frameCount, animOff, frameOff, dataOff, textOff;
  uint16_t textCount;
  uint8_t pad[10];
};
struct PetAnim { char label[32]; uint8_t role, loop, fps, pad; uint16_t first, count; uint8_t reserved[8]; };
struct PetFrame { uint32_t offset; uint16_t size, duration; };
struct PetText { uint16_t id, w, h, pad; uint32_t offset; };
#pragma pack(pop)

enum Role : uint8_t {
  R_IDLE = 0, R_FUN, R_CLICK, R_SLEEP_START, R_SLEEP_LOOP, R_SLEEP_END,
  R_THINKING, R_WORKING, R_RESULT, R_WAITING, R_SUCCESS, R_ERROR, R_COUNT
};
static const uint16_t ID_ANIM = 100, ID_PHRASE = 300;

static const PetHeader* hdr = nullptr;
static const PetAnim* anims = nullptr;
static const PetFrame* frames = nullptr;
static const PetText* texts = nullptr;
static const uint8_t* blob = nullptr;
static bool assetsOk = false;

// ---------------------------------------------------------------- state
static int baseAnim = -1, curAnim = -1, curFrame = 0;
static uint32_t nextFrameAt = 0, nextAutoMs = 0, manualUntil = 0;
static bool overlayActive = false;
enum DshState { DS_NONE = 0, DS_IDLE, DS_THINKING, DS_WORKING, DS_RESULT, DS_WAITING, DS_SUCCESS, DS_ERROR };
static DshState dshState = DS_NONE;
static uint16_t phraseId = 0;
static uint32_t lastSerialMs = 0;
static bool sleeping = false;
static int sleepPhase = 0;
static uint32_t framesDrawn = 0;
static char serialLine[128];
static size_t serialLen = 0;

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return lgfx::color565(r, g, b); }
static inline uint16_t sw(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static int findRole(uint8_t role) {
  for (int i = 0; i < (int)hdr->animCount; i++) if (anims[i].role == role) return i;
  return -1;
}
static int animIndexByLabel(const char* name) {
  for (int i = 0; i < (int)hdr->animCount; i++) if (strcasecmp(anims[i].label, name) == 0) return i;
  return -1;
}
static const PetText* findText(uint16_t id) {
  for (int i = 0; i < (int)hdr->textCount; i++) if (texts[i].id == id) return &texts[i];
  return nullptr;
}

static void pickPhrase(DshState s) {
  int base = -1, cnt = 0;
  switch (s) {
    case DS_THINKING: base = 0;  cnt = 3; break;
    case DS_WORKING:  base = 3;  cnt = 3; break;
    case DS_RESULT:   base = 6;  cnt = 2; break;
    case DS_WAITING:  base = 8;  cnt = 3; break;
    case DS_SUCCESS:  base = 11; cnt = 2; break;
    case DS_ERROR:    base = 13; cnt = 3; break;
    default: phraseId = 0; return;
  }
  uint16_t best = ID_PHRASE + base;
  int bestW = 1 << 14;
  for (int i = 0; i < cnt; i++) {
    const PetText* t = findText((uint16_t)(ID_PHRASE + base + i));
    if (t && t->w < bestW) { bestW = t->w; best = (uint16_t)(ID_PHRASE + base + i); }
  }
  phraseId = best;
}

// ---------------------------------------------------------------- render
static int drawBlock(JPEGDRAW* d) {
  uint16_t* dst = (uint16_t*)canvas.getBuffer();
  int w = d->iWidth;
  for (int y = 0; y < d->iHeight; y++) {
    int dy = d->y + y, dx = d->x;
    if (dy < 0 || dy >= SCR || dx < 0 || dx + w > SCR) continue;
    memcpy(dst + dy * SCR + dx, d->pPixels + y * w, w * 2);
  }
  return 1;
}

static void blitText(uint16_t id, int cx, int cy, uint16_t fg) {
  const PetText* t = findText(id);
  if (!t) return;
  int rb = (t->w + 7) / 8;
  int x0 = cx - t->w / 2, y0 = cy - t->h / 2;
  uint16_t* fb = (uint16_t*)canvas.getBuffer();
  for (int y = y0 - 5; y < y0 + t->h + 5; y++) {
    if (y < 0 || y >= SCR) continue;
    for (int x = x0 - 10; x < x0 + t->w + 10; x++) {
      if (x < 0 || x >= SCR) continue;
      uint16_t p = fb[y * SCR + x];
      uint16_t v = sw(p);
      v = (uint16_t)((v >> 1) & 0x7BEF);
      v = (uint16_t)((v >> 1) & 0x7BEF);
      fb[y * SCR + x] = sw(v);
    }
  }
  uint16_t f = sw(fg);
  for (int y = 0; y < t->h; y++) {
    const uint8_t* row = blob + t->offset + y * rb;
    for (int x = 0; x < t->w; x++) {
      if (!(row[x >> 3] & (0x80 >> (x & 7)))) continue;
      int px = x0 + x, py = y0 + y;
      if (px >= 0 && px < SCR && py >= 0 && py < SCR) fb[py * SCR + px] = f;
    }
  }
}

static void renderFrame(int anim, int f) {
  if (anim < 0 || anim >= (int)hdr->animCount) return;
  const PetAnim& a = anims[anim];
  if (f < 0 || f >= a.count) return;
  const PetFrame& pf = frames[a.first + f];
  jpeg.openRAM((uint8_t*)(blob + pf.offset), pf.size, drawBlock);
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
  if (jpeg.decode(0, 0, 0)) framesDrawn++;
  jpeg.close();

  if (dshState != DS_NONE && phraseId) blitText(phraseId, SCR / 2, SCR - 64, 0xFFFF);
  else if (curAnim >= 0) blitText((uint16_t)(ID_ANIM + curAnim), SCR / 2, SCR - 64, 0xFFFF);
  canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- playback
static void scheduleFrame() {
  if (curAnim < 0) return;
  const PetAnim& a = anims[curAnim];
  nextFrameAt += frames[a.first + curFrame].duration;
  if ((int32_t)(millis() - nextFrameAt) > 250) nextFrameAt = millis() + frames[a.first + curFrame].duration;
}
static void startAnim(int anim) {
  curAnim = anim; curFrame = 0;
  renderFrame(curAnim, 0);
  nextFrameAt = millis();
  scheduleFrame();
}
static void playBase(int anim) {
  if (anim < 0) return;
  baseAnim = anim;
  if (!overlayActive) startAnim(anim);
}
static void playOverlay(int anim) {
  if (anim < 0) return;
  overlayActive = true;
  startAnim(anim);
}
static void onAnimEnd() {
  overlayActive = false;
  if (sleepPhase == 1) { sleepPhase = 2; playBase(findRole(R_SLEEP_LOOP)); return; }
  if (sleepPhase == 3) { sleepPhase = 0; sleeping = false; playBase(findRole(R_IDLE)); return; }
  curAnim = baseAnim >= 0 ? baseAnim : findRole(R_IDLE);
  curFrame = 0;
  if (curAnim >= 0) { renderFrame(curAnim, 0); nextFrameAt = millis(); scheduleFrame(); }
  if (dshState == DS_NONE && !sleeping) nextAutoMs = millis() + random(6000, 14000);
}
static void tickPlayback() {
  if (curAnim < 0 || (int32_t)(millis() - nextFrameAt) < 0) return;
  const PetAnim& a = anims[curAnim];
  if (curFrame + 1 >= a.count) {
    if (a.loop) curFrame = 0; else { onAnimEnd(); return; }
  } else curFrame++;
  renderFrame(curAnim, curFrame);
  scheduleFrame();
}

// ---------------------------------------------------------------- modes
static void setDshState(DshState s) {
  if (s == dshState) return;
  dshState = s;
  pickPhrase(s);
  switch (s) {
    case DS_THINKING: playBase(findRole(R_THINKING)); break;
    case DS_WORKING:  playBase(findRole(R_WORKING)); break;
    case DS_RESULT:   playBase(findRole(R_RESULT)); break;
    case DS_WAITING:  playBase(findRole(R_WAITING)); break;
    case DS_SUCCESS:  playOverlay(findRole(R_SUCCESS)); break;
    case DS_ERROR:    playOverlay(findRole(R_ERROR)); break;
    default:          playBase(findRole(R_IDLE)); break;
  }
}
static void playRandomClick() {
  int f[R_COUNT], n = 0;
  for (int i = 0; i < (int)hdr->animCount; i++) if (anims[i].role == R_CLICK) f[n++] = i;
  if (n) playOverlay(f[random(n)]);
}
static void playRandomFun() {
  int f[64], n = 0;
  for (int i = 0; i < (int)hdr->animCount && n < 64; i++) if (anims[i].role == R_FUN) f[n++] = i;
  if (n) playOverlay(f[random(n)]);
}
static void stepManual(int dir) {
  if (!assetsOk) return;
  int n = (int)hdr->animCount;
  int idx = ((curAnim >= 0 ? curAnim : 0) + dir) % n;
  if (idx < 0) idx += n;
  manualUntil = millis() + 20000;
  playOverlay(idx);
}
static void toggleSleep() {
  if (sleepPhase == 0) { sleepPhase = 1; sleeping = true; playOverlay(findRole(R_SLEEP_START)); }
  else if (sleepPhase == 2) { sleepPhase = 3; playOverlay(findRole(R_SLEEP_END)); }
}

// ---------------------------------------------------------------- touch (CST9217)
static const uint8_t TOUCH_ADDR = 0x5A;
static bool touchInit() {
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW); delay(10); digitalWrite(2, HIGH); delay(60);
  Wire.begin(15, 14, 400000);
  Wire.beginTransmission(TOUCH_ADDR);
  return Wire.endTransmission() == 0;
}
// returns 0 none, 1 point
static bool touchRead(int* px, int* py) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 1) != 1) return false;
  uint8_t n = Wire.read() & 0x0F;
  if (!n) return false;
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x03);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 5) < 5) return false;
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  Wire.read();
  *px = ((xh & 0x0F) << 8) | xl;
  *py = ((yh & 0x0F) << 8) | yl;
  return true;
}

struct TouchState {
  bool down = false;
  uint32_t downMs = 0, lastTapMs = 0;
  int startX = 0, startY = 0, x = 0, y = 0;
};
static TouchState ts;

static void handleGesture(bool down, int x, int y) {
  uint32_t now = millis();
  if (down && !ts.down) {
    ts.down = true; ts.downMs = now; ts.startX = x; ts.startY = y;
  } else if (!down && ts.down) {
    ts.down = false;
    uint32_t held = now - ts.downMs;
    int dx = x - ts.startX;
    if (held > 700) {
      toggleSleep();
      return;
    }
    if (abs(dx) > 60) {
      stepManual(dx < 0 ? +1 : -1);
      return;
    }
    if (held < 400) {
      if (now - ts.lastTapMs < 350) {
        ts.lastTapMs = 0;
        stepManual(-1);          // double tap = previous
      } else {
        ts.lastTapMs = now;
        stepManual(+1);          // single tap = next
      }
    }
  }
  ts.x = x; ts.y = y;
}

static void pollTouch() {
  int x, y;
  bool down = touchRead(&x, &y);
  if (down) {
    x = x * SCR / 466;
    y = y * SCR / 466;
  }
  handleGesture(down, x, y);
}

// ---------------------------------------------------------------- serial
static DshState stateFromString(const char* v) {
  if (!strcasecmp(v, "thinking")) return DS_THINKING;
  if (!strcasecmp(v, "working")) return DS_WORKING;
  if (!strcasecmp(v, "result")) return DS_RESULT;
  if (!strcasecmp(v, "waiting")) return DS_WAITING;
  if (!strcasecmp(v, "success")) return DS_SUCCESS;
  if (!strcasecmp(v, "error")) return DS_ERROR;
  if (!strcasecmp(v, "idle")) return DS_IDLE;
  return DS_NONE;
}
static void handleLine(char* line) {
  while (*line == ' ') line++;
  if (!*line) return;
  lastSerialMs = millis();
  char cmd = line[0];
  char* arg = line + 1;
  while (*arg == ' ') arg++;
  if (cmd == 'S') {
    DshState s = stateFromString(arg);
    if (s == DS_NONE) { dshState = DS_NONE; phraseId = 0; playBase(findRole(R_IDLE)); }
    else setDshState(s);
    Serial.printf("ACK S %s\n", arg);
  } else if (cmd == 'A') {
    int idx = (arg[0] >= '0' && arg[0] <= '9') ? atoi(arg) : animIndexByLabel(arg);
    if (idx >= 0) { manualUntil = millis() + 20000; playOverlay(idx); Serial.printf("ACK A %d\n", idx); }
  } else if (cmd == 'P') {
    Serial.println("PONG");
  } else if (cmd == 'I') {
    Serial.printf("INFO anims=%u frames=%u texts=%u heap=%u cur=%s f=%d drawn=%u dsh=%d\n",
                  hdr->animCount, hdr->frameCount, hdr->textCount, (unsigned)ESP.getFreeHeap(),
                  curAnim >= 0 ? anims[curAnim].label : "?", curFrame,
                  (unsigned)framesDrawn, (int)dshState);
  } else if (cmd == 'V') {
    uint16_t* buf = (uint16_t*)canvas.getBuffer();
    Serial.printf("SHOT %d\n", SCR / 2);
    char line[8 + (SCR / 2) * 4 + 4];
    for (int y = 0; y < SCR; y += 2) {
      int n = snprintf(line, sizeof(line), "P%03d ", y);
      for (int x = 0; x < SCR; x += 2) n += snprintf(line + n, sizeof(line) - n, "%04x", buf[y * SCR + x]);
      Serial.println(line);
    }
    Serial.println("END");
  }
}
static void pollSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n') { serialLine[serialLen] = 0; handleLine(serialLine); serialLen = 0; }
    else if (c != '\r' && serialLen < sizeof(serialLine) - 1) serialLine[serialLen++] = (char)c;
  }
  if (dshState != DS_NONE && lastSerialMs && millis() - lastSerialMs > 90000) {
    dshState = DS_NONE; phraseId = 0; playBase(findRole(R_IDLE));
  }
}

// ---------------------------------------------------------------- setup
static bool mountAssets() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "assets");
  if (!part) { Serial.println("ERR no assets partition"); return false; }
  const void* mapped = nullptr;
  esp_partition_mmap_handle_t h;
  if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped, &h) != ESP_OK) {
    Serial.println("ERR mmap"); return false;
  }
  blob = (const uint8_t*)mapped;
  hdr = (const PetHeader*)blob;
  if (memcmp(hdr->magic, "PETD", 4) != 0 || hdr->version < 2) { Serial.println("ERR blob"); return false; }
  anims = (const PetAnim*)(blob + hdr->animOff);
  frames = (const PetFrame*)(blob + hdr->frameOff);
  texts = (const PetText*)(blob + hdr->textOff);
  Serial.printf("assets ok: %u anims %u frames %u texts\n", hdr->animCount, hdr->frameCount, hdr->textCount);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("STAGE serial");
  lcd.init();
  Serial.println("STAGE lcd");
  lcd.setRotation(0);
  lcd.setBrightness(200);

  Serial.println("STAGE canvas");
  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCR, SCR)) Serial.println("ERR sprite");
  canvas.fillScreen(0x0821);
  canvas.pushSprite(0, 0);

  Serial.println("STAGE assets");
  assetsOk = mountAssets();
  bool touchOk = touchInit();
  Serial.printf("touch %s\n", touchOk ? "ok" : "missing");
  if (!assetsOk) return;

  baseAnim = findRole(R_IDLE);
  curAnim = baseAnim;
  renderFrame(curAnim, 0);
  nextFrameAt = millis();
  scheduleFrame();
  nextAutoMs = millis() + 8000;
  Serial.println("READY dsh-pet-175c v1");
}

void loop() {
  if (!assetsOk) { delay(500); return; }
  pollTouch();
  pollSerial();
  if (!overlayActive && !sleeping && dshState == DS_NONE && (int32_t)(millis() - nextAutoMs) >= 0) {
    playRandomFun();
    nextAutoMs = millis() + random(8000, 16000);
  }
  tickPlayback();
  delay(2);
}
