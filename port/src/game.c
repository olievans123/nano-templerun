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

#ifdef AB_NANO
extern void port_crumb(const char *tag, uint32_t a, uint32_t b);   /* RAM trail that survives a reboot */
#define CRUMB(tag) port_crumb(tag, 0, 0)
#else
#define CRUMB(tag) ((void)0)
#endif

unsigned fe_time_draw_us;       /* the last frame's time in the engine's draw(), including the port's transform */
static uint32_t sGame, sScratch;
static uint32_t fSimulate, fDraw, fStart, fRestart, fTouchBegan, fTouchMoved, fTouchEnded, fTilt, fIsGameOver,
                fIsGameOverFinished, fGetScore, fGetCoins, fGetDistance, fIsPaused, fUnpause;
static int sW, sH, sState, sTouching;
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
    if (sState != GAME_RUNNING) {
        if (phase == 2 && sTouching) begin_run();
        sTouching = phase != 2;
        return;
    }
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
    if (sState == GAME_RUNNING && rt_invoke(fIsGameOverFinished, 1, sGame)) sState = GAME_OVER;
    fe_time_engine_us = (uint32_t)(plat_time_us() - t0);
    CRUMB("clear");
    fe_frame_begin(sW, sH);
    CRUMB("draw");
    t0 = plat_time_us();
    rt_invoke(fDraw, 1, sGame);
    fe_time_draw_us = (uint32_t)(plat_time_us() - t0);
    CRUMB("submit");
    fe_frame_end();
}
