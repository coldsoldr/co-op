#!/usr/bin/env python3
"""Print the coop stats text that the device wrote into the end of track 4's audio.

Disk mode never shows raw yaffs files (the drive is a FAT the stock disk layer synthesises), but it copies the
tape audio as it is. Since T3g `core/sdram/coop_stats.cc` writes its text into the last 4 KiB of track 4's
audio data (docs/performance.md, T3g). Copy `tape/track_4.aif` off the disk-mode drive and run:

    python3 -I tools/tape_tail_stats.py ~/Downloads/track_4.aif

Walks the AIFF chunks to the SSND data, takes its last 4096 bytes and prints them up to the first NUL. Says so
if the tail is silence (no stats written: the image did not run, or the tape was rebuilt after the last write).
"""
import struct, sys

TAIL = 4096

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"FORM" or data[8:12] not in (b"AIFC", b"AIFF"):
        sys.exit("not an AIFF/AIFC file")
    pos, frames, ssnd = 12, None, None
    while pos + 8 <= len(data):
        cid, ln = data[pos:pos + 4], struct.unpack(">I", data[pos + 4:pos + 8])[0]
        if cid == b"COMM":
            channels = struct.unpack(">H", data[pos + 8:pos + 10])[0]
            frames = struct.unpack(">I", data[pos + 10:pos + 14])[0]
            bits = struct.unpack(">H", data[pos + 14:pos + 16])[0]
        if cid == b"SSND":
            off = struct.unpack(">I", data[pos + 8:pos + 12])[0]
            ssnd = pos + 16 + off
            break
        pos += 8 + ln + (ln & 1)
    if ssnd is None or frames is None:
        sys.exit("no COMM/SSND chunk")
    end = ssnd + frames * channels * (bits // 8)
    tail = data[end - TAIL:end]
    text = tail.split(b"\0")[0]
    if not text.strip():
        sys.exit("the tail of the audio is silence: no stats text in this file")
    sys.stdout.write(text.decode("ascii", errors="replace"))

if __name__ == "__main__":
    main()
