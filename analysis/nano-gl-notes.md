# What the nano's GL driver accepts for a 3D scene (found with port/src/scene_test.c)

Tested on the iPod nano 7G on 2 October 2026 with the hardware test scene: 8 track pieces,
trees, the runner and three monkeys, 16 objects and about 8,700 vertices a frame. Each
result below is from crash trails kept in RAM across the reboot (`port_crumb`).

| Way of drawing | Result |
| --- | --- |
| Original PVRTC textures, 1024x1024 and 256x256, with mipmaps (`glCompressedTexImage2D`) | accepted, no errors, 2.1 MiB |
| Indexed meshes straight through GL (GL transforms and clips), lightmap on unit 1 | runs at 35 FPS, but reboots when large triangles cross the screen edge: twice at exactly frame 73, as the first gate's pillars were being clipped |
| Frame's vertices in one buffer object (331 KB `glBufferData`), then `glDrawArrays` | reboots at the first draw (Angry Birds' working buffer is at most 82 KB) |
| Clip-space positions in client arrays, lightmap on, triangles kept down to 1/8 pixel | reboots 10-14 ms after the first lightmapped draw, whichever batch is being drawn by then; clipping 0.2% to 1.5% inside the view volume makes no difference |
| The same without the lightmap | runs (38.6 FPS) |
| The same with the lightmap and nothing under 2 square pixels | runs (38.7-39.1 FPS, 18.2 ms of work a frame; without the lightmap 42.6 FPS, 17.1 ms) |

So: transform and clip on the CPU (the driver's own clipping is not safe), send clip-space
positions from client arrays, and with two texture units drop triangles under 2 square
pixels. Dropping those changes about 0.2% of the pixels in the test scene. The Mario Kart
port's notes (NANO_STATUS.md there) list the other known reboot triggers: zero-area
triangles, textures thinner than 4:1, more than about 80 live textures, texture uploads
between draws, and less than about 3 MB of OS heap left for the driver.

Doing the transform on the CPU costs no more than letting GL do it (18 ms against 21 ms),
which fits the earlier finding that the driver transforms on the CPU anyway.

## Found while bringing up the full game (2 October 2026)

- **Slivers reboot it too.** A triangle a tenth of a pixel wide and a hundred long has more
  than 2 square pixels of area, but the driver puts corners on a sub-pixel grid and there it
  has none. The score display's sprites reach past the screen edge, and clipping them left
  such slivers: the first frame that drew them rebooted the iPod. `drawable()` in
  `port/src/glfe.c` requires 2 square pixels and half a pixel across at the thinnest.
- **The 2-square-pixel rule holds with one texture unit as well** (a frame drawn with a
  1/8-pixel guard rebooted it after 180 frames at 2 pixels).
- **Large uncompressed textures are out** (the other ports keep them to 256x128), but PVRTC
  with alpha (0x8C02) is fine at 1024x1024, with or without smaller levels. A texture with one
  level and a mipmap filter is incomplete and draws white.
- **Fog** can be drawn as blending against the fog-coloured clear colour (colour and alpha
  scaled by the fog factor); blended two-texture batches are fine.
- **The newest file written is lost if the iPod reboots soon after**; write a second small
  file after a log you want to keep.
- **The resident's touch mailbox goes stale when frames are long** (about 45 ms here); the OS
  touch list at 0x089a5298 can be read directly as well.

## 2 October, later: three 1024-pixel sheets (not yet confirmed fixed)

- With every texture given a first draw of its own (a 40-pixel square, one new texture every
  twelve frames), both launches rebooted on the first draw with the **third** single-level
  1024x1024 PVRTC sheet (uiSheet and the interface sheet had drawn; the effects sheet did
  not). Earlier builds drew the title for minutes with two of them (uiSheet, effects) and
  rebooted on the frame that first used the third (interface, when a run starts). Which
  sheet is third does not matter.
- The scenery's 1024-pixel textures never send their largest level, so these three sheets
  were the only levels above 512 on the device. They are now sent at 512x512 (131 KB each
  instead of 524 KB).
- Builds that first drew two of the sheets in the same frame rebooted on that frame (seven
  launches of seven); the start-up squares stay until that is understood.
