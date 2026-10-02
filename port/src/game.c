/* game.c — the shell around the translated engine: what -[MainGameView ...] did on the phone.
 *
 * The engine (cGameController and everything under it) is the original code. The phone app
 * wrapped it in UIKit views for the title, pause and game-over screens; those are not part of
 * the engine, so the shell supplies its own, and drives the engine through the same calls the
 * view made: initalize / loadLevelInformation / start / simulate / draw, the touch handlers,
 * and the accelerometer. */
#include "game.h"
#include "glfe.h"
#include "platform.h"
#include "rt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned rt_host_load_texture(const char *name, const char *file, int repeat);
static void load_screens(void);
unsigned rt_texture_host(uint32_t id);

#ifdef AB_NANO
extern void port_crumb(const char *tag, uint32_t a, uint32_t b);   /* RAM trail that survives a reboot */
#define CRUMB(tag) port_crumb(tag, 0, 0)
#else
#define CRUMB(tag) ((void)0)
#endif

unsigned fe_time_draw_us;       /* the last frame's time in the engine's draw(), including the port's transform */
#define TESTS 5
#define TEST_SPACING 12
static unsigned sTest[TESTS];
#ifdef AB_NANO
static void port_test_mark(int test) { port_crumb("test", (uint32_t)test, 0); }
#else
static void port_test_mark(int test) { (void)test; }
#endif
static uint32_t sGame, sScratch;
static uint32_t fSimulate, fDraw, fStart, fRestart, fTouchBegan, fTouchMoved, fTouchEnded, fTilt, fIsGameOver,
                fIsGameOverFinished, fGetScore, fGetCoins, fGetDistance, fIsPaused, fUnpause;
static int sW, sH, sState, sTouching, sRuns, sWaitLift;
static uint32_t fGetDeathType;
static float sTilt;
/* The engine sizes its on-screen display for a screen 320 points wide; it has one scale for
 * that display (1 on the phone, 2 on the iPad), which the shell sets for the panel's width. */
#define DISPLAY_SCALE_OFFSET 0x10

int game_init(int panel_w, int panel_h, uint32_t heap_bytes, uint32_t seed) {
    sW = panel_w; sH = panel_h;
    fe_reset();
    CRUMB("rt");
    if (!rt_init(heap_bytes)) return 0;
    rt_srandom(seed);
    fSimulate = rt_lookup("__ZN15cGameController8simulateEf");
    fDraw = rt_lookup("__ZN15cGameController4drawEv");
    fStart = rt_lookup("__ZN15cGameController5startEv");
    fRestart = rt_lookup("__ZN15cGameController11restartGameEv");
    fTouchBegan = rt_lookup("__ZN15cGameController16handleTouchBeganE4vec2ii");
    fTouchMoved = rt_lookup("__ZN15cGameController16handleTouchMovedE4vec2ii");
    fTouchEnded = rt_lookup("__ZN15cGameController16handleTouchEndedE4vec2ii");
    fTilt = rt_lookup("__ZN15cGameController24handleAccelerometerForceERK4vec3");
    fIsGameOver = rt_lookup("__ZNK15cGameController10isGameOverEv");
    fIsGameOverFinished = rt_lookup("__ZNK15cGameController18isGameOverFinishedEv");
    fGetScore = rt_lookup("__ZNK15cGameController8getScoreEv");
    fGetCoins = rt_lookup("__ZNK15cGameController17getCoinsCollectedEv");
    fGetDistance = rt_lookup("__ZNK15cGameController14getDistanceRunEv");
    fIsPaused = rt_lookup("__ZNK15cGameController8isPausedEv");
    fUnpause = rt_lookup("__ZN15cGameController7unpauseEv");
    fGetDeathType = rt_lookup("__ZNK15cGameController12getDeathTypeEv");
    CRUMB("ctor");
    sGame = rt_alloc(596);
    rt_invoke(rt_lookup("__ZN15cGameControllerC1Efffb"), 5, sGame, rt_fbits((float)panel_w), rt_fbits((float)panel_h), rt_fbits(1.0f), 0u);
    MF(sGame + DISPLAY_SCALE_OFFSET) = (float)panel_w / 320.0f;
    sScratch = rt_alloc(32);
    CRUMB("engine");
    rt_invoke(rt_lookup("__ZN15cGameController9initalizeEv"), 1, sGame);
    CRUMB("level");
    rt_invoke(rt_lookup("__ZN15cGameController20loadLevelInformationEv"), 1, sGame);
    rt_invoke(fSimulate, 2, sGame, rt_fbits(0.01f));
    load_screens();
    CRUMB("ready");
    sState = GAME_TITLE;
    return 1;
}

int game_state(void) { return sState; }
int game_score(void) { return (int)rt_invoke(fGetScore, 1, sGame); }
int game_coins(void) { return (int)rt_invoke(fGetCoins, 1, sGame); }
int game_distance(void) { return (int)rt_invoke(fGetDistance, 1, sGame); }

static void begin_run(void) {
    if (sState == GAME_OVER) {
        rt_invoke(fRestart, 1, sGame);
        rt_invoke(fSimulate, 2, sGame, rt_fbits(0.01f));
    }
    rt_invoke(fStart, 1, sGame);
    sState = GAME_RUNNING;
}

void game_touch(int phase, float x, float y) {
    if (sState != GAME_RUNNING) {                       /* any touch starts a run, once the last one has lifted */
        if (phase == 0 && !sWaitLift) { begin_run(); sWaitLift = 1; }
        if (phase == 2) sWaitLift = 0;
        sTouching = 0;
        return;
    }
    if (sWaitLift) { if (phase == 2) sWaitLift = 0; return; }       /* the finger that started the run */
    if (phase == 2 && rt_invoke(fIsPaused, 1, sGame)) {            /* any tap resumes */
        rt_invoke(fUnpause, 1, sGame);
        sTouching = 0;
        return;
    }
    MF(sScratch) = x;
    MF(sScratch + 4) = (float)sH - y;               /* the engine's y runs up from the bottom edge */
    rt_invoke(phase == 0 ? fTouchBegan : phase == 1 ? fTouchMoved : fTouchEnded, 4, sGame, sScratch, 1u, 1u);
    sTouching = phase != 2;
}

void game_tilt(float x) { sTilt = x; }
static int sDrawing = 1, sWarm;
unsigned rt_texture_host(uint32_t id);
void game_set_drawing(int on) { sDrawing = on; }

/* ---- autopilot: plays through the same touch calls a finger makes ---------------------------- */
#define PLAYER_OFFSET 0x198
#define PATH_OFFSET 0x1b8
#define PATH_DIRECTION_OFFSET 0x1bc
static int sAuto, sAutoWait, sAutoSwipe, sAutoStep;
static float sAutoDX, sAutoDY;
static uint32_t fOverTurn, fNearest, fCanLeft, fCanRight, sAutoTurned;
void game_autopilot(int on) { sAuto = on; }

static void auto_swipe(float dx, float dy) { sAutoDX = dx; sAutoDY = dy; sAutoSwipe = 1; sAutoStep = 0; }

static void autopilot(void) {
    if (!fOverTurn) {
        fOverTurn = rt_lookup("__ZN12cPathElement29getPathElementIfPointOverTurnERK4vec2f15t_pathDirectionb");
        fNearest = rt_lookup("__ZN12cPathElement21getNearestPathElementERK4vec215t_pathDirectionbi");
        fCanLeft = rt_lookup("__ZNK12cPathElement11canTurnLeftE15t_pathDirection");
        fCanRight = rt_lookup("__ZNK12cPathElement12canTurnRightE15t_pathDirection");
    }
    if (sState != GAME_RUNNING) {                       /* tap to start, or to run again */
        if (++sAutoWait == 45) game_touch(0, (float)sW * 0.5f, (float)sH * 0.5f);
        if (sAutoWait == 47) { game_touch(2, (float)sW * 0.5f, (float)sH * 0.5f); sAutoWait = 0; }
        return;
    }
    if (sAutoSwipe) {                                   /* a swipe takes four frames */
        float cx = (float)sW * 0.5f, cy = (float)sH * 0.5f, t = (float)sAutoStep / 2.0f;
        if (sAutoStep == 0) game_touch(0, cx, cy);
        else if (sAutoStep <= 2) game_touch(1, cx + sAutoDX * t, cy + sAutoDY * t);
        else { game_touch(2, cx + sAutoDX, cy + sAutoDY); sAutoSwipe = 0; }
        sAutoStep++;
        return;
    }
    uint32_t player = M32(sGame + PLAYER_OFFSET), root = M32(sGame + PATH_OFFSET), dir = M32(sGame + PATH_DIRECTION_OFFSET);
    if (!player || !root || M8(player + 0xec)) return;
    float x = MF(player + 4), z = MF(player + 12), fx = MF(player + 0x28), fz = MF(player + 0x30);
    float vx = MF(player + 0x9c), vz = MF(player + 0xa4), speed = sqrtf(vx * vx + vz * vz);
    /* the direction of travel as the engine numbers it: 1 toward -z, 2 toward +x, 3 toward +z, 4 toward -x */
    uint32_t heading = fabsf(fz) >= fabsf(fx) ? (fz < 0.0f ? 1u : 3u) : (fx > 0.0f ? 2u : 4u);
    MF(sScratch + 20) = x; MF(sScratch + 24) = z;
    uint32_t turn = rt_invoke(fOverTurn, 5, root, sScratch + 20, rt_fbits(14.0f), dir, 1u);
    if (turn && turn != sAutoTurned) {
        int left = (int)rt_invoke(fCanLeft, 2, turn, heading), right = (int)rt_invoke(fCanRight, 2, turn, heading);
        if (left || right) {
            sAutoTurned = turn;
            auto_swipe(left && (!right || (rt_random_calls() & 1)) ? -90.0f : 90.0f, 0.0f);
            return;
        }
    }
    if (M8(player + 0x110) || M8(player + 0x118) || M8(player + 0x108)) return;     /* jumping, sliding or falling */
    float ahead = speed * 0.30f + 10.0f;
    MF(sScratch + 20) = x + fx * ahead; MF(sScratch + 24) = z + fz * ahead;
    uint32_t next = rt_invoke(fNearest, 5, root, sScratch + 20, dir, 1u, 8u);
    if (next && M8(next + 0x110)) {
        uint32_t kind = M32(next + 0x114);
        float dx = MF(next + 4) - x, dz = MF(next + 12) - z, along = dx * fx + dz * fz;
        if (along > 0.0f && along < ahead + 6.0f) {
            if (kind == 5 || kind == 6) auto_swipe(0.0f, 90.0f);        /* under the tree or the fire */
            else if (kind == 1 || kind == 3 || kind == 4) auto_swipe(0.0f, -90.0f);
        }
    }
}

/* ---- the port's own screens ---------------------------------------------------------------------
 * On the phone the title, pause and game-over screens were UIKit views over the game view.
 * These are drawn from the same pictures (packed by tools/convert_assets.py into uiSheet,
 * with ui.txt saying where each one is), sized for a panel 240 wide as the phone's were for 320. */
typedef struct { char name[16]; float x, y, w, h; } Sprite;
static Sprite sSprites[40];
static int sSpriteCount;
static unsigned sUiTexture;
#define SHEET 1024.0f
#define WHITE 0xffffffffu

static void load_screens(void) {
    uint32_t size = 0;
    char *text = plat_read_file("ui.txt", &size, 0);
    if (!text) return;
    for (uint32_t i = 0; i < size && sSpriteCount < 40;) {
        Sprite *sp = &sSprites[sSpriteCount];
        uint32_t n = 0;
        while (i < size && text[i] != ' ' && text[i] != '\n' && n < 15) sp->name[n++] = text[i++];
        sp->name[n] = 0;
        float value[4] = { 0, 0, 0, 0 };
        for (int k = 0; k < 4; k++) {
            while (i < size && text[i] == ' ') i++;
            while (i < size && text[i] >= '0' && text[i] <= '9') value[k] = value[k] * 10.0f + (float)(text[i++] - '0');
        }
        while (i < size && text[i] != '\n') i++;
        i++;
        if (n && value[2] > 0.0f) { sp->x = value[0]; sp->y = value[1]; sp->w = value[2]; sp->h = value[3]; sSpriteCount++; }
    }
    free(text);
    sUiTexture = rt_host_load_texture("uiSheet", "uiSheet.png", 0);
    sTest[0] = rt_texture_host(10);     /* wallTexture: the kind the test scene proved */
    sTest[3] = rt_texture_host(5);      /* fontCountdownTexture */
    sTest[4] = sUiTexture;
    sTest[1] = rt_host_load_texture("testMip256", "testMip256.png", 0);
    sTest[2] = rt_host_load_texture("testMip1024", "testMip1024.png", 0);
}

static const Sprite *sprite(const char *name) {
    for (int i = 0; i < sSpriteCount; i++) if (!strcmp(sSprites[i].name, name)) return &sSprites[i];
    return NULL;
}

/* Draw a sprite `width` panel pixels wide with its top left at (x, y); returns its height. */
static float picture(const char *name, float x, float y, float width, uint32_t colour) {
    const Sprite *sp = sprite(name);
    if (!sp || !sUiTexture) return 0.0f;
    float height = width * sp->h / sp->w;
    fe_overlay(sUiTexture, x, y, width, height, sp->x / SHEET, sp->y / SHEET, (sp->x + sp->w) / SHEET, (sp->y + sp->h) / SHEET, colour);
    return height;
}

/* A number with thousands separators, centred on cx; `scale` is panel pixels per sheet pixel. */
static void number(int value, const char *suffix, float cx, float y, float scale, uint32_t colour) {
    char digits[16], text[24];
    int n = 0, len = 0;
    if (value < 0) value = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value && n < 12);
    for (int i = n - 1; i >= 0; i--) { text[len++] = digits[i]; if (i && i % 3 == 0) text[len++] = ','; }
    for (; suffix && *suffix && len < 22; suffix++) text[len++] = *suffix;
    float width = 0.0f;
    char name[12];
    for (int i = 0; i < len; i++) {
        snprintf(name, sizeof name, text[i] == ',' ? "glyphComma" : "glyph%c", text[i]);
        const Sprite *sp = sprite(name);
        if (sp) width += sp->w * scale;
    }
    float x = cx - width * 0.5f;
    for (int i = 0; i < len; i++) {
        snprintf(name, sizeof name, text[i] == ',' ? "glyphComma" : "glyph%c", text[i]);
        const Sprite *sp = sprite(name);
        if (!sp) continue;
        picture(name, x, y, sp->w * scale, colour);
        x += sp->w * scale;
    }
}

static void draw_screens(void) {
    float w = (float)sW, h = (float)sH, k = w / 320.0f;        /* the phone's layouts were 320 wide */
    static const char *const death[8] = { "deathTree", "deathFallA", "deathTree", "deathSlide", "deathLedge", "deathBurnt",
                                          "deathEaten", "deathTangle" };
    if (sState == GAME_TITLE) {
        picture("logo", 0.0f, 12.0f * k, w, WHITE);
        picture("idol", (w - 170.0f * k) * 0.5f, h - 190.0f * k, 170.0f * k, WHITE);
    } else if (sState == GAME_OVER) {
        float pw = 300.0f * k, px = (w - pw) * 0.5f, py = 30.0f * k, y;
        uint32_t ink = 0xff0c2038u;                             /* dark brown: bytes r, g, b, a */
        picture("panel", px, py, pw, WHITE);
        uint32_t type = rt_invoke(fGetDeathType, 1, sGame);
        const char *art = type == 1 && (sRuns & 1) ? "deathWater" : death[type < 8 ? type : 0];
        y = py + 26.0f * k;
        y += picture(art, (w - 228.0f * k) * 0.5f, y, 228.0f * k, WHITE) + 14.0f * k;
        y += picture("score", (w - 200.0f * k) * 0.5f, y, 200.0f * k, WHITE) + 6.0f * k;
        number(game_score(), NULL, w * 0.5f, y, 1.0f * k, ink);
        y += 52.0f * k;
        number(game_distance(), "m", w * 0.5f, y, 0.62f * k, ink);
        y += 34.0f * k;
        float cw = 22.0f * k;
        picture("coin", w * 0.5f - 44.0f * k, y + 3.0f * k, cw, WHITE);
        number(game_coins(), NULL, w * 0.5f + 14.0f * k, y, 0.62f * k, ink);
        picture("runAgain", (w - 270.0f * k) * 0.5f, py + 494.0f * 300.0f / 320.0f * k - 92.0f * k, 270.0f * k, WHITE);
    } else if (rt_invoke(fIsPaused, 1, sGame)) {
        picture("paused", (w - 220.0f * k) * 0.5f, 120.0f * k, 220.0f * k, WHITE);
        picture("resume", (w - 270.0f * k) * 0.5f, 220.0f * k, 270.0f * k, WHITE);
    }
}

void game_frame(float dt) {
    if (dt > 0.25f) dt = 0.25f;                                     /* as -[EAGLView mainEventLoop] */
    if (dt <= 0.0f) dt = 0.001f;
    if (sAuto) autopilot();
    if (sState == GAME_RUNNING) {
        MF(sScratch + 8) = sTilt; MF(sScratch + 12) = 0.0f; MF(sScratch + 16) = 0.0f;
        rt_invoke(fTilt, 2, sGame, sScratch + 8);
    }
    uint64_t t0 = plat_time_us();
    rt_invoke(fSimulate, 2, sGame, rt_fbits(dt));
    if (sState == GAME_RUNNING && rt_invoke(fIsGameOverFinished, 1, sGame)) { sState = GAME_OVER; sRuns++; }
    fe_time_engine_us = (uint32_t)(plat_time_us() - t0);
    plat_poll();
    CRUMB("clear");
    fe_frame_begin(sW, sH);
    if (!sDrawing) return;
    if (sWarm < TESTS * TEST_SPACING) {
        /* Hardware test at start-up: the iPod rebooted about 25 ms after the first rectangle
         * drawn with one of the sprite sheets. One texture of each kind is shown in turn, a
         * dozen frames apart, so the trail says which kind the driver cannot take. */
        static const char *const kind[TESTS] = { "wall RGB levels", "256 RGBA levels", "1024 RGBA levels", "256 RGBA single", "1024 RGBA single" };
        int test = sWarm / TEST_SPACING;
        if (sWarm % TEST_SPACING == 0) { plat_log("test %d: %s", test, kind[test]); plat_log_flush(); port_test_mark(test); }
        for (int t = 0; t <= test; t++)
            if (sTest[t]) fe_overlay(sTest[t], 8.0f + 44.0f * (float)t, 8.0f, 40.0f, 40.0f, 0.3f, 0.3f, 0.7f, 0.7f, WHITE);
        sWarm++;
        fe_frame_end();
        return;
    }
    CRUMB("draw");
    t0 = plat_time_us();
    rt_invoke(fDraw, 1, sGame);
    fe_time_draw_us = (uint32_t)(plat_time_us() - t0);
    draw_screens();
    CRUMB("submit");
    fe_frame_end();
}
