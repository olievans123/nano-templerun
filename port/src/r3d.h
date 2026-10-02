/* r3d.h — the port's 3D submission layer.
 *
 * The nano's GL driver reboots the iPod on zero-area triangles (see the Mario Kart port's
 * notes), which a perspective scene produces now and then: far-off detail shrinking to a
 * point, slivers left by near-plane clipping. So, as that port does, vertices are
 * transformed here, triangles are clipped to the near plane and culled (back-facing,
 * off-screen, under about 1/8 pixel), and only the survivors go to GL, as clip-space
 * positions with identity matrices, batched per texture pair. */
#ifndef TR_R3D_H
#define TR_R3D_H
#include "mesh.h"

void r3d_init(void);
/* Column-major 4x4 matrices, as OpenGL. */
void r3d_begin(int panel_w, int panel_h, const float projection[16], const float view[16]);
/* tex1 = 0 for a single texture; cull = 1 drops faces turned away (the models' front faces
 * are clockwise). */
void r3d_mesh(const TRMesh *m, int frame, const float model[16], unsigned tex0, unsigned tex1, int cull);
void r3d_end(void);

extern int r3d_stat_draws, r3d_stat_vertices_in, r3d_stat_triangles_in, r3d_stat_triangles_out,
           r3d_stat_tiny, r3d_stat_clipped;
extern int r3d_stat_dropped;    /* triangles lost because the frame's vertex pool was full */
extern int r3d_stat_outside;    /* host builds: vertices that reached GL on or outside a clip boundary (must stay 0) */
#endif
