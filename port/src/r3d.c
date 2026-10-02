/* r3d.c — see r3d.h. */
#include <string.h>
#include "gl.h"
#include "platform.h"
#include "r3d.h"

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif

#define MAX_MESH_VERTS 2048
#define BATCHES        6
#define BATCH_VERTS    3072             /* a multiple of 3 */
#define NEAR_W         1.0f             /* clip at the camera's near plane (w = near distance) */

typedef struct { float x, y, z, w, u0, v0, u1, v1; } Vtx;
typedef struct { unsigned tex0, tex1; int count; Vtx v[BATCH_VERTS]; } Batch;

int r3d_stat_draws, r3d_stat_vertices_in, r3d_stat_triangles_in, r3d_stat_triangles_out, r3d_stat_tiny,
    r3d_stat_clipped;
static Batch sBatch[BATCHES];
static float sViewProj[16], sHalfW, sHalfH;
static Vtx sClip[MAX_MESH_VERTS];
static float sSX[MAX_MESH_VERTS], sSY[MAX_MESH_VERTS];
static unsigned char sCode[MAX_MESH_VERTS];

static void multiply(float out[16], const float a[16], const float b[16]) {      /* out = a * b */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

void r3d_init(void) {
    for (int i = 0; i < BATCHES; i++) sBatch[i].count = 0;
}

void r3d_begin(int panel_w, int panel_h, const float projection[16], const float view[16]) {
    multiply(sViewProj, projection, view);
    sHalfW = (float)panel_w * 0.5f;
    sHalfH = (float)panel_h * 0.5f;
    r3d_stat_draws = r3d_stat_vertices_in = r3d_stat_triangles_in = r3d_stat_triangles_out = 0;
    r3d_stat_tiny = r3d_stat_clipped = 0;
    for (int i = 0; i < BATCHES; i++) sBatch[i].count = 0;
}

static void flush(Batch *b) {
    if (!b->count) return;
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    if (b->tex1) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, b->tex1);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vtx), &b->v[0].u1);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, b->tex0);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, sizeof(Vtx), &b->v[0].u0);
    glVertexPointer(4, GL_FLOAT, sizeof(Vtx), &b->v[0].x);
    glDrawArrays(GL_TRIANGLES, 0, b->count);
    r3d_stat_draws++;
    r3d_stat_triangles_out += b->count / 3;
    b->count = 0;
}

static Batch *batch_for(unsigned tex0, unsigned tex1) {
    Batch *free_one = NULL;
    for (int i = 0; i < BATCHES; i++) {
        Batch *b = &sBatch[i];
        if (b->count && b->tex0 == tex0 && b->tex1 == tex1) return b;
        if (!b->count && !free_one) free_one = b;
    }
    if (!free_one) {                    /* all in use by other textures: send the fullest */
        free_one = &sBatch[0];
        for (int i = 1; i < BATCHES; i++) if (sBatch[i].count > free_one->count) free_one = &sBatch[i];
        flush(free_one);
    }
    free_one->tex0 = tex0;
    free_one->tex1 = tex1;
    return free_one;
}

/* One triangle wholly in front of the near plane: cull, guard, emit. */
static void emit(Batch **pb, unsigned tex0, unsigned tex1, const Vtx *a, const Vtx *b, const Vtx *c,
                 float ax, float ay, float bx, float by, float cx, float cy, int cull) {
    float area2 = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);       /* pixels, y up */
    if (cull && area2 >= 0.f) return;                                   /* front faces are clockwise */
    if (area2 < 0.f) area2 = -area2;
    if (!(area2 >= 0.25f)) { r3d_stat_tiny++; return; }                 /* also rejects NaN */
    Batch *bt = *pb;
    if (bt->count + 3 > BATCH_VERTS) { flush(bt); bt = *pb = batch_for(tex0, tex1); }
    bt->v[bt->count++] = *a;
    bt->v[bt->count++] = *b;
    bt->v[bt->count++] = *c;
}

static void lerp(Vtx *o, const Vtx *a, const Vtx *b) {                  /* where the edge meets w = NEAR_W */
    float t = (NEAR_W - a->w) / (b->w - a->w);
    o->x = a->x + (b->x - a->x) * t;  o->y = a->y + (b->y - a->y) * t;
    o->z = a->z + (b->z - a->z) * t;  o->w = NEAR_W;
    o->u0 = a->u0 + (b->u0 - a->u0) * t;  o->v0 = a->v0 + (b->v0 - a->v0) * t;
    o->u1 = a->u1 + (b->u1 - a->u1) * t;  o->v1 = a->v1 + (b->v1 - a->v1) * t;
}

void r3d_mesh(const TRMesh *m, int frame, const float model[16], unsigned tex0, unsigned tex1, int cull) {
    const TRFrame *f = &m->frames[frame];
    int n = f->vertex_count;
    if (n > MAX_MESH_VERTS || !tex0) return;
    float mvp[16];
    multiply(mvp, sViewProj, model);
    const unsigned char *src = m->vertices + (size_t)f->vertex_start * (size_t)m->vertex_size;
    int two = tex1 && m->uv_count > 1;
    if (!two) tex1 = 0;
    unsigned char all = 0xff;
    for (int i = 0; i < n; i++, src += m->vertex_size) {
        const float *p = (const float *)(const void *)(src + m->pos_offset);
        const float *t0 = (const float *)(const void *)(src + m->uv_offset[0]);
        Vtx *o = &sClip[i];
        float x = p[0], y = p[1], z = p[2];
        o->x = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
        o->y = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
        o->z = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
        o->w = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
        o->u0 = t0[0]; o->v0 = t0[1];
        if (two) {
            const float *t1 = (const float *)(const void *)(src + m->uv_offset[1]);
            o->u1 = t1[0]; o->v1 = t1[1];
        } else {
            o->u1 = o->v1 = 0.f;
        }
        unsigned char code = 0;
        if (o->w < NEAR_W) code |= 16;
        if (o->x < -o->w) code |= 1;
        if (o->x > o->w) code |= 2;
        if (o->y < -o->w) code |= 4;
        if (o->y > o->w) code |= 8;
        if (o->z > o->w) code |= 32;
        sCode[i] = code;
        all &= code;
        if (!(code & 16)) {
            float inv = 1.f / o->w;
            sSX[i] = o->x * inv * sHalfW;
            sSY[i] = o->y * inv * sHalfH;
        }
    }
    r3d_stat_vertices_in += n;
    r3d_stat_triangles_in += f->index_count / 3;
    if (all) return;                                    /* the whole mesh is off one side */
    Batch *b = batch_for(tex0, tex1);
    const uint16_t *idx = m->indices + f->index_start;
    for (int i = 0; i + 2 < f->index_count; i += 3) {
        unsigned ia = idx[i], ib = idx[i + 1], ic = idx[i + 2];
        if (ia >= (unsigned)n || ib >= (unsigned)n || ic >= (unsigned)n) continue;
        unsigned char ca = sCode[ia], cb = sCode[ib], cc = sCode[ic];
        if (ca & cb & cc) continue;                     /* off one side of the view (or behind) */
        if (!((ca | cb | cc) & 16)) {
            emit(&b, tex0, tex1, &sClip[ia], &sClip[ib], &sClip[ic], sSX[ia], sSY[ia], sSX[ib], sSY[ib], sSX[ic], sSY[ic], cull);
            continue;
        }
        /* crosses the near plane: clip to it (a triangle gives a triangle or a quad) */
        const Vtx *in[3] = { &sClip[ia], &sClip[ib], &sClip[ic] };
        Vtx poly[4];
        int count = 0;
        for (int k = 0; k < 3; k++) {
            const Vtx *p = in[k], *q = in[(k + 1) % 3];
            int pin = p->w >= NEAR_W, qin = q->w >= NEAR_W;
            if (pin) poly[count++] = *p;
            if (pin != qin) lerp(&poly[count++], p, q);
        }
        r3d_stat_clipped++;
        if (count < 3) continue;
        float sx[4], sy[4];
        for (int k = 0; k < count; k++) {
            float inv = 1.f / poly[k].w;
            sx[k] = poly[k].x * inv * sHalfW;
            sy[k] = poly[k].y * inv * sHalfH;
        }
        emit(&b, tex0, tex1, &poly[0], &poly[1], &poly[2], sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], cull);
        if (count == 4)
            emit(&b, tex0, tex1, &poly[0], &poly[2], &poly[3], sx[0], sy[0], sx[2], sy[2], sx[3], sy[3], cull);
    }
}

void r3d_end(void) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_CULL_FACE);                            /* culled above */
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    for (int i = 0; i < BATCHES; i++) flush(&sBatch[i]);
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
}
