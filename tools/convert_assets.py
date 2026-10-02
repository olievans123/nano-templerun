#!/usr/bin/env python3
"""Prepare the original app's data for the port. Everything written here comes from the
original app and stays untracked.

  <out>/host   decoded textures for the Mac build (<name>.rgba: width, height, RGBA8)
  <out>/nano   what goes on the iPod: the engine image and tunables (tools/make_image.py),
               the model, atlas, font and level files as they are, the PVRTC textures as
               they are, the PNG sheets as premultiplied RGBA4444 at half size, and files.lst

usage: convert_assets.py <out directory>
"""
import shutil
import struct
import subprocess
import sys
from pathlib import Path
from PIL import Image
import texture2ddecoder

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'original/v1.0/Payload/TempleRun.app'
PVR = ['terrainTexture', 'playerTexture', 'playerGlowTexture', 'enemyTexture', 'wallTexture', 'treeTexture', 'lightMapTexture']
# name the engine loads -> (largest side on the iPod)
PNG = {'interfaceTexture': 512, 'effectsTexture': 512, 'tutorialTexture': 64, 'fontNumbers': 256, 'fontCountdown': 256}

out = Path(sys.argv[1])
host, nano = out / 'host', out / 'nano'
for d in (host, nano):
    d.mkdir(parents=True, exist_ok=True)


def read_png(path):
    """A PNG as premultiplied RGBA. The app's PNGs are in Apple's "CgBI" variant (raw
    deflate, BGRA, already premultiplied), which ordinary readers reject."""
    data = path.read_bytes()
    if data[12:16] != b'CgBI':
        return premultiplied(Image.open(path))
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(data):
        length, kind = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            w, h, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if (depth, colour, interlace) != (8, 6, 0):
                raise ValueError('unexpected PNG layout: %s' % path.name)
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    import zlib
    raw = zlib.decompressobj(-15).decompress(idat)
    stride = w * 4
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                line[i] = (line[i] + ((line[i - 4] if i >= 4 else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return Image.frombytes('RGBA', (w, h), bytes(out), 'raw', 'BGRA')


def premultiplied(im):
    im = im.convert('RGBA')
    px = bytearray(im.tobytes())
    for i in range(0, len(px), 4):
        a = px[i + 3]
        if a != 255:
            px[i] = (px[i] * a + 127) // 255
            px[i + 1] = (px[i + 1] * a + 127) // 255
            px[i + 2] = (px[i + 2] * a + 127) // 255
    return Image.frombytes('RGBA', im.size, bytes(px))


for name in PVR:
    data = (APP / (name + '.pvr')).read_bytes()
    header, h, w, mips, flags, size = struct.unpack_from('<6I', data)
    if header != 52 or data[44:48] != b'PVR!' or flags & 0xff != 0x19:
        raise ValueError('unexpected PVR file: ' + name)
    bgra = texture2ddecoder.decode_pvrtc(data[52:52 + w * h // 2], w, h, 0)
    im = Image.frombytes('RGBA', (w, h), bgra, 'raw', 'BGRA')
    (host / (name + '.rgba')).write_bytes(struct.pack('<II', w, h) + im.tobytes())
    shutil.copyfile(APP / (name + '.pvr'), nano / (name + '.pvr'))

for name, side in PNG.items():
    im = read_png(APP / (name + '.png'))
    (host / (name + '.rgba')).write_bytes(struct.pack('<II', *im.size) + im.tobytes())
    if max(im.size) > side:
        im = im.resize((side * im.width // max(im.size), side * im.height // max(im.size)), Image.LANCZOS)
    px = im.tobytes()
    words = bytearray()
    for i in range(0, len(px), 4):
        r, g, b, a = px[i] >> 4, px[i + 1] >> 4, px[i + 2] >> 4, px[i + 3] >> 4
        words += struct.pack('<H', r << 12 | g << 8 | b << 4 | a)
    (nano / (name + '.4444')).write_bytes(struct.pack('<II', *im.size) + bytes(words))

for pattern in ('*.bksb', '*.atlas', '*.fnt', 'modelRegistry.lvl'):
    for f in sorted(APP.glob(pattern)):
        shutil.copyfile(f, nano / f.name)
subprocess.run([sys.executable, str(ROOT / 'tools/make_image.py'), str(nano)], check=True)
files = sorted(f for f in nano.iterdir() if f.name != 'files.lst')
(nano / 'files.lst').write_text(''.join('%s %d\n' % (f.name, f.stat().st_size) for f in files))
print('%d files for the iPod, %.1f MB' % (len(files), sum(f.stat().st_size for f in files) / 1e6))
