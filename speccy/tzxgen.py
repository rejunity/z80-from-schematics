#!/usr/bin/env python3
"""tzxgen.py OUTDIR -- the tapes speccy's tape test loads (test_tape.c).

test.tap holds a BASIC loader that runs LOAD "" CODE and a 700-byte CODE
block at 32768. Each .tzx encodes the same blocks another way; each must
load the same bytes through the ROM:

  std.tzx     standard speed data (0x10), with info blocks (0x30, 0x32, 0x33)
  turbo.tzx   turbo speed data (0x11) at other timings the ROM still takes
  pure.tzx    pure tone (0x12), pulse sequence (0x13) and pure data (0x14)
  gen.tzx     generalized data (0x19)
  direct.tzx  direct recording (0x15), sampled at 79 T-states (44.3 kHz)
  csw.tzx     CSW recording (0x18), RLE at 44.1 kHz
  cswz.tzx    the same, Z-RLE
  flow.tzx    groups, a selection, a jump, a loop, a call and return, a
              signal level, a stop after the BASIC part (0x20, 0 ms) and
              another between the CODE header and its data (0x2A), and
              blocks that play nothing (0x31, 0x34, 0x35, 0x4B, 0x5A)
"""
import os, struct, sys, zlib

PILOT, SYNC1, SYNC2, ZERO, ONE = 2168, 667, 735, 855, 1710
CLOCK = 3500000


def block(flag, body):
    data = bytes([flag]) + body
    x = 0
    for b in data:
        x ^= b
    return data + bytes([x])


def header(kind, name, length, p1, p2):
    return block(0x00, bytes([kind]) + name.ljust(10).encode() + struct.pack("<HHH", length, p1, p2))


def blocks():
    prog = bytes([0x00, 0x0A, 0x05, 0x00, 0xEF, 0x22, 0x22, 0xAF, 0x0D])  # 10 LOAD "" CODE
    code, x = bytearray(), 12345
    for _ in range(700):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        code.append(x >> 16 & 0xFF)
    return [header(0, "tzxtest", len(prog), 10, len(prog)), block(0xFF, prog),
            header(3, "data", len(code), 32768, 32768), block(0xFF, bytes(code))]


def junk():
    return [header(3, "junk", 300, 32768, 32768), block(0xFF, bytes(range(256)) + bytes(44))]


def tap(bl):
    return b"".join(struct.pack("<H", len(b)) + b for b in bl)


def tzx(*parts):
    return b"ZXTape!\x1a\x01\x14" + b"".join(parts)


def std(b, pause=1000):
    return bytes([0x10]) + struct.pack("<HH", pause, len(b)) + b


def turbo(b, pause=1000):
    pilot_n = 5000 if b[0] < 0x80 else 2500
    return bytes([0x11]) + struct.pack("<HHHHHHBH", 2100, 650, 720, 820, 1640, pilot_n, 8, pause) \
        + struct.pack("<I", len(b))[:3] + b


def pure(b, pause=1000):
    pilot_n = 8063 if b[0] < 0x80 else 3223
    return (bytes([0x12]) + struct.pack("<HH", PILOT, pilot_n)
            + bytes([0x13, 2]) + struct.pack("<HH", SYNC1, SYNC2)
            + bytes([0x14]) + struct.pack("<HHBH", ZERO, ONE, 8, pause) + struct.pack("<I", len(b))[:3] + b)


def gen(b, pause=1000):
    pilot_n = 8063 if b[0] < 0x80 else 3223
    body = struct.pack("<HIBBIBB", pause, 2, 2, 2, 8 * len(b), 2, 2)
    body += bytes([0]) + struct.pack("<HH", PILOT, 0) + bytes([0]) + struct.pack("<HH", SYNC1, SYNC2)
    body += bytes([0]) + struct.pack("<H", pilot_n) + bytes([1]) + struct.pack("<H", 1)
    body += bytes([0]) + struct.pack("<HH", ZERO, ZERO) + bytes([0]) + struct.pack("<HH", ONE, ONE)
    body += b
    return bytes([0x19]) + struct.pack("<I", len(body)) + body


def pulses(b):
    """the ROM's pulses for a block (its pause aside)"""
    out = [PILOT] * (8063 if b[0] < 0x80 else 3223) + [SYNC1, SYNC2]
    for byte in b:
        for i in range(7, -1, -1):
            out += [ONE, ONE] if byte >> i & 1 else [ZERO, ZERO]
    return out


def direct(b, pause=1000, tps=79):
    # the level toggles at each pulse's start; the tape starts low, so the
    # first pulse is high (and every block starts after a pause's low)
    edges, t = [], 0
    for p in pulses(b):
        edges.append(t)
        t += p
    samples, level, e = [], 0, 0
    for i in range(t // tps):
        mid = i * tps + tps // 2
        while e < len(edges) and edges[e] <= mid:
            level ^= 1
            e += 1
        samples.append(level)
    data = bytearray()
    for i in range(0, len(samples), 8):
        chunk = samples[i:i + 8]
        v = 0
        for k, s in enumerate(chunk):
            v |= s << (7 - k)
        data.append(v)
    last = len(samples) - 8 * (len(data) - 1)
    return bytes([0x15]) + struct.pack("<HHB", tps, pause, last) + struct.pack("<I", len(data))[:3] + bytes(data)


def csw(b, pause=1000, rate=44100, z=False):
    rle, t, at = bytearray(), 0, 0
    for p in pulses(b):
        t += p
        n = round(t * rate / CLOCK) - at
        at += n
        rle += bytes([n]) if 0 < n < 256 else b"\x00" + struct.pack("<I", n)
    npulses = len(pulses(b))
    data = zlib.compress(bytes(rle), 9) if z else bytes(rle)
    body = struct.pack("<H", pause) + struct.pack("<I", rate)[:3] + bytes([2 if z else 1]) \
        + struct.pack("<I", npulses) + data
    return bytes([0x18]) + struct.pack("<I", len(body)) + body


def text(s):
    return bytes([0x30, len(s)]) + s.encode()


def archive():
    entries = [(0x00, "tzxgen test"), (0x01, "speccy"), (0x03, "2026"), (0x07, "ROM")]
    body = bytes([len(entries)]) + b"".join(bytes([i, len(s)]) + s.encode() for i, s in entries)
    return bytes([0x32]) + struct.pack("<H", len(body)) + body


def flow(bl, jk):
    """blocks 0..26 of flow.tzx, the jumps relative to these numbers"""
    sel = [(2, "load"), (1, "junk")]
    sel_body = bytes([len(sel)]) + b"".join(struct.pack("<hB", o, len(s)) + s.encode() for o, s in sel)
    out = [
        bytes([0x5A]) + b"XTape!\x1a\x01\x14",                       # 0 glue
        text("tzxgen flow test"),                                     # 1
        archive(),                                                    # 2
        bytes([0x33, 1, 0, 0x00, 1]),                                 # 3 hardware: a 48K
        bytes([0x35]) + b"speccy test     " + struct.pack("<I", 4) + b"\0\1\2\3",  # 4 custom info
        bytes([0x21, 5]) + b"BASIC",                                  # 5 group
        std(bl[0]),                                                   # 6
        std(bl[1]),                                                   # 7
        bytes([0x22]),                                                # 8 group end
        bytes([0x20]) + struct.pack("<H", 0),                         # 9 stop the tape
        bytes([0x28]) + struct.pack("<H", len(sel_body)) + sel_body,  # 10 select: the first entry
        std(jk[0]),                                                   # 11 not played
        bytes([0x23]) + struct.pack("<h", 2),                         # 12 jump over 13
        std(jk[1]),                                                   # 13 not played
        bytes([0x24]) + struct.pack("<H", 2),                         # 14 loop twice
        bytes([0x12]) + struct.pack("<HH", PILOT, 10),                #    15 ten pilot pulses
        bytes([0x25]),                                                # 16 loop end
        bytes([0x2B]) + struct.pack("<I", 1) + bytes([0]),            # 17 level low
        std(bl[2]),                                                   # 18 the CODE header
        bytes([0x2A]) + struct.pack("<I", 0),                         # 19 stop if 48K
        bytes([0x26]) + struct.pack("<Hh", 1, 2),                     # 20 call 22
        bytes([0x23]) + struct.pack("<h", 3),                         # 21 jump to 24
        std(bl[3]),                                                   # 22 the CODE data
        bytes([0x27]),                                                # 23 return
        bytes([0x34]) + bytes(8),                                     # 24 emulation info (old)
        bytes([0x4B]) + struct.pack("<I", 3) + b"\0\0\0",             # 25 a block it does not know
        bytes([0x31, 2, 4]) + b"done",                                # 26 message
    ]
    return out


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    bl, jk = blocks(), junk()
    files = {
        "test.tap": tap(bl),
        "std.tzx": tzx(text("tzxgen standard"), archive(), bytes([0x33, 1, 0, 0x00, 1]),
                       *[std(b) for b in bl]),
        "turbo.tzx": tzx(*[turbo(b) for b in bl]),
        "pure.tzx": tzx(*[pure(b) for b in bl]),
        "gen.tzx": tzx(*[gen(b) for b in bl]),
        "direct.tzx": tzx(*[direct(b) for b in bl]),
        "csw.tzx": tzx(*[csw(b) for b in bl]),
        "cswz.tzx": tzx(*[csw(b, z=True) for b in bl]),
        "flow.tzx": tzx(*flow(bl, jk)),
    }
    for name, data in files.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
    with open(os.path.join(out, "code.bin"), "wb") as f:   # what must land at 32768
        f.write(bl[3][1:-1])


if __name__ == "__main__":
    main()
