/*
 * tvf_player.ino - a pocket TV.
 *
 * VERSION 1 - the clip lives in the ESP32's own flash, so about 15-20 seconds
 * max. An SD card version for full-length video is planned.
 *
 * Plays a .tvf clip from LittleFS on a GMT130 (ST7789 240x240) panel while an
 * MP3-TF-16P / DFPlayer Mini plays the matching audio from its own microSD.
 *
 * Board      ESP32-S V1.1 / ESP32 Dev Module (WROOM-32)
 * Libraries  Adafruit GFX, Adafruit ST7735/ST7789, TJpg_Decoder,
 *            DFRobotDFPlayerMini
 * Partition  Tools > Partition Scheme > "No OTA (2MB APP / 2MB SPIFFS)"
 * Data       put clip.tvf in a "data" folder next to this .ino and upload it
 *            with the LittleFS uploader (Ctrl+Shift+P > Upload LittleFS Data)
 *            Make the file with:  python tools/tvfpack.py in.mp4 out
 *                                   --mode mjpeg --width 240 --height 136
 *                                   --fps 15 -q 70 --audio
 *            Close the Serial Monitor before either upload - it holds the
 *            port and the upload will fail.
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
 *   MP3-TF-16P          ESP32        pin 1 is at the notch, SD slot facing you
 *     VCC  (1)   ---->  VIN / 5V     on 3V3 the amp is quiet and hissy
 *     RX   (2)   ---->  GPIO27       THROUGH A 1k RESISTOR
 *     TX   (3)   ---->  GPIO26
 *     GND  (7)   ---->  GND
 *     SPK1 (8)   ---->  speaker +    4-8 ohm, do not ground either leg
 *     SPK2 (6)   ---->  speaker -
 *     BUSY (16)         not used - track ends are detected over the serial link
 *
 *   Power: one 3.7V cell for ESP32 + panel, a second for the DFPlayer,
 *   grounds tied together.
 * ----------------------------------------------------------------
 *
 * If the screen stays black: check SDA/SCK first (swapping them fails
 * silently), then try SPI_MODE2 or SPI_MODE0 below.
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// ---------------------------------------------------------------- config

#define TFT_DC     15
#define TFT_RST    17
#define TFT_CS     -1        // GMT130 has no CS pin
#define PANEL_MODE SPI_MODE3 // found by sweep on this panel; try 2 or 0 if black
#define SPI_HZ      40000000

#define DF_TX_PIN  27        // ESP32 -> DFPlayer RX, through 1k
#define DF_RX_PIN  26        // DFPlayer TX -> ESP32
#define VOLUME     25        // 0..30

#define TVF_PATH   "/clip.tvf"
#define SELF_TEST  true      // 1.5s of colour bars at boot
#define ROTATION   2         // panel is mounted 180 degrees on this build

// ---------------------------------------------------------------- state

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini df;

// 24 used bytes inside a 32-byte header block; the spare 8 are padding.
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
};

TvfHeader hdr;
File      vid;
uint32_t *frameOffset = nullptr;
uint8_t  *frameBuf    = nullptr;

int16_t  originX = 0, originY = 0;
uint32_t frameInterval = 66;   // ms per frame, from hdr.fps

bool     audioOk       = false;
uint16_t trackCount    = 1;
uint16_t currentTrack  = 1;    // DFPlayer tracks are 1-based
uint32_t chunkStartMs  = 0;
uint32_t chunkBaseFrame = 0;
int32_t  lastDrawn     = -1;

// ---------------------------------------------------------------- helpers

// TJpg_Decoder hands back decoded blocks; push each one to the panel.
bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return false;
  tft.drawRGBBitmap(originX + x, originY + y, bitmap, w, h);
  return true;
}

void fail(const char *msg) {
  Serial.printf("FAILED: %s\n", msg);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_RED);
  tft.setTextSize(2);
  tft.setCursor(6, 110);
  tft.print(msg);
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

void loadHeader() {
  vid = LittleFS.open(TVF_PATH, "r");
  if (!vid) fail("no clip.tvf");

  if (vid.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr)) fail("short header");
  if (memcmp(hdr.magic, "TVF1", 4) != 0) fail("bad magic");
  if (hdr.mode != 1)                     fail("not mjpeg");
  if (hdr.frameCount == 0)               fail("no frames");
  // The index holds frameCount + 1 entries: the extra one marks the end of the
  // last frame, so a frame's length is just the gap to the next offset.
  if (hdr.indexOffset + (hdr.frameCount + 1UL) * 4UL > vid.size()) fail("file truncated");

  frameOffset = (uint32_t *)malloc((hdr.frameCount + 1UL) * sizeof(uint32_t));
  frameBuf    = (uint8_t  *)malloc(hdr.maxFrameSize);
  if (!frameOffset || !frameBuf) fail("out of memory");

  vid.seek(hdr.indexOffset);
  vid.read((uint8_t *)frameOffset, (hdr.frameCount + 1UL) * sizeof(uint32_t));

  originX = (tft.width()  - hdr.width)  / 2;
  originY = (tft.height() - hdr.height) / 2;
  frameInterval = 1000UL / (hdr.fps ? hdr.fps : 15);

  if (hdr.chunkSeconds) {
    uint32_t perChunk = (uint32_t)hdr.fps * hdr.chunkSeconds;
    trackCount = (hdr.frameCount + perChunk - 1) / perChunk;
  }

  Serial.printf("tvf: %ux%u @%ufps, %u frames, %u track(s)\n",
                hdr.width, hdr.height, hdr.fps, hdr.frameCount, trackCount);
}

void drawFrame(uint32_t i) {
  if (i >= hdr.frameCount) return;
  uint32_t len = frameOffset[i + 1] - frameOffset[i];   // gap to the next frame
  if (len == 0 || len > hdr.maxFrameSize) return;
  vid.seek(frameOffset[i]);
  if (vid.read(frameBuf, len) != (int)len) return;
  TJpgDec.drawJpg(0, 0, frameBuf, len);
}

void startTrack(uint16_t track) {
  currentTrack   = track;
  chunkBaseFrame = (uint32_t)(track - 1) * hdr.fps * hdr.chunkSeconds;
  chunkStartMs   = millis();
  lastDrawn      = -1;
  if (audioOk) df.playMp3Folder(track);   // /mp3/0001.mp3 ...
}

void restart() {
  if (audioOk && hdr.chunkSeconds) {
    startTrack(1);
  } else {
    chunkBaseFrame = 0;
    chunkStartMs   = millis();
    lastDrawn      = -1;
    if (audioOk) df.playMp3Folder(1);
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

  if (!LittleFS.begin()) fail("LittleFS mount failed");
  loadHeader();

  // Audio is optional - if the DFPlayer is missing or its card is empty the
  // video still plays, which makes bring-up much less painful.
  dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  delay(300);
  if (df.begin(dfSerial, /*isACK=*/true, /*doReset=*/true)) {
    df.volume(VOLUME);
    audioOk = true;
    Serial.println("DFPlayer ready");
  } else {
    Serial.println("DFPlayer not responding - playing video only");
  }

  restart();
}

// ---------------------------------------------------------------- loop

void loop() {
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
