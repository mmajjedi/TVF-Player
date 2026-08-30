#!/usr/bin/env python3
"""
tvfinfo.py - read back a .tvf and check it is sane.

Run this before flashing. It catches a truncated LittleFS upload or a bad
pack in two seconds, instead of you debugging a black screen for an hour.

    python tvfinfo.py out/data/clip.tvf
"""

import struct
import sys
from pathlib import Path

HEADER = struct.Struct("<4sBBHHBBIII")


def main():
    if len(sys.argv) != 2:
        print("usage: python tvfinfo.py <file.tvf>", file=sys.stderr)
        sys.exit(1)

    path = Path(sys.argv[1])
    raw = path.read_bytes()
    size = len(raw)

    if size < HEADER.size:
        print("file is smaller than the header - truncated or not a .tvf")
        sys.exit(2)

    (magic, version, mode, w, h, fps, chunk,
     count, index_offset, max_frame) = HEADER.unpack_from(raw, 0)

    print(f"file          {path.name}  ({size:,} bytes)")
    print(f"magic         {magic.decode('ascii', 'replace')}  version {version}")
    print(f"mode          {'mjpeg' if mode == 1 else 'mono'}")
    print(f"picture       {w}x{h} @ {fps}fps")
    print(f"frames        {count}  ({count / fps:.1f}s)")
    print(f"audio chunk   {chunk}s" if chunk else "audio chunk   none (single track)")
    print(f"index at      {index_offset:,}")
    print(f"largest frame {max_frame:,} bytes")

    problems = []
    if magic != b"TVF1":
        problems.append("bad magic - this is not a .tvf")
    if index_offset + (count + 1) * 4 > size:
        problems.append("index runs past the end of the file - upload was truncated")
    if count == 0:
        problems.append("no frames")

    if not problems:
        # spot-check first, middle and last frame
        for label, i in (("first", 0), ("middle", count // 2), ("last", count - 1)):
            off = struct.unpack_from("<I", raw, index_offset + i * 4)[0]
            nxt = struct.unpack_from("<I", raw, index_offset + (i + 1) * 4)[0]
            length = nxt - off
            if off + length > size or length <= 0:
                problems.append(f"{label} frame runs past the end of the file")
                break
            if mode == 1 and raw[off:off + 2] != b"\xff\xd8":
                problems.append(f"{label} frame is not a JPEG (missing SOI marker)")
                break
            print(f"{label:13} frame {i}: {length:,} bytes, "
                  f"{'JPEG ok' if mode == 1 else 'mono page block'}")

    if problems:
        print("\nPROBLEMS:")
        for p in problems:
            print(f"  - {p}")
        sys.exit(2)

    print("\nlooks good.")


if __name__ == "__main__":
    main()
