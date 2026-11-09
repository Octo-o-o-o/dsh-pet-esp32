// dsh-pet for M5Stack Dial v1.1
//
// Plays the DSH desktop-pet (dsh-pet) animations on the 240x240 round display.
// - Auto mode: idle loop with random "fun" interruptions.
// - Knob: one click = next animation; fast spin keeps switching; reverse goes back.
// - Button / touch: random click reaction; long press: sleep toggle.
// - USB serial: mirrors DSH work status from the dsh-pet host plugin and shows
//   that plugin's own status lines (pre-rendered, so no CJK font engine needed).
//
// Assets live in a raw data partition ("assets") as a packed JPEG frame blob,
// memory-mapped at boot so frames are decoded straight out of flash. The ring
// and the text are composited into the same sprite before the push, so nothing
// flickers at 10 fps.

#include <M5Dial.h>
#include <JPEGDEC.h>
#include <esp_partition.h>

static const int SCR = 240;

// ---------------------------------------------------------------- asset format
#pragma pack(push, 1)
struct PetHeader {
  char magic[4];
  uint16_t version;
  uint16_t animCount;
  uint32_t frameCount;
  uint32_t animOff;
  uint32_t frameOff;
  uint32_t dataOff;
  uint32_t textOff;
  uint16_t textCount;
  uint8_t pad[10];
};
struct PetAnim {
  char label[32];
  uint8_t role;
  uint8_t loop;
  uint8_t fps;
  uint8_t pad;
  uint16_t first;
  uint16_t count;
  uint8_t reserved[8];
};
struct PetFrame {
  uint32_t offset;
  uint16_t size;
  uint16_t duration;
};
struct PetText {
  uint16_t id;
  uint16_t w;
  uint16_t h;
  uint16_t pad;
  uint32_t offset;
};
#pragma pack(pop)

enum Role : uint8_t {
  R_IDLE = 0, R_FUN, R_CLICK, R_SLEEP_START, R_SLEEP_LOOP, R_SLEEP_END,
  R_THINKING, R_WORKING, R_RESULT, R_WAITING, R_SUCCESS, R_ERROR, R_COUNT
};

// text ids shared with tools/build_assets.py
static const uint16_t ID_ANIM = 100;
static const uint16_t ID_MODE_AUTO = 200, ID_MODE_MANUAL = 201, ID_MODE_SLEEP = 202, ID_MODE_DSH = 203;
static const uint16_t ID_PHRASE = 300;

static M5Canvas canvas(&M5Dial.Display);
static JPEGDEC jpeg;

static const PetHeader* hdr = nullptr;
static const PetAnim* anims = nullptr;
static const PetFrame* frames = nullptr;
static const PetText* texts = nullptr;
static const uint8_t* blob = nullptr;
static bool assetsOk = false;

// ---------------------------------------------------------------- playback state
static int baseAnim = -1;       // looping animation we return to
static int curAnim = -1;
static int curFrame = 0;
static uint32_t nextFrameAt = 0;
static bool overlayActive = false;
static int overlayAnim = -1;
static uint32_t nextAutoMs = 0;
static uint32_t manualUntil = 0;

enum DshState { DS_NONE = 0, DS_IDLE, DS_THINKING, DS_WORKING, DS_RESULT, DS_WAITING, DS_SUCCESS, DS_ERROR };
static DshState dshState = DS_NONE;
static uint16_t phraseId = 0;
static uint32_t lastSerialMs = 0;
static bool sleeping = false;
static int sleepPhase = 0;      // 0 none, 1 falling asleep, 2 sleeping, 3 waking

static int32_t encAccum = 0;
static int32_t encTotal = 0;
static uint32_t lastStepMs = 0;
static uint32_t lastDecodeMs = 0;
static uint32_t framesDrawn = 0;
static char serialLine[128];
static size_t serialLen = 0;

// ---------------------------------------------------------------- helpers
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return lgfx::color565(r, g, b); }
// the 16bpp sprite stores RGB565 byte-swapped (panel order)
static inline uint16_t sw(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static int findRole(uint8_t role) {
  for (int i = 0; i < (int)hdr->animCount; i++)
    if (anims[i].role == role) return i;
  return -1;
}

static int animIndexByLabel(const char* name) {
  for (int i = 0; i < (int)hdr->animCount; i++)
    if (strcasecmp(anims[i].label, name) == 0) return i;
  return -1;
}

static const PetText* findText(uint16_t id) {
  for (int i = 0; i < (int)hdr->textCount; i++)
    if (texts[i].id == id) return &texts[i];
  return nullptr;
}

static uint16_t stateColor() {
  switch (dshState) {
    case DS_THINKING: return rgb(200, 120, 255);
    case DS_WORKING:  return rgb(80, 220, 255);
    case DS_RESULT:   return rgb(120, 230, 150);
    case DS_WAITING:  return rgb(255, 170, 60);
    case DS_SUCCESS:  return rgb(190, 240, 120);
    case DS_ERROR:    return rgb(255, 80, 80);
    case DS_IDLE:     return rgb(120, 170, 230);
    default:          return rgb(120, 170, 230);
  }
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
  // pick the shortest variant: it renders at the largest font size
  uint16_t best = ID_PHRASE + base;
  int bestW = 1 << 14;
  for (int i = 0; i < cnt; i++) {
    const PetText* t = findText((uint16_t)(ID_PHRASE + base + i));
    if (t && t->w < bestW) { bestW = t->w; best = (uint16_t)(ID_PHRASE + base + i); }
  }
  phraseId = best;
}

// ---------------------------------------------------------------- rendering
static int drawBlock(JPEGDRAW* d) {
  uint16_t* dst = (uint16_t*)canvas.getBuffer();
  int w = d->iWidth;
  for (int y = 0; y < d->iHeight; y++) {
    int dy = d->y + y;
    int dx = d->x;
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

  // Dim the caption strip instead of outlining every glyph: a 1 px outline turns
  // 10 px CJK strokes into blobs. Halving RGB565 keeps it readable and subtle.
  for (int y = y0 - 3; y < y0 + t->h + 3; y++) {
    if (y < 0 || y >= SCR) continue;
    for (int x = x0 - 6; x < x0 + t->w + 6; x++) {
      if (x < 0 || x >= SCR) continue;
      uint16_t p = fb[y * SCR + x];
      // unswap -> quarter brightness -> swap back (green spans both bytes)
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

static void drawRing(uint16_t c565, bool busy) {
  uint16_t* fb = (uint16_t*)canvas.getBuffer();
  uint16_t c = sw(c565);
  for (int deg = 0; deg < 360; deg++) {
    float a = deg * 0.0174533f;
    for (int rad = 117; rad <= 118; rad++) {
      int x = SCR / 2 + (int)(cosf(a) * rad);
      int y = SCR / 2 + (int)(sinf(a) * rad);
      if (x >= 0 && x < SCR && y >= 0 && y < SCR) fb[y * SCR + x] = c;
    }
  }
  if (busy) {
    float a = (millis() % 2000) / 2000.0f * 6.2831853f;
    int px = SCR / 2 + (int)(cosf(a) * 118);
    int py = SCR / 2 + (int)(sinf(a) * 118);
    for (int dy = -4; dy <= 4; dy++) {
      for (int dx = -4; dx <= 4; dx++) {
        if (dx * dx + dy * dy > 16) continue;
        int qx = px + dx, qy = py + dy;
        if (qx >= 0 && qx < SCR && qy >= 0 && qy < SCR) fb[qy * SCR + qx] = c;
      }
    }
  }
}

// Keep it simple: just the pet and one big caption line. No ring, no mode chip.
static void drawOverlay() {
  if (dshState != DS_NONE && phraseId) {
    blitText(phraseId, SCR / 2, SCR - 46, 0xFFFF);
  } else if (curAnim >= 0) {
    blitText((uint16_t)(ID_ANIM + curAnim), SCR / 2, SCR - 46, 0xFFFF);
  }
}

static void renderFrame(int anim, int f) {
  if (anim < 0 || anim >= (int)hdr->animCount) return;
  const PetAnim& a = anims[anim];
  if (f < 0 || f >= a.count) return;
  const PetFrame& pf = frames[a.first + f];

  uint32_t t0 = millis();
  jpeg.openRAM((uint8_t*)(blob + pf.offset), pf.size, drawBlock);
  jpeg.setPixelType(RGB565_BIG_ENDIAN);   // the sprite/panel expects swapped bytes
  int rc = jpeg.decode(0, 0, 0);
  jpeg.close();
  lastDecodeMs = millis() - t0;
  if (rc) framesDrawn++;

  drawOverlay();
  canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- playback
static void scheduleFrame() {
  if (curAnim < 0) return;
  const PetAnim& a = anims[curAnim];
  uint32_t dur = frames[a.first + curFrame].duration;
  nextFrameAt += dur;                       // absolute schedule: absorb render time
  if ((int32_t)(millis() - nextFrameAt) > 200) nextFrameAt = millis() + dur;  // resync after a stall
}

static void startAnim(int anim) {
  curAnim = anim;
  curFrame = 0;
  renderFrame(curAnim, 0);
  nextFrameAt = millis();   // restart the clock: never inherit the previous animation's schedule
  scheduleFrame();
}

static void playBase(int anim) {
  if (anim < 0) return;
  baseAnim = anim;
  if (overlayActive) return;   // overlay keeps running; base changes underneath
  startAnim(anim);
}

static void playOverlay(int anim) {
  if (anim < 0) return;
  overlayAnim = anim;
  overlayActive = true;
  startAnim(anim);
}

static void onAnimEnd() {
  overlayActive = false;
  overlayAnim = -1;
  if (sleepPhase == 1) { sleepPhase = 2; playBase(findRole(R_SLEEP_LOOP)); return; }
  if (sleepPhase == 3) { sleepPhase = 0; sleeping = false; playBase(findRole(R_IDLE)); return; }
  curAnim = baseAnim >= 0 ? baseAnim : findRole(R_IDLE);
  curFrame = 0;
  if (curAnim >= 0) {
    renderFrame(curAnim, curFrame);
    nextFrameAt = millis();
    scheduleFrame();
  }
  if (dshState == DS_NONE && !sleeping) nextAutoMs = millis() + random(6000, 14000);
}

static void tickPlayback() {
  if (curAnim < 0) return;
  if ((int32_t)(millis() - nextFrameAt) < 0) return;
  const PetAnim& a = anims[curAnim];
  if (curFrame + 1 >= a.count) {
    if (a.loop) {
      curFrame = 0;
    } else {
      onAnimEnd();
      return;
    }
  } else {
    curFrame++;
  }
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
    case DS_IDLE:
    case DS_NONE:     playBase(findRole(R_IDLE)); break;
    default: break;
  }
}

static void playRandomClick() {
  int found[R_COUNT], n = 0;
  for (int i = 0; i < (int)hdr->animCount; i++)
    if (anims[i].role == R_CLICK) found[n++] = i;
  if (n == 0) return;
  playOverlay(found[random(n)]);
}

static void playRandomFun() {
  int found[64], n = 0;
  for (int i = 0; i < (int)hdr->animCount && n < 64; i++)
    if (anims[i].role == R_FUN) found[n++] = i;
  if (n == 0) return;
  playOverlay(found[random(n)]);
}

static void stepManual(int dir) {
  if (!assetsOk) return;
  int n = (int)hdr->animCount;
  int idx = (curAnim >= 0 ? curAnim : 0) + dir;
  idx %= n;
  if (idx < 0) idx += n;
  manualUntil = millis() + 20000;
  playOverlay(idx);
  M5Dial.Speaker.tone(1800, 10);
}

static void toggleSleep() {
  if (sleepPhase == 0) {
    sleepPhase = 1;
    sleeping = true;
    playOverlay(findRole(R_SLEEP_START));
  } else if (sleepPhase == 2) {
    sleepPhase = 3;
    playOverlay(findRole(R_SLEEP_END));
  }
}

// ---------------------------------------------------------------- input
// M5Dial's Encoder::read() returns the absolute position and never clears it —
// readAndReset() is the delta since the previous call. One physical click gives
// 2..4 counts depending on the unit, so step at half a detent and lock out for
// 40 ms: one click = one animation, a fast spin still advances continuously.
static void pollEncoder() {
  int32_t d = M5Dial.Encoder.readAndReset();
  if (d == 0) return;
  if (d > 240 || d < -240) return;   // glitch guard
  encTotal += d > 0 ? d : -d;
  encAccum += d;

  int32_t mag = encAccum > 0 ? encAccum : -encAccum;
  if (mag < 2) return;
  uint32_t now = millis();
  if (now - lastStepMs < 40) { encAccum = 0; return; }   // same click: drop, don't bank it
  lastStepMs = now;
  int dir = encAccum > 0 ? 1 : -1;
  encAccum = 0;
  stepManual(dir);
}

static void pollButton() {
  if (M5Dial.BtnA.wasClicked()) {
    manualUntil = millis() + 20000;
    playRandomClick();
  }
  if (M5Dial.BtnA.wasHold()) toggleSleep();
  auto t = M5Dial.Touch.getDetail();
  if (t.wasClicked()) {
    manualUntil = millis() + 20000;
    playRandomClick();
  }
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
    int idx = -1;
    if (arg[0] >= '0' && arg[0] <= '9') idx = atoi(arg);
    else idx = animIndexByLabel(arg);
    if (idx >= 0) { manualUntil = millis() + 20000; playOverlay(idx); Serial.printf("ACK A %d\n", idx); }
  } else if (cmd == 'P') {
    Serial.println("PONG");
  } else if (cmd == 'E') {
    // test hook: pretend the knob moved by <n> counts
    M5Dial.Encoder.write(atoi(arg));
    Serial.printf("ACK E %s\n", arg);
  } else if (cmd == 'I') {
    Serial.printf("INFO anims=%u frames=%u texts=%u heap=%u cur=%s f=%d dec=%ums drawn=%u dsh=%d enc=%ld\n",
                  hdr->animCount, hdr->frameCount, hdr->textCount, (unsigned)ESP.getFreeHeap(),
                  curAnim >= 0 ? anims[curAnim].label : "?", curFrame,
                  (unsigned)lastDecodeMs, (unsigned)framesDrawn, (int)dshState, (long)encTotal);
  } else if (cmd == 'V') {
    // dump the sprite (every 2nd pixel) as hex so the host can eyeball the render
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
    if (c == '\n') {
      serialLine[serialLen] = 0;
      handleLine(serialLine);
      serialLen = 0;
    } else if (c != '\r' && serialLen < sizeof(serialLine) - 1) {
      serialLine[serialLen++] = (char)c;
    }
  }
  // DSH went quiet -> back to standalone auto mode
  if (dshState != DS_NONE && lastSerialMs && millis() - lastSerialMs > 90000) {
    dshState = DS_NONE;
    phraseId = 0;
    playBase(findRole(R_IDLE));
  }
}

// ---------------------------------------------------------------- setup
static void fillBackground() {
  uint16_t* fb = (uint16_t*)canvas.getBuffer();
  uint16_t c = sw(rgb(10, 14, 26));
  for (int i = 0; i < SCR * SCR; i++) fb[i] = c;
}

static bool mountAssets() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "assets");
  if (!part) {
    Serial.println("ERR no assets partition");
    return false;
  }
  const void* mapped = nullptr;
  esp_partition_mmap_handle_t handle;
  if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped, &handle) != ESP_OK) {
    Serial.println("ERR mmap failed");
    return false;
  }
  blob = (const uint8_t*)mapped;
  hdr = (const PetHeader*)blob;
  if (memcmp(hdr->magic, "PETD", 4) != 0 || hdr->version < 2) {
    Serial.println("ERR bad blob (need v2)");
    return false;
  }
  anims = (const PetAnim*)(blob + hdr->animOff);
  frames = (const PetFrame*)(blob + hdr->frameOff);
  texts = (const PetText*)(blob + hdr->textOff);
  Serial.printf("assets ok: %u anims, %u frames, %u texts\n",
                hdr->animCount, hdr->frameCount, hdr->textCount);
  return true;
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);
  M5Dial.Display.setRotation(0);
  M5Dial.Display.setBrightness(120);
  Serial.begin(115200);

  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCR, SCR)) {
    Serial.println("ERR canvas alloc");
  }
  fillBackground();
  canvas.pushSprite(0, 0);

  assetsOk = mountAssets();
  if (!assetsOk) {
    Serial.println("ERR assets");
    return;
  }

  baseAnim = findRole(R_IDLE);
  curAnim = baseAnim;
  curFrame = 0;
  renderFrame(curAnim, 0);
  nextFrameAt = millis();
  scheduleFrame();
  nextAutoMs = millis() + 8000;
  Serial.println("READY dsh-pet-dial v2");
}

void loop() {
  M5Dial.update();
  if (!assetsOk) { delay(500); return; }

  pollEncoder();
  pollButton();
  pollSerial();

  if (!overlayActive && !sleeping && dshState == DS_NONE &&
      (int32_t)(millis() - nextAutoMs) >= 0) {
    playRandomFun();
    nextAutoMs = millis() + random(8000, 16000);
  }

  tickPlayback();
  delay(2);
}
