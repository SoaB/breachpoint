/******************************************************************************************
 * main.c -- platform entry point.
 *
 * WHAT BELONGS HERE, AND WHAT DOES NOT
 *   This file owns exactly four things: the GL context, the window, the frame loop, and the
 *   differences between desktop and web. It contains no game rules, no simulation state and
 *   no subsystem ordering beyond "boot, loop, shut down". Everything else lives in bp_game.c
 *   and below it.
 *
 *   That separation is not stylistic. It means the whole game can be driven from a test
 *   harness that calls bp_game_frame() in a loop with a synthetic delta and no window at all
 *   -- which is the only practical way to regression-test a fixed-timestep simulation.
 *
 * BOOT ORDER IS LOAD-BEARING
 *   1. bp_log_init()        -- the ring buffer must exist before anything logs, including
 *                              raylib's own trace output during window creation.
 *   2. bp_game_init()       -- zeroes the game state and selects SCENE_BOOT. No GL touched.
 *   3. bp_save_boot()       -- reads the profile. Window size, vsync and MSAA come from it,
 *                              and all three can only be applied *before* InitWindow.
 *                              Settings are deliberately NOT applied here: that would touch
 *                              the renderer and audio device, which do not exist yet.
 *   4. InitWindow()
 *   5. the loop             -- bp_game_frame() advances one boot stage per frame, so the
 *                              loading bar is real: it reports which subsystem just came up.
 *
 * WEB
 *   The browser owns the frame loop, so the web build registers a callback with
 *   emscripten_set_main_loop and never returns from main(). Two consequences shape this file:
 *     - Nothing may block. A while(!WindowShouldClose()) loop would freeze the tab.
 *     - The canvas size is authoritative. We never force a resolution; we react to
 *       IsWindowResized(), because the embedder (a store page, an iframe) decides the size.
 *   We also deliberately do NOT request pointer lock. It requires a user gesture and is
 *   refused inside cross-origin iframes, which is exactly how a web build gets embedded.
 *   GetMouseDelta() still delivers look input while the canvas has focus, and that code path
 *   is identical on desktop -- which is why bp_input contains no platform ifdefs at all.
 ******************************************************************************************/

#include "engine/bp_common.h"
#include "engine/bp_input.h"
#include "game/bp_game.h"
#include "game/bp_save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PLATFORM_WEB
#include <emscripten/emscripten.h>
#endif

#include "engine/bp_assets.h"
#include "engine/bp_camera.h"
#include "engine/bp_gfx.h"
#include "game/bp_enemies.h"
#include "game/bp_fx.h"
#include "game/bp_player.h"
#include "game/bp_waves.h"
#include "game/bp_weapons.h"
#include "game/bp_world.h"
#include "game/bp_hud.h"
#include "game/bp_menu.h"

/* -------------------------------------------------------------------------------------- */
/* self-test                                                                               */
/* -------------------------------------------------------------------------------------- */
/*
 * --selftest drives the real frame loop with synthetic input and no player, then reports a
 * pass/fail summary and exits with a status code.
 *
 * Why this exists in main.c rather than in tests/: it exercises the *whole* stack including
 * the GL context, the renderer, the audio device path and the scene machine. A unit test can
 * prove bp_math is correct; only this can prove that booting, loading a level, spawning a
 * wave, firing a weapon and drawing the HUD all work together in one process. That is the
 * difference between "it compiles" and "it ships", and it is the first thing to run in CI.
 *
 * It is compiled in unconditionally rather than behind a debug flag: a shipped build that
 * can verify itself is worth the few kilobytes, and it means a support engineer can ask a
 * player to run one command and paste the output.
 */
/*
 * Fixed synthetic frame time.
 *
 * The self-test advances game time by exactly 1/60 s per frame regardless of how long the
 * frame actually took to render. That is deliberate and it is the difference between a test
 * that works and one that silently passes or fails depending on the machine:
 *
 *   - Determinism. The same frame count always covers the same amount of simulated time, so
 *     "600 frames" means "ten seconds of gameplay" on a 4090 and on a software rasteriser.
 *   - Coverage. Wall-clock driving meant a slow CI box simulated two seconds of game in the
 *     time it took to run, which never got past the three-second level intro card -- so the
 *     test reported zero enemies spawned and zero shots fired and looked like four separate
 *     gameplay bugs. It was one timing assumption.
 *   - Speed. Under llvmpipe a real frame costs ~7 ms, so decoupling sim time from render
 *     time lets the test cover twenty seconds of combat in a couple of seconds of wall clock.
 *
 * It does mean the test cannot detect "the game runs too slowly" -- that is what the frame
 * budget assertion and the fps check at the end are for.
 */
#define SELFTEST_DT (1.0f / 60.0f)

static int selftest_frames = 0;

static void selftest_frame(void)
{
    selftest_frames++;

    /*
     * Synthetic input goes through the virtual-input layer, never by writing into
     * g_input.act[] directly: bp_input_begin_frame() re-derives every action from the
     * physical devices on the next frame and would erase it. bp_input_set_virtual() is
     * edge-aware, so holding FIRE for a hundred frames fires a hundred times while JUMP
     * pressed for one frame jumps once.
     */
    bool fighting = (g_game.scene == SCENE_PLAYING || g_game.scene == SCENE_LEVEL_INTRO);

    if (g_game.scene == SCENE_TITLE && !fighting)
    {
        /*
         * Start the run by pressing ACCEPT on the title screen rather than by calling
         * bp_game_start_level() directly.
         *
         * It is two extra lines and it covers the real entry path a player uses: menu row
         * selection, action dispatch, the level-load handoff and the scene transition. A
         * test that calls the API straight through would still pass with the menu broken.
         * Frame 150 is past boot (17 frames) and leaves the title banner up long enough to
         * be visible in a recorded run.
         */
        bp_input_clear_virtual();
        if (selftest_frames >= 150) bp_input_set_virtual(ACT_MENU_ACCEPT, true);
    }
    else if (!fighting)
    {
        bp_input_clear_virtual();
    }
    else
    {
        /* Aim towards the nearest enemy so combat and waves actually progress, or sweep if none */
        bp_handle nh = BP_NULL_HANDLE;
        if (bp_enemy_nearest_distance(g_player.pos, &nh) < 120.0f)
        {
            bp_enemy *e = bp_enemy_get(nh);
            if (e && e->state != ES_DYING && e->state != ES_DEAD)
            {
                Vector3 to = Vector3Subtract(e->pos, g_player.pos);
                g_camera.yaw = atan2f(to.x, to.z) * BP_RAD2DEG;
                float dist = sqrtf(to.x * to.x + to.z * to.z);
                float dy = (e->pos.y + 0.9f) - (g_player.pos.y + 1.62f);
                g_camera.pitch = atan2f(dy, BP_MAX(dist, 0.1f)) * BP_RAD2DEG;
            }
            else g_camera.yaw += 2.2f;
        }
        else
        {
            g_camera.yaw += 2.2f;
        }

        /* Move in a circle: exercises acceleration, friction, air control, wall collision,
         * the step-up solver and footstep audio. */
        bp_input_set_virtual(ACT_MOVE_FORWARD, true);
        bp_input_set_virtual(ACT_MOVE_LEFT, (selftest_frames / 40) % 2 == 0);
        bp_input_set_virtual(ACT_JUMP, (selftest_frames % 55) == 0);

        /* Fire continuously and cycle/reload periodically so every weapon state machine,
         * the projectile path and the hitscan path all get exercised. */
        bp_input_set_virtual(ACT_FIRE, (selftest_frames % 4) < 2);
        bp_input_set_virtual(ACT_RELOAD, (selftest_frames % 240) == 0);
        bp_input_set_virtual(ACT_WEAPON_NEXT, (selftest_frames % 400) == 0);
        bp_input_set_virtual(ACT_SPRINT, (selftest_frames / 90) % 2 == 0);
        bp_input_set_virtual(ACT_CROUCH, (selftest_frames / 150) % 2 == 0);

        if (selftest_frames == 160)
        {
            bp_weapons_give(WPN_ID_SHOTGUN, 40, false);
            bp_weapons_give(WPN_ID_SMG, 300, true);
        }

        /* Self-test synthetic player survival: keep ammo stocked and health up so all 1800 frames run combat */
        if (g_weapons.reserve[g_weapons.current] < 30)
            bp_weapons_add_ammo(g_weapons.current, 60);
        if (g_player.health < 40.0f)
            g_player.health = 100.0f;
    }

    BeginDrawing();
    ClearBackground(BLACK);
    bp_game_frame(SELFTEST_DT);
    EndDrawing();

}

static int run_selftest(int frames)
{
    BP_LOGI("selftest: %d frames at a fixed %.1f Hz = %.1f s of simulated play",
            frames, 60.0, (double)frames / 60.0);

    /* Per-batch wall clock: this is how you find out that a scene costs 300 ms to render on
     * a software rasteriser instead of staring at a frozen console for five minutes. */
    double t0 = GetTime();
    for (int i = 0; i < frames && !g_engine.quit_requested; i++)
    {
        selftest_frame();
        if ((i % 120) == 119)
        {
            double now = GetTime();
            BP_LOGI("selftest: frame %4d  sim %5.1fs  scene %d  wave %d  %2d alive  %4d particles  %.1f ms/frame",
                    i + 1, (double)g_game.total_time, g_game.scene, g_waves.wave_index,
                    g_enemies.alive_count, g_fx.particle_live, (now - t0) * 1000.0 / 120.0);
            t0 = now;
        }
    }

    /* Baseline the error counter *after* the run: the assertions below log at ERROR level
     * when they fail, and counting those as pre-existing errors makes the final
     * "no errors were raised" check fail on its own output. */
    int errorsBefore = bp_log_error_count();

    int fails = 0;
    #define CHECK(cond, fmt, ...) do { \
        if (cond) BP_LOGI("selftest: PASS  " fmt, __VA_ARGS__); \
        else { BP_LOGE("selftest: FAIL  " fmt, __VA_ARGS__); fails++; } } while (0)

    CHECK(g_game.boot_done,            "boot completed (%d stages)", BOOT_COUNT);
    CHECK(g_assets.ready,              "assets generated (%.0f ms)", (double)g_assets.load_ms);
    CHECK(g_assets.fonts_valid,        "bitmap font atlas usable (%d glyphs)", 0);
    CHECK(g_world.loaded,              "level loaded: %d solids, %d triggers", g_world.solid_count, g_world.trigger_count);
    CHECK(g_world.batch_count > 0,     "static geometry baked into %d batches", g_world.batch_count);
    CHECK(g_enemies.spawned_total > 0, "enemies spawned: %d (%d alive at end)", g_enemies.spawned_total, g_enemies.alive_count);
    CHECK(g_enemies.killed_total > 0,  "combat produced kills: %d", g_enemies.killed_total);
    CHECK(g_weapons.shots_fired > 0,   "weapons fired: %d shots, %d hits (%d%%)",
          g_weapons.shots_fired, g_weapons.shots_hit,
          g_weapons.shots_fired ? (g_weapons.shots_hit * 100) / g_weapons.shots_fired : 0);
    CHECK(g_waves.wave_index >= 2 || (g_waves.wave_index == 1 && g_waves.state == WAVE_INTERMISSION),
          "wave director advanced past wave 1 to %d (%s)", g_waves.wave_index, bp_waves_state_name());
    CHECK(g_player.time_alive > 1.0f,  "player simulated %.1fs alive", (double)g_player.time_alive);
    CHECK(g_player.score >= 0,         "scoring ran: %d points, %d kills, streak %d",
          g_player.score, g_player.kills, g_player.streak);
    CHECK(g_fx.particle_live >= 0,     "particle pool live count %d of %d", g_fx.particle_live, MAX_PARTICLES);
    CHECK(g_game.total_time > 0.0f,    "simulated %.1fs of game time in %d frames", (double)g_game.total_time, selftest_frames);
    CHECK(g_engine.perf.fps > 0,       "frame rate measured at %d fps", g_engine.perf.fps);
    CHECK(g_game.sim_steps > 0,        "fixed-step loop ran (last frame: %d steps)", g_game.sim_steps);

    /* A collision bug shows up as a fall through the world long before it shows up as
     * anything a human would report, so this is the cheapest high-value assertion here. */
    CHECK(g_player.pos.y > g_world.bounds_min.y - 1.0f,
          "player stayed inside the level (y=%.2f, floor=%.2f)",
          (double)g_player.pos.y, (double)g_world.bounds_min.y);
    CHECK(g_player.pos.x > g_world.bounds_min.x - 1.0f && g_player.pos.x < g_world.bounds_max.x + 1.0f &&
          g_player.pos.z > g_world.bounds_min.z - 1.0f && g_player.pos.z < g_world.bounds_max.z + 1.0f,
          "player stayed inside the level bounds (%.1f, %.1f)",
          (double)g_player.pos.x, (double)g_player.pos.z);

    /* The save system must survive a run: submitting writes the profile, and a corrupt or
     * unwritable profile is the kind of bug that only shows up on a player's machine. */
    CHECK(g_save.best_score >= 0,      "save system reachable (best score %d, kills %d)",
          g_save.best_score, g_save.total_kills);

    int errorsAfter = bp_log_error_count();
    CHECK(errorsAfter == errorsBefore, "no error-level log lines raised by the run (%d new)", errorsAfter - errorsBefore);
    #undef CHECK

    bp_input_clear_virtual();
    BP_LOGI("selftest: %s -- %d checks failed", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}

/* -------------------------------------------------------------------------------------- */
/* window                                                                                  */
/* -------------------------------------------------------------------------------------- */
/*
 * Window flags must be set BEFORE InitWindow: raylib reads them while creating the GL
 * context and ignores later changes (there is no runtime vsync toggle for the same reason).
 * So the profile is loaded first -- bp_save_boot() in main() -- and applied here.
 */
static void apply_window_flags(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);

    /* MSAA is only requested at HIGH and above. Below that the internal render scale is
     * already under 1.0 and supersampling a downscaled buffer costs fill rate the player
     * explicitly asked us not to spend. */
    if (g_save.settings.quality >= BP_QUALITY_HIGH) SetConfigFlags(FLAG_MSAA_4X_HINT);
    if (g_save.settings.vsync) SetConfigFlags(FLAG_VSYNC_HINT);

    /* Keep simulating while minimised. The alternative is a backgrounded window that stops
     * ticking and then hands us a multi-second frame delta when it is restored -- the
     * accumulator clamp in bp_game_frame() handles that, but not resuming cleanly is worse. */
    SetConfigFlags(FLAG_WINDOW_ALWAYS_RUN);
}

static void create_window(void)
{
    int w = g_save.settings.window_w;
    int h = g_save.settings.window_h;

    /*
     * Clamp to the monitor we will actually land on.
     *
     * A profile carries the window size from the machine it was written on. Restoring
     * 2560x1440 onto a 1366x768 laptop puts the HUD off-screen and, on some drivers, makes
     * the context creation fail outright -- which presents to the player as a game that
     * refuses to start with no error message. Clamping is cheap and removes the whole class.
     * The 120/80 margins leave room for the taskbar and the title bar.
     */
    if (w <= 0 || h <= 0) { w = 1280; h = 720; }
#if !defined(PLATFORM_WEB)
    int mw = GetMonitorWidth(GetCurrentMonitor());
    int mh = GetMonitorHeight(GetCurrentMonitor());
    if (mw > 320 && mh > 240)
    {
        w = BP_MIN(w, mw - 120);
        h = BP_MIN(h, mh - 120);
    }
#endif

    InitWindow(w, h, BP_GAME_NAME " v" BP_VERSION_STRING "  --  " BP_STUDIO);
    SetWindowMinSize(640, 360);
    SetWindowTitle(BP_GAME_NAME " v" BP_VERSION_STRING);

    /*
     * Event-wait mode starts OFF and is unlocked per-frame once boot has finished -- see
     * run_frame() and bp_engine::event_wait_allowed for why it cannot simply be enabled here
     * or at the end of the boot sequence.
     */
    g_engine.event_wait_allowed = false;
    bp_apply_event_wait();
}

/* -------------------------------------------------------------------------------------- */
/* frame                                                                                   */
/* -------------------------------------------------------------------------------------- */
/*
 * One rendered frame.
 *
 * BeginDrawing/EndDrawing bracket everything: bp_game_frame() resolves the offscreen scene
 * target to the backbuffer and then draws the HUD and menus, so all of it must happen inside
 * the pair. ClearBackground is BLACK rather than the level's ambient colour because the scene
 * resolve covers every pixel anyway, and clearing to black makes any letterbox gap or missed
 * region immediately visible during development instead of hiding behind a plausible colour.
 */
static void run_frame(void)
{
    /* GetFrameTime() returns 0 on the very first frame and can return an absurd value after
     * the tab has been backgrounded. Both are clamped here so that bp_game_frame's own clamp
     * is a second line of defence rather than the only one -- the web build will hit this. */
    float dt = GetFrameTime();
    if (!(dt > 0.0f)) dt = SIM_DT;
    if (dt > MAX_FRAME_DT) dt = MAX_FRAME_DT;

    BeginDrawing();
    ClearBackground(BLACK);
    bp_game_frame(dt);

    /*
     * Hand over to event-waiting mode on the first frame after boot completes -- and only
     * here.
     *
     * bp_game_frame() runs between BeginDrawing() and EndDrawing(), and raylib implements
     * event waiting by blocking inside EndDrawing() until the OS delivers an event. If the
     * last boot stage flipped the flag itself, that same frame would be the first one able to
     * park forever in PollInputEvents on a display that produces no events: a headless
     * X server, a window that lost focus, a kiosk. The symptom is a fully-initialised game
     * that stops printing and never draws another frame, which looks exactly like a crash in
     * the renderer.
     *
     * Doing it here means the check runs after the frame's work is finished but before the
     * blocking call, so the boot frame itself always completes and the *next* frame is the
     * first one allowed to block. It costs one branch per frame.
     */
    if (!g_engine.event_wait_allowed && g_game.boot_done && !g_engine.quit_requested)
    {
        g_engine.event_wait_allowed = true;
        bp_apply_event_wait();
    }

    EndDrawing();
}

/* -------------------------------------------------------------------------------------- */
/* entry                                                                                   */
/* -------------------------------------------------------------------------------------- */
#ifdef PLATFORM_WEB
/* Emscripten's main loop callback is void(void), so per-frame state lives in the modules
 * rather than in locals here. */
static void web_main_loop(void) { run_frame(); }
#endif

int main(int argc, char **argv)
{
    bool selftest = false;
    int selftestFrameCount = 1800;     /* 30 s of simulated play at the fixed 60 Hz below */
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--selftest")) selftest = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
        {
            /* Deliberately not BP_MAX(...): that macro evaluates its arguments more than
             * once and ++i would run twice. See the warning at the macro definition. */
            i++;
            selftestFrameCount = atoi(argv[i]);
            if (selftestFrameCount < 1) selftestFrameCount = 1800;
        }
        else if (!strcmp(argv[i], "--help"))
        {
            printf("%s v%s -- %s\n", BP_GAME_NAME, BP_VERSION_STRING, BP_STUDIO);
            printf("  --selftest          run a synthetic play-through and report pass/fail\n");
            printf("  --frames N          self-test length in frames at 60 Hz sim (default 1800)\n");
            return 0;
        }
    }

    bp_log_init();
    BP_LOGI("main: %s v%s -- %s", BP_GAME_NAME, BP_VERSION_STRING, BP_STUDIO);
    BP_LOGI("main: platform %s",
#ifdef PLATFORM_WEB
            "web / WebGL (OpenGL ES 2.0)"
#else
            "desktop / OpenGL 3.3"
#endif
    );

    bp_game_init();
    bp_save_boot();

    apply_window_flags();
    create_window();

    BP_LOGI("main: window %dx%d (monitor %dx%d), vsync %s, event-wait %s",
            GetScreenWidth(), GetScreenHeight(),
            GetMonitorWidth(GetCurrentMonitor()), GetMonitorHeight(GetCurrentMonitor()),
            g_save.settings.vsync ? "on" : "off", g_save.settings.vsync ? "blocking" : "polling");

    /* The canvas gets keyboard focus only after a click on the web. Saying so on screen is
     * worth more than any amount of documentation: without it a player clicks "play", sees a
     * menu that ignores the keyboard, and concludes the build is broken. */
#ifdef PLATFORM_WEB
    BP_LOGI("main: web build -- click the canvas once to capture keyboard and mouse");
#endif

    if (selftest)
    {
        /* Force polling for the whole test. It measures correctness, not idle power, and
         * blocking on the event queue with no events arriving would make it take minutes of
         * wall clock -- or hang outright on a headless display. */
        g_engine.event_wait_allowed = false;
        bp_apply_event_wait();
        /* Boot is staged one subsystem per frame, so it needs frames to run. Event waiting
         * stays disabled for the whole test -- see bp_engine::event_wait_allowed. */
        int rc = 0;
        while (!g_game.boot_done && rc < 240) { selftest_frame(); rc++; }
        if (!g_game.boot_done) { BP_LOGE("selftest: boot did not complete in %d frames", rc); return 2; }
        int result = run_selftest(selftestFrameCount);
        bp_game_shutdown();
        CloseWindow();
        return result;
    }

#ifdef PLATFORM_WEB
    /* 0 means "run as fast as the browser allows", i.e. requestAnimationFrame-driven.
     * Passing a fixed rate here would fight the display's refresh and judder on 144 Hz. */
    emscripten_set_main_loop(web_main_loop, 0, 1);
    /* With simulate_infinite_loop=1 this never returns, so the shutdown path below is
     * unreachable on the web. That is correct: the browser tears the instance down and
     * IDBFS has already been synced by bp_save_sync_platform(). */
#else
    while (!WindowShouldClose() && !g_engine.quit_requested)
    {
        run_frame();
    }

    BP_LOGI("main: loop exited after %.1fs of game time", (double)g_engine.time_total);
    bp_game_shutdown();
    CloseWindow();
    BP_LOGI("main: clean exit");
#endif

    return 0;
}
