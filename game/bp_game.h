/******************************************************************************************
 * bp_game.h -- scene state machine, fixed-timestep loop, level lifecycle, run stats.
 *
 * RESPONSIBILITY SPLIT (this is the architecture, not just a file)
 *   main.c            window, GL context, the literal while(!WindowShouldClose()) loop,
 *                     platform quirks. It contains no game rules.
 *   bp_game.c         which scene is live, when a level loads/unloads, when a run ends, and
 *                     the fixed-step accumulator that every subsystem is ticked from.
 *   everything else   one job each. Nothing below bp_game knows a scene exists.
 *
 *   The dependency graph is therefore a strict one-way tree:
 *       main -> game -> {menu, hud, save, waves} -> {player, weapons, enemies} ->
 *              {world, hitdetect, fx} -> engine
 *   No module ever calls upward. That is what makes it possible to unit-test bp_math or
 *   bp_entity by linking three files, and what stops a "temporary" cross-module call from
 *   becoming a knot.
 *
 * FIXED TIMESTEP
 *   Simulation runs at SIM_HZ (120) with an accumulator; rendering runs at display rate and
 *   interpolates. Consequences that are load-bearing rather than aesthetic:
 *     - Hit registration is identical on a 30 fps laptop and a 240 fps monitor. A hitscan
 *       game whose accuracy depends on frame rate is not a competitive game.
 *     - Enemy AI and movement tuning are expressed in seconds and stay valid forever.
 *     - The web build cannot be broken by a backgrounded tab: a long frame is clamped to
 *       MAX_FRAME_DT and the accumulator cannot spiral.
 *   The camera is simulated per step and rendered per frame with alpha interpolation, which
 *   is why it has _simulate() and _render(alpha) instead of one update().
 ******************************************************************************************/
#ifndef BP_GAME_H
#define BP_GAME_H

#include "bp_common.h"

typedef enum {
    SCENE_BOOT = 0,       /* one frame: subsystems come up                */
    SCENE_TITLE,          /* main menu over a live drifting camera        */
    SCENE_LEVEL_INTRO,    /* area name card, world loaded, player parked   */
    SCENE_PLAYING,        /* the game                                     */
    SCENE_PAUSED,         /* simulation frozen, menu live                 */
    SCENE_DEAD,           /* death cam, then results                      */
    SCENE_RESULTS,        /* run summary                                  */
    SCENE_LEVEL_COMPLETE, /* exit reached: tally, then next area or menu   */
    SCENE_GAME_COMPLETE   /* every area cleared                           */
} bp_scene;

/* Boot stages, in order. One stage runs per frame so the loading bar reports real progress
 * and the window stays responsive; see bp_game.c. */
enum {
    BOOT_LOG = 0, BOOT_INPUT, BOOT_ASSETS, BOOT_AUDIO, BOOT_GFX,
    BOOT_FX, BOOT_WORLD, BOOT_WEAPONS, BOOT_ENEMIES, BOOT_PLAYER,
    BOOT_WAVES, BOOT_SAVE, BOOT_UI, BOOT_HUD, BOOT_MENU, BOOT_LEVEL,
    BOOT_COUNT
};

typedef struct {
    /* run bookkeeping */
    int    level_index;
    int    difficulty;
    int    score_at_level_start;
    int    kills_at_level_start;
    float  level_start_time;
    int    deaths_this_run;

    /* run quality metrics, reset per level */
    float  damage_taken;
    int    headshots_this_level;
    float  best_streak;
    bool   secret_this_level;
    bool   submitted;          /* guard: a run is submitted exactly once */

    /* scene timing */
    bp_scene scene;
    bp_scene scene_before_pause;
    float  scene_time;
    float  accumulator;        /* fixed-step remainder                   */
    int    sim_steps;
    float  alpha;              /* render interpolation factor 0..1       */

    /* attract-mode camera for menus */
    float  orbit_yaw;
    float  orbit_pitch;
    float  orbit_radius;
    Vector3 orbit_center;

    /* boot */
    float  boot_progress;      /* 0..1, for the loading bar              */
    char   boot_status[64];
    bool   boot_done;
    bool   quit_requested;
    float  total_time;

    bool   initialised;
} bp_game;

extern bp_game g_game;

/* lifecycle */
void bp_game_init(void);
void bp_game_shutdown(void);

/* Called once per rendered frame from main(). Owns the accumulator, so it may run zero or
 * several simulation steps. `frameDt` must already be clamped to MAX_FRAME_DT by the caller
 * -- bp_game clamps again defensively, because a backgrounded browser tab can hand us a
 * sixty-second delta and the accumulator must not try to catch up. */
void bp_game_frame(float frameDt);

/* scene control */
void bp_game_set_scene(bp_scene s);
bp_scene bp_game_scene(void);
bool bp_game_is_playing(void);

void bp_game_start_level(int levelIndex);
void bp_game_restart_level(void);
void bp_game_next_level(void);
void bp_game_return_to_title(void);
void bp_game_request_quit(void);

/* called by the menu layer for actions that need game-side effects */
void bp_game_apply_menu_action(int action);

/* run stats, used by the results screen and the save */
void bp_game_submit_run(bool victory);

#endif /* BP_GAME_H */
