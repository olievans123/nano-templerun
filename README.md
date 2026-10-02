# Temple Run on the iPod nano 7G

The original iPhone Temple Run (version 1.0, 2011) running on a 7th-generation iPod nano
as a homebrew app: the real game engine, not a remake.

It plays at 28 to 29 frames a second on the nano's 240x432 screen, with swipes, tilt, the
original title art, the game-over panel and a saved best score. There is no sound.

**No game files are in this repository.** You need your own copy of Temple Run 1.0 for
iOS; everything derived from it (the translated engine, the converted textures and models)
is generated on your machine at build time and is never committed.

## How it works

Temple Run 1.0's engine is C++ compiled to ARM code, shipped with full symbols. Instead of
rewriting the game, this port translates that machine code into C, one C function per
original function, and compiles the result for the nano.

- **`tools/recomp.py`** reads the 741 engine functions the game needs (about 68,700
  instructions) and writes each as C statements on local register variables and a block of
  "guest" memory laid out like the original process. Calls to iOS and the C++ library go to
  a small runtime instead (`port/src/rt.c`). The Objective-C half of the app (menus, Game
  Center, the store) is left out.
- **`tools/original_oracle.py`** runs the *original* engine code in the Unicorn CPU
  emulator. `tests/compare_engine.py` gives both the same random seed and the same scripted
  swipes and tilts, then compares the data segment and the whole heap. They match word for
  word after thousands of frames, so the game logic is the original's, not an approximation.
- **`port/src/glfe.c`** stands in for OpenGL ES 1.1. The nano's graphics driver reboots the
  iPod on many ordinary things, so this layer keeps the GL state itself, transforms, clips,
  culls and fogs on the CPU, and hands the driver only what it is known to survive.
- **`port/src/game.c`** is the shell around the engine: it drives the same calls the
  iPhone app's view did, and draws the title, pause and game-over screens (UIKit views on
  the phone) from the original artwork.

## What the nano's graphics driver will not take

Found the hard way, one reboot at a time (`analysis/nano-gl-notes.md` has the detail):

| Rule | What happens otherwise |
| --- | --- |
| Clip everything yourself, slightly inside the view | reboot when a large triangle crosses the screen edge |
| No triangle under 2 square pixels, none thinner than half a pixel | reboot |
| No more than about 1,500 triangles in a frame (this port holds to 1,300) | reboot; the limit follows the free memory |
| No texture level above 512x512 in a sheet; three 1024x1024 sheets were too many | reboot on the first draw with the third |
| Give each texture its first draw alone, in a light frame | reboot when two large sheets are first used in one frame |
| No large uncompressed textures; PVRTC is fine | reboot |
| No large vertex buffer objects; client arrays are fine | reboot at the first draw |
| A single-level texture must not have a mipmap filter | draws white |

To stay inside the triangle limit the port ranks each frame's scenery by how much of it
is actually seen (area times how far it shows through the fog) and leaves out the least
visible first.

## Layout

```
tools/        recomp.py (ARM -> C), original_oracle.py (reference emulation),
              convert_assets.py, pvrtc.py (a small PVRTC encoder), make_image.py
port/src/     rt.c/rt.h (runtime for the translated code), glfe.c (GL layer), game.c (shell)
port/host/    a headless Mac build that renders to PNG, for testing without the iPod
port/nano/    the nano build: platform layer, allocator, app entry
tests/        compare_engine.py, trace_engine.py: translated engine against the original
analysis/     notes on the nano's GL driver
```

A few files under `port/src` and `port/host` (`r3d.c`, `mesh.c`, `tex.c`, `scene_test.c`,
`viewer.c`, `host_scene.c`) are from the first renderer experiments and are no longer part
of the game build.

## Building

You need:

- Temple Run 1.0 for iOS, decrypted, unpacked to `original/v1.0/Payload/TempleRun.app`
- Python 3 with `capstone`, `numpy`, `pillow` and `texture2ddecoder` (and `unicorn` for the tests)
- `arm-none-eabi-gcc` for the nano build
- the NanoApps SDK and a nano 7G set up for homebrew: see
  [nano7-untethered](https://github.com/olievans123/nano7-untethered)

```sh
# the armv7 half of the original executable (sha256 91d85b35...afe01392, 1,439,184 bytes)
mkdir -p build
lipo -thin armv7 original/v1.0/Payload/TempleRun.app/TempleRun -output build/TempleRun-armv7

python3 tools/recomp.py build/recomp            # the engine, as C
python3 tools/convert_assets.py build/assets    # textures, models, engine image: build/assets/nano
```

### On a Mac, without the iPod

```sh
cc -O2 -ffp-contract=off -fno-strict-aliasing -fwrapv -w -DRT_CHECK \
   -Iport/host -Iport/src -Ibuild/recomp -o build/game_host \
   port/host/game_host.c port/src/game.c port/src/glfe.c port/src/rt.c port/src/rt_gl.c \
   build/recomp/recomp_*.c -framework OpenGL -lz -lm

mkdir -p build/shots
./build/game_host build/assets/nano build/assets/host build/shots 3000 3 150
```

That plays 3,000 frames by itself with random seed 3 and writes a screenshot every 150
frames. It also checks every frame against the driver rules above and reports any breach.

To check the translation against the original:

```sh
cc -O2 -ffp-contract=off -fno-strict-aliasing -fwrapv -w -Iport/src -Ibuild/recomp \
   -o build/engine_run port/tests/engine_run.c port/src/rt.c port/src/rt_gl.c \
   port/src/glfe_null.c build/recomp/recomp_*.c -lm
python3 tests/compare_engine.py 1200 3          # expect "0 differing words"
```

### For the nano

```sh
make -C port/nano NANOAPPS=/path/to/NanoApps RECOMP=$PWD/build/recomp
mkdir -p /path/to/NanoApps/apps/templerun && cp port/nano/app/* /path/to/NanoApps/apps/templerun/
make -C /path/to/NanoApps/apps/templerun TR_DIR=$PWD/port
```

This gives `templerun.hbapp`. On the iPod it goes in `/Apps/Executables/` as
`Temple Run.hbapp`, and the contents of `build/assets/nano` go in `/Apps/Data/TempleRun/`.
`tools/stage_nano.py` adds the app to a NanoApps app pack so it appears on the Home screen.

The floating-point flags matter: the translated code must be built with
`-ffp-contract=off -fno-strict-aliasing -fwrapv`, or it stops matching the original.

## Status

Working: the whole game loop, swipes, tilt, coins, power meter, the monkeys, distance
boards, the High Score banner, pause, game over with the original death illustrations, a
best score kept between launches.

Not there: sound, the tutorial, the store and achievements screens, Game Center.

Tested on one iPod nano 7G (firmware build 39579dba). The platform layer uses fixed
firmware addresses for touch and keep-awake, so another firmware version will need those
found again.

## Credits

Temple Run is by Imangi Studios. This is an unofficial port for a device the game never
shipped on, made for the fun of seeing it run there; it contains none of their code or art.
The homebrew environment is [nano7-untethered](https://github.com/olievans123/nano7-untethered).
