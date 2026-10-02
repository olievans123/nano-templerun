/* glfe.c — the engine's OpenGL ES 1.1 calls, drawn through a pipeline the nano's driver accepts.
 *
 * The original draws about two dozen indexed meshes a frame from buffer objects with the
 * fixed-function matrices, two texture units (base and light map), linear fog, and blended
 * sprites for effects and the score display. The nano's driver reboots the iPod on its own
 * clipping, on zero-area triangles, on small triangles when two texture units are on, and
 * on large buffer objects (analysis/nano-gl-notes.md), and it has no fog. So this file keeps
 * the GL state itself and does the transform, clipping, culling and fog on the CPU; GL is
 * given clip-space triangles from client arrays, in draws of at most 384 vertices.
 *
 * Fog without glFog: the fog colour is also the clear colour, so a fogged triangle is drawn
 * blended, with its colour and alpha scaled by the fog factor — what shows through is the
 * background, or geometry farther away that is nearer still to the fog colour. Triangles
 * closer than the fog start are drawn opaque first, the fogged ones after them, then
 * everything the engine draws without fog (effects, the runner, the display) in its order. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "gl.h"
#include "glfe.h"
#include "platform.h"
#include "rt.h"

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#define FE_ARRAY_BUFFER 0x8892u
#define FE_ELEMENT_BUFFER 0x8893u
#define FE_MODELVIEW 0x1700u
#define FE_PROJECTION 0x1701u
#define FE_TEXTURE 0x1702u

#ifdef AB_NANO
extern void port_crumb(const char *tag, uint32_t a, uint32_t b);   /* RAM trail that survives a reboot */
#define CRUMB(tag, a) port_crumb(tag, (uint32_t)(a), 0)
#else
#define CRUMB(tag, a) ((void)0)
#endif

unsigned rt_texture_host(uint32_t id);

/* ---- state ------------------------------------------------------------------------------- */
typedef struct { int enabled, size, stride; uint32_t type, pointer, buffer; } Array;
/* A buffer object. The models' vertex buffers (position and one or two texture coordinates,
 * all floats) are kept packed as 16-bit integers with a scale each: half the memory, and the
 * steps are far below a pixel and a texel. Everything else is kept as the engine gave it. */
typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint8_t packed, channels, stride, packed_stride;    /* stride: of the engine's layout */
    float pos_scale, uv_scale[2];
} Buffer;
#define BUFFERS 640
static Buffer sBuffers[BUFFERS];
static uint32_t sNames = 100, sArrayBuffer, sElementBuffer;

static float sModelView[8][16], sProjection[2][16], sTexture[2][2][16];
static int sModelViewTop, sProjectionTop, sTextureTop[2];
static uint32_t sMatrixMode = FE_MODELVIEW;
static int sUnit, sClientUnit;
static int sTexEnabled[2] = { 1, 1 };
static uint32_t sTexBound[2];
static Array sVertexArray, sColorArray, sTexCoordArray[2];
static float sColor[4] = { 1, 1, 1, 1 };
static float sClear[4] = { 0.196078f, 0.321569f, 0.337255f, 1.0f };
static int sFog, sBlend, sDepthTest = 1, sDepthMask = 1, sCull = 1;
static float sFogStart = 100.0f, sFogEnd = 400.0f;

static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

static float *current(void) {
    if (sMatrixMode == FE_PROJECTION) return sProjection[sProjectionTop];
    if (sMatrixMode == FE_TEXTURE) return sTexture[sUnit][sTextureTop[sUnit]];
    return sModelView[sModelViewTop];
}

static void multiply(float out[16], const float a[16], const float b[16]) {      /* out = a * b */
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            t[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    memcpy(out, t, sizeof t);
}

void fe_matrix_mode(uint32_t mode) { sMatrixMode = mode; }
void fe_load_identity(void) { memcpy(current(), kIdentity, sizeof kIdentity); }
void fe_mult_matrix(const float *m) { float t[16]; memcpy(t, m, sizeof t); multiply(current(), current(), t); }
void fe_translate(float x, float y, float z) {
    float *m = current();
    for (int r = 0; r < 4; r++) m[12 + r] += m[r] * x + m[4 + r] * y + m[8 + r] * z;
}
void fe_push_matrix(void) {
    if (sMatrixMode == FE_PROJECTION) { if (sProjectionTop < 1) { memcpy(sProjection[1], sProjection[0], 64); sProjectionTop = 1; } }
    else if (sMatrixMode == FE_TEXTURE) { if (sTextureTop[sUnit] < 1) { memcpy(sTexture[sUnit][1], sTexture[sUnit][0], 64); sTextureTop[sUnit] = 1; } }
    else if (sModelViewTop < 7) { memcpy(sModelView[sModelViewTop + 1], sModelView[sModelViewTop], 64); sModelViewTop++; }
}
void fe_pop_matrix(void) {
    if (sMatrixMode == FE_PROJECTION) { if (sProjectionTop) sProjectionTop--; }
    else if (sMatrixMode == FE_TEXTURE) { if (sTextureTop[sUnit]) sTextureTop[sUnit]--; }
    else if (sModelViewTop) sModelViewTop--;
}
void fe_frustum(float l, float r, float b, float t, float n, float f) {
    float m[16] = { 2 * n / (r - l), 0, 0, 0, 0, 2 * n / (t - b), 0, 0,
                    (r + l) / (r - l), (t + b) / (t - b), -(f + n) / (f - n), -1, 0, 0, -2 * f * n / (f - n), 0 };
    multiply(current(), current(), m);
}
void fe_ortho(float l, float r, float b, float t, float n, float f) {
    float m[16] = { 2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, 0, 0, -2 / (f - n), 0,
                    -(r + l) / (r - l), -(t + b) / (t - b), -(f + n) / (f - n), 1 };
    multiply(current(), current(), m);
}

void fe_active_texture(uint32_t unit) { sUnit = unit == GL_TEXTURE1; }
void fe_client_active_texture(uint32_t unit) { sClientUnit = unit == GL_TEXTURE1; }
void fe_bind_texture(uint32_t target, uint32_t id) { (void)target; sTexBound[sUnit] = id; }
void fe_enable(uint32_t cap, int on) {
    switch (cap) {
    case 0x0de1: sTexEnabled[sUnit] = on; break;
    case 0x0b60: sFog = on; break;
    case 0x0be2: sBlend = on; break;
    case 0x0b71: sDepthTest = on; break;
    case 0x0b44: sCull = on; break;
    default: break;
    }
}
void fe_client_state(uint32_t array, int on) {
    if (array == 0x8074) sVertexArray.enabled = on;
    else if (array == 0x8076) sColorArray.enabled = on;
    else if (array == 0x8078) sTexCoordArray[sClientUnit].enabled = on;
}
void fe_clear_color(float r, float g, float b, float a) { sClear[0] = r; sClear[1] = g; sClear[2] = b; sClear[3] = a; }
void fe_color(float r, float g, float b, float a) { sColor[0] = r; sColor[1] = g; sColor[2] = b; sColor[3] = a; }
void fe_depth_mask(int on) { sDepthMask = on; }
void fe_fogf(uint32_t name, float value) { if (name == 0x0b63) sFogStart = value; else if (name == 0x0b64) sFogEnd = value; }
void fe_fogfv(uint32_t name, const float *values) { (void)name; (void)values; }
void fe_hint(uint32_t target, uint32_t mode) { (void)target; (void)mode; }

static void set_array(Array *a, int size, uint32_t type, int stride, uint32_t pointer) {
    a->size = size; a->type = type; a->stride = stride; a->pointer = pointer; a->buffer = sArrayBuffer;
}
void fe_vertex_pointer(int size, uint32_t type, int stride, uint32_t pointer) { set_array(&sVertexArray, size, type, stride, pointer); }
void fe_color_pointer(int size, uint32_t type, int stride, uint32_t pointer) { set_array(&sColorArray, size, type, stride, pointer); }
void fe_texcoord_pointer(int size, uint32_t type, int stride, uint32_t pointer) { set_array(&sTexCoordArray[sClientUnit], size, type, stride, pointer); }
void fe_normal_pointer(uint32_t type, int stride, uint32_t pointer) { (void)type; (void)stride; (void)pointer; }

/* ---- buffer objects: kept here; the engine's own block is used when it can be held --------- */
void fe_gen_buffers(uint32_t count, uint32_t *names) { for (uint32_t i = 0; i < count; i++) names[i] = ++sNames; }
static Buffer *buffer(uint32_t name) { return name > 100 && name - 100 < BUFFERS ? &sBuffers[name - 100] : NULL; }
static uint32_t sMesh;
void fe_note_mesh(uint32_t mesh) { sMesh = mesh; }
unsigned fe_buffer_bytes;               /* memory held by buffer objects */
static void buffer_release(Buffer *b) {
    if (b->data) { fe_buffer_bytes -= b->packed ? b->size / b->stride * b->packed_stride : b->size; free((void *)b->data); }
    memset(b, 0, sizeof *b);
}

static int16_t quantize(float v, float inverse) {
    float q = v * inverse;
    return (int16_t)(q >= 0.f ? q + 0.5f : q - 0.5f);
}

/* Pack a model's vertices if its layout is the plain one: position first, then one or two
 * pairs of texture coordinates, nothing else. */
static int pack_vertices(Buffer *b, const uint8_t *data, uint32_t size) {
    uint32_t mesh = sMesh;
    if (!mesh || M8(mesh + 8) || M8(mesh + 9)) return 0;            /* normals or colours */
    uint32_t stride = M32(mesh + 0x5c), channels = M32(mesh + 0xc), offsets = M32(mesh + 0x6c);
    if (M32(mesh + 0x60) != 0 || channels < 1 || channels > 2 || stride != 12 + 8 * channels || !offsets || size % stride) return 0;
    for (uint32_t c = 0; c < channels; c++) if (M32(offsets + 4 * c) != 12 + 8 * c) return 0;
    uint32_t count = size / stride, packed_stride = 6 + 4 * channels;
    float pos_max = 0.f, uv_max[2] = { 0.f, 0.f };
    for (uint32_t i = 0; i < count; i++) {
        float v[7];
        memcpy(v, data + (size_t)i * stride, stride);
        for (int k = 0; k < 3; k++) { float a = v[k] < 0.f ? -v[k] : v[k]; if (a > pos_max) pos_max = a; }
        for (uint32_t c = 0; c < channels; c++)
            for (int k = 0; k < 2; k++) { float a = v[3 + 2 * c + k]; if (a < 0.f) a = -a; if (a > uv_max[c]) uv_max[c] = a; }
    }
    if (!(pos_max < 1e6f) || !(uv_max[0] < 1e4f) || !(uv_max[1] < 1e4f)) return 0;     /* also rejects NaN */
    int16_t *out = malloc((size_t)count * packed_stride);
    if (!out) return 0;
    b->pos_scale = pos_max > 0.f ? pos_max / 32767.f : 1.f;
    for (uint32_t c = 0; c < 2; c++) b->uv_scale[c] = uv_max[c] > 0.f ? uv_max[c] / 32767.f : 1.f;
    float ip = 1.f / b->pos_scale, iu[2] = { 1.f / b->uv_scale[0], 1.f / b->uv_scale[1] };
    int16_t *o = out;
    for (uint32_t i = 0; i < count; i++) {
        float v[7];
        memcpy(v, data + (size_t)i * stride, stride);
        *o++ = quantize(v[0], ip); *o++ = quantize(v[1], ip); *o++ = quantize(v[2], ip);
        for (uint32_t c = 0; c < channels; c++) { *o++ = quantize(v[3 + 2 * c], iu[c]); *o++ = quantize(v[4 + 2 * c], iu[c]); }
    }
    b->data = (const uint8_t *)out;
    b->packed = 1; b->channels = (uint8_t)channels; b->stride = (uint8_t)stride; b->packed_stride = (uint8_t)packed_stride;
    fe_buffer_bytes += count * packed_stride;
    return 1;
}
void fe_bind_buffer(uint32_t target, uint32_t name) {
    if (target == FE_ARRAY_BUFFER) sArrayBuffer = name; else if (target == FE_ELEMENT_BUFFER) sElementBuffer = name;
}
void fe_buffer_data(uint32_t target, uint32_t size, const void *data, uint32_t usage) {
    (void)usage;
    Buffer *b = buffer(target == FE_ARRAY_BUFFER ? sArrayBuffer : sElementBuffer);
    if (!b) return;
    buffer_release(b);
    b->size = size;
    if (target == FE_ARRAY_BUFFER && data && pack_vertices(b, data, size)) return;
    uint8_t *copy = malloc(size ? size : 1);
    if (!copy) { plat_fatal("no memory for a vertex buffer"); return; }
    if (data) memcpy(copy, data, size); else memset(copy, 0, size);
    b->data = copy;
    fe_buffer_bytes += size;
}
void fe_delete_buffers(uint32_t count, const uint32_t *names) {
    for (uint32_t i = 0; i < count; i++) { Buffer *b = buffer(names[i]); if (b) buffer_release(b); }
}
void fe_delete_textures(uint32_t count, const uint32_t *names) { (void)count; (void)names; }

/* ---- the frame's output ------------------------------------------------------------------ */
typedef struct { float x, y, z, w, u0, v0, u1, v1; uint32_t color; } OutVtx;   /* what GL is given */
#define CHUNK_VERTS 384                 /* one draw call; a multiple of 3 */
#define CHUNKS 36
#define BATCHES 48
#define BATCH_CHUNKS 36
enum { OPAQUE, FOGGED, ORDERED };
typedef struct {
    unsigned tex0, tex1;
    uint8_t kind, blend, depth_test, depth_mask, chunks, chunk[BATCH_CHUNKS];
    int last;                           /* vertices in the newest chunk */
} Batch;
static OutVtx sPool[CHUNKS * CHUNK_VERTS];
static Batch sBatches[BATCHES];
static int sBatchCount, sChunksUsed, sOrderedOpen = -1;
static float sHalfW = 120.0f, sHalfH = 216.0f;
int fe_stat_draws, fe_stat_vertices_in, fe_stat_triangles_in, fe_stat_triangles_out, fe_stat_tiny, fe_stat_clipped,
    fe_stat_dropped, fe_stat_calls, fe_stat_fogged, fe_stat_outside;

unsigned fe_time_engine_us;
/* Switches for finding what the nano's driver accepts: draw fogged triangles blended (1) or
 * plain (0); the smallest single-texture triangle kept, as twice its area in pixels. */
int fe_option_fog_blend = 1;
float fe_option_min_area2 = 4.0f;
unsigned fe_time_clear_us, fe_time_first_draw_us;       /* this frame: the clear; the first draw call given to GL */
unsigned fe_time_transform_us, fe_time_submit_us;      /* this frame: in draw calls here; handing the frame to GL */
int fe_peak_vertices, fe_peak_chunks, fe_peak_batches;   /* the most one draw call, one frame have needed */
static Batch *batch_for(int kind, unsigned tex0, unsigned tex1, int blend, int depth_test, int depth_mask) {
    if (kind == ORDERED) {
        if (sOrderedOpen >= 0) {
            Batch *b = &sBatches[sOrderedOpen];
            if (b->tex0 == tex0 && b->tex1 == tex1 && b->blend == blend && b->depth_test == depth_test && b->depth_mask == depth_mask)
                return b;
        }
    } else {
        for (int i = 0; i < sBatchCount; i++) {
            Batch *b = &sBatches[i];
            if (b->kind == kind && b->tex0 == tex0 && b->tex1 == tex1) return b;
        }
    }
    if (sBatchCount == BATCHES) return NULL;
    Batch *b = &sBatches[sBatchCount];
    b->kind = (uint8_t)kind; b->tex0 = tex0; b->tex1 = tex1;
    b->blend = (uint8_t)blend; b->depth_test = (uint8_t)depth_test; b->depth_mask = (uint8_t)depth_mask;
    b->chunks = 0;
    b->last = CHUNK_VERTS;              /* forces a first chunk */
    if (kind == ORDERED) sOrderedOpen = sBatchCount;
    sBatchCount++;
    if (sBatchCount > fe_peak_batches) fe_peak_batches = sBatchCount;
    return b;
}

static OutVtx *reserve(Batch *b) {
    if (b->last + 3 > CHUNK_VERTS) {
        if (sChunksUsed == CHUNKS || b->chunks == BATCH_CHUNKS) { fe_stat_dropped++; return NULL; }
        b->chunk[b->chunks++] = (uint8_t)sChunksUsed++;
        if (sChunksUsed > fe_peak_chunks) fe_peak_chunks = sChunksUsed;
        b->last = 0;
    }
    OutVtx *v = &sPool[b->chunk[b->chunks - 1] * CHUNK_VERTS + b->last];
    b->last += 3;
    return v;
}

/* ---- transform, clip, cull ----------------------------------------------------------------- */
/* Each vertex a draw call uses is transformed once into the form GL is given (OutVtx), with
 * its fog factor already folded into the colour; triangles then copy three of them. Only a
 * triangle that crosses the edge of the view is unpacked to floats (Vtx) for clipping. */
typedef struct { float x, y, z, w, u0, v0, u1, v1, r, g, b, a; } Vtx;
#define MAX_VERTS 2560
static OutVtx sOut[MAX_VERTS];
static float sSX[MAX_VERTS], sSY[MAX_VERTS];
static uint8_t sCode[MAX_VERTS], sFogged[MAX_VERTS];
/* Everything is clipped a little inside the view volume GL would clip to, so the driver
 * never clips: 1/1024 of the half-screen at the sides (a ninth of a pixel), and the same
 * fraction of the depth range. */
#define INSET 0.9990234375f

static float plane(const Vtx *v, int bit) {
    switch (bit) {
    case 0: return v->x + v->w * INSET;
    case 1: return v->w * INSET - v->x;
    case 2: return v->y + v->w * INSET;
    case 3: return v->w * INSET - v->y;
    case 4: return v->z + v->w * INSET;
    default: return v->w * INSET - v->z;
    }
}
static int clip_plane(const Vtx *in, int n, Vtx *out, int bit) {
    int count = 0;
    for (int k = 0; k < n; k++) {
        const Vtx *p = &in[k], *q = &in[(k + 1) % n];
        float dp = plane(p, bit), dq = plane(q, bit);
        if (dp >= 0.f) out[count++] = *p;
        if ((dp >= 0.f) != (dq >= 0.f)) {
            float t = dp / (dp - dq);
            const float *a = &p->x, *b = &q->x;
            float *o = &out[count++].x;
            for (int i = 0; i < (int)(sizeof(Vtx) / sizeof(float)); i++) o[i] = a[i] + (b[i] - a[i]) * t;
        }
    }
    return count;
}

static inline uint32_t byte_of(float v) { return v >= 255.f ? 255u : v <= 0.f ? 0u : (uint32_t)(v + 0.5f); }
static void unpack(Vtx *v, const OutVtx *o) {
    v->x = o->x; v->y = o->y; v->z = o->z; v->w = o->w; v->u0 = o->u0; v->v0 = o->v0; v->u1 = o->u1; v->v1 = o->v1;
    v->r = (float)(o->color & 255u); v->g = (float)((o->color >> 8) & 255u);
    v->b = (float)((o->color >> 16) & 255u); v->a = (float)(o->color >> 24);
}

typedef struct { unsigned tex0, tex1; int fogged_scene, cull; float min_area2; Batch *plain, *fogged; } DrawState;

/* The batch a triangle of this draw call goes to; looked up once per call and kind. */
static Batch *target(DrawState *s, int fogged) {
    Batch **slot = fogged ? &s->fogged : &s->plain;
    if (!*slot) {
        if (s->fogged_scene) *slot = batch_for(fogged ? FOGGED : OPAQUE, s->tex0, s->tex1, fogged, 1, 1);
        else *slot = batch_for(ORDERED, s->tex0, s->tex1, sBlend, sDepthTest, sDepthMask);
    }
    return *slot;
}

static const uint8_t *array_base(const Array *a) {
    if (a->buffer) {
        Buffer *b = buffer(a->buffer);
        return b && b->data && !b->packed ? b->data + a->pointer : NULL;
    }
    return a->pointer ? g_mem + a->pointer : NULL;
}
static Buffer *packed_buffer(const Array *a) {
    Buffer *b = a->buffer ? buffer(a->buffer) : NULL;
    return b && b->data && b->packed ? b : NULL;
}

static void draw_triangles(int count, const uint16_t *idx, int first) {
    fe_stat_calls++;
    Buffer *packed = sVertexArray.enabled ? packed_buffer(&sVertexArray) : NULL;
    const uint8_t *pos = sVertexArray.enabled && !packed ? array_base(&sVertexArray) : NULL;
    if ((!pos && !packed) || count < 3) return;
    int n = 0;
    if (idx) { for (int i = 0; i < count; i++) if (idx[i] >= n) n = idx[i] + 1; }
    else n = first + count;
    if (n > fe_peak_vertices) fe_peak_vertices = n;
    if (n > MAX_VERTS) { fe_stat_dropped++; return; }
    if (packed && sVertexArray.pointer / packed->stride + (uint32_t)n > packed->size / packed->stride) return;

    DrawState s;
    s.plain = s.fogged = NULL;
    s.tex0 = sTexEnabled[0] ? rt_texture_host(sTexBound[0]) : 0;
    const uint8_t *uv0 = s.tex0 && sTexCoordArray[0].enabled ? array_base(&sTexCoordArray[0]) : NULL;
    const uint8_t *uv1 = sTexEnabled[1] && sTexCoordArray[1].enabled ? array_base(&sTexCoordArray[1]) : NULL;
    /* a packed buffer holds the texture coordinates too: which of its pairs each unit reads */
    int pk0 = -1, pk1 = -1;
    const int16_t *pk = NULL;
    if (packed) {
        uint32_t base = sVertexArray.pointer / packed->stride;
        if (sVertexArray.pointer % packed->stride || (int)packed->stride != sVertexArray.stride) return;
        pk = (const int16_t *)(const void *)packed->data + (size_t)base * (packed->packed_stride / 2);
        for (int u = 0; u < 2; u++) {
            const Array *t = &sTexCoordArray[u];
            if (!t->enabled || t->buffer != sVertexArray.buffer || t->pointer < sVertexArray.pointer) continue;
            uint32_t rel = t->pointer - sVertexArray.pointer;
            if (rel == 12 || (rel == 20 && packed->channels > 1)) { if (u == 0) pk0 = rel == 12 ? 0 : 1; else pk1 = rel == 12 ? 0 : 1; }
        }
        if (!s.tex0) pk0 = -1;
        if (!sTexEnabled[1]) pk1 = -1;
    }
    s.tex1 = (uv1 || pk1 >= 0) ? rt_texture_host(sTexBound[1]) : 0;
    if (!s.tex1) { uv1 = NULL; pk1 = -1; }
    const uint8_t *col = sColorArray.enabled ? array_base(&sColorArray) : NULL;
    s.fogged_scene = sFog && !sBlend && sDepthTest && sDepthMask;
    s.cull = sCull;
    /* The driver reboots on small triangles: with two texture units (found with the test
     * scene) and also with one (the first frame drawn with a 1/8-pixel guard rebooted the iPod,
     * after 180 frames at 2 square pixels). Twice the area is compared. */
    s.min_area2 = s.tex1 ? 4.0f : fe_option_min_area2;

    float m[16];
    const float *mv = sModelView[sModelViewTop];
    multiply(m, sProjection[sProjectionTop], mv);
    const float tu0 = sTexture[0][sTextureTop[0]][12], tv0 = sTexture[0][sTextureTop[0]][13];
    const float tu1 = sTexture[1][sTextureTop[1]][12], tv1 = sTexture[1][sTextureTop[1]][13];
    const int fog = sFog && sFogEnd > sFogStart, fog_blend = fog && s.fogged_scene && fe_option_fog_blend;
    const float fog_scale = fog ? 1.0f / (sFogEnd - sFogStart) : 0.0f;
    const float f2 = mv[2], f6 = mv[6], f10 = mv[10], f14 = mv[14], fog_end = sFogEnd;
    const int vs = sVertexArray.stride ? sVertexArray.stride : 12, cs = sColorArray.stride ? sColorArray.stride : 4;
    const int t0s = sTexCoordArray[0].stride ? sTexCoordArray[0].stride : 8, t1s = sTexCoordArray[1].stride ? sTexCoordArray[1].stride : 8;
    const int pks = packed ? packed->packed_stride / 2 : 0;
    const float ps = packed ? packed->pos_scale : 0.f;
    const float us0 = packed && pk0 >= 0 ? packed->uv_scale[pk0] : 0.f, us1 = packed && pk1 >= 0 ? packed->uv_scale[pk1] : 0.f;
    const uint32_t flat = byte_of(sColor[0] * 255.f) | byte_of(sColor[1] * 255.f) << 8 | byte_of(sColor[2] * 255.f) << 16
                          | byte_of(sColor[3] * 255.f) << 24;
    const float hw = sHalfW, hh = sHalfH;
    uint8_t all = 0xff;
    for (int i = idx ? 0 : first; i < n; i++) {
        float x, y, z;
        OutVtx *o = &sOut[i];
        if (packed) {
            const int16_t *q = pk + (size_t)i * (size_t)pks;
            x = (float)q[0] * ps; y = (float)q[1] * ps; z = (float)q[2] * ps;
            if (pk0 >= 0) { o->u0 = (float)q[3 + 2 * pk0] * us0 + tu0; o->v0 = (float)q[4 + 2 * pk0] * us0 + tv0; }
            if (pk1 >= 0) { o->u1 = (float)q[3 + 2 * pk1] * us1 + tu1; o->v1 = (float)q[4 + 2 * pk1] * us1 + tv1; }
        } else {
            float p[3];
            __builtin_memcpy(p, pos + (size_t)i * (size_t)vs, 12);
            x = p[0]; y = p[1]; z = p[2];
        }
        float cx = m[0] * x + m[4] * y + m[8] * z + m[12], cy = m[1] * x + m[5] * y + m[9] * z + m[13];
        float cz = m[2] * x + m[6] * y + m[10] * z + m[14], cw = m[3] * x + m[7] * y + m[11] * z + m[15];
        o->x = cx; o->y = cy; o->z = cz; o->w = cw;
        if (uv0) { float t[2]; __builtin_memcpy(t, uv0 + (size_t)i * (size_t)t0s, 8); o->u0 = t[0] + tu0; o->v0 = t[1] + tv0; }
        else if (pk0 < 0) o->u0 = o->v0 = 0.f;
        if (uv1) { float t[2]; __builtin_memcpy(t, uv1 + (size_t)i * (size_t)t1s, 8); o->u1 = t[0] + tu1; o->v1 = t[1] + tv1; }
        else if (pk1 < 0) o->u1 = o->v1 = 0.f;
        uint32_t c = flat;
        if (col) __builtin_memcpy(&c, col + (size_t)i * (size_t)cs, 4);
        uint8_t fogged = 0;
        if (fog_blend) {
            float ez = f2 * x + f6 * y + f10 * z + f14;
            float f = (fog_end - (ez < 0.f ? -ez : ez)) * fog_scale;
            if (f < 0.998f) {                           /* colour and alpha scaled by the fog factor */
                uint32_t k = f <= 0.f ? 0u : (uint32_t)(f * 256.f);
                c = ((c & 0x00ff00ffu) * k >> 8 & 0x00ff00ffu) | ((c >> 8 & 0x00ff00ffu) * k & 0xff00ff00u);
                fogged = k < 2 ? 2 : 1;                 /* 2: lost in the fog */
            }
        }
        o->color = c;
        sFogged[i] = fogged;
        float e = cw * INSET;
        uint8_t code = (uint8_t)((cx < -e) | (cx > e) << 1 | (cy < -e) << 2 | (cy > e) << 3 | (cz < -e) << 4 | (cz > e) << 5);
        sCode[i] = code;
        all &= code;
        if (!code) {
            float inv = 1.f / cw;
            sSX[i] = cx * inv * hw;
            sSY[i] = cy * inv * hh;
        }
    }
    fe_stat_vertices_in += n;
    fe_stat_triangles_in += count / 3;
    if (all) return;                                    /* the whole mesh is off one side */
    const int cull = s.cull;
    const float min_area2 = s.min_area2;
    for (int i = 0; i + 2 < count; i += 3) {
        unsigned ia = idx ? idx[i] : (unsigned)(first + i), ib = idx ? idx[i + 1] : (unsigned)(first + i + 1),
                 ic = idx ? idx[i + 2] : (unsigned)(first + i + 2);
        uint8_t ca = sCode[ia], cb = sCode[ib], cc = sCode[ic];
        if (ca & cb & cc) continue;
        uint8_t fa = sFogged[ia], fb = sFogged[ib], fc = sFogged[ic];
        if (fa & fb & fc & 2) continue;                 /* lost in the fog */
        int fogged = (fa | fb | fc) != 0;
        if (!(ca | cb | cc)) {
            float ax = sSX[ia], ay = sSY[ia];
            float area2 = (sSX[ib] - ax) * (sSY[ic] - ay) - (sSX[ic] - ax) * (sSY[ib] - ay);    /* pixels, y up */
            if (cull && area2 >= 0.f) continue;         /* front faces are clockwise */
            if (area2 < 0.f) area2 = -area2;
            if (!(area2 >= min_area2)) { fe_stat_tiny++; continue; }    /* also rejects NaN */
            Batch *bt = target(&s, fogged);
            OutVtx *v = bt ? reserve(bt) : NULL;
            if (!v) continue;
            v[0] = sOut[ia]; v[1] = sOut[ib]; v[2] = sOut[ic];
            fe_stat_fogged += fogged;
            continue;
        }
        /* crosses the edge of the view: clip to every plane it crosses */
        Vtx bufa[10], bufb[10], *in = bufa, *out = bufb;
        uint8_t any = ca | cb | cc;
        int k = 3;
        unpack(&in[0], &sOut[ia]); unpack(&in[1], &sOut[ib]); unpack(&in[2], &sOut[ic]);
        for (int bit = 0; bit < 6 && k >= 3; bit++) {
            if (!(any & (1 << bit))) continue;
            k = clip_plane(in, k, out, bit);
            Vtx *t = in; in = out; out = t;
        }
        fe_stat_clipped++;
        if (k < 3) continue;
        float sx[10], sy[10];
        for (int j = 0; j < k; j++) {
            float inv = 1.f / in[j].w;
            sx[j] = in[j].x * inv * hw;
            sy[j] = in[j].y * inv * hh;
        }
        for (int j = 1; j + 1 < k; j++) {
            float area2 = (sx[j] - sx[0]) * (sy[j + 1] - sy[0]) - (sx[j + 1] - sx[0]) * (sy[j] - sy[0]);
            if (cull && area2 >= 0.f) continue;
            if (area2 < 0.f) area2 = -area2;
            if (!(area2 >= min_area2)) { fe_stat_tiny++; continue; }
            Batch *bt = target(&s, fogged);
            OutVtx *v = bt ? reserve(bt) : NULL;
            if (!v) continue;
            const Vtx *tri[3] = { &in[0], &in[j], &in[j + 1] };
            for (int t = 0; t < 3; t++) {
                const Vtx *p = tri[t];
                v[t].x = p->x; v[t].y = p->y; v[t].z = p->z; v[t].w = p->w;
                v[t].u0 = p->u0; v[t].v0 = p->v0; v[t].u1 = p->u1; v[t].v1 = p->v1;
                v[t].color = byte_of(p->r) | byte_of(p->g) << 8 | byte_of(p->b) << 16 | byte_of(p->a) << 24;
            }
            fe_stat_fogged += fogged;
        }
    }
}

static void draw(int count, const uint16_t *idx, int first) {
    uint64_t t0 = plat_time_us();
    draw_triangles(count, idx, first);
    fe_time_transform_us += (unsigned)(plat_time_us() - t0);
}

void fe_draw_elements(uint32_t mode, int count, uint32_t type, uint32_t indices) {
    if (mode != 4 || type != 0x1403) return;            /* the engine only draws indexed triangle lists */
    const uint8_t *base;
    if (sElementBuffer) {
        Buffer *b = buffer(sElementBuffer);
        if (!b || !b->data) return;
        base = b->data + indices;
    } else base = g_mem + indices;
    draw(count, (const uint16_t *)(const void *)base, 0);
}
void fe_draw_arrays(uint32_t mode, int first, int count) { if (mode == 4) draw(count, NULL, first); }

/* ---- frame ------------------------------------------------------------------------------- */
void fe_frame_begin(int panel_w, int panel_h) {
    sHalfW = (float)panel_w * 0.5f;
    sHalfH = (float)panel_h * 0.5f;
    sBatchCount = sChunksUsed = 0;
    sOrderedOpen = -1;
    fe_stat_draws = fe_stat_vertices_in = fe_stat_triangles_in = fe_stat_triangles_out = fe_stat_tiny = 0;
    fe_stat_clipped = fe_stat_dropped = fe_stat_calls = fe_stat_fogged = 0;
    fe_time_transform_us = fe_time_submit_us = 0;
    uint64_t t0 = plat_time_us();
    glViewport(0, 0, panel_w, panel_h);
    glClearColor(sClear[0], sClear[1], sClear[2], sClear[3]);
    glClearDepthf(1.f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    fe_time_clear_us = (unsigned)(plat_time_us() - t0);
    fe_time_first_draw_us = 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glShadeModel(GL_SMOOTH);
}

static void draw_batch(const Batch *b, int index) {
    CRUMB("batch", (uint32_t)(index << 16) | (uint32_t)(b->tex1 ? 2 : 1) | (uint32_t)b->kind << 8);
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    if (b->tex1) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, b->tex1);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    if (b->tex0) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, b->tex0);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    if (b->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (b->depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(b->depth_mask ? GL_TRUE : GL_FALSE);
    for (int k = 0; k < b->chunks; k++) {
        int n = k + 1 == b->chunks ? b->last : CHUNK_VERTS;
        if (!n) continue;
        CRUMB("arrays", (uint32_t)(b->chunk[k] << 16) | (uint32_t)n);
        if (!fe_stat_draws) {
            uint64_t t0 = plat_time_us();
            glDrawArrays(GL_TRIANGLES, b->chunk[k] * CHUNK_VERTS, n);
            fe_time_first_draw_us = (unsigned)(plat_time_us() - t0);
        } else
        glDrawArrays(GL_TRIANGLES, b->chunk[k] * CHUNK_VERTS, n);
        fe_stat_draws++;
        fe_stat_triangles_out += n / 3;
    }
}

static void submit(void);
void fe_frame_end(void) {
    uint64_t t0 = plat_time_us();
    submit();
    fe_time_submit_us = (unsigned)(plat_time_us() - t0);
}
static void submit(void) {
    if (!sChunksUsed) return;
#ifndef AB_NANO
    for (int i = 0; i < sBatchCount; i++) {             /* host check: nothing given to GL may touch a clip boundary */
        const Batch *bt = &sBatches[i];
        for (int k = 0; k < bt->chunks; k++) {
            int n = k + 1 == bt->chunks ? bt->last : CHUNK_VERTS;
            for (int j = 0; j < n; j++) {
                const OutVtx *v = &sPool[bt->chunk[k] * CHUNK_VERTS + j];
                if (!(v->w > 0.f && v->x > -v->w && v->x < v->w && v->y > -v->w && v->y < v->w && v->z > -v->w && v->z < v->w))
                    fe_stat_outside++;
            }
        }
    }
#endif
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_CULL_FACE);                            /* culled above */
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);        /* the engine's colours are premultiplied */
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    const unsigned char *base = (const unsigned char *)sPool;
    glVertexPointer(4, GL_FLOAT, sizeof(OutVtx), base + offsetof(OutVtx, x));
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(OutVtx), base + offsetof(OutVtx, color));
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    glTexCoordPointer(2, GL_FLOAT, sizeof(OutVtx), base + offsetof(OutVtx, u1));
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glTexCoordPointer(2, GL_FLOAT, sizeof(OutVtx), base + offsetof(OutVtx, u0));
    for (int kind = OPAQUE; kind <= ORDERED; kind++)
        for (int i = 0; i < sBatchCount; i++)
            if (sBatches[i].kind == kind) draw_batch(&sBatches[i], i);
    CRUMB("drawn-all", fe_stat_triangles_out);
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

/* A new GL view starts with none of this file's GL state; the engine's state is kept. */
void fe_reset(void) {
    for (int i = 0; i < 8; i++) memcpy(sModelView[i], kIdentity, sizeof kIdentity);
    for (int i = 0; i < 2; i++) {
        memcpy(sProjection[i], kIdentity, sizeof kIdentity);
        memcpy(sTexture[0][i], kIdentity, sizeof kIdentity);
        memcpy(sTexture[1][i], kIdentity, sizeof kIdentity);
    }
}
