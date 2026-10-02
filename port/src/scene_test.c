/* scene_test.c — see scene_test.h. Camera numbers are the game's own defaults
 * (ImangiConfigDefaults.plist): field of view 50, clipping 1..400, follow distance 50,
 * follow height 35, focus height 20; the runner starts at 100 units a second. */
#include <stdlib.h>
#include <string.h>
#include "gl.h"
#include "platform.h"
#include "mesh.h"
#include "tex.h"
#include "scene_test.h"

#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

int scene_stat_draws, scene_stat_vertices, scene_stat_triangles;
static TRMesh *sStraight[3], *sTower, *sTree[2], *sPlayer, *sEnemy;
static unsigned sWall, sLight, sTreeTex, sPlayerTex, sEnemyTex;
static float sDistance, sAnim;

static TRMesh *model(const char *name) {
    uint32_t size = 0;
    uint8_t *d = plat_read_file(name, &size, 0);
    TRMesh *m = d ? mesh_load(d, size) : NULL;
    free(d);
    if (!m) plat_log("model %s: could not be loaded", name);
    return m;
}

int scene_init(void) {
    sStraight[0] = model("templeStraightVert.bksb");
    sStraight[1] = model("templeStraightVert3.bksb");
    sStraight[2] = model("templeStraightVert4.bksb");
    sTower = model("templeStraightTowerVert.bksb");
    sTree[0] = model("treeAlone2.bksb");
    sTree[1] = model("treeAlone3.bksb");
    sPlayer = model("player.bksb");
    sEnemy = model("enemy.bksb");
    if (!sStraight[0] || !sStraight[1] || !sStraight[2] || !sTower || !sTree[0] || !sTree[1] || !sPlayer || !sEnemy)
        return -1;
    sWall = tex_load("wallTexture");
    sLight = tex_load("lightMapTexture");
    sTreeTex = tex_load("treeTexture");
    sPlayerTex = tex_load("playerTexture");
    sEnemyTex = tex_load("enemyTexture");
    plat_log("scene: textures %u %u %u %u %u, %u KiB", sWall, sLight, sTreeTex, sPlayerTex, sEnemyTex, tex_bytes / 1024u);
    return sWall && sPlayerTex ? 0 : -1;
}

static void draw(const TRMesh *m, int frame, unsigned base, unsigned light, float x, float y, float z, float scale) {
    const TRFrame *f = &m->frames[frame];
    const uint8_t *v = m->vertices + (size_t)f->vertex_start * (size_t)m->vertex_size;
    float matrix[16] = { scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0, x, y, z, 1 };
    glVertexPointer(3, GL_FLOAT, m->vertex_size, v + m->pos_offset);
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    if (light && m->uv_count > 1) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, light);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, m->vertex_size, v + m->uv_offset[1]);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, base);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, m->vertex_size, v + m->uv_offset[0]);
    glPushMatrix();
    glMultMatrixf(matrix);
    glDrawElements(GL_TRIANGLES, f->index_count, GL_UNSIGNED_SHORT, m->indices + f->index_start);
    glPopMatrix();
    scene_stat_draws++;
    scene_stat_vertices += f->vertex_count;
    scene_stat_triangles += f->index_count / 3;
}

void scene_frame(int w, int h, float dt, int mode) {
    scene_stat_draws = scene_stat_vertices = scene_stat_triangles = 0;
    sDistance += 100.f * dt;
    sAnim += 12.f * dt;
    if (sAnim >= 12000.f) sAnim -= 12000.f;
    float runner_z = -sDistance;

    glViewport(0, 0, w, h);
    glClearColor(50 / 255.f, 82 / 255.f, 86 / 255.f, 1.f);
    glClearDepthf(1.f);
    glDepthMask(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);                   /* the models' front faces are clockwise */
    glShadeModel(GL_SMOOTH);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float near = 1.f, far = 400.f, top = near * 0.46630766f /* tan(25 deg) */, right = top * (float)w / (float)h;
    glFrustumf(-right, right, -top, top, near, far);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    /* camera 50 behind and 35 above the runner, looking 20 above them */
    float eye[3] = { 0, 35, runner_z + 50 };
    float fwd[3] = { 0, -0.28734788f, -0.95782629f };       /* (0, 20 - 35, -50), normalised */
    /* side = (1,0,0), up = side x fwd */
    float up[3] = { 0, -fwd[2], fwd[1] };
    float view[16] = { 1, up[0], 0, 0, 0, up[1], -fwd[1], 0, 0, up[2], -fwd[2], 0, 0, 0, 0, 1 };
    glMultMatrixf(view);
    glTranslatef(-eye[0], -eye[1], -eye[2]);

    int first = (int)(sDistance / 60.f) - 1;
    for (int i = first; i < first + 8; i++) {
        const TRMesh *m = (i % 5) == 4 ? sTower : sStraight[(unsigned)i % 3u];
        draw(m, 0, sWall, mode == 0 ? sLight : 0, 0, 0, -60.f * (float)i, 1.f);
        if (i & 1) {
            draw(sTree[(unsigned)(i >> 1) & 1u], 0, sTreeTex, mode == 0 ? sLight : 0, (i & 2) ? -45.f : 45.f, 0, -60.f * (float)i, 1.f);
        }
    }
    if (mode != 2) {
        draw(sPlayer, (int)sAnim % 12, sPlayerTex, 0, 0, 0, runner_z, 1.5f);
        for (int i = 0; i < 3; i++)
            draw(sEnemy, ((int)sAnim + i * 5) % sEnemy->frame_count, sEnemyTex, 0, -8.f + 8.f * (float)i, 0,
                 runner_z + 20.f + 4.f * (float)(i & 1), 1.f);
    }
    glActiveTexture(GL_TEXTURE1);
    glClientActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
}
