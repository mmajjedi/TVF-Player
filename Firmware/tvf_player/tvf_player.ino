/*
 * tvf_player.ino - a pocket TV.
 *
 * Two places the video can live - pick one with VIDEO_SOURCE below:
 *
 *   SOURCE_FLASH   one clip in the ESP32's own flash (LittleFS). No extra
 *                  parts, about 15-20 seconds max. Plays and loops.
 *   SOURCE_SDCARD  any number of clips on a microSD card in an SD card
 *                  module, each limited only by the card (FAT32 caps one file
 *                  at 4 GB - hours at 240x136). A menu lists them; three
 *                  buttons scroll and pick. A card with a single clip skips
 *                  the menu and just plays it, so the buttons are optional.
 *
 * Plays .tvf clips on a GMT130 (ST7789 240x240) panel while an
 * MP3-TF-16P / DFPlayer Mini plays the matching audio from its own microSD.
 * With SOURCE_SDCARD that's two cards: video in the SD module, sound in the
 * DFPlayer. Each .tvf records which DFPlayer folder holds its sound (the
 * packer's --slot), so the menu can pair them without any config here.
 *
 * Board      ESP32-S V1.1 / ESP32 Dev Module (WROOM-32)
 * Libraries  Adafruit GFX, Adafruit ST7735/ST7789, TJpg_Decoder,
 *            DFRobotDFPlayerMini   (LittleFS, SD and SPI come with the core)
 *
 * SOURCE_FLASH
 *   Pack       python tools/tvfpack.py in.mp4 out --mode mjpeg --width 240
 *                --height 136 --fps 15 -q 70 --audio
 *   Partition  Tools > Partition Scheme > "No OTA (2MB APP / 2MB SPIFFS)"
 *   Data       put clip.tvf in a "data" folder next to this .ino and upload it
 *              with the LittleFS uploader (Ctrl+Shift+P > Upload LittleFS Data)
 *              Close the Serial Monitor before either upload - it holds the
 *              port and the upload will fail.
 * SOURCE_SDCARD
 *   Pack       same, plus  --sdcard --slot N  with a different N (1-99) for
 *              every clip - N is the DFPlayer folder its sound goes in
 *   Partition  anything - the default is fine
 *   Data       copy the .tvf files to the root of the SD module's card, and
 *              the numbered audio folders to the root of the DFPlayer's card.
 *              Both FAT32. No LittleFS upload step.
 *
 * ---------------------------- WIRING ----------------------------
 *   GMT130 panel        ESP32
 *     VCC        ---->  3V3          not 5V
 *     GND        ---->  GND
 *     SDA        ---->  GPIO23       hardware SPI MOSI, fixed
 *     SCK        ---->  GPIO18       hardware SPI clock, fixed
 *     RES        ---->  GPIO17
 *     DC         ---->  GPIO15
 *     BLK        ---->  3V3          constant backlight, no flicker on camera
 *                                    (this panel has no CS pin -> TFT_CS = -1)
 *
 *   SD card module      ESP32        SOURCE_SDCARD only. Its OWN SPI bus
 *     VCC        ---->  VIN / 5V     if the module has a regulator (the common
 *                                    blue one does); 3V3 if it has none
 *     GND        ---->  GND
 *     SCK        ---->  GPIO14
 *     MOSI       ---->  GPIO13
 *     MISO       ---->  GPIO19
 *     CS         ---->  GPIO4
 *   The panel has no CS pin, so it would read SD traffic as pixels if the two
 *   shared GPIO18/23 - keep them on separate buses. Avoid GPIO12 for MISO: it
 *   is a boot strapping pin and an SD card on it can stop the ESP32 booting.
 *
 *   Buttons             ESP32        SOURCE_SDCARD only. Each one between the
 *     UP         ---->  GPIO32       pin and GND - no resistors, the internal
 *     DOWN       ---->  GPIO33       pull-ups are used.
 *     OK         ---->  GPIO25       menu: UP/DOWN scroll, OK plays
 *                                    playing: UP/DOWN volume, OK back to menu
 *
 *   MP3-TF-16P          ESP32        pin 1 is at the notch, SD slot facing you
 *     VCC  (1)   ---->  VIN / 5V     on 3V3 the amp is quiet and hissy
 *     RX   (2)   ---->  GPIO27       THROUGH A 1k RESISTOR
 *     TX   (3)   ---->  GPIO26
 *     GND  (7)   ---->  GND
 *     SPK1 (8)   ---->  speaker +    4-8 ohm, do not ground either leg
 *     SPK2 (6)   ---->  speaker -
 *     BUSY (16)         not used - track ends are detected over the serial link
 *
 *   Power: one 3.7V cell for ESP32 + panel (+ SD module), a second for the
 *   DFPlayer, grounds tied together.
 * ----------------------------------------------------------------
 *
 * If the screen stays black: check SDA/SCK first (swapping them fails
 * silently), then try SPI_MODE2 or SPI_MODE0 below.
 * If it says "no SD card": check the four SD wires and that the card is
 * FAT32. The sketch already retries at a slow clock before giving up.
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <TJpg_Decoder.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------- config

#define SOURCE_FLASH  0
#define SOURCE_SDCARD 1
#define VIDEO_SOURCE  SOURCE_FLASH   // <- SOURCE_SDCARD to play from the SD module

#define TFT_DC     15
#define TFT_RST    17
#define TFT_CS     -1        // GMT130 has no CS pin
#define PANEL_MODE SPI_MODE3 // found by sweep on this panel; try 2 or 0 if black
#define SPI_HZ      40000000

#define SD_SCK     14
#define SD_MISO    19
#define SD_MOSI    13
#define SD_CS       4
#define SD_HZ      20000000  // retried at SD_SLOW_HZ if the card won't mount
#define SD_SLOW_HZ  4000000

#define BTN_UP     32
#define BTN_DOWN   33
#define BTN_OK     25

#define DF_TX_PIN  27        // ESP32 -> DFPlayer RX, through 1k
#define DF_RX_PIN  26        // DFPlayer TX -> ESP32
#define VOLUME     25        // 0..30, starting volume

#define TVF_PATH   "/clip.tvf"   // SOURCE_FLASH plays this one file
#define MAX_CLIPS  100           // SOURCE_SDCARD lists up to this many
#define SELF_TEST  true      // 1.5s of colour bars at boot
#define ROTATION   2         // panel is mounted 180 degrees on this build

// A full-length clip has too many frames to hold the whole index in RAM
// (an hour at 15 fps is 216 kB), so it is read from the file in blocks.
#define INDEX_BLOCK 256      // frames per block -> about 1 kB of RAM

const bool HAS_MENU = VIDEO_SOURCE == SOURCE_SDCARD;

// ---------------------------------------------------------------- state

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
SPIClass sdSPI(HSPI);
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini df;

// 25 used bytes inside a 32-byte header block; the spare 7 are padding.
struct __attribute__((packed)) TvfHeader {
  char     magic[4];       // "TVF1"
  uint8_t  version;
  uint8_t  mode;           // 0 = mono pages, 1 = mjpeg
  uint16_t width;
  uint16_t height;
  uint8_t  fps;
  uint8_t  chunkSeconds;   // audio chunk length, 0 = no audio / single track
                           // (was a reserved byte - older files read 0 here)
  uint32_t frameCount;
  uint32_t indexOffset;
  uint32_t maxFrameSize;
  uint8_t  audioFolder;    // DFPlayer folder 1-99 with 001.mp3...,
                           // 0 = the /mp3 folder with 0001.mp3... (was padding)
};

fs::FS   *store = nullptr;
TvfHeader hdr;
File      vid;
uint8_t  *frameBuf = nullptr;

uint32_t indexCache[INDEX_BLOCK + 1];   // +1: a frame's end is the next start
int32_t  indexBase = -1;                // first frame in indexCache, -1 = empty

int16_t  originX = 0, originY = 0;
uint32_t frameInterval = 66;   // ms per frame, from hdr.fps

bool     audioOk       = false;
uint8_t  volume        = VOLUME;
uint16_t trackCount    = 1;
uint16_t currentTrack  = 1;    // DFPlayer tracks are 1-based
uint32_t chunkStartMs  = 0;
uint32_t chunkBaseFrame = 0;
int32_t  lastDrawn     = -1;

std::vector<String> clips;     // .tvf paths on the card, sorted by name
int      selected      = 0;
int      menuTop       = 0;    // first clip shown in the list
bool     playing       = false;
uint32_t volShownMs    = 0;    // when the volume readout went up, 0 = hidden

// ---------------------------------------------------------------- helpers

// TJpg_Decoder hands back decoded blocks; push each one to the panel.
bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return false;
  tft.drawRGBBitmap(originX + x, originY + y, bitmap, w, h);
  return true;
}

void showMessage(const char *msg) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_RED);
  tft.setTextSize(2);
  tft.setCursor(6, 110);
  tft.print(msg);
}

void fail(const char *msg) {
  Serial.printf("FAILED: %s\n", msg);
  showMessage(msg);
  while (true) delay(1000);
}

void selfTest() {
  // If these bars look right, wiring and init are both fine and anything
  // that goes wrong after this is the file, not the hardware.
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0,   0, 240, 80, ST77XX_RED);
  tft.fillRect(0,  80, 240, 80, ST77XX_GREEN);
  tft.fillRect(0, 160, 240, 80, ST77XX_BLUE);
  delay(1500);
  tft.fillScreen(ST77XX_BLACK);   // clear the letterbox area before playback
}

// Mounts whichever storage VIDEO_SOURCE picks. Both are an fs::FS, so
// everything after this is the same code for flash and card.
fs::FS &mountStorage() {
#if VIDEO_SOURCE == SOURCE_SDCARD
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, SD_HZ)) {
    // Long jumpers and the level shifter on cheap modules often can't take
    // the fast clock. A slow card is still plenty for 15 fps.
    Serial.println("SD mount failed at full speed, retrying slow");
    SD.end();
    if (!SD.begin(SD_CS, sdSPI, SD_SLOW_HZ)) fail("no SD card");
  }
  Serial.println("video source: SD card");
  return SD;
#else
  if (!LittleFS.begin()) fail("LittleFS mount failed");
  Serial.println("video source: flash");
  return LittleFS;
#endif
}

// Opens a clip and gets it ready to play. Returns an error, or nullptr.
const char *openClip(const char *path) {
  if (vid) vid.close();
  free(frameBuf);
  frameBuf   = nullptr;
  indexBase  = -1;
  trackCount = 1;

  vid = store->open(path, "r");
  if (!vid) return "can't open file";

  if (vid.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr)) return "short header";
  if (memcmp(hdr.magic, "TVF1", 4) != 0) return "bad magic";
  if (hdr.mode != 1)                     return "not mjpeg";
  if (hdr.frameCount == 0)               return "no frames";
  // The index holds frameCount + 1 entries: the extra one marks the end of the
  // last frame, so a frame's length is just the gap to the next offset.
  if (hdr.indexOffset + (hdr.frameCount + 1ULL) * 4ULL > vid.size()) return "file truncated";

  frameBuf = (uint8_t *)malloc(hdr.maxFrameSize);
  if (!frameBuf) return "out of memory";

  originX = (tft.width()  - hdr.width)  / 2;
  originY = (tft.height() - hdr.height) / 2;
  frameInterval = 1000UL / (hdr.fps ? hdr.fps : 15);

  if (hdr.chunkSeconds) {
    uint32_t perChunk = (uint32_t)hdr.fps * hdr.chunkSeconds;
    trackCount = (hdr.frameCount + perChunk - 1) / perChunk;
  }

  Serial.printf("%s: %ux%u @%ufps, %u frames, %u track(s) in %s%02u\n", path,
                hdr.width, hdr.height, hdr.fps, hdr.frameCount, trackCount,
                hdr.audioFolder ? "folder " : "/mp3", hdr.audioFolder);
  return nullptr;
}

// Where frame i starts and how long it is, loading its index block on demand.
bool frameSpan(uint32_t i, uint32_t &offset, uint32_t &len) {
  uint32_t base = i - (i % INDEX_BLOCK);
  if ((int32_t)base != indexBase) {
    uint32_t n = min((uint32_t)INDEX_BLOCK + 1, hdr.frameCount + 1 - base);
    vid.seek(hdr.indexOffset + base * 4UL);
    if (vid.read((uint8_t *)indexCache, n * 4) != n * 4) {
      indexBase = -1;
      return false;
    }
    indexBase = base;
  }
  offset = indexCache[i - base];
  len    = indexCache[i - base + 1] - offset;
  return true;
}

void drawFrame(uint32_t i) {
  if (i >= hdr.frameCount) return;
  uint32_t offset, len;
  if (!frameSpan(i, offset, len)) return;
  if (len == 0 || len > hdr.maxFrameSize) return;
  vid.seek(offset);
  if (vid.read(frameBuf, len) != len) return;
  TJpgDec.drawJpg(0, 0, frameBuf, len);
}

void playAudio(uint16_t track) {
  if (!audioOk) return;
  if (hdr.audioFolder) df.playFolder(hdr.audioFolder, track);  // /03/001.mp3 ...
  else                 df.playMp3Folder(track);                // /mp3/0001.mp3 ...
}

void startTrack(uint16_t track) {
  currentTrack   = track;
  chunkBaseFrame = (uint32_t)(track - 1) * hdr.fps * hdr.chunkSeconds;
  chunkStartMs   = millis();
  lastDrawn      = -1;
  playAudio(track);
}

void restart() {
  if (audioOk && hdr.chunkSeconds) {
    startTrack(1);
  } else {
    chunkBaseFrame = 0;
    chunkStartMs   = millis();
    lastDrawn      = -1;
    playAudio(1);
  }
}

// ---------------------------------------------------------------- buttons

struct Button {
  uint8_t  pin;
  bool     down      = false;
  uint32_t changedMs = 0;
  uint32_t repeatMs  = 0;

  // True once per press, debounced. With repeat, holding it keeps firing -
  // that's what makes a long list quick to scroll.
  bool pressed(bool repeat) {
    bool     now = digitalRead(pin) == LOW;
    uint32_t t   = millis();
    if (now != down && t - changedMs > 25) {
      down      = now;
      changedMs = t;
      repeatMs  = t + 400;
      return now;
    }
    if (repeat && down && (int32_t)(t - repeatMs) >= 0) {
      repeatMs = t + 120;
      return true;
    }
    return false;
  }
};

Button btnUp   {BTN_UP};
Button btnDown {BTN_DOWN};
Button btnOk   {BTN_OK};

// ---------------------------------------------------------------- menu

#define MENU_Y    32         // list starts below the title
#define ROW_H     24
#define ROWS       8
#define ROW_CHARS 19         // text size 2 is 12 px a character

void scanClips() {
  File root = store->open("/");
  for (File f = root.openNextFile(); f && clips.size() < MAX_CLIPS;
       f = root.openNextFile()) {
    String name = f.name();
    name = name.substring(name.lastIndexOf('/') + 1);   // older cores give a path
    bool isClip = !f.isDirectory() && name.length() > 4 &&
                  name.substring(name.length() - 4).equalsIgnoreCase(".tvf");
    // "._name.tvf" files are macOS metadata, not videos.
    if (isClip && !name.startsWith(".")) clips.push_back("/" + name);
    f.close();
  }
  root.close();

  std::sort(clips.begin(), clips.end(), [](const String &a, const String &b) {
    String x = a, y = b;
    x.toLowerCase();
    y.toLowerCase();
    return x < y;
  });
  Serial.printf("%u clip(s) on the card\n", (unsigned)clips.size());
}

// "/My Movie.tvf" -> "My Movie", cut to fit one row.
String clipLabel(const String &path) {
  String s = path.substring(1, path.length() - 4);
  if (s.length() > ROW_CHARS) s = s.substring(0, ROW_CHARS - 2) + "..";
  return s;
}

void drawMenuRow(int row) {
  int  i   = menuTop + row;
  int  y   = MENU_Y + row * ROW_H;
  bool sel = i == selected;
  tft.fillRect(0, y, 240, ROW_H, sel ? ST77XX_BLUE : ST77XX_BLACK);
  if (i >= (int)clips.size()) return;
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(6, y + 4);
  tft.print(clipLabel(clips[i]));
}

// Redraws only what changed, so scrolling doesn't flash the whole screen.
void drawMenu(bool full) {
  if (selected < menuTop)       menuTop = selected;
  if (selected >= menuTop + ROWS) menuTop = selected - ROWS + 1;

  if (full) tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.fillRect(0, 0, 240, MENU_Y, ST77XX_BLACK);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(6, 8);
  tft.printf("Videos  %d/%d", selected + 1, (int)clips.size());

  for (int r = 0; r < ROWS; r++) drawMenuRow(r);
}

void openMenu() {
  playing = false;
  if (audioOk) df.stop();
  drawMenu(true);
}

void playSelected() {
  if (const char *err = openClip(clips[selected].c_str())) {
    // A bad file shouldn't take the whole menu down - say so and go back.
    Serial.printf("%s: %s\n", clips[selected].c_str(), err);
    showMessage(err);
    delay(2000);
    drawMenu(true);
    return;
  }
  tft.fillScreen(ST77XX_BLACK);
  playing    = true;
  volShownMs = 0;
  restart();
}

void menuLoop() {
  if (audioOk && df.available()) df.read();   // drain anything the DFPlayer says

  int n = clips.size();
  if (btnUp.pressed(true)) {
    selected = (selected + n - 1) % n;        // wraps from the top to the bottom
    drawMenu(false);
  }
  if (btnDown.pressed(true)) {
    selected = (selected + 1) % n;
    drawMenu(false);
  }
  if (btnOk.pressed(false)) playSelected();
}

// ---------------------------------------------------------------- playback

// Volume shows in the letterbox bar above the picture for a moment.
void showVolume() {
  tft.fillRect(0, 0, 240, 24, ST77XX_BLACK);
  tft.fillRect(6, 8, volume * 228 / 30, 8, ST77XX_WHITE);
  volShownMs = millis();
}

void setVolume(int v) {
  volume = constrain(v, 0, 30);
  if (audioOk) df.volume(volume);
  showVolume();
}

void playLoop() {
  if (HAS_MENU) {
    if (btnOk.pressed(false))  { openMenu(); return; }
    if (btnUp.pressed(true))   setVolume(volume + 1);
    if (btnDown.pressed(true)) setVolume(volume - 1);
    if (volShownMs && millis() - volShownMs > 1500) {
      tft.fillRect(0, 0, 240, 24, ST77XX_BLACK);
      volShownMs = 0;
    }
  }

  // Audio is the master clock. When a chunk ends the DFPlayer tells us over
  // serial, and we snap the video index to that boundary - so drift can never
  // grow past one chunk, no BUSY pin needed.
  if (audioOk && df.available()) {
    uint8_t type = df.readType();
    df.read();
    if (type == DFPlayerPlayFinished) {
      if (hdr.chunkSeconds && currentTrack < trackCount) {
        startTrack(currentTrack + 1);
      } else {
        restart();
      }
    }
  }

  uint32_t elapsed = millis() - chunkStartMs;
  uint32_t want    = chunkBaseFrame + elapsed / frameInterval;

  if (want >= hdr.frameCount) {
    // Video ran out first. With audio, wait for the track-end message so the
    // two stay locked; without it, just loop.
    if (!audioOk) restart();
    return;
  }

  if ((int32_t)want != lastDrawn) {
    drawFrame(want);          // late frames are skipped, never queued
    lastDrawn = want;
  }
}

// ---------------------------------------------------------------- setup

void setup() {
  Serial.begin(115200);
  delay(200);

  SPI.begin(18, -1, 23, -1);          // SCK, MISO (unused), MOSI, SS (unused)
  tft.init(240, 240, PANEL_MODE);
  tft.setSPISpeed(SPI_HZ);
  tft.setRotation(ROTATION);
  tft.fillScreen(ST77XX_BLACK);
  if (SELF_TEST) selfTest();

  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(tftOutput);

  store = &mountStorage();

  // Audio is optional - if the DFPlayer is missing or its card is empty the
  // video still plays, which makes bring-up much less painful.
  dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  delay(300);
  if (df.begin(dfSerial, /*isACK=*/true, /*doReset=*/true)) {
    df.volume(volume);
    audioOk = true;
    Serial.println("DFPlayer ready");
  } else {
    Serial.println("DFPlayer not responding - playing video only");
  }

  if (!HAS_MENU) {
    if (const char *err = openClip(TVF_PATH)) fail(err);
    playing = true;
    restart();
    return;
  }

  pinMode(BTN_UP,   INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_OK,   INPUT_PULLUP);

  scanClips();
  if (clips.empty()) fail("no .tvf on card");
  if (clips.size() == 1) playSelected();      // nothing to choose - just play
  else                   openMenu();
}

// ---------------------------------------------------------------- loop

void loop() {
  if (playing) playLoop();
  else         menuLoop();
}
