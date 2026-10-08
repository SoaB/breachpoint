/******************************************************************************************
 * bp_menu.c -- menu screens.
 *
 * Note on frame rate: bp_menu_update() runs once per rendered frame, not once per 120 Hz sim
 * step. Menus are pure input/UI and have no physics, so tying them to the frame keeps edge
 * triggers correct -- an input edge exists for exactly one frame, and consuming it inside a
 * loop that runs 1-8 times per frame would either double-fire or drop it.
 *
 * Note on layout: the virtual canvas is 640x360, which fits about 15 rows. The options screen
 * has more than that, so rows flow into a second column (`column_rows`) instead of the menu
 * growing a scrollbar. Two columns of a list is a pattern players already know; a scrollbar
 * in a game menu is a pattern nobody has ever enjoyed.
 ******************************************************************************************/
#include "bp_menu.h"

#include "bp_ui.h"
#include "bp_save.h"
#include "bp_world.h"
#include "bp_waves.h"
#include "bp_player.h"
#include "bp_weapons.h"
#include "bp_hud.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_gfx.h"
#include "../engine/bp_input.h"
#include "../engine/bp_camera.h"
#include "../engine/bp_assets.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

bp_menu g_menu = { 0 };

/* -------------------------------------------------------------------------------------- */
/* layout constants                                                                        */
/* -------------------------------------------------------------------------------------- */
#define MENU_ROW_H         18.0f
#define MENU_LIST_W       296.0f
#define MENU_COL_GAP       12.0f
#define MENU_MARGIN         8.0f
#define MENU_HINT_Y        ((float)UI_VH - 26.0f)
#define MENU_SLIDE_TIME     0.22f

/* FOV lives in the profile as an int but the slider row works on floats, so the options
 * screen edits this mirror and pushes it back on APPLY. Nothing outside this file reads it. */
static float s_fov_mirror = 95.0f;

/* -------------------------------------------------------------------------------------- */
/* small helpers                                                                           */
/* -------------------------------------------------------------------------------------- */
static inline Color mc(float r, float g, float b, float a)
{
    return (Color){ (unsigned char)BP_CLAMP((int)r, 0, 255), (unsigned char)BP_CLAMP((int)g, 0, 255),
                    (unsigned char)BP_CLAMP((int)b, 0, 255), (unsigned char)BP_CLAMP((int)a, 0, 255) };
}

/* Action ids. Negatives are reserved by the framework (see bp_menu.h). */
enum {
    A_PLAY = 1, A_LEVELS, A_OPTIONS, A_CONTROLS, A_CREDITS, A_QUIT,
    A_RESUME, A_RESTART, A_ABORT,
    A_RETRY, A_NEXT_LEVEL, A_TO_MENU,
    A_BACK, A_RESET_BINDS, A_RESET_SETTINGS, A_APPLY,
    A_LEVEL_BASE = 100,          /* + level index  */
    A_BIND_BASE  = 200           /* + action index */
};

static const char *s_quality_choices[] = { "LOW", "MEDIUM", "HIGH", "ULTRA" };
static const char *s_diff_choices[]    = { "RECRUIT", "VETERAN", "NIGHTMARE", "BREACH" };

/* -------------------------------------------------------------------------------------- */
/* list plumbing                                                                           */
/* -------------------------------------------------------------------------------------- */
static void list_clear(void)
{
    memset(g_menu.list.rows, 0, sizeof(g_menu.list.rows));
    g_menu.list.count = 0;
    g_menu.list.column_rows = 0;
    g_menu.list.column_gap = MENU_COL_GAP;
    g_menu.list.selected = 0;
    g_menu.list.hovered = -1;
}

static bp_menu_row *list_push(bp_menu_row_kind kind, const char *label, int id)
{
    if (g_menu.list.count >= MENU_MAX_ROWS) return NULL;
    bp_menu_row *r = &g_menu.list.rows[g_menu.list.count++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->id = id;
    r->enabled = true;
    snprintf(r->label, sizeof(r->label), "%s", label ? label : "");
    return r;
}

static inline bool row_selectable(const bp_menu_row *r)
{
    return r->enabled && r->kind != MROW_LABEL && r->kind != MROW_SPACER;
}

/* Row i's rectangle in virtual space, honouring the column flow. */
static void row_rect(int i, float *out_x, float *out_y)
{
    int per = g_menu.list.column_rows;
    int col = 0, row = i;
    if (per > 0 && i >= per)
    {
        col = i / per;
        row = i % per;
    }
    *out_x = g_menu.list.x + (float)col * (MENU_LIST_W + g_menu.list.column_gap);
    *out_y = g_menu.list.y + (float)row * MENU_ROW_H;
}

static void list_layout(void)
{
    /* Single column, centred. With two columns the pair is centred as a block. */
    float totalW = (g_menu.list.column_rows > 0 && g_menu.list.count > g_menu.list.column_rows)
                 ? MENU_LIST_W * 2.0f + g_menu.list.column_gap
                 : MENU_LIST_W;
    g_menu.list.w = MENU_LIST_W;
    g_menu.list.x = ((float)UI_VW - totalW) * 0.5f;

    if (g_menu.list.selected >= g_menu.list.count) g_menu.list.selected = BP_MAX(0, g_menu.list.count - 1);
    if (g_menu.list.count > 0 && !row_selectable(&g_menu.list.rows[g_menu.list.selected]))
    {
        for (int i = 0; i < g_menu.list.count; i++)
        {
            int idx = (g_menu.list.selected + i) % g_menu.list.count;
            if (row_selectable(&g_menu.list.rows[idx])) { g_menu.list.selected = idx; break; }
        }
    }
}

static void move_selection(int dir)
{
    int n = g_menu.list.count;
    if (n <= 0) return;
    for (int step = 1; step <= n; step++)
    {
        int idx = (g_menu.list.selected + dir * step) % n;
        if (idx < 0) idx += n;
        if (!row_selectable(&g_menu.list.rows[idx])) continue;
        if (idx == g_menu.list.selected) return;
        g_menu.list.selected = idx;
        bp_audio_play(SFX_UI_MOVE, 0.55f);
        return;
    }
}

static void refresh_value(bp_menu_row *r)
{
    switch (r->kind)
    {
        case MROW_TOGGLE:
            snprintf(r->value, sizeof(r->value), "%s", (r->bval && *r->bval) ? "ON" : "OFF");
            break;
        case MROW_CYCLE:
            if (r->ival && r->choices && r->choice_count > 0)
            {
                int i = BP_CLAMP(*r->ival, 0, r->choice_count - 1);
                snprintf(r->value, sizeof(r->value), "%s", r->choices[i]);
            }
            break;
        case MROW_SLIDER:
            if (r->fval)
            {
                float v = BP_CLAMP(*r->fval, r->vmin, r->vmax);
                /* Percent for 0..1 ranges, integer otherwise. Inferring the unit from the
                 * range saves a format string per row and every slider here is one of those
                 * two shapes. */
                if (r->vmax <= 1.001f && r->vmin >= -0.001f)
                    snprintf(r->value, sizeof(r->value), "%d%%", (int)(v * 100.0f + 0.5f));
                else
                    snprintf(r->value, sizeof(r->value), "%d", (int)(v + 0.5f));
            }
            break;
        default: break;
    }
}

static void nudge_value(bp_menu_row *r, int dir)
{
    if (!r || !r->enabled) return;
    bool changed = false;

    if (r->kind == MROW_TOGGLE && r->bval) { *r->bval = !*r->bval; changed = true; }
    else if (r->kind == MROW_CYCLE && r->ival && r->choice_count > 0)
    {
        int n = r->choice_count;
        *r->ival = ((*r->ival + dir) % n + n) % n;
        changed = true;
    }
    else if (r->kind == MROW_SLIDER && r->fval)
    {
        float step = r->vstep > 0.0f ? r->vstep : (r->vmax - r->vmin) * 0.05f;
        float v = BP_CLAMP(*r->fval + step * (float)dir, r->vmin, r->vmax);
        if (v != *r->fval) { *r->fval = v; changed = true; }
    }

    if (changed)
    {
        refresh_value(r);
        bp_audio_play(SFX_UI_MOVE, 0.5f);
        g_save.dirty = true;
    }
}

/* -------------------------------------------------------------------------------------- */
/* screen construction                                                                     */
/* -------------------------------------------------------------------------------------- */
static void build_title(void)
{
    list_clear();
    g_menu.list.y = 140.0f;
    bp_menu_row *r;

    int lvl = BP_CLAMP(g_save.settings.last_level, 0, BP_MAX(g_level_count - 1, 0));
    int dif = BP_CLAMP(g_save.settings.difficulty, 0, g_difficulty_count - 1);

    r = list_push(MROW_ACTION, "NEW RUN", A_PLAY);
    if (r) snprintf(r->hint, sizeof(r->hint), "%s -- %s",
                    g_level_count > 0 ? g_levels[lvl].display_name : "arena",
                    g_difficulties[dif].name);

    r = list_push(MROW_ACTION, "SELECT AREA", A_LEVELS);
    if (r) snprintf(r->hint, sizeof(r->hint), "%d%% of area/difficulty pairs cleared",
                    bp_save_completion_percent());

    r = list_push(MROW_ACTION, "OPTIONS", A_OPTIONS);
    if (r) snprintf(r->hint, sizeof(r->hint), "Video, audio, gameplay, HUD");

    r = list_push(MROW_ACTION, "CONTROLS", A_CONTROLS);
    if (r) snprintf(r->hint, sizeof(r->hint), "Rebind any key, reset to defaults");

    r = list_push(MROW_ACTION, "CREDITS", A_CREDITS);

    r = list_push(MROW_ACTION, "QUIT", A_QUIT);
    if (r) snprintf(r->hint, sizeof(r->hint), "Progress is saved automatically");
}

static void build_levels(void)
{
    list_clear();
    g_menu.list.y = 84.0f;

    for (int i = 0; i < g_level_count && i < SAVE_MAX_LEVELS; i++)
    {
        const bp_level_def *ld = &g_levels[i];
        bool unlocked = bp_save_level_unlocked(i);

        bp_menu_row *r = list_push(MROW_ACTION, ld->display_name, unlocked ? (A_LEVEL_BASE + i) : 0);
        if (!r) break;
        r->enabled = unlocked;

        /* Best record across all difficulties, so the row reads as "your history here"
         * rather than as one difficulty's numbers. */
        int bestScore = 0, bestWave = 0, clears = 0;
        float bestTime = 0.0f;
        for (int d = 0; d < 4; d++)
        {
            const bp_level_record *rec = &g_save.levels[i][d];
            if (rec->best_score > bestScore) bestScore = rec->best_score;
            if (rec->best_wave  > bestWave)  bestWave  = rec->best_wave;
            if (rec->completed) clears++;
            if (rec->best_time > 0.0f && (bestTime <= 0.0f || rec->best_time < bestTime)) bestTime = rec->best_time;
        }

        if (!unlocked)          snprintf(r->value, sizeof(r->value), "LOCKED");
        else if (clears > 0)    snprintf(r->value, sizeof(r->value), "%d   %02d:%02d", bestScore,
                                         (int)(bestTime / 60.0f), ((int)bestTime) % 60);
        else if (bestWave > 0)  snprintf(r->value, sizeof(r->value), "WAVE %d   %d", bestWave, bestScore);
        else                    snprintf(r->value, sizeof(r->value), "%d WAVES", ld->waves);

        if (unlocked) snprintf(r->hint, sizeof(r->hint), "%s", ld->subtitle);
        else snprintf(r->hint, sizeof(r->hint), "Clear %s to unlock",
                      (i > 0 && i - 1 < g_level_count) ? g_levels[i - 1].display_name : "the first area");
    }

    bp_menu_row *b = list_push(MROW_ACTION, "BACK", A_BACK);
    if (b) b->hint[0] = '\0';
}

static void build_options(void)
{
    list_clear();
    g_menu.list.y = 78.0f;
    /* 20 rows would not fit in one 360-unit-tall canvas; flow into two columns of 11. */
    g_menu.list.column_rows = 11;

    bp_settings *s = &g_save.settings;
    bp_menu_row *r;

    r = list_push(MROW_LABEL, "VIDEO", 0);

    r = list_push(MROW_CYCLE, "Quality", 0);
    if (r) { r->ival = (int *)&s->quality; r->choices = s_quality_choices; r->choice_count = BP_QUALITY_COUNT;
             snprintf(r->hint, sizeof(r->hint), "Internal resolution and post-FX preset"); }

    r = list_push(MROW_SLIDER, "Resolution Scale", 0);
    if (r) { r->fval = &s->resolution_scale; r->vmin = 0.0f; r->vmax = 1.5f; r->vstep = 0.25f;
             snprintf(r->hint, sizeof(r->hint), "0 = follow the quality preset"); }

    r = list_push(MROW_SLIDER, "Field of View", 0);
    if (r) { r->fval = &s_fov_mirror; r->vmin = 70.0f; r->vmax = 120.0f; r->vstep = 1.0f;
             snprintf(r->hint, sizeof(r->hint), "Applied on APPLY"); }

    r = list_push(MROW_TOGGLE, "CRT Filter", 0);
    if (r) { r->bval = &s->crt_filter; snprintf(r->hint, sizeof(r->hint), "Scanlines and a slight barrel tint"); }

    r = list_push(MROW_TOGGLE, "VSync", 0);
    if (r) { r->bval = &s->vsync; snprintf(r->hint, sizeof(r->hint), "Event-wait mode changes immediately"); }

    r = list_push(MROW_LABEL, "AUDIO", 0);

    r = list_push(MROW_SLIDER, "Master Volume", 0);
    if (r) { r->fval = &s->vol_master; r->vmin = 0.0f; r->vmax = 1.0f; r->vstep = 0.05f; }
    r = list_push(MROW_SLIDER, "Effects", 0);
    if (r) { r->fval = &s->vol_sfx; r->vmin = 0.0f; r->vmax = 1.0f; r->vstep = 0.05f; }
    r = list_push(MROW_SLIDER, "Music", 0);
    if (r) { r->fval = &s->vol_music; r->vmin = 0.0f; r->vmax = 1.0f; r->vstep = 0.05f;
             snprintf(r->hint, sizeof(r->hint), "Procedural score; intensity follows the fight"); }

    r = list_push(MROW_LABEL, "GAMEPLAY", 0);

    r = list_push(MROW_SLIDER, "Sensitivity", 0);
    if (r) { r->fval = &s->sensitivity; r->vmin = 0.02f; r->vmax = 1.0f; r->vstep = 0.01f; }
    r = list_push(MROW_TOGGLE, "Invert Y", 0);
    if (r) { r->bval = &s->invert_y; }
    r = list_push(MROW_CYCLE, "Difficulty", 0);
    if (r) { r->ival = &s->difficulty; r->choices = s_diff_choices; r->choice_count = g_difficulty_count;
             snprintf(r->hint, sizeof(r->hint), "%s",
                      g_difficulties[BP_CLAMP(s->difficulty, 0, g_difficulty_count - 1)].blurb); }

    r = list_push(MROW_LABEL, "HUD", 0);
    r = list_push(MROW_TOGGLE, "Show HUD", 0);
    if (r) { r->bval = &s->hud_visible; }
    r = list_push(MROW_TOGGLE, "Minimal HUD", 0);
    if (r) { r->bval = &s->hud_minimal; snprintf(r->hint, sizeof(r->hint), "Health, ammo and crosshair only"); }
    r = list_push(MROW_TOGGLE, "Show FPS", 0);
    if (r) { r->bval = &s->show_fps; }

    r = list_push(MROW_ACTION, "APPLY", A_APPLY);
    if (r) snprintf(r->hint, sizeof(r->hint), "Pushes settings live and writes them to disk");
    r = list_push(MROW_ACTION, "RESET DEFAULTS", A_RESET_SETTINGS);
    if (r) snprintf(r->hint, sizeof(r->hint), "Settings only -- run records are never touched");
    r = list_push(MROW_ACTION, "BACK", A_BACK);
}

static void build_controls(void)
{
    list_clear();
    g_menu.list.y = 46.0f;
    g_menu.list.row_h = MENU_ROW_H;
    g_menu.list.column_rows = 12;

    /* Only actions a player would plausibly rebind are exposed. Menu navigation is
     * deliberately absent: rebinding ACT_MENU_UP away from W would break this very screen. */
    static const bp_action exposed[] = {
        ACT_MOVE_FORWARD, ACT_MOVE_BACK, ACT_MOVE_LEFT, ACT_MOVE_RIGHT,
        ACT_JUMP, ACT_CROUCH, ACT_SPRINT,
        ACT_FIRE, ACT_ALT_FIRE, ACT_RELOAD,
        ACT_WEAPON_1, ACT_WEAPON_2, ACT_WEAPON_3, ACT_WEAPON_4, ACT_WEAPON_5,
        ACT_WEAPON_NEXT, ACT_WEAPON_PREV, ACT_USE, ACT_PAUSE, ACT_SCOREBOARD
    };
    const int exposedCount = (int)(sizeof(exposed) / sizeof(exposed[0]));

    for (int i = 0; i < exposedCount; i++)
    {
        bp_action a = exposed[i];
        bp_menu_row *r = list_push(MROW_ACTION, g_input.act[a].name, A_BIND_BASE + (int)a);
        if (!r) break;

        const char *k0 = bp_input_key_name(g_input.act[a].keys[0]);
        const char *k1 = bp_input_key_name(g_input.act[a].keys[1]);
        /* bp_input_key_name returns "--" for an empty slot; only show a second bind when
         * there actually is one, so the column stays narrow and scannable. */
        if (k1 && k1[0] && strcmp(k1, "--") != 0) snprintf(r->value, sizeof(r->value), "%s / %s", k0, k1);
        else             snprintf(r->value, sizeof(r->value), "%s", k0);

        if (g_menu.capturing_action == (int)a)
            snprintf(r->hint, sizeof(r->hint), "Waiting for input...");
        else
            snprintf(r->hint, sizeof(r->hint), "Click or select, then press a key");
    }

    bp_menu_row *rb = list_push(MROW_ACTION, "RESET BINDINGS", A_RESET_BINDS);
    if (rb) snprintf(rb->hint, sizeof(rb->hint), "Restores the shipped keymap");
    bp_menu_row *b = list_push(MROW_ACTION, "BACK", A_BACK);
    if (b) snprintf(b->hint, sizeof(b->hint), "Bindings are saved automatically");
}

static void build_pause(void)
{
    list_clear();
    g_menu.list.y = 96.0f;
    bp_menu_row *r;

    r = list_push(MROW_ACTION, "RESUME", A_RESUME);
    /* Clamped to the real ranges so the compiler can prove the hint cannot truncate:
     * g_world.name is 48 bytes and the hint is 64, so an unclamped "%d" (up to 10 digits
     * in the compiler's view) is not provably safe. */
    if (r) snprintf(r->hint, sizeof(r->hint), "%s -- wave %d/%d",
                    g_world.loaded ? g_world.name : "arena",
                    BP_CLAMP(g_waves.wave_index, 1, 999), BP_CLAMP(g_waves.wave_total, 1, 999));

    r = list_push(MROW_ACTION, "OPTIONS", A_OPTIONS);
    r = list_push(MROW_ACTION, "CONTROLS", A_CONTROLS);

    r = list_push(MROW_ACTION, "RESTART AREA", A_RESTART);
    if (r) snprintf(r->hint, sizeof(r->hint), "Keeps your score, resets the arena");

    r = list_push(MROW_ACTION, "ABANDON RUN", A_ABORT);
    if (r) snprintf(r->hint, sizeof(r->hint), "Records this run, returns to the title");
}

static void build_results(void)
{
    list_clear();
    g_menu.list.y = 268.0f;
    bp_menu_row *r;

    int next = g_save.settings.last_level + 1;
    if (g_menu.result_victory && next < g_level_count && bp_save_level_unlocked(next))
    {
        r = list_push(MROW_ACTION, "NEXT AREA", A_NEXT_LEVEL);
        if (r) snprintf(r->hint, sizeof(r->hint), "%s -- %s", g_levels[next].display_name, g_levels[next].subtitle);
    }

    r = list_push(MROW_ACTION, g_menu.result_victory ? "RUN IT AGAIN" : "RETRY", A_RETRY);
    if (r) snprintf(r->hint, sizeof(r->hint), "Same area, same difficulty");

    r = list_push(MROW_ACTION, "MAIN MENU", A_TO_MENU);
    if (r) r->hint[0] = '\0';
}

static void build_credits(void)
{
    list_clear();
    g_menu.list.y = (float)UI_VH - 44.0f;
    bp_menu_row *r = list_push(MROW_ACTION, "BACK", A_BACK);
    if (r) r->hint[0] = '\0';
}

static void build_quit(void)
{
    list_clear();
    g_menu.list.y = 168.0f;
    bp_menu_row *r = list_push(MROW_ACTION, "YES, QUIT", A_QUIT);
    if (r) snprintf(r->hint, sizeof(r->hint), "Progress is already on disk");
    r = list_push(MROW_ACTION, "NO, GO BACK", A_BACK);
    if (r) r->hint[0] = '\0';
    g_menu.list.selected = 1;      /* default to the safe answer */
}

static void build_screen(bp_menu_screen s)
{
    int keepSelected = g_menu.list.selected;
    switch (s)
    {
        case MENU_SCREEN_TITLE:    build_title(); break;
        case MENU_SCREEN_LEVELS:   build_levels(); break;
        case MENU_SCREEN_OPTIONS:  s_fov_mirror = (float)g_save.settings.fov; build_options(); break;
        case MENU_SCREEN_CONTROLS: build_controls(); break;
        case MENU_SCREEN_PAUSE:    build_pause(); break;
        case MENU_SCREEN_RESULTS:  build_results(); break;
        case MENU_SCREEN_CREDITS:  build_credits(); break;
        case MENU_SCREEN_QUIT:     build_quit(); break;
        default: list_clear(); break;
    }
    /* Rebuilding a screen (after a rebind, for example) must not throw the player back to
     * the top of the list. */
    if (s == g_menu.screen && keepSelected < g_menu.list.count) g_menu.list.selected = keepSelected;
    list_layout();
    g_menu.list.open_time = 0.0f;
    g_menu.list.hovered = -1;
}

/* -------------------------------------------------------------------------------------- */
/* lifecycle                                                                               */
/* -------------------------------------------------------------------------------------- */
void bp_menu_init(void)
{
    memset(&g_menu, 0, sizeof(g_menu));
    g_menu.screen = MENU_SCREEN_NONE;
    g_menu.capturing_action = -1;
    g_menu.initialised = true;
    build_screen(MENU_SCREEN_TITLE);
    g_menu.screen = MENU_SCREEN_NONE;
}

void bp_menu_open(bp_menu_screen screen)
{
    if (g_menu.screen != MENU_SCREEN_NONE) g_menu.return_to = g_menu.screen;
    g_menu.screen = screen;
    g_menu.capturing_action = -1;

    /* Each builder sets its own default selection (0 for most screens, the "safe answer"
     * row for confirm dialogs), so opening always lands somewhere deliberate. Clearing
     * mouse_used matters too: a stale hover flag would let the next frame's cursor position
     * override that default before the player has moved the mouse. */
    build_screen(screen);
    g_menu.list.mouse_used = false;
    g_menu.list.hovered = -1;
    bp_audio_play(SFX_UI_SELECT, 0.7f);
}

void bp_menu_close(void)
{
    g_menu.screen = MENU_SCREEN_NONE;
    g_menu.capturing_action = -1;
}

bool bp_menu_is_open(void) { return g_menu.screen != MENU_SCREEN_NONE; }

void bp_menu_set_results(bool victory, const char *title, int score, int kills,
                         float time, float accuracy01, int wave)
{
    g_menu.result_victory = victory;
    snprintf(g_menu.result_title, sizeof(g_menu.result_title), "%s",
             title ? title : (victory ? "AREA CLEAR" : "RUN OVER"));
    g_menu.result_score = score;
    g_menu.result_kills = kills;
    g_menu.result_time = time;
    g_menu.result_accuracy = (int)(BP_CLAMP(accuracy01, 0.0f, 1.0f) * 100.0f + 0.5f);
    g_menu.result_wave = wave;
    g_menu.result_line_count = 0;
    g_menu.result_lines[0][0] = '\0';
}

void bp_menu_add_result_line(const char *fmt, ...)
{
    int cap = (int)(sizeof(g_menu.result_lines) / sizeof(g_menu.result_lines[0]));
    if (g_menu.result_line_count >= cap) return;
    char *dst = g_menu.result_lines[g_menu.result_line_count++];
    va_list a; va_start(a, fmt); vsnprintf(dst, sizeof(g_menu.result_lines[0]), fmt, a); va_end(a);
}

/* Accessors for the FOV mirror. bp_game needs to commit it on APPLY, and a getter is the
 * only way to do that without exporting the static (which would invite other modules to
 * start editing menu-local state). */
float bp_menu_fov_mirror(void) { return s_fov_mirror; }

/* Rebuild whichever screen is currently open, preserving the selection. Used after a bulk
 * settings change so every row's displayed value is re-read from the profile. */
void bp_menu_rebuild_current(void)
{
    if (g_menu.screen != MENU_SCREEN_NONE) build_screen(g_menu.screen);
}

void bp_menu_capture_key_for(int action) { g_menu.capturing_action = action; }
bool bp_menu_is_capturing(void) { return g_menu.capturing_action >= 0; }

/* -------------------------------------------------------------------------------------- */
/* activation                                                                              */
/* -------------------------------------------------------------------------------------- */
static int activate_row(bp_menu_row *r)
{
    if (!r || !r->enabled) return MENU_ACT_NONE;

    switch (r->kind)
    {
        case MROW_TOGGLE:
        case MROW_CYCLE:
        case MROW_SLIDER:
            nudge_value(r, 1);
            /* The difficulty blurb depends on the value just changed, so refresh that hint. */
            for (int i = 0; i < g_menu.list.count; i++)
                if (!strcmp(g_menu.list.rows[i].label, "Difficulty"))
                    snprintf(g_menu.list.rows[i].hint, sizeof(g_menu.list.rows[i].hint), "%s",
                             g_difficulties[BP_CLAMP(g_save.settings.difficulty, 0, g_difficulty_count - 1)].blurb);
            return MENU_ACT_NONE;

        case MROW_ACTION:
            bp_audio_play(SFX_UI_SELECT, 0.85f);
            return r->id;

        default:
            return MENU_ACT_NONE;
    }
}

static void commit_bind(int actionIndex, int code)
{
    if (actionIndex < 0 || actionIndex >= ACT_COUNT) return;
    bp_input_rebind((bp_action)actionIndex, 0, code);
    g_save.settings.binds[actionIndex][0] = code;
    g_save.dirty = true;
    g_menu.capturing_action = -1;
    bp_audio_play(SFX_UI_SELECT, 0.8f);
    build_screen(g_menu.screen);
}

/* -------------------------------------------------------------------------------------- */
/* update                                                                                  */
/* -------------------------------------------------------------------------------------- */
int bp_menu_update(float dt)
{
    if (!g_menu.initialised || g_menu.screen == MENU_SCREEN_NONE) return MENU_ACT_NONE;

    g_menu.time += dt;
    g_menu.logo_pulse = 0.5f + 0.5f * sinf(g_menu.time * 1.7f);
    g_menu.list.open_time += dt;

    /* ---- key capture swallows all other navigation ---- */
    if (g_menu.capturing_action >= 0)
    {
        /* ESC cancels rather than binding, because ESC is also ACT_PAUSE and binding pause
         * to itself would be a confusing no-op that looks like a failure. */
        if (IsKeyPressed(KEY_ESCAPE) || bp_pressed(ACT_MENU_BACK))
        {
            g_menu.capturing_action = -1;
            bp_audio_play(SFX_UI_BACK, 0.6f);
            build_screen(g_menu.screen);
            return MENU_ACT_NONE;
        }
        int key = GetKeyPressed();
        if (key > 0) { commit_bind(g_menu.capturing_action, key); return MENU_ACT_NONE; }

        for (int mb = 0; mb < 3; mb++)
        {
            if (IsMouseButtonPressed(mb))
            {
                /* Mouse buttons live in the same int space as keys in our binding table, so
                 * storing MOUSE_BUTTON_LEFT directly is correct and rebindable. */
                commit_bind(g_menu.capturing_action, BP_BIND_MOUSE_BASE + mb);
                return MENU_ACT_NONE;
            }
        }
        return MENU_ACT_NONE;
    }

    int action = MENU_ACT_NONE;

    /* ---- mouse ---- */
    Vector2 mvp = bp_ui_mouse_virtual();
    Vector2 msp = GetMousePosition();
    g_menu.list.hovered = -1;

    /*
     * Only let the cursor move the selection when it is genuinely inside the window.
     *
     * bp_ui_mouse_virtual() maps screen space into the 640x360 canvas and clamps, so a
     * cursor that is outside the window -- at the origin because nothing has moved it yet,
     * or over another monitor -- lands on a plausible-looking canvas coordinate. Without
     * this guard the very first frame of every menu hover-selects whichever row happens to
     * sit at that clamped point and silently retargets ACCEPT: the player presses enter on
     * NEW RUN and gets OPTIONS. It is invisible on a desktop because the cursor is inside
     * the window by the time anyone presses a key, and it is permanent on a headless or
     * kiosk display where the cursor never moves.
     */
    bool mouseInside = msp.x >= 0.0f && msp.y >= 0.0f &&
                       msp.x < (float)GetScreenWidth() && msp.y < (float)GetScreenHeight();

    for (int i = 0; mouseInside && i < g_menu.list.count; i++)
    {
        float rx, ry;
        row_rect(i, &rx, &ry);
        if (mvp.y < ry || mvp.y > ry + MENU_ROW_H - 2.0f) continue;
        if (mvp.x < rx - 6.0f || mvp.x > rx + g_menu.list.w + 6.0f) continue;
        if (!row_selectable(&g_menu.list.rows[i])) continue;

        g_menu.list.hovered = i;
        g_menu.list.selected = i;
        g_menu.list.mouse_used = true;

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            bp_menu_row *r = &g_menu.list.rows[i];
            float mid = rx + g_menu.list.w * 0.58f;
            if ((r->kind == MROW_SLIDER || r->kind == MROW_CYCLE || r->kind == MROW_TOGGLE) && mvp.x > mid)
            {
                /* Clicking the right third of a value row steps forward, the middle third
                 * steps back. That gives sliders a usable mouse interaction without a drag
                 * widget, which a bitmap-font UI has no business growing. */
                float third = rx + g_menu.list.w * 0.80f;
                nudge_value(r, mvp.x > third ? 1 : -1);
            }
            else if (r->kind == MROW_ACTION && r->id >= A_BIND_BASE && r->id < A_BIND_BASE + ACT_COUNT)
            {
                g_menu.capturing_action = r->id - A_BIND_BASE;
                bp_audio_play(SFX_UI_MOVE, 0.6f);
                build_screen(g_menu.screen);
                return MENU_ACT_NONE;
            }
            else action = activate_row(r);
        }
        break;
    }

    /* ---- keyboard / gamepad ---- */
    if (bp_pressed(ACT_MENU_UP))   { move_selection(-1); g_menu.list.mouse_used = false; }
    if (bp_pressed(ACT_MENU_DOWN)) { move_selection(+1); g_menu.list.mouse_used = false; }

    bp_menu_row *sel = (g_menu.list.count > 0 && g_menu.list.selected >= 0 &&
                        g_menu.list.selected < g_menu.list.count)
                     ? &g_menu.list.rows[g_menu.list.selected] : NULL;

    bool adjustable = sel && (sel->kind == MROW_SLIDER || sel->kind == MROW_CYCLE || sel->kind == MROW_TOGGLE);
    if (adjustable && bp_pressed(ACT_MENU_LEFT))  nudge_value(sel, -1);
    if (adjustable && bp_pressed(ACT_MENU_RIGHT)) nudge_value(sel, +1);

    if (bp_pressed(ACT_MENU_ACCEPT) && sel)
    {
        if (sel->kind == MROW_ACTION && sel->id >= A_BIND_BASE && sel->id < A_BIND_BASE + ACT_COUNT)
        {
            g_menu.capturing_action = sel->id - A_BIND_BASE;
            bp_audio_play(SFX_UI_MOVE, 0.6f);
            build_screen(g_menu.screen);
            return MENU_ACT_NONE;
        }
        int a = activate_row(sel);
        if (a != MENU_ACT_NONE) action = a;
    }

    if (bp_pressed(ACT_MENU_BACK))
    {
        bp_audio_play(SFX_UI_BACK, 0.7f);
        action = MENU_ACT_BACK;
    }

    for (int i = 0; i < g_menu.list.count; i++) refresh_value(&g_menu.list.rows[i]);
    return action;
}

/* -------------------------------------------------------------------------------------- */
/* drawing                                                                                 */
/* -------------------------------------------------------------------------------------- */
static void draw_rows(void)
{
    float openT = g_menu.list.open_time;

    for (int i = 0; i < g_menu.list.count; i++)
    {
        const bp_menu_row *r = &g_menu.list.rows[i];
        float rx, ry;
        row_rect(i, &rx, &ry);
        if (ry > (float)UI_VH - 14.0f) continue;

        /* Staggered slide-in. Total travel 14px, total time under a quarter second: fast
         * enough that nobody waits on it, slow enough that the eye tracks the list forming. */
        float stagger = BP_CLAMP((openT - (float)i * 0.016f) / MENU_SLIDE_TIME, 0.0f, 1.0f);
        float ease = 1.0f - (1.0f - stagger) * (1.0f - stagger);
        float a = ease;
        float dx = rx - (1.0f - ease) * 14.0f;

        if (r->kind == MROW_SPACER) continue;

        if (r->kind == MROW_LABEL)
        {
            bp_ui_rect(dx, ry + 12.0f, g_menu.list.w, 1.0f, mc(255, 138, 32, 70 * a));
            bp_ui_text_align(dx + 2.0f, ry + 2.0f, UI_FS_TINY, UI_ALIGN_LEFT,
                             mc(255, 138, 32, 200 * a), "%s", r->label);
            continue;
        }

        bool selected = (i == g_menu.list.selected);
        float dim = a * (r->enabled ? 1.0f : 0.40f);

        if (selected)
        {
            bp_ui_rect(dx - 4.0f, ry, g_menu.list.w + 8.0f, MENU_ROW_H - 2.0f, mc(255, 138, 32, 40 * dim));
            bp_ui_rect(dx - 4.0f, ry, 2.0f, MENU_ROW_H - 2.0f, mc(255, 138, 32, 255 * dim));
        }

        Color labelCol = selected ? mc(250, 251, 253, 255 * dim) : mc(196, 202, 212, 235 * dim);
        bp_ui_text_align(dx + 2.0f, ry + 2.0f, UI_FS_SMALL, UI_ALIGN_LEFT, labelCol, "%s", r->label);

        if (!r->value[0]) continue;

        bool capturing = (g_menu.capturing_action >= 0 && r->kind == MROW_ACTION &&
                          r->id == A_BIND_BASE + g_menu.capturing_action);
        Color valCol = capturing   ? mc(255, 138, 32, 255 * (0.5f + 0.5f * sinf(g_menu.time * 9.0f)))
                     : selected    ? g_assets.pal_accent2
                                   : mc(150, 158, 172, 225 * dim);

        bp_ui_text_align(dx + g_menu.list.w - 2.0f, ry + 2.0f, UI_FS_SMALL, UI_ALIGN_RIGHT, valCol,
                   "%s", capturing ? "..." : r->value);

        if (r->kind == MROW_SLIDER && r->fval)
        {
            float frac = BP_CLAMP((*r->fval - r->vmin) / BP_MAX(r->vmax - r->vmin, 0.0001f), 0.0f, 1.0f);
            float tw = 58.0f, tx = dx + g_menu.list.w - 2.0f - tw;
            bp_ui_rect(tx, ry + 14.0f, tw, 2.0f, mc(18, 20, 26, 220 * a));
            bp_ui_rect(tx, ry + 14.0f, tw * frac, 2.0f, mc(92, 226, 196, 230 * a));
            bp_ui_rect(tx + tw * frac - 1.0f, ry + 12.0f, 2.0f, 6.0f, mc(240, 244, 250, 240 * a));
        }
    }
}

static void draw_hint(void)
{
    const bp_menu_row *r = (g_menu.list.count > 0 && g_menu.list.selected >= 0 &&
                            g_menu.list.selected < g_menu.list.count)
                         ? &g_menu.list.rows[g_menu.list.selected] : NULL;

    bp_ui_rect(0.0f, MENU_HINT_Y - 4.0f, (float)UI_VW, 22.0f, (Color){ 8, 9, 12, 205 });
    bp_ui_rect(0.0f, MENU_HINT_Y - 4.0f, (float)UI_VW, 1.0f, (Color){ 255, 138, 32, 55 });

    bp_ui_text_align(MENU_MARGIN, MENU_HINT_Y, UI_FS_TINY, UI_ALIGN_LEFT, g_assets.pal_text_dim,
               g_menu.capturing_action >= 0
                 ? "PRESS A KEY OR MOUSE BUTTON -- ESC CANCELS"
                 : "W/S NAVIGATE    A/D ADJUST    ENTER SELECT    ESC BACK    MOUSE WORKS TOO");

    if (r && r->hint[0])
        bp_ui_text_align((float)UI_VW - MENU_MARGIN, MENU_HINT_Y, UI_FS_TINY, UI_ALIGN_RIGHT,
                         g_assets.pal_accent2, "%s", r->hint);
}

static void draw_title_logo(void)
{
    float cx = (float)UI_VW * 0.5f;
    float y = 30.0f;

    /* The title is drawn twice with a small offset in the secondary colour: a chromatic
     * split that reads as "printed on a plate" instead of "glowing neon". */
    float split = 2.0f + g_menu.logo_pulse * 1.0f;
    bp_ui_text_align(cx + split, y, UI_FS_HUGE, UI_ALIGN_CENTER, mc(92, 226, 196, 120), "BREACH");
    bp_ui_text_align(cx - split * 0.5f, y, UI_FS_HUGE, UI_ALIGN_CENTER, mc(255, 138, 32, 90), "BREACH");
    bp_ui_text_shadow(cx, y, UI_FS_HUGE, UI_ALIGN_CENTER, (Color){ 246, 248, 252, 255 },
                      (Color){ 0, 0, 0, 235 }, "BREACH");

    bp_ui_text_shadow(cx, y + 50.0f, UI_FS_LARGE, UI_ALIGN_CENTER, g_assets.pal_accent,
                      (Color){ 0, 0, 0, 225 }, "POINT");

    float tw = bp_ui_text_width(BP_GAME_TAGLINE, UI_FS_TINY);
    float ly = y + 82.0f;
    bp_ui_rect(cx - tw * 0.5f - 44.0f, ly + 4.0f, 38.0f, 1.0f, (Color){ 255, 138, 32, 120 });
    bp_ui_rect(cx + tw * 0.5f + 6.0f, ly + 4.0f, 38.0f, 1.0f, (Color){ 255, 138, 32, 120 });
    bp_ui_text_align(cx, ly, UI_FS_TINY, UI_ALIGN_CENTER, g_assets.pal_text_dim, "%s", BP_GAME_TAGLINE);
}

static void draw_heading(const char *title, const char *subtitle)
{
    bp_ui_text_shadow((float)UI_VW * 0.5f, 22.0f, UI_FS_LARGE, UI_ALIGN_CENTER,
                      g_assets.pal_accent, (Color){ 0, 0, 0, 230 }, "%s", title);
    if (subtitle && subtitle[0])
        bp_ui_text_align((float)UI_VW * 0.5f, 50.0f, UI_FS_TINY, UI_ALIGN_CENTER,
                   g_assets.pal_text_dim, "%s", subtitle);
    float hw = bp_ui_text_width(title, UI_FS_LARGE) * 0.5f + 26.0f;
    float cx = (float)UI_VW * 0.5f;
    bp_ui_rect(cx - hw, 44.0f, hw * 2.0f, 1.0f, (Color){ 255, 138, 32, 70 });
}

static void draw_results(void)
{
    float cx = (float)UI_VW * 0.5f;
    Color title = g_menu.result_victory ? g_assets.pal_accent2 : g_assets.pal_danger;

    bp_ui_text_shadow(cx, 20.0f, UI_FS_LARGE, UI_ALIGN_CENTER, title, (Color){ 0, 0, 0, 230 },
                      "%s", g_menu.result_title);

    float gw = 268.0f, gx = cx - gw * 0.5f, gy = 54.0f;

    /* The label always goes through an explicit "%s": passing a plain string as a format
     * argument would trip -Wformat-security the moment a label contained a percent sign. */
    #define STAT(label, fmt, col, ...) do { \
        bp_ui_text_align(gx, gy, UI_FS_SMALL, UI_ALIGN_LEFT, g_assets.pal_text_dim, "%s", label); \
        bp_ui_text_align(gx + gw, gy, UI_FS_SMALL, UI_ALIGN_RIGHT, col, fmt, __VA_ARGS__); \
        gy += 14.0f; } while (0)

    STAT("SCORE",    "%d",        g_assets.pal_ammo, g_menu.result_score);
    STAT("KILLS",    "%d",        g_assets.pal_text, g_menu.result_kills);
    STAT("ACCURACY", "%d%%",      g_assets.pal_text, g_menu.result_accuracy);
    STAT("TIME",     "%02d:%02d", g_assets.pal_text,
         (int)(g_menu.result_time / 60.0f), ((int)g_menu.result_time) % 60);
    STAT("WAVE",     "%d / %d",   g_assets.pal_text, g_menu.result_wave,
         BP_MAX(1, g_waves.wave_total));
    #undef STAT

    if (g_menu.result_line_count > 0)
    {
        gy += 2.0f;
        bp_ui_rect(gx, gy, gw, 1.0f, (Color){ 255, 138, 32, 70 });
        gy += 7.0f;
        for (int i = 0; i < g_menu.result_line_count; i++)
        {
            bp_ui_text_align(cx, gy, UI_FS_TINY, UI_ALIGN_CENTER, g_assets.pal_text_dim,
                       "%s", g_menu.result_lines[i]);
            gy += 11.0f;
        }
    }

    /* personal-best marker: the one piece of feedback that makes a failed run worth doing */
    const bp_level_record *rec = bp_save_record(g_save.settings.last_level, g_save.settings.difficulty);
    if (g_menu.result_score >= rec->best_score && g_menu.result_score > 0)
        bp_ui_text_align(cx, gy + 2.0f, UI_FS_TINY, UI_ALIGN_CENTER, g_assets.pal_accent,
                   "* PERSONAL BEST *");
}

static void draw_credits(void)
{
    static const char *lines[] = {
        "BREACH POINT", "A high-velocity arena shooter", "",
        "DESIGN / CODE / ART / AUDIO", "Nullstate Interactive", "",
        "BUILT WITH", "C11 + raylib 5.5", "",
        "EVERY TEXTURE, MODEL, SOUND EFFECT",
        "AND NOTE OF MUSIC IN THIS BUILD IS",
        "GENERATED BY CODE AT STARTUP.",
        "THERE ARE NO ASSET FILES.", "",
        "THANK YOU FOR PLAYING."
    };
    float y = 20.0f;
    for (int i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); i++)
    {
        bool head = (i == 0);
        bool sub = (i == 4 || i == 7);
        float size = head ? UI_FS_LARGE : (sub ? UI_FS_SMALL : UI_FS_TINY);
        Color c = head ? g_assets.pal_accent : (sub ? g_assets.pal_accent2 : g_assets.pal_text_dim);
        if (lines[i][0])
            bp_ui_text_shadow((float)UI_VW * 0.5f, y, size, UI_ALIGN_CENTER, c,
                              (Color){ 0, 0, 0, 200 }, "%s", lines[i]);
        y += head ? 28.0f : (lines[i][0] ? 14.0f : 8.0f);
    }
}

void bp_menu_draw(void)
{
    if (g_menu.screen == MENU_SCREEN_NONE) return;

    bool overlay = (g_menu.screen == MENU_SCREEN_PAUSE || g_menu.screen == MENU_SCREEN_QUIT);
    bp_ui_dim(overlay ? 0.58f : 0.74f);

    /* Edge darkening so menu text never fights a bright level behind it. */
    bp_ui_vgradient(0.0f, 0.0f, (float)UI_VW, 96.0f, (Color){ 5, 6, 9, 195 }, (Color){ 5, 6, 9, 0 });
    bp_ui_vgradient(0.0f, (float)UI_VH - 56.0f, (float)UI_VW, 56.0f, (Color){ 5, 6, 9, 0 }, (Color){ 5, 6, 9, 205 });

    switch (g_menu.screen)
    {
        case MENU_SCREEN_TITLE:
            draw_title_logo();
            draw_rows();
            bp_ui_text_align(MENU_MARGIN, (float)UI_VH - 12.0f, UI_FS_TINY, UI_ALIGN_LEFT,
                             (Color){ 108, 114, 126, 225 }, "v%s   %s", BP_VERSION_STRING, BP_STUDIO);
            bp_ui_text_align((float)UI_VW - MENU_MARGIN, (float)UI_VH - 12.0f, UI_FS_TINY, UI_ALIGN_RIGHT,
                             (Color){ 108, 114, 126, 225 }, "%d%% CLEARED    BEST %d    KILLS %d",
                       bp_save_completion_percent(), g_save.best_score, g_save.total_kills);
            break;

        case MENU_SCREEN_LEVELS:
            draw_heading("SELECT AREA", TextFormat("DIFFICULTY: %s   (CHANGE IT IN OPTIONS)",
                         g_difficulties[BP_CLAMP(g_save.settings.difficulty, 0, g_difficulty_count - 1)].name));
            draw_rows();
            draw_hint();
            break;

        case MENU_SCREEN_OPTIONS:
            draw_heading("OPTIONS", "LEFT/RIGHT ADJUSTS -- APPLY WRITES TO DISK");
            draw_rows();
            draw_hint();
            break;

        case MENU_SCREEN_CONTROLS:
            draw_heading("CONTROLS", "SELECT A ROW AND PRESS A KEY");
            draw_rows();
            draw_hint();
            break;

        case MENU_SCREEN_CREDITS:
            draw_credits();
            draw_rows();
            break;

        case MENU_SCREEN_RESULTS:
            draw_results();
            draw_rows();
            draw_hint();
            break;

        case MENU_SCREEN_PAUSE:
            draw_heading("PAUSED", TextFormat("%s   WAVE %d/%d   SCORE %d",
                         g_world.loaded ? g_world.name : "",
                         BP_CLAMP(g_waves.wave_index, 1, 999), BP_CLAMP(g_waves.wave_total, 1, 999),
                         g_player.score));
            draw_rows();
            draw_hint();
            break;

        case MENU_SCREEN_QUIT:
            bp_ui_text_shadow((float)UI_VW * 0.5f, 124.0f, UI_FS_LARGE, UI_ALIGN_CENTER,
                              g_assets.pal_danger, (Color){ 0, 0, 0, 230 }, "QUIT BREACH POINT?");
            draw_rows();
            break;

        default: break;
    }
}
