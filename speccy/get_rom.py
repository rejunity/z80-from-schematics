#!/usr/bin/env python3
"""get_rom.py OUT: the 48K Spectrum ROM, from FUSE 1.6.0's source release.

Amstrad, who hold its copyright, allow the ROM to be distributed but not
sold (FUSE's roms/README.copyright); it is fetched, not kept in this tree.
"""
import hashlib, io, os, sys, tarfile, urllib.request

URL = "https://downloads.sourceforge.net/project/fuse-emulator/fuse/1.6.0/fuse-1.6.0.tar.gz"
MEMBER = "fuse-1.6.0/roms/48.rom"
SHA1 = "5ea7c2b824672e914525d1d5c419d71b84a426a2"   # the 48K ROM (CRC32 ddee531f)

out = sys.argv[1]
data = urllib.request.urlopen(URL, timeout=60).read()
with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
    rom = tar.extractfile(MEMBER).read()
if hashlib.sha1(rom).hexdigest() != SHA1:
    sys.exit(f"get_rom.py: {MEMBER} is not the 48K ROM (SHA-1 {hashlib.sha1(rom).hexdigest()})")
os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
with open(out, "wb") as f:
    f.write(rom)
print(f"{out}: the 48K ROM, SHA-1 {SHA1}")
