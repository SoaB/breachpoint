/******************************************************************************************
 * bp_common.h -- BREACH POINT :: shared types, macros and engine-wide constants
 * ----------------------------------------------------------------------------------------
 * Every module includes this first. Nothing in here allocates, nothing in here depends on
 * game logic, so the include graph stays a strict DAG:
 *
 *      bp_common.h  <-  (engine modules)  <-  (game modules)  <-  main.c
 *
 * Design rules enforced across the whole codebase:
 *   1. NO dynamic allocation after boot. Every pool is sized by a MAX_* constant below and
 *      pre-allocated once in bp_boot(). This keeps frame time flat and makes the Web/WASM
 *      build's linear memory footprint deterministic.
 *   2. NO global mutable state outside of the module singletons declared in the headers.
 *   3. Fixed-timestep simulation (SIM_DT) with render interpolation -> identical gameplay
 *      at 30 Hz and 300 Hz, which is a hard requirement for a competitive shooter.
 ******************************************************************************************/
#ifndef BP_COMMON_H
#define BP_COMMON_H

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

/* ====================================================================================== */
/* Build identity                                                                          */
/* ====================================================================================== */
#define BP_GAME_NAME        "BREACH POINT"
#define BP_GAME_TAGLINE     "A high-velocity arena shooter"
#define BP_VERSION_MAJOR    0
#define BP_VERSION_MINOR    9
#define BP_VERSION_PATCH    0
#define BP_VERSION_STRING   "0.9.0"
#define BP_STUDIO           "Nullstate Interactive"

#if defined(PLATFORM_WEB)
    #define BP_PLATFORM_NAME "WebAssembly"
#elif defined(_WIN32)
    #define BP_PLATFORM_NAME "Windows"
#elif defined(__APPLE__)
    #define BP_PLATFORM_NAME "macOS"
#elif defined(__linux__)
    #define BP_PLATFORM_NAME "Linux"
#else
    #define BP_PLATFORM_NAME "Unknown"
#endif

/* ====================================================================================== */
/* Pool capacities -- the "budget" of the game. Tune these, never malloc at runtime.       */
/* ====================================================================================== */
#define MAX_ENTITIES            1024    /* generic pooled entities (props, triggers...)   */
#define MAX_ENEMIES              128    /* live enemies on screen                         */
#define MAX_PROJECTILES          256    /* player + enemy projectiles                     */
#define MAX_HITSCAN_TRACERS       32    /* visible beam tracers                           */
#define MAX_PARTICLES           4096    /* impact sparks, smoke, blood, shells            */
#define MAX_DECALS               256    /* bullet holes / scorch marks on geometry        */
#define MAX_PICKUPS              128    /* health, armour, ammo drops                     */
#define MAX_DAMAGE_NUMBERS       128    /* floating combat text                           */
#define MAX_SFX_VOICES            32    /* concurrent one-shot sounds                     */
#define MAX_MAP_CELLS         128 * 128 /* collision grid                                 */
#define MAX_MAP_SPAWNS          64      /* enemy spawn points                             */
#define MAX_MAP_ARENAS          24      /* arena geometry boxes                           */
#define MAX_WEAPON_DEFS           12    /* weapons loaded from data files                 */
#define MAX_ENEMY_DEFS            16    /* enemy archetypes loaded from data files        */
#define MAX_LEVELS                 8    /* level table entries                            */
#define MAX_MENU_ITEMS            24    /* rows in any single menu                        */
#define MAX_LOG_LINES            256    /* ring buffer of engine/game log                 */
#define MAX_SAVE_SLOTS             4

/* ====================================================================================== */
/* Simulation constants                                                                    */
/* ====================================================================================== */
#define SIM_HZ                  120.0f  /* physics/logic ticks per second                 */
#define SIM_DT              (1.0f / SIM_HZ)
#define MAX_FRAME_DT            0.1f    /* clamp: avoid spiral of death after a hitch     */
#define MAX_SIM_STEPS_PER_FRAME   8

#define GRAVITY                 -28.0f  /* units/s^2 -- snappier than real gravity        */
#define UNIT_SCALE               1.0f   /* 1 world unit == 1 metre                        */

#define PLAYER_EYE_HEIGHT        1.62f
#define PLAYER_RADIUS            0.42f
#define PLAYER_HEIGHT            1.85f
#define PLAYER_MAX_HEALTH       100.0f
#define PLAYER_MAX_ARMOUR       100.0f

/* ====================================================================================== */
/* Small utilities                                                                         */
/* ====================================================================================== */
#define BP_ARRAY_COUNT(a)   ((int)(sizeof(a) / sizeof((a)[0])))
/*
 * MIN / MAX / CLAMP.
 *
 * WARNING: these are plain ternaries and evaluate their arguments up to three times.
 * Never pass an expression with a side effect -- BP_MAX(1, atoi(argv[++i])) increments i
 * twice and reads past the end of argv, which is a segfault with no diagnostic. Assign to a
 * local first. (C11 has no clean way to write these as statement-expressions portably
 * across gcc/clang/emcc without a warning under -Wshadow, so the rule is a convention
 * enforced by review rather than by the compiler.)
 */
#define BP_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define BP_MAX(a, b)        ((a) > (b) ? (a) : (b))
#define BP_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define BP_ABS(a)           ((a) < 0 ? -(a) : (a))
#define BP_SIGN(a)          ((a) < 0 ? -1.0f : 1.0f)
#define BP_UNUSED(x)        ((void)(x))

#define BP_PI               3.14159265358979323846f
#define BP_DEG2RAD          (BP_PI / 180.0f)
#define BP_RAD2DEG          (180.0f / BP_PI)
#define BP_TAU              (BP_PI * 2.0f)

/* Linear interpolation as a function rather than a macro. raylib's own `Lerp` is a macro
 * over doubles, which trips -Wdouble-promotion at every call site and silently promotes
 * the whole expression; this version keeps every simulation step in single precision. */
static inline float bp_lerp(float a, float b, float t) { return a + (b - a) * t; }
#define BP_LERP(a, b, t)    bp_lerp((a), (b), (t))

/* Smooth exponential approach that is frame-rate independent. */
static inline float bp_damp(float current, float target, float lambda, float dt)
{
    return target + (current - target) * expf(-lambda * dt);
}

/* Move `current` toward `target` by at most `maxDelta`. */
static inline float bp_approach(float current, float target, float maxDelta)
{
    float d = target - current;
    if (d >  maxDelta) return current + maxDelta;
    if (d < -maxDelta) return current - maxDelta;
    return target;
}

static inline float bp_lerp_angle(float a, float b, float t)
{
    float d = fmodf(b - a + 540.0f, 360.0f) - 180.0f;
    return a + d * t;
}

/* ====================================================================================== */
/* Logging                                                                                 */
/* ====================================================================================== */
typedef enum {
    BP_LOG_DEBUG = 0,
    BP_LOG_INFO,
    BP_LOG_WARN,
    BP_LOG_ERROR
} bp_log_level;

void bp_log(bp_log_level level, const char *fmt, ...);
const char **bp_log_lines(int *out_count);   /* ring buffer, oldest first */
void bp_log_clear(void);

#define BP_LOGD(...) bp_log(BP_LOG_DEBUG, __VA_ARGS__)
#define BP_LOGI(...) bp_log(BP_LOG_INFO,  __VA_ARGS__)
#define BP_LOGW(...) bp_log(BP_LOG_WARN,  __VA_ARGS__)
#define BP_LOGE(...) bp_log(BP_LOG_ERROR, __VA_ARGS__)

/* ====================================================================================== */
/* Deterministic RNG (xorshift32) -- seeded per run so replays/demos can be reproduced.     */
/* ====================================================================================== */
typedef struct { uint32_t state; } bp_rng;

static inline void bp_rng_seed(bp_rng *r, uint32_t seed)
{
    r->state = seed ? seed : 0x9E3779B9u;
}

static inline uint32_t bp_rng_next(bp_rng *r)
{
    uint32_t x = r->state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->state = x;
    return x;
}

static inline float bp_rng_range(bp_rng *r, float lo, float hi)
{
    return lo + (hi - lo) * ((float)(bp_rng_next(r) & 0xFFFFFF) / 16777215.0f);
}

static inline int bp_rng_int(bp_rng *r, int n) { return (n > 0) ? (int)(bp_rng_next(r) % (uint32_t)n) : 0; }

/* ====================================================================================== */
/* Timing / performance instrumentation                                                    */
/* ====================================================================================== */
typedef struct {
    float frame_ms;         /* smoothed wall time per frame            */
    float sim_ms;           /* time spent inside the fixed-step update */
    float draw_ms;          /* time spent issuing draw calls           */
    int   fps;
    int   sim_steps;        /* simulation ticks executed last frame    */
    int   draw_calls;
    int   tris;
    bool  show_overlay;
} bp_perf;

/* ====================================================================================== */
/* Global engine singleton                                                                 */
/* ====================================================================================== */
typedef struct {
    bp_rng      rng;
    bp_perf     perf;
    float       time_total;     /* seconds since boot                      */
    float       time_scale;     /* slow-motion / speed-up multiplier       */
    int         screen_w;
    int         screen_h;
    int         render_scale;   /* internal resolution divisor (perf knob) */
    bool        vsync;
    bool        window_focused;
    bool        quit_requested;

    /* Permission for the frame loop to block on the OS event queue.
     *
     * raylib's vsync only covers the buffer swap; blocking on input is separate
     * (EnableEventWaiting, which parks inside EndDrawing -> PollInputEvents). It saves a
     * whole core at idle and we want it -- but if it is enabled before boot completes, a
     * display with no input activity (headless, unfocused window, kiosk) never delivers the
     * event EndDrawing is waiting for, and no further frame ever runs. Boot is staged one
     * subsystem per frame, so that presents as a hang on a black screen with the loading bar
     * at ~75%. This flag is the single place that encodes "it is safe to block now". */
    bool        event_wait_allowed;
    char        data_dir[256];  /* writable dir for saves (platform aware) */
} bp_engine;

extern bp_engine g_engine;

/* ====================================================================================== */
/* Engine subsystem entry points                                                           */
/* ====================================================================================== */
void bp_log_init(void);

/* Counters, so a test harness can assert "this run raised no errors" instead of scraping
 * stdout. Warnings are counted separately: a missing optional config file is a warning and
 * must never fail a build. */
int  bp_log_error_count(void);
int  bp_log_warn_count(void);

/* The only sanctioned way to turn event waiting on or off. See bp_engine.event_wait_allowed. */
void bp_apply_event_wait(void);
void bp_assets_init(void);      /* procedural textures, models, fonts */
void bp_assets_shutdown(void);
void bp_audio_init(void);       /* procedural SFX + music synthesis   */
void bp_audio_shutdown(void);
void bp_audio_update(float dt);
void bp_gfx_init(void);
void bp_gfx_begin_frame(void);
void bp_gfx_end_frame(void);

#endif /* BP_COMMON_H */
