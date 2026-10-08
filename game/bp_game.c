/******************************************************************************************
 * bp_game.c -- scene machine, fixed-step loop, level lifecycle.
 *
 * BOOT IS STAGED, ONE SUBSYSTEM PER FRAME
 *   Assets, audio and graphics each take long enough to matter (procedural texture
 *   generation is tens of milliseconds; the world bake is more). Doing all of it inside one
 *   function before the first EndDrawing() would show the player a frozen black window with
 *   an OS "not responding" hint on desktop and a blank canvas on the web. Instead bp_game
 *   advances one boot stage per frame, so the loading bar is real: it is literally reporting
 *   which subsystem just came up.
 *
 * WHO CALLS WHOM
 *   main.c calls bp_game_frame(frameDt) once per rendered frame and nothing else. Everything
 *   below -- how many simulation steps run, which scene ticks, when the UI draws -- is
 *   decided here. That single entry point is why the whole game can be driven headlessly
 *   from a test harness.
 ******************************************************************************************/
#include "bp_game.h"

#include "bp_ui.h"
#include "bp_hud.h"
#include "bp_menu.h"
#include "bp_save.h"
#include "bp_world.h"
#include "bp_waves.h"
#include "bp_player.h"
#include "bp_weapons.h"
#include "bp_enemies.h"
#include "bp_fx.h"
#include "bp_hitdetect.h"
#include "../engine/bp_assets.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_camera.h"
#include "../engine/bp_gfx.h"
#include "../engine/bp_input.h"
#include "../engine/bp_math.h"

#include <stdio.h>
#include <string.h>

bp_game g_game = { 0 };

static inline Vector3 vec3y(float y) { return (Vector3){ 0.0f, y, 0.0f }; }

static inline Color mc_game(float r, float g, float b, float a)
{
    return (Color){ (unsigned char)BP_CLAMP((int)r, 0, 255), (unsigned char)BP_CLAMP((int)g, 0, 255),
                    (unsigned char)BP_CLAMP((int)b, 0, 255), (unsigned char)BP_CLAMP((int)a, 0, 255) };
}

/* Menu action ids. Duplicated from bp_menu.c on purpose: bp_menu owns building its rows and
 * bp_game owns acting on them, and sharing an enum would make the menu header a dependency
 * of the game's control flow. The values are asserted at boot (see sanity_check_ids). */
enum {
    GA_PLAY = 1, GA_LEVELS, GA_OPTIONS, GA_CONTROLS, GA_CREDITS, GA_QUIT,
    GA_RESUME, GA_RESTART, GA_ABORT,
    GA_RETRY, GA_NEXT_LEVEL, GA_TO_MENU,
    GA_BACK, GA_RESET_BINDS, GA_RESET_SETTINGS, GA_APPLY,
    GA_LEVEL_BASE = 100,
    GA_BIND_BASE  = 200
};

/* boot stages: declared in the header because the self-test reports the count */

#define INTRO_CARD_TIME   3.0f
#define DEAD_TO_RESULTS   2.2f
#define ORBIT_SPEED       6.5f      /* degrees per second for the menu camera */

/* -------------------------------------------------------------------------------------- */
/* forward declarations                                                                    */
/* -------------------------------------------------------------------------------------- */
static void simulate_step(float dt);
static void render_frame(void);
static void enter_scene(bp_scene s);
static void update_attract_camera(float dt);

/* Implemented in bp_menu.c. The options screen edits FOV through a float mirror because the
 * slider row type works on floats while the profile stores an int; these two thunks keep
 * bp_game from reaching into another translation unit's statics. */
float bp_menu_fov_mirror(void);
void  bp_menu_rebuild_current(void);

/* -------------------------------------------------------------------------------------- */
/* scene helpers                                                                           */
/* -------------------------------------------------------------------------------------- */
static inline bool scene_is_menu(bp_scene s)
{
    return s == SCENE_TITLE || s == SCENE_RESULTS || s == SCENE_PAUSED || s == SCENE_GAME_COMPLETE;
}

void bp_game_set_scene(bp_scene s) { enter_scene(s); }
bp_scene bp_game_scene(void) { return g_game.scene; }
bool bp_game_is_playing(void) { return g_game.scene == SCENE_PLAYING; }

static void banner_for_scene(bp_scene s)
{
    switch (s)
    {
        case SCENE_LEVEL_INTRO:
        {
            int i = BP_CLAMP(g_game.level_index, 0, BP_MAX(g_level_count - 1, 0));
            bp_menu_open(MENU_SCREEN_NONE);      /* close any menu */
            g_menu.screen = MENU_SCREEN_NONE;
            bp_hud_banner(g_levels[i].display_name, g_levels[i].subtitle, INTRO_CARD_TIME);
            bp_hud_toast(TextFormat("DIFFICULTY: %s",
                         g_difficulties[BP_CLAMP(g_game.difficulty, 0, g_difficulty_count - 1)].name),
                         TOAST_INFO, INTRO_CARD_TIME);
            break;
        }
        case SCENE_LEVEL_COMPLETE:
            bp_hud_banner("AREA CLEAR", g_world.name, 2.4f);
            break;
        case SCENE_GAME_COMPLETE:
            bp_hud_banner("ALL AREAS CLEAR", "Nothing left to breach.", 3.0f);
            break;
        default: break;
    }
}

static void enter_scene(bp_scene s)
{
    if (s == SCENE_PAUSED) g_game.scene_before_pause = g_game.scene;
    g_game.scene = s;
    g_game.scene_time = 0.0f;
    banner_for_scene(s);

    switch (s)
    {
        case SCENE_TITLE:
            bp_menu_open(MENU_SCREEN_TITLE);
            bp_input_capture(false);
            bp_audio_set_intensity(0.20f, 1.0f);
            break;

        case SCENE_LEVEL_INTRO:
            bp_menu_close();
            /* The cursor stays captured through the intro card so the first wave does not
             * begin with the player hunting for the window. */
            bp_input_capture(true);
            bp_audio_set_intensity(0.30f, 0.8f);
            break;

        case SCENE_PLAYING:
            bp_menu_close();
            bp_input_capture(true);
            break;

        case SCENE_PAUSED:
            bp_menu_open(MENU_SCREEN_PAUSE);
            bp_input_capture(false);
            bp_audio_set_intensity(0.14f, 0.4f);
            break;

        case SCENE_DEAD:
            bp_menu_close();
            bp_input_capture(false);
            bp_audio_set_intensity(0.10f, 1.2f);
            break;

        case SCENE_RESULTS:
        case SCENE_LEVEL_COMPLETE:
        case SCENE_GAME_COMPLETE:
            bp_menu_open(MENU_SCREEN_RESULTS);
            bp_input_capture(false);
            bp_audio_set_intensity(0.16f, 1.0f);
            break;

        default: break;
    }
    BP_LOGI("game: scene -> %d", (int)s);
}

/* -------------------------------------------------------------------------------------- */
/* level lifecycle                                                                         */
/* -------------------------------------------------------------------------------------- */
static void reset_run_stats(void)
{
    g_game.damage_taken = 0.0f;
    g_game.headshots_this_level = 0;
    g_game.best_streak = 0.0f;
    g_game.secret_this_level = false;
    g_game.submitted = false;
    g_game.level_start_time = g_engine.time_total;
    g_game.score_at_level_start = g_player.score;
    g_game.kills_at_level_start = g_player.kills;
}

void bp_game_start_level(int levelIndex)
{
    if (levelIndex < 0 || levelIndex >= g_level_count) levelIndex = 0;
    g_game.level_index = levelIndex;
    g_game.difficulty = BP_CLAMP(g_save.settings.difficulty, 0, g_difficulty_count - 1);

    if (!bp_world_load(levelIndex))
    {
        BP_LOGE("game: failed to load level %d", levelIndex);
        enter_scene(SCENE_TITLE);
        return;
    }

    bp_fx_reset();
    bp_enemies_clear();
    bp_pickup_clear();
    bp_weapons_reset_loadout(false);
    bp_hud_reset();

    float startYaw = 0.0f;
    Vector3 start = bp_world_player_start(&startYaw);
    bp_player_reset(start, startYaw);

    /* Starting kit: the sidearm plus whatever the level's first weapon trigger hands out.
     * Giving a full arsenal at wave 1 would remove the reason to fight for pickups. */
    bp_weapons_give(WPN_ID_PISTOL, g_weapon_table[WPN_ID_PISTOL].mag_size * 2, true);

    g_save.settings.last_level = levelIndex;
    g_save.dirty = true;

    reset_run_stats();
    g_waves.difficulty = g_game.difficulty;
    bp_waves_start_level(levelIndex);

    bp_hud_toast(g_world.name, TOAST_BIG, INTRO_CARD_TIME);
    enter_scene(SCENE_LEVEL_INTRO);

    BP_LOGI("game: level %d '%s' started on %s", levelIndex, g_world.name,
            g_difficulties[g_game.difficulty].name);
}

void bp_game_restart_level(void)
{
    /* A restart keeps the run score: the player earned it, and wiping it would make "retry"
     * feel like a punishment for dying rather than a second chance. */
    int keepScore = g_player.score;
    int keepKills = g_player.kills;
    g_game.deaths_this_run = 0;
    bp_game_start_level(g_game.level_index);
    g_player.score = keepScore;
    g_player.kills = keepKills;
}

void bp_game_next_level(void)
{
    int next = g_game.level_index + 1;
    if (next >= g_level_count) { enter_scene(SCENE_GAME_COMPLETE); return; }
    bp_game_start_level(next);
}

void bp_game_return_to_title(void)
{
    if (!g_game.submitted && g_world.loaded) bp_game_submit_run(false);
    g_game.deaths_this_run = 0;
    bp_pickup_clear();
    bp_enemies_clear();
    bp_fx_reset();
    bp_audio_set_intensity(0.20f, 1.0f);

    /* Keep the last level loaded as the attract-mode backdrop: reloading costs a frame hitch
     * and buys nothing, since the menu camera only ever orbits whatever is there. */
    enter_scene(SCENE_TITLE);
}

void bp_game_request_quit(void)
{
    if (!g_game.submitted && g_world.loaded && g_game.scene == SCENE_PLAYING)
        bp_game_submit_run(false);
    bp_save_write_settings();
    bp_save_write_progress();
    bp_save_sync_platform();
    g_game.quit_requested = true;
}

/* -------------------------------------------------------------------------------------- */
/* results                                                                                 */
/* -------------------------------------------------------------------------------------- */
void bp_game_submit_run(bool victory)
{
    if (g_game.submitted) return;      /* exactly once per level: a retry must not double-count */
    g_game.submitted = true;

    float time = g_engine.time_total - g_game.level_start_time;
    float acc = bp_weapons_accuracy();
    int score = g_player.score - g_game.score_at_level_start;
    int kills = g_player.kills - g_game.kills_at_level_start;

    bp_save_submit_run(g_game.level_index, g_game.difficulty, victory, score, kills,
                       time, acc, g_waves.wave_index, g_waves.perfect_waves,
                       g_player.secret_reached, g_player.state != PST_ALIVE);

    bp_menu_set_results(victory, victory ? "AREA CLEAR" : "RUN OVER",
                        score, kills, time, acc, g_waves.wave_index);

    bp_menu_add_result_line("%s ON %s", g_world.loaded ? g_world.name : "AREA",
                            g_difficulties[BP_CLAMP(g_game.difficulty, 0, g_difficulty_count - 1)].name);
    bp_menu_add_result_line("WAVES CLEARED %d / %d", g_waves.waves_completed, BP_MAX(1, g_waves.wave_total));
    bp_menu_add_result_line("PERFECT WAVES %d", g_waves.perfect_waves);
    bp_menu_add_result_line("HEADSHOTS %d", g_weapons.headshots);
    bp_menu_add_result_line("DAMAGE DEALT %.0f", (double)g_weapons.damage_dealt);
    bp_menu_add_result_line("DAMAGE TAKEN %.0f", (double)g_game.damage_taken);
    bp_menu_add_result_line("BEST STREAK x%d", g_player.streak);
    bp_menu_add_result_line(g_player.secret_reached ? "SECRET FOUND" : "SECRET MISSED");
}

/* -------------------------------------------------------------------------------------- */
/* menu actions                                                                            */
/* -------------------------------------------------------------------------------------- */
void bp_game_apply_menu_action(int action)
{
    bp_menu_screen from = g_menu.screen;

    switch (action)
    {
        case GA_PLAY:
            bp_game_start_level(BP_CLAMP(g_save.settings.last_level, 0, BP_MAX(g_level_count - 1, 0)));
            break;

        case GA_LEVELS:   bp_menu_open(MENU_SCREEN_LEVELS); break;
        case GA_OPTIONS:  bp_menu_open(MENU_SCREEN_OPTIONS); break;
        case GA_CONTROLS: bp_menu_open(MENU_SCREEN_CONTROLS); break;
        case GA_CREDITS:  bp_menu_open(MENU_SCREEN_CREDITS); break;
        case GA_QUIT:
            if (from == MENU_SCREEN_TITLE) bp_menu_open(MENU_SCREEN_QUIT);
            else bp_game_request_quit();
            break;

        case GA_RESUME:
            enter_scene(g_game.scene_before_pause == SCENE_PLAYING ? SCENE_PLAYING : SCENE_LEVEL_INTRO);
            break;

        case GA_RESTART:
            bp_game_restart_level();
            break;

        case GA_ABORT:
            bp_game_submit_run(false);
            bp_game_return_to_title();
            break;

        case GA_RETRY:
            bp_game_restart_level();
            break;

        case GA_NEXT_LEVEL:
            bp_game_next_level();
            break;

        case GA_TO_MENU:
            bp_game_return_to_title();
            break;

        case GA_APPLY:
        {
            /* Push the FOV mirror back into the profile, then apply everything live. */
            g_save.settings.fov = BP_CLAMP((int)(bp_menu_fov_mirror() + 0.5f), 70, 120);
            bp_save_apply_settings();
            bp_save_write_settings();
            bp_save_sync_platform();
            bp_hud_toast("SETTINGS APPLIED", TOAST_GOOD, 1.6f);
            bp_menu_open(from == MENU_SCREEN_PAUSE ? MENU_SCREEN_PAUSE : MENU_SCREEN_TITLE);
            break;
        }

        case GA_RESET_SETTINGS:
        {
            int keepScore = g_save.best_score;
            bp_level_record keep[SAVE_MAX_LEVELS][4];
            memcpy(keep, g_save.levels, sizeof(keep));

            bp_save_defaults();
            memcpy(g_save.levels, keep, sizeof(g_save.levels));
            g_save.best_score = keepScore;
            bp_save_apply_settings();
            bp_save_write_settings();
            bp_hud_toast("SETTINGS RESET -- RECORDS KEPT", TOAST_INFO, 2.0f);
            bp_menu_rebuild_current();
            break;
        }

        case GA_RESET_BINDS:
            bp_input_reset_binds();
            for (int a = 0; a < ACT_COUNT; a++)
                for (int k = 0; k < BP_MAX_BINDS_PER_ACTION; k++)
                    g_save.settings.binds[a][k] = g_input.act[a].keys[k];
            g_save.dirty = true;
            bp_save_write_settings();
            bp_hud_toast("DEFAULT KEYMAP RESTORED", TOAST_INFO, 2.0f);
            bp_menu_open(MENU_SCREEN_CONTROLS);
            break;

        case GA_BACK:
            if (from == MENU_SCREEN_LEVELS || from == MENU_SCREEN_CREDITS ||
                from == MENU_SCREEN_CONTROLS)
            {
                bp_menu_open(g_menu.return_to == MENU_SCREEN_NONE ? MENU_SCREEN_TITLE : g_menu.return_to);
            }
            else if (from == MENU_SCREEN_OPTIONS)
            {
                bp_menu_open(g_menu.return_to == MENU_SCREEN_NONE ? MENU_SCREEN_TITLE : g_menu.return_to);
            }
            else if (from == MENU_SCREEN_QUIT) bp_menu_open(MENU_SCREEN_TITLE);
            else if (from == MENU_SCREEN_PAUSE) enter_scene(SCENE_PLAYING);
            else if (from == MENU_SCREEN_RESULTS) { /* results is a terminal screen */ }
            break;

        default:
            if (action >= GA_LEVEL_BASE && action < GA_LEVEL_BASE + SAVE_MAX_LEVELS)
                bp_game_start_level(action - GA_LEVEL_BASE);
            /* GA_BIND_BASE rows are consumed inside bp_menu (key capture), never here. */
            break;
    }
}

/* -------------------------------------------------------------------------------------- */
/* pause / debug toggles handled outside the menu                                          */
/* -------------------------------------------------------------------------------------- */
static void handle_global_input(void)
{
    if (bp_pressed(ACT_DEBUG_OVERLAY))
    {
        g_engine.perf.show_overlay = !g_engine.perf.show_overlay;
        BP_LOGI("game: debug overlay %s", g_engine.perf.show_overlay ? "on" : "off");
    }

    if (g_game.scene == SCENE_PLAYING || g_game.scene == SCENE_LEVEL_INTRO)
    {
        if (bp_pressed(ACT_PAUSE))
        {
            enter_scene(SCENE_PAUSED);
            return;
        }
        /* F1-style HUD toggle lives on the scoreboard action's neighbour: reuse SCOREBOARD
         * long-press is too clever, so bind it to the debug overlay's second key slot. */
        if (IsKeyPressed(KEY_H))
        {
            g_hud.visible = !g_hud.visible;
            g_save.settings.hud_visible = g_hud.visible;
            g_save.dirty = true;
        }
        if (IsKeyPressed(KEY_G))
        {
            g_hud.minimal = !g_hud.minimal;
            g_save.settings.hud_minimal = g_hud.minimal;
            g_save.dirty = true;
            bp_hud_toast(g_hud.minimal ? "MINIMAL HUD" : "FULL HUD", TOAST_INFO, 1.2f);
        }
    }
    else if (g_game.scene == SCENE_PAUSED)
    {
        if (bp_pressed(ACT_PAUSE)) enter_scene(SCENE_PLAYING);
    }
    else if (g_game.scene == SCENE_TITLE)
    {
        if (bp_pressed(ACT_PAUSE) && g_menu.screen == MENU_SCREEN_TITLE)
            bp_menu_open(MENU_SCREEN_QUIT);
    }
}

/* -------------------------------------------------------------------------------------- */
/* simulation                                                                              */
/* -------------------------------------------------------------------------------------- */
static void handle_combat_input(void)
{
    /* ADS is published here rather than inside bp_weapons so that exactly one place decides
     * what "aim" means; the weapon code only consumes the flag. */
    g_weapons.ads_held = bp_down(ACT_ALT_FIRE);

    if (bp_pressed(ACT_RELOAD)) bp_weapons_try_reload();

    /* Direct slot selection, then cycling. Slot keys win over the wheel so a player who
     * binds 1-5 never has their intent overridden by an accidental scroll. */
    bool slotPressed = false;
    for (int i = 0; i < WPN_ID_COUNT; i++)
    {
        if (bp_pressed((bp_action)(ACT_WEAPON_1 + i)))
        {
            bp_weapons_try_select((bp_weapon_id)i);
            slotPressed = true;
        }
    }
    if (!slotPressed)
    {
        if (bp_pressed(ACT_WEAPON_NEXT)) bp_weapons_cycle(+1);
        if (bp_pressed(ACT_WEAPON_PREV)) bp_weapons_cycle(-1);
        float wheel = GetMouseWheelMove();
        if (wheel > 0.05f) bp_weapons_cycle(+1);
        else if (wheel < -0.05f) bp_weapons_cycle(-1);
    }

    /* Fire. Auto weapons read the held state; semi-autos read the edge. Passing both lets
     * bp_weapons decide, which is where the full_auto flag already lives. */
    bp_weapons_try_fire(bp_down(ACT_FIRE), bp_pressed(ACT_FIRE));
}

static void simulate_step(float dt)
{
    g_game.total_time += dt;
    g_game.sim_steps++;

    switch (g_game.scene)
    {
        case SCENE_PLAYING:
        {
            handle_combat_input();

            float healthBefore = g_player.health;
            bp_player_update(dt);
            if (g_player.health < healthBefore)
            {
                float dmg = healthBefore - g_player.health;
                g_game.damage_taken += dmg;
                bp_hud_notify_damage(g_player.last_damage_dir, BP_CLAMP(dmg / 30.0f, 0.25f, 1.0f));
            }

            Camera3D cam = g_camera.cam;
            bp_weapons_update(dt, cam, g_player.vel, g_player.on_ground);
            bp_enemy_update(dt, g_player.pos, bp_player_eye(), bp_player_alive());
            bp_world_update(dt);
            bp_fx_update(dt, vec3y(g_world.bounds_min.y));
            bp_waves_update(dt);
            bp_audio_update(dt);

            /* pickup feedback the HUD wants to know about immediately */
            if (g_player.secret_reached && !g_game.secret_this_level)
            {
                g_game.secret_this_level = true;
                bp_hud_toast("SECRET FOUND  +500", TOAST_GOOD, 2.4f);
            }

            /* state transitions */
            if (g_player.state == PST_DYING || g_player.state == PST_DEAD)
            {
                if (g_player.state == PST_DEAD) enter_scene(SCENE_DEAD);
            }
            else if (g_waves.state == WAVE_COMPLETE && g_player.exit_reached)
            {
                bp_game_submit_run(true);
                enter_scene(SCENE_LEVEL_COMPLETE);
            }
            break;
        }

        case SCENE_LEVEL_INTRO:
            /* The world keeps ticking (movers, triggers, particles) but nothing spawns and
             * the player does not move: the intro card is a moment to read the arena. */
            bp_world_update(dt);
            bp_fx_update(dt, vec3y(g_world.bounds_min.y));
            bp_audio_update(dt);
            bp_camera_simulate(dt, g_player.pos, 0.0f, true, false, 0.0f);
            if (g_game.scene_time >= INTRO_CARD_TIME) enter_scene(SCENE_PLAYING);
            /* Or let an impatient player skip it. Skipping must not skip the wave start. */
            else if (bp_pressed(ACT_MENU_ACCEPT) || bp_pressed(ACT_FIRE))
                enter_scene(SCENE_PLAYING);
            break;

        case SCENE_DEAD:
            bp_player_update(dt);
            bp_world_update(dt);
            bp_fx_update(dt, vec3y(g_world.bounds_min.y));
            bp_audio_update(dt);
            /* The death cam keeps orbiting slowly rather than freezing: a static frame reads
             * as a hang, a drifting one reads as a camera. */
            g_camera.yaw += dt * 9.0f;
            if (g_game.scene_time >= DEAD_TO_RESULTS)
            {
                bp_game_submit_run(false);
                enter_scene(SCENE_RESULTS);
            }
            break;

        case SCENE_RESULTS:
        case SCENE_LEVEL_COMPLETE:
            bp_fx_update(dt, vec3y(g_world.bounds_min.y));
            bp_audio_update(dt);
            update_attract_camera(dt);
            /* LEVEL_COMPLETE advances once the player has seen the tally; RESULTS waits for
             * an explicit menu choice. */
            if (g_game.scene == SCENE_LEVEL_COMPLETE && g_game.scene_time > 1.2f &&
                bp_pressed(ACT_MENU_ACCEPT))
                bp_game_next_level();
            break;

        case SCENE_GAME_COMPLETE:
            bp_audio_update(dt);
            update_attract_camera(dt);
            if (g_game.scene_time > 2.0f && bp_pressed(ACT_MENU_ACCEPT)) bp_game_return_to_title();
            break;

        case SCENE_PAUSED:
            /* Simulation is frozen -- that is the definition of pause -- and so is the view.
             * Orbiting the camera while paused would look like the game is still running. */
            bp_audio_update(dt);
            break;

        case SCENE_TITLE:
            /* The title screen orbits whatever level is loaded, so the menu always has a
             * real arena behind it instead of a flat colour. */
            bp_audio_update(dt);
            update_attract_camera(dt);
            break;

        default: break;
    }

    g_game.scene_time += dt;
}

/*
 * Menu backdrop camera: a slow orbit around the arena centre, high enough to read the layout.
 *
 * Running the real camera through its normal path (rather than assigning g_camera.cam
 * directly) matters: bp_camera_render() applies shake, roll and FOV smoothing, so the menu
 * backdrop inherits exactly the same rendering contract as gameplay and cannot drift out of
 * sync when those systems change.
 */
static void update_attract_camera(float dt)
{
    if (!g_world.loaded) return;

    g_game.orbit_yaw += ORBIT_SPEED * dt;
    if (g_game.orbit_yaw > 360.0f) g_game.orbit_yaw -= 360.0f;

    Vector3 c = Vector3Scale(Vector3Add(g_world.bounds_min, g_world.bounds_max), 0.5f);
    c.y = g_world.bounds_min.y;
    g_game.orbit_center = c;

    float radius = 26.0f + sinf(g_game.orbit_yaw * BP_DEG2RAD * 0.5f) * 6.0f;
    float yawRad = g_game.orbit_yaw * BP_DEG2RAD;
    Vector3 pos = { c.x + sinf(yawRad) * radius, c.y + 11.0f, c.z + cosf(yawRad) * radius };

    /* The camera looks at the centre from outside it, so yaw points inward: +180 from the
     * orbit angle. Pitch is derived from the height difference, not authored, so changing the
     * orbit height cannot accidentally point the camera at the floor. */
    g_camera.yaw = g_game.orbit_yaw + 180.0f;
    float dy = c.y + 1.5f - pos.y;
    float hd = BP_MAX(radius, 0.001f);
    g_camera.pitch = BP_CLAMP(atan2f(dy, hd) * BP_RAD2DEG, -g_camera.pitch_limit, g_camera.pitch_limit);

    bp_camera_simulate(dt, pos, 0.0f, true, false, 0.0f);
}

/* -------------------------------------------------------------------------------------- */
/* render                                                                                  */
/* -------------------------------------------------------------------------------------- */
static void draw_boot_screen(void)
{
    bp_ui_begin_frame();
    bp_ui_dim(1.0f);

    float cx = (float)UI_VW * 0.5f;
    bp_ui_text_shadow(cx, 132.0f, UI_FS_LARGE, UI_ALIGN_CENTER, g_assets.pal_accent,
                      (Color){ 0, 0, 0, 230 }, "%s", BP_GAME_NAME);

    float bw = 260.0f, bx = cx - bw * 0.5f, by = 186.0f;
    bp_ui_bar(bx, by, bw, 8.0f, g_game.boot_progress, g_assets.pal_accent2,
              (Color){ 14, 16, 20, 255 }, (Color){ 92, 226, 196, 90 });
    bp_ui_text_align(cx, by + 14.0f, UI_FS_TINY, UI_ALIGN_CENTER, g_assets.pal_text_dim, "%s",
               g_game.boot_status[0] ? g_game.boot_status : "STARTING");
    bp_ui_text_align(cx, (float)UI_VH - 20.0f, UI_FS_TINY, UI_ALIGN_CENTER, (Color){ 96, 102, 114, 220 },
               "v%s   %s", BP_VERSION_STRING, BP_STUDIO);
    bp_ui_end_frame();
}

static void draw_intro_card(void)
{
    /* The name card is drawn by the HUD banner; here we only add the difficulty line and a
     * "press fire to skip" prompt once the card has been up long enough to be read. */
    if (g_game.scene_time > 1.0f)
        bp_ui_text_align((float)UI_VW * 0.5f, (float)UI_VH - 44.0f, UI_FS_TINY, UI_ALIGN_CENTER,
                   g_assets.pal_text_dim, "CLICK TO DEPLOY");
}

static void render_frame(void)
{
    if (!g_game.boot_done) { draw_boot_screen(); return; }

    bp_gfx_begin_scene();

    /* One interpolated camera for the whole frame. Every draw below uses this exact value,
     * so there is no possibility of the world and the viewmodel disagreeing by a frame. */
    Camera3D cam = bp_camera_render(g_game.alpha);

    if (g_world.loaded)
    {
        bp_world_draw_sky(cam);
        bp_world_draw(cam, cam.position);

        Vector3 viewer = cam.position;
        bp_enemy_draw(cam, viewer);
        bp_weapons_draw_projectiles(cam);
        bp_pickup_draw(cam, viewer);
        bp_fx_draw(cam);

        /* The viewmodel is a separate orthographic pass inside the same 3D frame; it must be
         * drawn last so it cannot be occluded by world geometry. */
        if (g_game.scene == SCENE_PLAYING || g_game.scene == SCENE_LEVEL_INTRO ||
            g_game.scene == SCENE_PAUSED)
            bp_weapons_draw_viewmodel(cam, bp_player_speed01());

        if (g_engine.perf.show_overlay)
        {
            bp_world_draw_debug();
            bp_enemy_draw_debug();
            bp_player_draw_debug();
        }
    }

    bp_gfx_end_scene();

    /* raylib 5.5 deliberately does not expose a draw-call counter through its public API
     * (rlgl keeps one internally but publishes no accessor), so the overlay reports what can
     * actually be measured: FPS, wall time per frame, and our own sim/draw timers. Reporting
     * a fabricated draw-call number would be worse than reporting none. */

    /* ---- UI: after the scene resolve, at native resolution ---- */
    bp_ui_begin_frame();

    switch (g_game.scene)
    {
        case SCENE_PLAYING:
        case SCENE_LEVEL_INTRO:
            bp_hud_draw();
            if (g_game.scene == SCENE_LEVEL_INTRO) draw_intro_card();
            break;

        case SCENE_PAUSED:
            bp_hud_draw();
            bp_menu_draw();
            break;

        case SCENE_DEAD:
        {
            /* Death: desaturating vignette + the one line the player needs ("you died, and
             * here is what it cost"). No menu -- the transition is automatic and short. */
            float k = BP_CLAMP(g_game.scene_time / 0.6f, 0.0f, 1.0f);
            bp_ui_dim(0.30f * k);
            bp_ui_vgradient(0.0f, 0.0f, (float)UI_VW, (float)UI_VH,
                            (Color){ 60, 6, 10, (unsigned char)(110 * k) },
                            (Color){ 10, 2, 4, (unsigned char)(150 * k) });
            bp_ui_text_shadow((float)UI_VW * 0.5f, (float)UI_VH * 0.5f - 10.0f, UI_FS_LARGE,
                              UI_ALIGN_CENTER, mc_game(232, 52, 60, 255 * k), (Color){ 0, 0, 0, 220 },
                              "YOU DIED");
            bp_ui_text_align((float)UI_VW * 0.5f, (float)UI_VH * 0.5f + 18.0f, UI_FS_TINY, UI_ALIGN_CENTER,
                       mc_game(200, 206, 216, 220 * k), "SCORE %d   KILLS %d   WAVE %d/%d",
                       g_player.score, g_player.kills, BP_MAX(1, g_waves.wave_index),
                       BP_MAX(1, g_waves.wave_total));
            break;
        }

        case SCENE_RESULTS:
        case SCENE_LEVEL_COMPLETE:
        case SCENE_GAME_COMPLETE:
            bp_hud_draw();
            bp_menu_draw();
            break;

        case SCENE_TITLE:
            bp_menu_draw();
            break;

        default: break;
    }

    bp_ui_end_frame();
}

/* -------------------------------------------------------------------------------------- */
/* boot                                                                                    */
/* -------------------------------------------------------------------------------------- */
static void boot_stage(int stage)
{
    switch (stage)
    {
        case BOOT_LOG:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "INITIALISING");
            BP_LOGI("game: %s v%s -- %s", BP_GAME_NAME, BP_VERSION_STRING, BP_STUDIO);
            break;
        case BOOT_INPUT:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "INPUT");
            bp_input_init();
            break;
        case BOOT_ASSETS:
            /* The single slowest boot stage: every texture, model and glyph atlas in the game
             * is generated here. It logs its own per-group timings, so a slow machine can be
             * diagnosed from the console without a profiler. */
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "GENERATING TEXTURES");
            bp_assets_init();
            break;
        case BOOT_AUDIO:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "SYNTHESISING AUDIO BANK");
            bp_audio_init();
            break;
        case BOOT_GFX:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "RENDERER");
            bp_gfx_init();
            break;
        case BOOT_FX:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "PARTICLE SYSTEM");
            bp_fx_init();
            break;
        case BOOT_WORLD:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "WORLD SYSTEMS");
            bp_world_init();
            break;
        case BOOT_WEAPONS:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "WEAPONS");
            bp_weapons_init();
            break;
        case BOOT_ENEMIES:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "ENEMIES");
            bp_enemies_init();
            break;
        case BOOT_PLAYER:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "PLAYER CONTROLLER");
            bp_player_init();
            bp_camera_init();
            break;
        case BOOT_WAVES:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "ENCOUNTER DIRECTOR");
            bp_waves_init();
            break;
        case BOOT_SAVE:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "LOADING PROFILE");
            bp_save_init();
            break;
        case BOOT_UI:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "INTERFACE");
            bp_ui_init();
            break;
        case BOOT_HUD:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "HUD");
            bp_hud_init();
            /* The debug overlay flag is a setting, not a HUD default: restore it here, after
             * bp_save_init() has run, so a developer's saved preference survives a restart. */
            g_engine.perf.show_overlay = g_save.settings.show_fps;
            break;
        case BOOT_MENU:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "MENUS");
            bp_menu_init();
            break;
        case BOOT_LEVEL:
            /* Load level 0 as the attract-mode backdrop. This is the slowest single stage
             * (geometry bake), so it is last and gets its own frame. */
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "BUILDING %s",
                     g_level_count > 0 ? g_levels[0].display_name : "ARENA");
            bp_world_load(0);
            g_game.level_index = 0;
            float y = 0.0f;
            Vector3 start = bp_world_player_start(&y);
            g_game.orbit_center = start;
            g_game.orbit_yaw = 0.0f;
            bp_player_reset(start, y);
            bp_weapons_reset_loadout(true);
            break;
        default: break;
    }
}

static int s_boot_stage = 0;

static void boot_tick(void)
{
    if (s_boot_stage < BOOT_COUNT)
    {
        boot_stage(s_boot_stage);
        s_boot_stage++;
        g_game.boot_progress = (float)s_boot_stage / (float)BOOT_COUNT;
        return;
    }

    /* Runs exactly once. Without this guard the tail re-executes on every subsequent frame:
     * it would re-enter SCENE_TITLE forever (so the attract camera and banner never
     * progress), re-log "boot complete" every frame, and -- worst -- re-enable event waiting
     * and override any caller that deliberately asked for polling, such as --selftest. */
    g_game.boot_done = true;
    g_game.boot_progress = 1.0f;

    /*
     * Deliberately does NOT enable event waiting here.
     *
     * bp_game_frame() is called between BeginDrawing() and EndDrawing(), so anything it does
     * to the window's blocking mode takes effect in *this* frame's EndDrawing(). Enabling it
     * here means the very frame that finishes boot is the first one that can park forever in
     * PollInputEvents on a display that delivers no events -- which is exactly how a
     * headless or unfocused window hangs with a fully-initialised game and no output.
     *
     * Blocking on the event queue is a presentation concern, so main() decides it: it flips
     * g_engine.event_wait_allowed once boot is done and calls bp_apply_event_wait() outside
     * the frame bracket. See bp_engine::event_wait_allowed.
     */
    enter_scene(SCENE_TITLE);
    BP_LOGI("game: boot complete in %.2fs", (double)g_engine.time_total);
}

/* -------------------------------------------------------------------------------------- */
/* frame                                                                                   */
/* -------------------------------------------------------------------------------------- */
void bp_game_frame(float frameDt)
{
    /* Clamp twice: the caller is supposed to, and a backgrounded tab will hand us a
     * multi-second delta anyway. Without this the accumulator tries to run hundreds of
     * catch-up steps and the game locks up exactly when the player comes back. */
    if (!(frameDt > 0.0f)) frameDt = SIM_DT;
    if (frameDt > MAX_FRAME_DT) frameDt = MAX_FRAME_DT;

    g_engine.time_total += frameDt;
    g_engine.perf.frame_ms = bp_damp(g_engine.perf.frame_ms, frameDt * 1000.0f, 8.0f, frameDt);
    g_engine.perf.fps = g_engine.perf.frame_ms > 0.01f ? (int)(1000.0f / g_engine.perf.frame_ms) : 0;
    if (IsWindowResized()) { bp_gfx_resize(); bp_ui_resize(); }
    if (WindowShouldClose() || g_game.quit_requested) g_engine.quit_requested = true;

    bp_input_begin_frame();

    /* Virtual input is layered on *after* real polling and *before* the simulation steps, so
     * a self-test or a replay can drive the game without the OS keyboard agreeing with it.
     * begin_frame() derives held/pressed from physical state, which would otherwise erase
     * anything injected from outside the frame. See bp_input.h. */
    bp_input_apply_virtual();

    if (!g_game.boot_done)
    {
        boot_tick();
        /* Still let the renderer breathe: begin/end frame so the loading bar actually paints
         * instead of the window staying blank until boot finishes. */
        bp_gfx_begin_scene();
        bp_gfx_end_scene();
        render_frame();
        bp_input_end_frame();
        return;
    }

    handle_global_input();

    /* Menus are frame-rate driven, not simulation driven: see bp_menu.c. */
    if (bp_menu_is_open())
    {
        int action = bp_menu_update(frameDt);
        if (action == MENU_ACT_BACK) bp_game_apply_menu_action(GA_BACK);
        else if (action > 0) bp_game_apply_menu_action(action);
    }

    /* ---- fixed timestep ---- */
    float timeScale = BP_CLAMP(g_engine.time_scale, 0.0f, 2.0f);
    g_game.accumulator += frameDt * timeScale;
    g_game.sim_steps = 0;

    int steps = 0;
    while (g_game.accumulator >= SIM_DT && steps < MAX_SIM_STEPS_PER_FRAME)
    {
        simulate_step(SIM_DT);
        g_game.accumulator -= SIM_DT;
        steps++;
    }
    /* If we hit the step cap, discard the backlog rather than falling further behind. This
     * is the "spiral of death" guard: a machine that cannot keep up drops simulation time
     * instead of consuming every frame trying to repay a debt it cannot afford. */
    if (g_game.accumulator > SIM_DT * (float)MAX_SIM_STEPS_PER_FRAME) g_game.accumulator = 0.0f;
    g_game.alpha = BP_CLAMP(g_game.accumulator / SIM_DT, 0.0f, 1.0f);

    g_engine.perf.sim_steps = steps;

    /* ---- HUD is frame-driven: its smoothing must match the display rate, not the sim rate,
     * or a 240 Hz player sees a jitterier health bar than a 60 Hz one. ---- */
    if (g_game.scene == SCENE_PLAYING || g_game.scene == SCENE_LEVEL_INTRO ||
        g_game.scene == SCENE_PAUSED || g_game.scene == SCENE_DEAD)
        bp_hud_update(frameDt);

    render_frame();
    bp_input_end_frame();
}

/* -------------------------------------------------------------------------------------- */
/* init / shutdown                                                                         */
/* -------------------------------------------------------------------------------------- */
void bp_game_init(void)
{
    memset(&g_game, 0, sizeof(g_game));
    g_game.scene = SCENE_BOOT;
    g_game.difficulty = 1;
    g_game.level_index = 0;
    g_game.orbit_radius = 26.0f;
    g_engine.time_scale = 1.0f;
    s_boot_stage = 0;
    BP_LOGI("game: bp_game_init (boot is staged, %d stages)", BOOT_COUNT);
}

void bp_game_shutdown(void)
{
    if (g_save.dirty)
    {
        bp_save_write_settings();
        bp_save_write_progress();
    }
    bp_save_sync_platform();
    bp_world_unload();
    bp_world_shutdown();
    bp_audio_shutdown();
    bp_gfx_shutdown();
    bp_assets_shutdown();
    BP_LOGI("game: shutdown complete");
}
