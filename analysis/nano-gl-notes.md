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
