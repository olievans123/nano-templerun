/* game_host.c — the port on the Mac with no window: runs the game for a number of frames
 * with a scripted finger and writes screenshots. The drawing goes through the same pipeline
 * as on the iPod (port/src/glfe.c) at the nano's 240 by 432.
 *
 * usage: game_host <iPod data dir> <host texture dir> <out dir> <frames> [seed] [shots every n] */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <zlib.h>
#include <OpenGL/OpenGL.h>
#include "gl.h"
#include <OpenGL/glext.h>
#include "game.h"
#include "glfe.h"
#include "platform.h"
#include "rt.h"

#define W 240
#define H 432
static const char *sDirs[2];

uint64_t plat_time_us(void) { struct timeval tv; gettimeofday(&tv, NULL); return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec; }
void *plat_read_file(const char *name, uint32_t *size, int save) {
    if (save) return NULL;
    for (int i = 0; i < 2; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", sDirs[i], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        void *data = malloc((size_t)n + 1);
        if (fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); return NULL; }
        fclose(f);
        *size = (uint32_t)n;
        return data;
    }
    return NULL;
}
int plat_write_file(const char *name, const void *data, uint32_t size) {
    (void)data;
    if (getenv("TR_RUNS")) fprintf(stderr, "write %s: %u bytes\n", name, (unsigned)size);
    return 0;
}
void plat_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
void plat_poll(void) {}
void plat_log_flush(void) {}
void plat_fatal(const char *message) { fprintf(stderr, "fatal: %s\n", message); exit(2); }
void rt_host_sound(const char *name, int loop, float pitch, int stop) {
    if (getenv("TR_SOUNDS")) fprintf(stderr, "sound %s%s pitch %.2f%s\n", stop ? "stop " : "", name, (double)pitch, loop ? " (loop)" : "");
}

unsigned rt_host_load_texture(const char *name, const char *file, int repeat) {
    char base[64], path[80];
    uint32_t size = 0;
    snprintf(base, sizeof base, "%s", file);
    char *dot = strrchr(base, '.');
    int pvr = dot && !strcmp(dot, ".pvr");
    if (dot) *dot = 0;
    snprintf(path, sizeof path, "%s.rgba", base);
    uint8_t *d = plat_read_file(path, &size, 0);
    if (!d || size < 8) { plat_log("texture %s: %s is missing", name, path); return 0; }
    uint32_t w, h;
    memcpy(&w, d, 4); memcpy(&h, d + 4, 4);
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, pvr ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    if (pvr) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d + 8);
    free(d);
    return t;
}

static void put32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len) {
    unsigned char b[4]; uint32_t crc = crc32(0, (const unsigned char *)type, 4);
    if (len) crc = crc32(crc, data, len);
    put32(b, len); fwrite(b, 1, 4, f); fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    put32(b, crc); fwrite(b, 1, 4, f);
}
static void screenshot(const char *path) {
    unsigned char *px = malloc((size_t)W * H * 4), *raw = malloc((size_t)(W * 3 + 1) * H), hdr[13];
    uLongf zlen = compressBound((uLong)(W * 3 + 1) * H); unsigned char *z = malloc(zlen);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < H; y++) {
        unsigned char *row = raw + (size_t)y * (W * 3 + 1), *src = px + (size_t)(H - 1 - y) * W * 4;
        row[0] = 0;
        for (int x = 0; x < W; x++) { row[1 + x * 3] = src[x * 4]; row[2 + x * 3] = src[x * 4 + 1]; row[3 + x * 3] = src[x * 4 + 2]; }
    }
    compress2(z, &zlen, raw, (uLong)(W * 3 + 1) * H, 6);
    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    put32(hdr, (uint32_t)W); put32(hdr + 4, (uint32_t)H); hdr[8] = 8; hdr[9] = 2; hdr[10] = hdr[11] = hdr[12] = 0;
    chunk(f, "IHDR", hdr, 13); chunk(f, "IDAT", z, (uint32_t)zlen); chunk(f, "IEND", NULL, 0);
    fclose(f); free(px); free(raw); free(z);
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: game_host <iPod data dir> <host texture dir> <out dir> <frames> [seed] [shot every n]\n"); return 2; }
    sDirs[0] = argv[1]; sDirs[1] = argv[2];
    int frames = atoi(argv[4]), every = argc > 6 ? atoi(argv[6]) : 60;
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
                                        kCGLPFADepthSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf; GLint npf; CGLContextObj ctx;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &ctx)) return 1;
    CGLSetCurrentContext(ctx);
    GLuint fbo, rb[2];
    glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(2, rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[0]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, W, H);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb[0]);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[1]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, W, H);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb[1]);
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    if (!game_init(W, H, getenv("TR_HEAP") ? (uint32_t)strtoul(getenv("TR_HEAP"), 0, 0) : 8u << 20, argc > 5 ? (uint32_t)atoi(argv[5]) : 1u)) return 1;
    fprintf(stderr, "loaded: guest heap %u bytes high water, %u in use; buffer objects %u bytes\n", (unsigned)rt_heap_peak(),
            (unsigned)rt_heap_used(), fe_buffer_bytes);
    long tris = 0, tiny = 0, clipped = 0, dropped = 0, fogged = 0, draws = 0;
    int runs = 0, last_state = GAME_TITLE, best = 0, last_over = 0;
    game_autopilot(getenv("TR_NOAUTO") ? 0 : 1);

    uint64_t t0 = plat_time_us();
    for (int f = 0; f < frames; f++) {

        game_frame(1.0f / 30.0f);
        glFinish();
        if (game_state() == GAME_OVER && last_state == GAME_RUNNING) {
            runs++;
            if (game_distance() > best) best = game_distance();
            if (getenv("TR_RUNS")) fprintf(stderr, "run %d ended at frame %d: distance %d, score %d, coins %d\n", runs, f, game_distance(), game_score(), game_coins());
        }
        last_state = game_state();
        tris += fe_stat_triangles_out; tiny += fe_stat_tiny; clipped += fe_stat_clipped; dropped += fe_stat_dropped;
        fogged += fe_stat_fogged; draws += fe_stat_draws;
        if ((every && f % every == every - 1) || (getenv("TR_SHOT_STATES") && (f == 30 || (game_state() == GAME_OVER && last_over != runs && (last_over = runs))))) {
            char path[1024];
            snprintf(path, sizeof path, "%s/game%04d.png", argv[3], f + 1);
            screenshot(path);
        }
    }
    fprintf(stderr, "%d frames in %.2f s: per frame %ld triangles (%ld fogged), %ld draw calls, %ld clipped, %ld tiny, %ld dropped; "
            "%d outside clip bounds; %d runs, best distance %d; state %d score %d coins %d distance %d; heap %u high, %u in use\n",
            frames, (double)(plat_time_us() - t0) / 1e6, tris / frames, fogged / frames, draws / frames, clipped / frames,
            tiny / frames, dropped, fe_stat_outside, runs, best, game_state(), game_score(), game_coins(), game_distance(),
            (unsigned)rt_heap_peak(), (unsigned)rt_heap_used());
    fprintf(stderr, "peaks: %d vertices in one draw, %d chunks of 384 vertices and %d batches in one frame\n", fe_peak_vertices,
            fe_peak_chunks, fe_peak_batches);
    return 0;
}
