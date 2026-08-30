# ESP32 Pocket TV

A tiny television. A small screen, a small speaker, a battery, and a
microcontroller playing a video clip with its sound — so it keeps working when
the power is out.

Built by [@tridee.lab](https://instagram.com/tridee.lab) /
[@majjcrafts](https://instagram.com/majjcrafts). This is the full build from
the explainer reel: what to install, how to wire it, and how to get your own
video onto it.

---

## ⚠️ Version 1 — short clips only

The video lives inside the ESP32's own memory, and there is only about **2 MB**
of it. That works out to roughly **15–20 seconds** of video. This version is
built for a demo, a prop, a reel — not a full episode.

**Version 2 will add an SD card module**, which moves the video off the chip
and onto a card. That removes the limit completely: full-length episodes, and
a menu on screen to choose between several videos. That version is in
progress — watch the repo.

So if you build this today, keep your clip short. The packing tool will tell
you if it's too long, and exactly what to change.

---

## How it works, in one paragraph

A microcontroller can't play an MP4 the way a phone does — there's no video
player inside it. So the work happens on your computer first: a script chops
the video into hundreds of small images, packs them into a single file, and
cuts the sound into separate audio tracks. The ESP32 then just shows those
images one after another, fast enough to look like video, while a separate
little music module plays the sound. Each time an audio track finishes, the
ESP32 jumps to the matching image — that's what keeps picture and sound
together instead of slowly drifting apart.

---

## What you need

| Part | Notes |
|---|---|
| ESP32 dev board | ESP32-S V1.1 / any classic WROOM-32 with 4 MB flash |
| GMT130-V1.0 screen | 1.3", 240×240 colour IPS |
| MP3-TF-16P (DFPlayer Mini) | the little music module |
| microSD card | a small one is fine, formatted **FAT32** |
| Speaker | 4–8 Ω, 3 W max |
| 1 kΩ resistor | important — see the wiring note |
| 2 batteries (3.7 V) | one for the board + screen, one for the music module |
| Jumper wires | |

---

## Step 0 — Install everything

This part takes the longest, and skipping one item causes most of the problems
people hit later. Do all of it before touching the wires.

### On your computer

1. **Arduino IDE 2.x** — [arduino.cc/en/software](https://www.arduino.cc/en/software)
2. **Python** — [python.org](https://www.python.org/downloads/). During the
   install, tick **"Add Python to PATH"**. On Windows you type `python`, not
   `python3`. Then install the two libraries the converter needs:

   ```
   python -m pip install numpy pillow
   ```

   Use `python -m pip`, not bare `pip` — on Windows a bare `pip` can point at
   a different Python install than the one running your script.
3. **ffmpeg** — the tool that reads your video. On Windows open PowerShell and
   run `winget install Gyan.FFmpeg`, then close and reopen it.
4. **CP210x USB driver** — if your board never shows up as a COM port, this is
   why. Search "Silicon Labs CP210x VCP driver" and install it.

### Inside the Arduino IDE

5. **ESP32 board support.** **Tools → Board → Boards Manager**, search `esp32`,
   install **"esp32 by Espressif Systems"**. Then choose
   **Tools → Board → ESP32 Arduino → ESP32 Dev Module**.

6. **Four libraries.** **Tools → Manage Libraries**, then search and install
   each one:

   - `Adafruit GFX Library`
   - `Adafruit ST7735 and ST7789 Library`
   - `TJpg_Decoder` (by Bodmer)
   - `DFRobotDFPlayerMini`

7. **The LittleFS uploader.** This is the one everybody misses. It's a plugin,
   not a library, and without it there's no way to put the video file onto the
   board.

   LittleFS is just the name of the small filesystem inside the ESP32's
   memory — think of it as a tiny USB stick built into the chip. That's where
   your video file goes.

   Download **`arduino-littlefs-upload`** (by earlephilhower) from GitHub —
   take the `.vsix` file from its Releases page — and put it in:

   - **Windows:** `C:\Users\<you>\.arduinoIDE\plugins\`
   - **macOS / Linux:** `~/.arduinoIDE/plugins/`

   Create the `plugins` folder if it doesn't exist, then fully close and
   reopen the IDE. To check it worked: press **Ctrl+Shift+P** and type
   `littlefs` — **"Upload LittleFS to Pico/ESP8266/ESP32"** should appear.

   *(On the older Arduino IDE 1.8 you want the "ESP32 Sketch Data Upload" tool
   instead — same job, different install.)*

### Board settings

With the board selected, set these under **Tools**. The partition one is not
optional — get it wrong and the video won't fit.

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Flash Size | 4MB (32Mb) |
| **Partition Scheme** | **No OTA (2MB APP / 2MB SPIFFS)** |
| Upload Speed | 921600 |

---

## Step 1 — Wire it

Power off while wiring. Only one pin here is 5 V, and it's marked.

### The screen

| Screen pin | Goes to | |
|---|---|---|
| VCC | 3V3 | **not 5V** |
| GND | GND | |
| SDA | GPIO23 | |
| SCK | GPIO18 | |
| RES | GPIO17 | |
| DC | GPIO15 | |
| BLK | 3V3 | keeps the backlight always on |

SDA and SCK must be exactly 23 and 18 — those two are built into the chip and
can't be moved. Swap them and the screen simply stays black with no error
message. It's the most common mistake by far.

This screen has no CS pin. That's normal, and it's already handled in the code.

### The sound module

Hold it with the memory card slot facing you. Pin 1 is next to the small notch.

| Module pin | Goes to | |
|---|---|---|
| VCC (1) | VIN (5V) | the only 5 V connection |
| RX (2) | GPIO27 **through the 1 kΩ resistor** | don't skip the resistor |
| TX (3) | GPIO26 | |
| GND (7) | GND | |
| SPK1 (8) | speaker + | |
| SPK2 (6) | speaker − | |

That resistor matters. The module's input isn't fully safe at the ESP32's
voltage, and wiring it directly is how people burn theirs. One cheap part,
module saved.

Don't connect either speaker wire to ground — the module drives both sides.

### Power

One battery feeds the ESP32 (into VIN) and the screen. A second battery feeds
the sound module. **Connect the two grounds together.**

Two batteries because the amplifier pulls sudden bursts of current that can
make the ESP32 restart in the middle of the video. If you'd rather run it all
from one battery, solder a **470 µF capacitor** across the sound module's VCC
and GND to smooth those bursts out.

---

## Step 2 — Turn your video into device files

One command handles picture and sound together. Open a terminal in the project
folder:

```
python tools/tvfpack.py myvideo.mp4 out --mode mjpeg --width 240 --height 136 --fps 15 -q 70 --start 300 --duration 17 --audio
```

- `--mode mjpeg` for the colour screen (`mono` is for small 1-bit OLEDs)
- `--width` / `--height` must match the picture size you want. 240×136 suits
  widescreen footage; the player centres it on the 240×240 panel.
- `-q 70` is the JPEG quality. Lower makes a smaller file and a worse picture.
- `--start 300` starts 300 seconds (5 minutes) into your video
- `--duration 17` takes 17 seconds from there
- `--audio` also cuts the sound into MP3 tracks

You get one folder, split by where each thing has to go:

```
out/data/clip.tvf      the picture  -> goes onto the ESP32
out/sd/mp3/0001.mp3    the sound    -> goes onto the memory card
```

**If it warns the file is too big**, that's version 1's memory limit talking.
Keep it under about 1.7 MB — shorten `--duration`, or drop `-q` a little, or
lower `--fps`.

Worth doing once, to be sure the file is fine before you upload it:

```
python tools/tvfinfo.py out/data/clip.tvf
```

It should end with `looks good.`

## Step 3 — Copy the sound to the memory card

Format the microSD as **FAT32**. Copy the whole `mp3` folder from `out/sd/`
onto it, so the card looks like this:

```
/mp3/0001.mp3
/mp3/0002.mp3     (only if your clip was long enough to be split)
```

The folder must be named `mp3` and the files must keep their four-digit names.
Put the card into the sound module.

---

## Step 4 — Put the video onto the board

1. Copy `out/data/clip.tvf` into `firmware/tvf_player/data/`. Create that
   `data` folder if it isn't there — the name must be exactly `data`, sitting
   right next to the `.ino` file.
2. Open `firmware/tvf_player/tvf_player.ino` in the Arduino IDE.
3. Plug the board in and select its port under **Tools → Port**.
4. **Close the Serial Monitor if it's open.** While it's open it holds the
   port, and the upload fails or hangs. This catches people constantly,
   because the monitor is exactly what you were using a minute earlier to
   check things.
5. Press **Ctrl+Shift+P** and choose
   **"Upload LittleFS to Pico/ESP8266/ESP32"**.

Wait for it to finish. This uploads the video only.

## Step 5 — Upload the program

Press **Ctrl+U**, the normal upload arrow. Serial Monitor closed for this one
too.

Steps 4 and 5 are two completely separate uploads. One sends the video, the
other sends the program. Doing only one of them is why people end up with
sound but no picture, or picture but nothing else.

## Step 6 — Watch it

Power it up. You should get red, green and blue bars for a second and a half —
that's the test pattern saying the screen and wiring are good. Then your clip
plays, with sound, and loops forever.

If you saw the bars, the hardware is fine. Anything wrong after that point is
the video file, not the wiring.

---

## If something doesn't work

| What you see | What it usually is |
|---|---|
| Screen completely black | SDA and SCK swapped. Check those two first. |
| Test bars, then black | The video file didn't upload, or it's damaged. Redo step 4, then run `tvfinfo.py`. |
| Picture upside down | Your screen is mounted the other way round. In the code, change `ROTATION` from `2` to `0`. |
| Board never appears as a COM port | Install the CP210x driver. On Windows a Bluetooth COM port can also steal the slot — pick the right one manually. |
| Upload fails immediately, or hangs at "Connecting..." | **Serial Monitor is open** — close it and retry. Otherwise wrong port, or another program holding it. |
| Video plays, no sound | Card isn't FAT32, or the folder isn't named `mp3`. Open Serial Monitor at 115200 — it says `DFPlayer not responding` if it's a wiring problem. |
| Sound plays, no video | Step 4 was skipped, or the partition scheme is wrong. |
| Board restarts during playback | Those current bursts. Separate the batteries, or add the 470 µF capacitor. |
| Picture blocky or mushy | `-q` is too low. Raise it (try `-q 80`) and shorten the clip to pay for it. |
| Speckles or torn lines that move on a still picture | Not the file — that's the wiring. Shorter jumper wires, check the ground, and see the note below the table. |
| "File too big" when packing | Version 1's limit. Shorter clip, lower `-q`, or lower `--fps`. |

**About that last one:** if the noise is random speckles or torn horizontal
lines that keep moving even when the picture is frozen, no packing setting will
fix it — the data is getting scrambled on the way to the screen. Use short
jumper wires (under 10 cm), make sure the ground connection is solid, and if it
persists, add `tft.setSPISpeed(27000000);` right after `tft.init(...)` in the
sketch to slow the link down.

---

## What's in this repo

```
firmware/tvf_player/     the program for the ESP32
tools/tvfpack.py         turns a video into the two device files
tools/tvfinfo.py         checks a packed file before you upload it
docs/FORMAT.md           how the video file is built, for the curious
```

`docs/FORMAT.md` is the technical part. You don't need it to build this — only
if you want to understand or change how the format works.

## Roadmap

- [x] v1 — short clip from internal memory, with sound
- [ ] v2 — SD card module: full-length videos, several files, on-screen menu
- [ ] a 3D printed case, so it actually looks like a television

## Licence

MIT — see `LICENSE`. Use it, change it, sell it. A credit is appreciated.
