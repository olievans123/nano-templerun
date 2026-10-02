/* glfe.h — the OpenGL ES 1.1 calls the engine makes, as the port receives them.
 *
 * rt_gl.c unpacks the engine's calls into these; an implementation either draws them
 * (glfe.c, through the port's guarded pipeline) or ignores them (glfe_null.c, for comparing
 * the translated engine against the original). Pointers are host pointers into guest memory. */
#ifndef TR_GLFE_H
#define TR_GLFE_H
#include <stdint.h>

void fe_active_texture(uint32_t unit);
void fe_client_active_texture(uint32_t unit);
void fe_bind_texture(uint32_t target, uint32_t engine_id);
void fe_bind_buffer(uint32_t target, uint32_t name);
void fe_buffer_data(uint32_t target, uint32_t size, const void *data, uint32_t usage);
void fe_gen_buffers(uint32_t count, uint32_t *names);
void fe_delete_buffers(uint32_t count, const uint32_t *names);
void fe_delete_textures(uint32_t count, const uint32_t *names);
void fe_enable(uint32_t cap, int on);
void fe_client_state(uint32_t array, int on);
void fe_clear_color(float r, float g, float b, float a);
void fe_color(float r, float g, float b, float a);
void fe_depth_mask(int on);
void fe_fogf(uint32_t name, float value);
void fe_fogfv(uint32_t name, const float *values);
void fe_hint(uint32_t target, uint32_t mode);
void fe_matrix_mode(uint32_t mode);
void fe_load_identity(void);
void fe_push_matrix(void);
void fe_pop_matrix(void);
void fe_mult_matrix(const float *m);
void fe_translate(float x, float y, float z);
void fe_frustum(float l, float r, float b, float t, float n, float f);
void fe_ortho(float l, float r, float b, float t, float n, float f);
/* pointer is a guest address: an offset when a buffer is bound, else an address in guest memory */
void fe_vertex_pointer(int size, uint32_t type, int stride, uint32_t pointer);
void fe_texcoord_pointer(int size, uint32_t type, int stride, uint32_t pointer);
void fe_color_pointer(int size, uint32_t type, int stride, uint32_t pointer);
void fe_normal_pointer(uint32_t type, int stride, uint32_t pointer);
void fe_draw_arrays(uint32_t mode, int first, int count);
void fe_draw_elements(uint32_t mode, int count, uint32_t type, uint32_t indices);

/* ---- for the port's own code (glfe.c) -------------------------------------------------------- */
void fe_reset(void);                                /* once, before the engine starts */
void fe_frame_begin(int panel_w, int panel_h);      /* clears; the engine's draw() goes between these */
void fe_frame_end(void);                            /* hands the frame's triangles to OpenGL */
extern int fe_stat_draws, fe_stat_vertices_in, fe_stat_triangles_in, fe_stat_triangles_out, fe_stat_tiny,
           fe_stat_clipped, fe_stat_dropped, fe_stat_calls, fe_stat_fogged;
extern int fe_stat_outside;     /* host builds: vertices that reached GL on or outside a clip boundary (must stay 0) */
#endif
