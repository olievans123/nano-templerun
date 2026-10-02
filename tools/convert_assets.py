#!/usr/bin/env python3
"""Prepare the original app's data for the port. Everything written here comes from the
original app and stays untracked.

  <out>/host   decoded textures for the Mac build (<name>.rgba: width, height, RGBA8)
  <out>/nano   what goes on the iPod: the engine image and tunables (tools/make_image.py),
               the model, atlas, font and level files as they are, the PVRTC textures as
               they are, the PNG sheets re-encoded as PVRTC (tools/pvrtc.py), and files.lst

usage: convert_assets.py <out directory>
"""
import shutil
import struct
import subprocess
import sys
from pathlib import Path
from PIL import Image
import texture2ddecoder

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pvrtc     # noqa: E402
from apple_png import read_png     # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'original/v1.0/Payload/TempleRun.app'
PVR = ['terrainTexture', 'playerTexture', 'playerGlowTexture', 'enemyTexture', 'wallTexture', 'treeTexture', 'lightMapTexture']
# name the engine loads -> (largest side on the iPod)
# The sheets keep their size and get no smaller levels, as on the phone: their sprites sit a
# pixel or two apart, and any shrinking lets neighbours show at a sprite's edge. (The tutorial
# sheet is not used: the tutorial is off.)
PNG = {'interfaceTexture': 1024, 'effectsTexture': 1024, 'tutorialTexture': 64, 'fontNumbers': 256, 'fontCountdown': 256}

out = Path(sys.argv[1])
host, nano = out / 'host', out / 'nano'
for d in (host, nano):
    d.mkdir(parents=True, exist_ok=True)


for name in PVR:
    data = (APP / (name + '.pvr')).read_bytes()
    header, h, w, mips, flags, size = struct.unpack_from('<6I', data)
    if header != 52 or data[44:48] != b'PVR!' or flags & 0xff != 0x19:
        raise ValueError('unexpected PVR file: ' + name)
    bgra = texture2ddecoder.decode_pvrtc(data[52:52 + w * h // 2], w, h, 0)
    im = Image.frombytes('RGBA', (w, h), bgra, 'raw', 'BGRA')
    (host / (name + '.rgba')).write_bytes(struct.pack('<II', w, h) + im.tobytes())
    shutil.copyfile(APP / (name + '.pvr'), nano / (name + '.pvr'))

# The PNG sheets go to the iPod as PVRTC too: its driver reboots on large uncompressed
# textures (the other ports keep those to 256x128). The Mac build is given the decoded result,
# so it shows what the iPod will.
for name, side in PNG.items():
    im = read_png(APP / (name + '.png'))
    if max(im.size) > side:
        im = im.resize((side, side), Image.LANCZOS)
    pvr = pvrtc.encode_pvr(im, mipmaps=False)
    (nano / (name + '.pvr')).write_bytes(pvr)
    w, h = im.size
    decoded = Image.frombytes('RGBA', (w, h), texture2ddecoder.decode_pvrtc(pvr[52:52 + w * h // 2], w, h, 0), 'raw', 'BGRA')
    (host / (name + '.rgba')).write_bytes(struct.pack('<II', w, h) + decoded.tobytes())
    stale = nano / (name + '.4444')
    if stale.exists():
        stale.unlink()

for pattern in ('*.bksb', '*.atlas', '*.fnt', 'modelRegistry.lvl'):
    for f in sorted(APP.glob(pattern)):
        shutil.copyfile(f, nano / f.name)
subprocess.run([sys.executable, str(ROOT / 'tools/make_image.py'), str(nano)], check=True)
files = sorted(f for f in nano.iterdir() if f.name != 'files.lst')
(nano / 'files.lst').write_text(''.join('%s %d\n' % (f.name, f.stat().st_size) for f in files))
print('%d files for the iPod, %.1f MB' % (len(files), sum(f.stat().st_size for f in files) / 1e6))
