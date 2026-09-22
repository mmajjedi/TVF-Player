#!/usr/bin/env python3
"""
tvfpack.py - encode a video into the files the ESP32 player needs.

    python tvfpack.py test.mp4 out --mode mjpeg --width 240 --height 136 --fps 15 -q 70 --audio

Everything lands in one output folder, split by where it has to go:

    out/data/clip.tvf       -> the sketch's data/ folder (LittleFS)
    out/sd/mp3/0001.mp3     -> the DFPlayer's microSD, at the card root
    out/sd/mp3/0002.mp3

With --sdcard the video is meant for the SD card module instead of the
ESP32's flash, so there is no 2 MB limit, and it is named after the input
(or --name) so several can sit on one card and show up in the menu:

    out/videocard/My Movie.tvf  -> the SD module's microSD, at the card root

With several videos, give each one its own --slot (1-99). Its sound then goes
in that numbered DFPlayer folder, and the number is stored in the .tvf, so the
player finds the right audio for whichever video is picked:

    python tvfpack.py movie.mp4 out --sdcard --slot 3 --audio ...
    out/videocard/movie.tvf  -> SD module's card
    out/sd/03/001.mp3        -> DFPlayer's card, as /03/001.mp3

Modes:
    mjpeg   colour panels (ST7789, SSD1351). Baseline JPEG frames, decoded
            on the ESP32 by TJpgDec.
    mono    1-bit panels (SSD1306, SH1106). Page-packed frames that go
            straight out over SPI/I2C with no processing on the ESP32.

Audio:
    --audio also cuts the soundtrack into numbered MP3 files. The chunk length
    is written into the .tvf header, so the player can keep picture and sound
    locked together without anything being configured by hand.

Needs: ffmpeg on PATH, plus  python -m pip install numpy pillow
"""

import argparse
import io
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageEnhance

HEADER_SIZE = 32
MAGIC = b"TVF1"
VERSION = 1
MODE_MONO = 0
MODE_MJPEG = 1


def die(msg):
    sys.exit(f"error: {msg}")


def ffmpeg_exists():
    try:
        subprocess.run(["ffmpeg", "-version"], capture_output=True, check=True)
        return True
    except (OSError, subprocess.CalledProcessError):
        return False


def probe_audio(path):
    """True if the source has an audio stream."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "a:0",
             "-show_entries", "stream=index", "-of", "json", str(path)],
            capture_output=True, text=True, check=True).stdout
        return bool(json.loads(out).get("streams"))
    except Exception:
        return False


def decode_frames(path, width, height, fps, start, duration):
    """
    Pipe raw RGB frames out of ffmpeg, one at a time. Never holds more than
    a single frame in memory, so episode-length sources are fine.
    """
    cmd = ["ffmpeg", "-v", "error"]
    if start:
        cmd += ["-ss", str(start)]
    cmd += ["-i", str(path)]
    if duration:
        cmd += ["-t", str(duration)]
    cmd += [
        "-vf", f"scale={width}:{height}",
        "-r", str(fps),
        "-f", "image2pipe",
        "-pix_fmt", "rgb24",
        "-vcodec", "rawvideo",
        "pipe:1",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    frame_bytes = width * height * 3
    try:
        while True:
            raw = proc.stdout.read(frame_bytes)
            if len(raw) < frame_bytes:
                break
            yield Image.frombytes("RGB", (width, height), raw)
    finally:
        proc.stdout.close()
        err = proc.stderr.read().decode("utf-8", "replace")
        proc.wait()
        if proc.returncode not in (0, None) and "Output file is empty" not in err:
            tail = "\n".join(err.strip().splitlines()[-8:])
            if tail:
                print(f"ffmpeg: {tail}", file=sys.stderr)


def encode_mjpeg(img, quality):
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=quality, optimize=True,
             progressive=False, subsampling=2)
    return buf.getvalue()


def encode_mono(img, width, height, contrast, invert):
    """Page-packed 1-bit, the layout SSD1306/SH1106 expect: 8 rows per byte."""
    g = img.convert("L")
    if contrast != 1.0:
        g = ImageEnhance.Contrast(g).enhance(contrast)
    a = np.array(g.convert("1", dither=Image.FLOYDSTEINBERG), dtype=np.uint8)
    if invert:
        a = 1 - a
    pages = height // 8
    out = np.zeros((pages, width), dtype=np.uint8)
    for p in range(pages):
        block = a[p * 8:(p + 1) * 8, :]
        for bit in range(8):
            out[p] |= (block[bit] & 1) << bit
    return out.tobytes()


def extract_audio(src, out_dir, pattern, chunk, bitrate, start, duration):
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("*.mp3"):
        old.unlink()

    cmd = ["ffmpeg", "-y", "-v", "error"]
    if start:
        cmd += ["-ss", str(start)]
    cmd += ["-i", str(src)]
    if duration:
        cmd += ["-t", str(duration)]
    cmd += [
        "-vn",
        "-ar", "44100", "-ac", "2", "-b:a", f"{bitrate}k",
        "-f", "segment",
        "-segment_time", str(chunk),
        "-segment_start_number", "1",
        "-reset_timestamps", "1",
        str(out_dir / pattern),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        tail = "\n".join(proc.stderr.strip().splitlines()[-8:])
        die(f"audio extraction failed:\n{tail}")
    return sorted(out_dir.glob("*.mp3"))


def main():
    p = argparse.ArgumentParser(
        description="Encode a video into a .tvf file for the ESP32 player.")
    p.add_argument("input")
    p.add_argument("output", help="output folder (created if missing)")
    p.add_argument("--mode", choices=["mjpeg", "mono"], default="mjpeg")
    p.add_argument("--width", type=int, default=240)
    p.add_argument("--height", type=int, default=240)
    p.add_argument("--fps", type=int, default=15)
    p.add_argument("-q", "--quality", type=int, default=70,
                   help="JPEG quality, mjpeg mode (default 70)")
    p.add_argument("--contrast", type=float, default=1.4,
                   help="mono mode only (default 1.4)")
    p.add_argument("--invert", action="store_true", help="mono mode only")
    p.add_argument("--start", type=float, default=0, help="seconds into the source")
    p.add_argument("--duration", type=float, default=None, help="seconds to encode")
    p.add_argument("--sdcard", action="store_true",
                   help="video goes on the SD card module, not the ESP32's flash "
                        "(no 2 MB limit; set VIDEO_SOURCE to SOURCE_SDCARD)")
    p.add_argument("--name", default=None,
                   help="--sdcard only: file name shown in the player's menu "
                        "(default: the input's name)")

    # --- audio (new) ---
    p.add_argument("--audio", action="store_true",
                   help="also write numbered MP3s for the DFPlayer")
    p.add_argument("--audio-chunk", type=int, default=30,
                   help="seconds per MP3 track, 1-255 (default 30)")
    p.add_argument("--audio-bitrate", type=int, default=128, help="kbps (default 128)")
    p.add_argument("--slot", type=int, default=0,
                   help="DFPlayer folder 1-99 for this video's sound, so several "
                        "videos can share one card - use a different slot for "
                        "each (default: the /mp3 folder, for a single video)")

    a = p.parse_args()

    if not ffmpeg_exists():
        die("ffmpeg not found on PATH")
    if not Path(a.input).exists():
        die(f"{a.input} not found")
    if a.mode == "mono" and a.height % 8:
        die(f"mono mode needs a height that is a multiple of 8, got {a.height}")
    if not 1 <= a.audio_chunk <= 255:
        die("--audio-chunk must be between 1 and 255 (it is stored in one byte)")

    if not 0 <= a.slot <= 99:
        die("--slot must be between 1 and 99 (the DFPlayer's folder numbers)")
    if a.name and not a.sdcard:
        die("--name only applies with --sdcard - the flash version always plays clip.tvf")

    out_dir = Path(a.output)
    if a.sdcard:
        name = Path(a.name).stem if a.name else Path(a.input).stem
        out_path = out_dir / "videocard" / f"{name}.tvf"
    else:
        out_path = out_dir / "data" / "clip.tvf"
    # DFPlayer naming: /mp3/0001.mp3 by default, /03/001.mp3 in a slot.
    if a.slot:
        mp3_dir, mp3_pattern = out_dir / "sd" / f"{a.slot:02d}", "%03d.mp3"
    else:
        mp3_dir, mp3_pattern = out_dir / "sd" / "mp3", "%04d.mp3"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    do_audio = a.audio and probe_audio(a.input)
    if a.audio and not do_audio:
        print("note: source has no audio stream, skipping audio", file=sys.stderr)
    chunk_field = a.audio_chunk if do_audio else 0

    offsets = []
    biggest = 0
    count = 0

    with open(out_path, "wb") as out:
        out.write(b"\0" * HEADER_SIZE)
        for img in decode_frames(a.input, a.width, a.height, a.fps,
                                 a.start, a.duration):
            if a.mode == "mjpeg":
                data = encode_mjpeg(img, a.quality)
            else:
                data = encode_mono(img, a.width, a.height, a.contrast, a.invert)
            offsets.append(out.tell())
            out.write(data)
            biggest = max(biggest, len(data))
            count += 1
            if count % 100 == 0:
                print(f"\r{count} frames...", end="", file=sys.stderr, flush=True)

        if count == 0:
            die("no frames encoded - check --start / --duration")

        offsets.append(out.tell())          # sentinel: end of the last frame
        index_offset = out.tell()
        out.write(struct.pack(f"<{len(offsets)}I", *offsets))

        out.seek(0)
        out.write(struct.pack(
            "<4sBBHHBBIIIB", MAGIC, VERSION,
            MODE_MJPEG if a.mode == "mjpeg" else MODE_MONO,
            a.width, a.height, a.fps, chunk_field,
            count, index_offset, biggest, a.slot))

    print(f"\r{count} frames, {a.width}x{a.height} @ {a.fps}fps", file=sys.stderr)
    size = out_path.stat().st_size
    print(f"video   {size/1024/1024:.2f} MB  ({count/a.fps:.1f}s, "
          f"largest frame {biggest/1024:.1f} kB)")

    if a.sdcard:
        if size >= 4 * 1024**3:
            print(f"\nwarning: {size/1024**3:.2f} GB is over FAT32's 4 GB per-file "
                  f"limit.\nshorten with --duration, or lower -q / --fps.",
                  file=sys.stderr)
    elif size > 1_800_000:
        print(f"\nwarning: {size/1024/1024:.2f} MB will not fit a 2 MB LittleFS "
              f"partition.\nkeep it under about 1.7 MB - shorten with --duration, "
              f"or lower -q / --fps.\nor pack it with --sdcard and play it from an "
              f"SD card module, which has no limit.", file=sys.stderr)

    if do_audio:
        # A numbered DFPlayer folder only holds tracks 001-255.
        needed = -(-count // (a.fps * a.audio_chunk))
        if a.slot and needed > 255:
            longer = -(-count // (a.fps * 255))
            die(f"{needed} audio tracks won't fit one DFPlayer folder (max 255).\n"
                f"rerun with --audio-chunk {longer} or more")
        tracks = extract_audio(a.input, mp3_dir, mp3_pattern, a.audio_chunk,
                               a.audio_bitrate, a.start, a.duration)
        total = sum(t.stat().st_size for t in tracks)
        print(f"audio   {total/1024/1024:.2f} MB  ({len(tracks)} track(s) of "
              f"{a.audio_chunk}s)")

    print(f"\nwrote {out_dir}/")
    if a.sdcard:
        print(f"  videocard/{out_path.name}\n"
              f"      -> copy onto the SD card module's FAT32 microSD, at the root")
    else:
        print(f"  data/clip.tvf\n"
              f"      -> copy into firmware/tvf_player/data/, then Upload LittleFS Data")
    if do_audio:
        print(f"  sd/{mp3_dir.name}/{mp3_pattern % 1} ...\n"
              f"      -> copy the whole {mp3_dir.name}/ folder onto the DFPlayer's "
              f"FAT32 microSD, at the root")
        if a.sdcard and not a.slot:
            print("\nnote: this video's sound went in /mp3. Fine for one video on the "
                  "card - for\nseveral, pack each with its own --slot so their sound "
                  "doesn't overlap.", file=sys.stderr)


if __name__ == "__main__":
    main()
