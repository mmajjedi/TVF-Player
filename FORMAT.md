# The `.tvf` container

A deliberately dumb format: no compression beyond the JPEGs themselves, no
seeking logic, no metadata. The ESP32 should never have to think.

All integers are little-endian, matching the ESP32's native order, so the
header can be read straight into a packed struct with one `read()`.

## Header — 24 used bytes in a 32-byte block, at offset 0

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | char[4] | `magic` | always `TVF1` |
| 4 | uint8 | `version` | 1 |
| 5 | uint8 | `mode` | 0 = mono pages, 1 = mjpeg |
| 6 | uint16 | `width` | picture width, ≤ 240 |
| 8 | uint16 | `height` | picture height, ≤ 240 |
| 10 | uint8 | `fps` | playback rate |
| 11 | uint8 | `chunkSeconds` | audio chunk length; 0 = no audio |
| 12 | uint32 | `frameCount` | |
| 16 | uint32 | `indexOffset` | absolute offset of the index |
| 20 | uint32 | `maxFrameSize` | largest frame, used to size one heap buffer |
| 24 | — | padding | 8 spare bytes, zero |

Byte 11 was a reserved byte in the first version. Files made before audio
existed read 0 there, which the player treats as "no audio", so old files
still play.

## Frames — from offset 32

Back to back, no separators and no per-frame headers.

- **mjpeg mode:** baseline JPEG, never progressive. Sizes vary per frame.
- **mono mode:** page-packed 1-bit blocks, 8 rows per byte, the layout an
  SSD1306/SH1106 takes directly. Every frame is exactly
  `width × height / 8` bytes.

## Index — at `indexOffset`

`frameCount + 1` × `uint32`, absolute file offsets. Entry *n* is where frame
*n* starts; the extra entry at the end marks where the last frame stops. So a
frame's length is simply `index[n+1] - index[n]` — no length fields stored
anywhere, and one seek plus one read per frame.

## Why this and not MJPEG-in-AVI

An AVI index needs parsing, and a parser is code, RAM, and bugs on a board
that has none to spare. Here the index is already in the layout the firmware
wants: `frameOffset[n]`, seek, read, decode.

## Notes

- The picture is centred on the 240×240 panel by the firmware, from
  `width`/`height`. Letterboxing is not stored.
- `chunkSeconds` is what keeps audio and video locked. The firmware derives
  the track count as `ceil(frameCount / (fps × chunkSeconds))`, so nothing has
  to be configured by hand when a clip changes.
- `maxFrameSize` exists so the firmware can `malloc` exactly one buffer at
  boot and never allocate again.
