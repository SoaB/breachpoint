/******************************************************************************************
 * bp_save.c -- persistence implementation.
 *
 * See bp_save.h for the format and durability contract. Implementation notes:
 *
 *   - Atomic write is done with plain stdio (fopen/fclose) plus remove()+rename(), not
 *     raylib's SaveFileText(). SaveFileText writes straight to the destination, so a crash
 *     or a full disk mid-write leaves a truncated file that the next boot would happily
 *     parse as a legitimate (but empty) profile. rename() is atomic on POSIX and on
 *     Emscripten's MEMFS, so the destination is either the old file or the new one, never
 *     a mixture.
 *
 *   - The serialiser writes into one file-static buffer and a single fwrite. Two reasons:
 *     a save must not be interleaved with anything else touching the file, and one write
 *     means one syscall, which on IDBFS is the difference between a fast save and a stall.
 *
 *   - Records are stored per (level, difficulty). Merging difficulties into one row would
 *     make "best time" meaningless, because a RECRUIT run and a BREACH run are not the same
 *     achievement.
 ******************************************************************************************/
#include "bp_save.h"

#include "../engine/bp_config.h"
#include "../engine/bp_camera.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_gfx.h"
#include "../engine/bp_input.h"
#include "bp_hud.h"
#include "bp_player.h"
#include "bp_waves.h"
#include "bp_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PLATFORM_WEB
#include <emscripten.h>
#endif

bp_save g_save = { 0 };

#define SAVE_BUF_SIZE   (48 * 1024)
static char s_buf[SAVE_BUF_SIZE];

/* -------------------------------------------------------------------------------------- */
/* paths                                                                                   */
/* -------------------------------------------------------------------------------------- */
static void resolve_paths(void)
{
#ifdef PLATFORM_WEB
    /*
     * On the web we write into Emscripten's virtual FS and then sync it to IndexedDB.
     * The mount is created once, here, and failures are tolerated: if IDBFS is unavailable
     * (private mode, storage blocked) saves still work for the session and simply do not
     * survive a reload, which is strictly better than refusing to boot.
     */
    EM_ASM({
        try {
            FS.mkdir('/bpdata');
        } catch (e) { /* already exists */ }
        try {
            FS.mount(IDBFS, {}, '/bpdata');
            FS.syncfs(true, function (err) {
                if (err) console.warn('[bp_save] IDBFS initial sync failed:', err);
            });
        } catch (e) {
            console.warn('[bp_save] IDBFS unavailable, saves will not persist:', e);
        }
    });
    snprintf(g_save.path_settings, sizeof(g_save.path_settings), "/bpdata/settings.cfg");
    snprintf(g_save.path_progress, sizeof(g_save.path_progress), "/bpdata/progress.cfg");
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = GetApplicationDirectory();

    const char *xdg = getenv("XDG_DATA_HOME");
    /* dir is deliberately shorter than the destination buffers it feeds, so gcc can prove
     * the "/settings.cfg" suffix always fits. A 256-byte dir into a 256-byte path cannot be
     * proven safe and would warn (correctly) about truncation. */
    char dir[200];
    if (xdg && xdg[0]) snprintf(dir, sizeof(dir), "%s/BreachPoint", xdg);
    else               snprintf(dir, sizeof(dir), "%s/.local/share/BreachPoint", home);

    MakeDirectory(dir);
    snprintf(g_save.path_settings, sizeof(g_save.path_settings), "%s/settings.cfg", dir);
    snprintf(g_save.path_progress, sizeof(g_save.path_progress), "%s/progress.cfg", dir);
    snprintf(g_engine.data_dir, sizeof(g_engine.data_dir), "%s", dir);
    BP_LOGI("save: data directory '%s'", dir);
#endif
}

void bp_save_sync_platform(void)
{
#ifdef PLATFORM_WEB
    /* Push the virtual FS out to IndexedDB. Without this the browser discards everything
     * when the tab closes -- the single most common web-save bug. */
    EM_ASM({
        FS.syncfs(false, function (err) {
            if (err) console.warn('[bp_save] syncfs failed:', err);
        });
    });
#endif
}

const char *bp_save_result_name(bp_save_result r)
{
    switch (r)
    {
        case SAVE_OK:          return "OK";
        case SAVE_MISSING:     return "MISSING";
        case SAVE_CORRUPT:     return "CORRUPT";
        case SAVE_OLD_VERSION: return "OLD_VERSION";
        case SAVE_NEW_VERSION: return "NEW_VERSION";
        case SAVE_IO_ERROR:    return "IO_ERROR";
        default:               return "?";
    }
}

/* -------------------------------------------------------------------------------------- */
/* defaults                                                                                */
/* -------------------------------------------------------------------------------------- */
void bp_save_defaults(void)
{
    bp_settings *s = &g_save.settings;
    memset(s, 0, sizeof(*s));

    s->quality = BP_QUALITY_HIGH;
    s->resolution_scale = 0.0f;      /* 0 = follow the quality preset */
    s->fov = 95;
    s->vsync = true;
    s->crt_filter = false;
    s->fullscreen = false;
    s->window_w = 1280;
    s->window_h = 720;

    s->vol_master = 0.85f;
    s->vol_sfx = 1.00f;
    s->vol_music = 0.65f;

    s->sensitivity = 0.13f;
    s->invert_y = false;

    s->hud_visible = true;
    s->hud_minimal = false;
    s->show_fps = false;

    s->difficulty = 1;               /* VETERAN */
    s->last_level = 0;

    snprintf(s->profile_name, sizeof(s->profile_name), "PLAYER");

    /* Bindings come from the input module's own default table, so there is exactly one
     * source of truth for the default keymap. We snapshot it into the profile so that a
     * rebind persists and a "reset to defaults" is possible without restarting. */
    bp_input_reset_binds();
    for (int a = 0; a < ACT_COUNT; a++)
    {
        for (int k = 0; k < BP_MAX_BINDS_PER_ACTION; k++) s->binds[a][k] = g_input.act[a].keys[k];
        for (int k = 0; k < 2; k++) s->pad_binds[a][k] = g_input.act[a].padButtons[k];
    }

    g_save.version = SAVE_VERSION;
    g_save.dirty = false;
}

/* -------------------------------------------------------------------------------------- */
/* apply                                                                                   */
/* -------------------------------------------------------------------------------------- */
void bp_save_apply_settings(void)
{
    const bp_settings *s = &g_save.settings;

    bp_gfx_set_quality(s->quality);
    if (s->resolution_scale > 0.05f) g_gfx.resolution_scale = s->resolution_scale;
    g_gfx.crt_enabled = s->crt_filter;
    bp_gfx_resize();

    bp_camera_set_fov_base((float)BP_CLAMP(s->fov, 70, 120));

    /* raylib reads FLAG_VSYNC_HINT only at window creation, so changing vsync needs a
     * restart. What we can change live is whether EndDrawing() blocks on the event queue --
     * that decision is centralised in bp_apply_event_wait(), which refuses while boot is
     * still running (bp_save_init is itself a boot stage, so blocking here would stop every
     * later stage from running). See bp_engine::event_wait_allowed. */
    g_engine.vsync = s->vsync;
    bp_apply_event_wait();

    bp_audio_set_volumes(s->vol_master, s->vol_sfx, s->vol_music);

    g_input.sensitivity = BP_CLAMP(s->sensitivity, 0.02f, 1.0f);
    g_input.invert_y = s->invert_y;

    /* Re-apply the stored keymap on top of the defaults. Any action whose stored codes are
     * all zero keeps its default binding, which is how a save from an older version that
     * predates a new action still produces a playable control scheme. */
    bp_input_reset_binds();
    for (int a = 0; a < ACT_COUNT; a++)
    {
        bool any = false;
        for (int k = 0; k < BP_MAX_BINDS_PER_ACTION; k++) if (s->binds[a][k] != 0) any = true;
        if (!any) continue;
        for (int k = 0; k < BP_MAX_BINDS_PER_ACTION; k++)
            if (s->binds[a][k] != 0) bp_input_rebind((bp_action)a, k, s->binds[a][k]);
        for (int k = 0; k < 2; k++) g_input.act[a].padButtons[k] = s->pad_binds[a][k];
    }

    g_hud.visible = s->hud_visible;
    g_hud.minimal = s->hud_minimal;
    g_hud.show_fps = s->show_fps;
    g_engine.vsync = s->vsync;

    g_waves.difficulty = BP_CLAMP(s->difficulty, 0, g_difficulty_count - 1);
    g_waves.diff_health = g_difficulties[g_waves.difficulty].enemy_health;
    g_waves.diff_damage = g_difficulties[g_waves.difficulty].enemy_damage;
    g_waves.diff_score = g_difficulties[g_waves.difficulty].score;
    g_waves.diff_resupply = g_difficulties[g_waves.difficulty].resupply;
}

/* -------------------------------------------------------------------------------------- */
/* loading                                                                                 */
/* -------------------------------------------------------------------------------------- */
static void load_settings_file(bp_config *cfg)
{
    bp_settings *s = &g_save.settings;

    if (!bp_config_find_section(cfg, "settings", "profile")) return;
    while (bp_config_next_key(cfg))
    {
        const char *k = cfg->key;
        if      (!strcmp(k, "version"))           g_save.version = bp_cfg_int(cfg, SAVE_VERSION);
        else if (!strcmp(k, "profile_name"))      snprintf(s->profile_name, sizeof(s->profile_name), "%s", bp_cfg_str(cfg, s->profile_name));
        else if (!strcmp(k, "quality"))           s->quality = (bp_quality)BP_CLAMP(bp_cfg_int(cfg, (int)s->quality), 0, BP_QUALITY_COUNT - 1);
        else if (!strcmp(k, "resolution_scale"))  s->resolution_scale = bp_cfg_float(cfg, s->resolution_scale);
        else if (!strcmp(k, "fov"))               s->fov = BP_CLAMP(bp_cfg_int(cfg, s->fov), 70, 120);
        else if (!strcmp(k, "vsync"))             s->vsync = bp_cfg_bool(cfg, s->vsync);
        else if (!strcmp(k, "crt_filter"))        s->crt_filter = bp_cfg_bool(cfg, s->crt_filter);
        else if (!strcmp(k, "fullscreen"))        s->fullscreen = bp_cfg_bool(cfg, s->fullscreen);
        else if (!strcmp(k, "window_w"))          s->window_w = BP_CLAMP(bp_cfg_int(cfg, s->window_w), 640, 7680);
        else if (!strcmp(k, "window_h"))          s->window_h = BP_CLAMP(bp_cfg_int(cfg, s->window_h), 360, 4320);
        else if (!strcmp(k, "vol_master"))        s->vol_master = BP_CLAMP(bp_cfg_float(cfg, s->vol_master), 0.0f, 1.0f);
        else if (!strcmp(k, "vol_sfx"))           s->vol_sfx = BP_CLAMP(bp_cfg_float(cfg, s->vol_sfx), 0.0f, 1.0f);
        else if (!strcmp(k, "vol_music"))         s->vol_music = BP_CLAMP(bp_cfg_float(cfg, s->vol_music), 0.0f, 1.0f);
        else if (!strcmp(k, "sensitivity"))       s->sensitivity = BP_CLAMP(bp_cfg_float(cfg, s->sensitivity), 0.02f, 1.0f);
        else if (!strcmp(k, "invert_y"))          s->invert_y = bp_cfg_bool(cfg, s->invert_y);
        else if (!strcmp(k, "hud_visible"))       s->hud_visible = bp_cfg_bool(cfg, s->hud_visible);
        else if (!strcmp(k, "hud_minimal"))       s->hud_minimal = bp_cfg_bool(cfg, s->hud_minimal);
        else if (!strcmp(k, "show_fps"))          s->show_fps = bp_cfg_bool(cfg, s->show_fps);
        else if (!strcmp(k, "difficulty"))        s->difficulty = BP_CLAMP(bp_cfg_int(cfg, s->difficulty), 0, 3);
        else if (!strcmp(k, "last_level"))        s->last_level = BP_CLAMP(bp_cfg_int(cfg, s->last_level), 0, SAVE_MAX_LEVELS - 1);
        else if (!strncmp(k, "bind_", 5))
        {
            /* bind_<action>_<slot> = keycode */
            int action = atoi(k + 5);
            const char *us = strrchr(k, '_');
            int slot = us ? atoi(us + 1) : 0;
            if (action >= 0 && action < ACT_COUNT && slot >= 0 && slot < BP_MAX_BINDS_PER_ACTION)
                s->binds[action][slot] = bp_cfg_int(cfg, s->binds[action][slot]);
        }
        else if (!strncmp(k, "pad_", 4))
        {
            int action = atoi(k + 4);
            const char *us = strrchr(k, '_');
            int slot = us ? atoi(us + 1) : 0;
            if (action >= 0 && action < ACT_COUNT && slot >= 0 && slot < 2)
                s->pad_binds[action][slot] = bp_cfg_int(cfg, s->pad_binds[action][slot]);
        }
        /* Unknown keys are ignored silently on purpose: a save written by a newer build
         * must still load in an older one, minus the fields the older build lacks. */
    }
}

static void load_progress_file(bp_config *cfg)
{
    while (bp_config_next_section(cfg))
    {
        if (!strcmp(bp_config_section_type(cfg), "totals"))
        {
            while (bp_config_next_key(cfg))
            {
                const char *k = cfg->key;
                if      (!strcmp(k, "version"))         g_save.version = bp_cfg_int(cfg, SAVE_VERSION);
                else if (!strcmp(k, "total_runs"))      g_save.total_runs = bp_cfg_int(cfg, 0);
                else if (!strcmp(k, "total_kills"))     g_save.total_kills = bp_cfg_int(cfg, 0);
                else if (!strcmp(k, "total_deaths"))    g_save.total_deaths = bp_cfg_int(cfg, 0);
                else if (!strcmp(k, "total_play_time")) g_save.total_play_time = bp_cfg_float(cfg, 0.0f);
                else if (!strcmp(k, "highest_streak"))  g_save.highest_streak = bp_cfg_int(cfg, 0);
                else if (!strcmp(k, "best_score"))      g_save.best_score = bp_cfg_int(cfg, 0);
            }
            continue;
        }

        if (strcmp(bp_config_section_type(cfg), "level")) continue;

        /* section id is "<levelIndex>-<difficulty>", e.g. "2-3" */
        int lvl = atoi(bp_config_section_id(cfg));
        const char *dash = strchr(bp_config_section_id(cfg), '-');
        int diff = dash ? atoi(dash + 1) : 1;
        if (lvl < 0 || lvl >= SAVE_MAX_LEVELS || diff < 0 || diff > 3) continue;

        bp_level_record *r = &g_save.levels[lvl][diff];
        while (bp_config_next_key(cfg))
        {
            const char *k = cfg->key;
            if      (!strcmp(k, "completed"))      r->completed = bp_cfg_bool(cfg, false);
            else if (!strcmp(k, "secret_found"))   r->secret_found = bp_cfg_bool(cfg, false);
            else if (!strcmp(k, "best_score"))     r->best_score = bp_cfg_int(cfg, 0);
            else if (!strcmp(k, "best_kills"))     r->best_kills = bp_cfg_int(cfg, 0);
            else if (!strcmp(k, "best_time"))      r->best_time = bp_cfg_float(cfg, 0.0f);
            else if (!strcmp(k, "best_accuracy"))  r->best_accuracy = BP_CLAMP(bp_cfg_float(cfg, 0.0f), 0.0f, 1.0f);
            else if (!strcmp(k, "best_wave"))      r->best_wave = bp_cfg_int(cfg, 0);
            else if (!strcmp(k, "attempts"))       r->attempts = bp_cfg_int(cfg, 0);
            else if (!strcmp(k, "perfect_waves"))  r->perfect_waves = bp_cfg_int(cfg, 0);
            else if (!strcmp(k, "total_deaths"))   r->total_deaths = bp_cfg_int(cfg, 0);
        }
    }
}

bp_save_result bp_save_load(void)
{
    bp_save_defaults();

    bp_config cfg;
    bp_save_result result = SAVE_OK;

    if (bp_config_load(&cfg, g_save.path_settings))
    {
        load_settings_file(&cfg);
        if (g_save.version > SAVE_VERSION)
        {
            /* Newer file than this build understands. Keep the file on disk untouched and
             * run on defaults: overwriting it would destroy data a newer build can read. */
            BP_LOGW("save: settings.cfg is version %d, this build understands %d -- using defaults",
                    g_save.version, SAVE_VERSION);
            result = SAVE_NEW_VERSION;
            g_save.version = SAVE_VERSION;
            bp_save_defaults();
        }
        else if (g_save.version < SAVE_VERSION)
        {
            BP_LOGI("save: migrating settings from version %d to %d", g_save.version, SAVE_VERSION);
            result = SAVE_OLD_VERSION;
            g_save.version = SAVE_VERSION;
        }
    }
    else result = (result == SAVE_OK) ? SAVE_MISSING : result;

    g_save.version = SAVE_VERSION;

    if (bp_config_load(&cfg, g_save.path_progress))
    {
        load_progress_file(&cfg);
    }
    else if (result == SAVE_OK) result = SAVE_MISSING;

    g_save.last_result = result;
    g_save.loaded = true;
    g_save.dirty = false;
    BP_LOGI("save: loaded (%s) -- %d runs, %d kills, best score %d",
            bp_save_result_name(result), g_save.total_runs, g_save.total_kills, g_save.best_score);
    return result;
}

/* -------------------------------------------------------------------------------------- */
/* writing                                                                                 */
/* -------------------------------------------------------------------------------------- */
/*
 * Serialise into s_buf, then write "<path>.tmp" and rename over "<path>".
 * Returns true on success. `len` receives the byte count for logging.
 */
static bool atomic_write(const char *path, const char *data, int len)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f)
    {
        BP_LOGE("save: cannot open '%s' for writing", tmp);
        return false;
    }
    size_t wrote = fwrite(data, 1, (size_t)len, f);
    int closed = fclose(f);
    if ((int)wrote != len || closed != 0)
    {
        BP_LOGE("save: short write to '%s' (%d of %d bytes)", tmp, (int)wrote, len);
        remove(tmp);
        return false;
    }

    /* remove() first: rename() over an existing file is atomic on POSIX, but on some
     * Windows/Emscripten combinations it fails if the destination exists. Removing first
     * costs a window where neither file exists, which is acceptable because the loader
     * treats "missing" as "use defaults" rather than as a fatal error. */
    remove(path);
    if (rename(tmp, path) != 0)
    {
        BP_LOGE("save: rename '%s' -> '%s' failed", tmp, path);
        return false;
    }
    return true;
}

bp_save_result bp_save_write_settings(void)
{
    const bp_settings *s = &g_save.settings;
    int n = 0;

    n += snprintf(s_buf + n, SAVE_BUF_SIZE - (size_t)n,
        "# BREACH POINT -- player settings (version %d)\n"
        "# Hand-editable. Unknown keys are ignored, missing keys keep their default.\n"
        "# Delete this file to reset everything to factory defaults.\n\n"
        "settings profile\n", SAVE_VERSION);

    #define KV_INT(key, val)    n += snprintf(s_buf + n, SAVE_BUF_SIZE - (size_t)n, "    %-18s = %d\n", key, (int)(val))
    #define KV_FLOAT(key, val)  n += snprintf(s_buf + n, SAVE_BUF_SIZE - (size_t)n, "    %-18s = %.4f\n", key, (double)(val))
    #define KV_BOOL(key, val)   n += snprintf(s_buf + n, SAVE_BUF_SIZE - (size_t)n, "    %-18s = %s\n", key, (val) ? "true" : "false")
    #define KV_STR(key, val)    n += snprintf(s_buf + n, SAVE_BUF_SIZE - (size_t)n, "    %-18s = %s\n", key, (val))

    KV_INT("version", SAVE_VERSION);
    KV_STR("profile_name", s->profile_name);

    KV_INT("quality", (int)s->quality);
    KV_FLOAT("resolution_scale", s->resolution_scale);
    KV_INT("fov", s->fov);
    KV_BOOL("vsync", s->vsync);
    KV_BOOL("crt_filter", s->crt_filter);
    KV_BOOL("fullscreen", s->fullscreen);
    KV_INT("window_w", s->window_w);
    KV_INT("window_h", s->window_h);

    KV_FLOAT("vol_master", s->vol_master);
    KV_FLOAT("vol_sfx", s->vol_sfx);
    KV_FLOAT("vol_music", s->vol_music);

    KV_FLOAT("sensitivity", s->sensitivity);
    KV_BOOL("invert_y", s->invert_y);

    KV_BOOL("hud_visible", s->hud_visible);
    KV_BOOL("hud_minimal", s->hud_minimal);
    KV_BOOL("show_fps", s->show_fps);

    KV_INT("difficulty", s->difficulty);
    KV_INT("last_level", s->last_level);

    n += snprintf(s_buf + n, SAVE_BUF_SIZE - (size_t)n, "\n    # key bindings: bind_<action>_<slot> = raylib key code\n");
    for (int a = 0; a < ACT_COUNT; a++)
    {
        for (int k = 0; k < BP_MAX_BINDS_PER_ACTION; k++)
        {
            char key[32];
            snprintf(key, sizeof(key), "bind_%d_%d", a, k);
            KV_INT(key, s->binds[a][k]);
        }
        for (int k = 0; k < 2; k++)
        {
            char key[32];
            snprintf(key, sizeof(key), "pad_%d_%d", a, k);
            KV_INT(key, s->pad_binds[a][k]);
        }
    }
    #undef KV_INT
    #undef KV_FLOAT
    #undef KV_BOOL
    #undef KV_STR

    n += snprintf(s_buf + n, SAVE_BUF_SIZE - (size_t)n, "end\n");
    if (n >= SAVE_BUF_SIZE) { BP_LOGE("save: settings buffer overflow"); return SAVE_IO_ERROR; }

    bool ok = atomic_write(g_save.path_settings, s_buf, n);
    g_save.dirty = false;
    g_save.last_result = ok ? SAVE_OK : SAVE_IO_ERROR;
    if (ok) BP_LOGD("save: wrote settings (%d bytes)", n);
    return g_save.last_result;
}

bp_save_result bp_save_write_progress(void)
{
    int n = 0;
    n += snprintf(s_buf + n, SAVE_BUF_SIZE - (size_t)n,
        "# BREACH POINT -- run records (version %d)\n"
        "# One block per level/difficulty pair. best_time 0 means never finished.\n\n"
        "totals all\n"
        "    version          = %d\n"
        "    total_runs       = %d\n"
        "    total_kills      = %d\n"
        "    total_deaths     = %d\n"
        "    total_play_time  = %.2f\n"
        "    highest_streak   = %d\n"
        "    best_score       = %d\n"
        "end\n\n",
        SAVE_VERSION, SAVE_VERSION, g_save.total_runs, g_save.total_kills, g_save.total_deaths,
        (double)g_save.total_play_time, g_save.highest_streak, g_save.best_score);

    for (int lvl = 0; lvl < SAVE_MAX_LEVELS && lvl < BP_MAX(g_level_count, 1); lvl++)
    {
        for (int d = 0; d < 4; d++)
        {
            const bp_level_record *r = &g_save.levels[lvl][d];
            /* Skip untouched rows: a progress file for a player who tried one level should
             * not be 64 empty blocks long. */
            if (r->attempts <= 0 && !r->completed) continue;

            n += snprintf(s_buf + n, SAVE_BUF_SIZE - (size_t)n,
                "level %d-%d\n"
                "    completed        = %s\n"
                "    secret_found     = %s\n"
                "    best_score       = %d\n"
                "    best_kills       = %d\n"
                "    best_time        = %.2f\n"
                "    best_accuracy    = %.4f\n"
                "    best_wave        = %d\n"
                "    attempts         = %d\n"
                "    perfect_waves    = %d\n"
                "    total_deaths     = %d\n"
                "end\n\n",
                lvl, d, r->completed ? "true" : "false", r->secret_found ? "true" : "false",
                r->best_score, r->best_kills, (double)r->best_time, (double)r->best_accuracy,
                r->best_wave, r->attempts, r->perfect_waves, r->total_deaths);

            if (n >= SAVE_BUF_SIZE - 512) { BP_LOGW("save: progress truncated at level %d", lvl); break; }
        }
    }

    bool ok = atomic_write(g_save.path_progress, s_buf, n);
    g_save.last_result = ok ? SAVE_OK : SAVE_IO_ERROR;
    if (ok) BP_LOGD("save: wrote progress (%d bytes)", n);
    return g_save.last_result;
}

/* -------------------------------------------------------------------------------------- */
/* records                                                                                 */
/* -------------------------------------------------------------------------------------- */
bp_level_record *bp_save_record(int level, int difficulty)
{
    level = BP_CLAMP(level, 0, SAVE_MAX_LEVELS - 1);
    difficulty = BP_CLAMP(difficulty, 0, 3);
    return &g_save.levels[level][difficulty];
}

void bp_save_submit_run(int level, int difficulty, bool completed, int score,
                        int kills, float time, float accuracy, int wave,
                        int perfectWaves, bool secret, bool died)
{
    bp_level_record *r = bp_save_record(level, difficulty);

    r->attempts++;
    if (died) r->total_deaths++;
    if (completed) r->completed = true;
    if (secret) r->secret_found = true;

    /* Bests only improve. A "best" that can go down is not a record, it is a last value, and
     * players read the difference immediately. */
    if (score > r->best_score) r->best_score = score;
    if (kills > r->best_kills) r->best_kills = kills;
    if (wave > r->best_wave) r->best_wave = wave;
    if (accuracy > r->best_accuracy) r->best_accuracy = BP_CLAMP(accuracy, 0.0f, 1.0f);
    if (perfectWaves > r->perfect_waves) r->perfect_waves = perfectWaves;

    /* Time is "best" = fastest, and only counts for a completed run: a 40-second death is
     * not a record. */
    if (completed && time > 0.0f && (r->best_time <= 0.0f || time < r->best_time)) r->best_time = time;

    g_save.total_runs++;
    g_save.total_kills += kills;
    if (died) g_save.total_deaths++;
    g_save.total_play_time += BP_MAX(0.0f, time);
    if (score > g_save.best_score) g_save.best_score = score;
    if (g_player.streak > g_save.highest_streak) g_save.highest_streak = g_player.streak;

    g_save.settings.last_level = level;
    g_save.dirty = true;

    /* Persist immediately. A run result is exactly the thing a player would be furious to
     * lose, and the write is a few kilobytes -- cheap enough to do synchronously here. */
    bp_save_write_progress();
    bp_save_write_settings();
    bp_save_sync_platform();

    BP_LOGI("save: run submitted -- %s %s, score %d, %d kills, %.1fs, acc %d%%",
            g_level_count > level ? g_levels[level].display_name : "?",
            completed ? "CLEARED" : "FAILED", score, kills, (double)time, (int)(accuracy * 100.0f));
}

/*
 * Level 0 is always open. Level N unlocks when level N-1 has been completed on *any*
 * difficulty -- otherwise a player who struggles on VETERAN is locked out of content they
 * would enjoy on RECRUIT, which is the wrong incentive for a game with no multiplayer.
 */
bool bp_save_level_unlocked(int level)
{
    if (level <= 0) return true;
    if (level >= SAVE_MAX_LEVELS) return false;
    for (int d = 0; d < 4; d++)
        if (g_save.levels[level - 1][d].completed) return true;
    return false;
}

int bp_save_completion_percent(void)
{
    int total = 0, done = 0;
    int levels = BP_MIN(SAVE_MAX_LEVELS, BP_MAX(g_level_count, 1));
    for (int l = 0; l < levels; l++)
        for (int d = 0; d < 4; d++) { total++; if (g_save.levels[l][d].completed) done++; }
    return total > 0 ? (done * 100) / total : 0;
}

/* -------------------------------------------------------------------------------------- */
/* init                                                                                    */
/* -------------------------------------------------------------------------------------- */
void bp_save_boot(void)
{
    memset(&g_save, 0, sizeof(g_save));
    resolve_paths();
    bp_save_result r = bp_save_load();

    /* A first run writes the default profile immediately, so the player has a file to edit
     * and so a read-only filesystem is detected at boot rather than at the first time they
     * change an option and it silently fails to stick. */
    if (r == SAVE_MISSING) bp_save_write_settings();

    /* NOTE: settings are NOT applied here. bp_save_apply_settings() touches the renderer,
     * the audio device and the camera, none of which exist before their boot stages run. */
}

void bp_save_init(void)
{
    /* Called from the boot sequence after gfx/audio/input/camera are up. */
    bp_save_apply_settings();
    BP_LOGI("save: settings applied (quality %s, fov %d, sens %.2f, difficulty %s)",
            bp_gfx_quality_name(g_gfx.quality), g_save.settings.fov,
            (double)g_save.settings.sensitivity,
            g_difficulties[BP_CLAMP(g_save.settings.difficulty, 0, g_difficulty_count - 1)].name);
}
