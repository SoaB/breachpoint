/******************************************************************************************
 * bp_save.c -- 儲存系統（persistence）的實作。
 *
 * 檔案格式與耐用性（durability）契約見 bp_save.h，這裡只記錄實作上的取捨：
 *
 *   - 原子寫入（atomic write）用的是純 stdio（fopen/fclose）加上 remove()+rename()，而不是
 *     raylib 的 SaveFileText()。SaveFileText 是直接寫進目標檔案的，因此中途當機或磁碟寫滿
 *     都會留下被截斷的檔案，而下次開機的載入器會把它當成一份「合法（但空白）」的設定檔。
 *     rename() 在 POSIX 與 Emscripten 的 MEMFS 上都是原子的，所以目的地永遠是舊檔或新檔，
 *     絕不會是兩者的混合體。
 *
 *   - 序列化一次寫進單一的檔案靜態緩衝區，並且只做一次 fwrite。理由有二：存檔不能與其他
 *     存取同一檔案的動作交錯；而一次寫入就是一次系統呼叫（syscall），在 IDBFS 上這正是
 *     「快速存檔」與「明顯卡頓」的差別。
 *
 *   - 紀錄以 (level, difficulty) 為單位存放。把難度合併成單一列會讓「最佳時間」失去意義，
 *     因為 RECRUIT 難度的一局與 BREACH 難度的一局並不是同一個成就。
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
/* 路徑                                                                                      */
/* -------------------------------------------------------------------------------------- */
static void resolve_paths(void)
{
#ifdef PLATFORM_WEB
    /*
     * 在 Web 上我們寫進 Emscripten 的虛擬檔案系統（virtual FS），再把它同步到 IndexedDB。
     * 掛載點只在這裡建立一次，而且失敗是可容忍的：若 IDBFS 無法使用（私密模式、儲存空間被
     * 封鎖），存檔在本次連線期間照樣能用，只是重新整理頁面後不會保留 —— 這總比直接拒絕開機
     * 來得好。
     */
    EM_ASM({
        try {
            FS.mkdir('/bpdata');
        } catch (e) { /* 已存在，忽略即可 */ }
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
    /* dir 刻意比它所餵入的目標緩衝區還要短，好讓 gcc 能證明 "/settings.cfg" 這段後綴一定
     * 放得進去。把 256 位元組的 dir 塞進 256 位元組的 path 無法被證明是安全的，編譯器便會
     * （正確地）針對可能的截斷發出警告。 */
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
    /* 把虛擬檔案系統推送（sync）到 IndexedDB。少了這一步，瀏覽器會在分頁關閉時丟棄一切
     * —— 這正是 Web 存檔最常見的 bug。 */
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
/* 預設值                                                                                    */
/* -------------------------------------------------------------------------------------- */
void bp_save_defaults(void)
{
    bp_settings *s = &g_save.settings;
    memset(s, 0, sizeof(*s));

    s->quality = BP_QUALITY_HIGH;
    s->resolution_scale = 0.0f;      /* 0 = 交給品質預設值決定 */
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

    /* 綁定取自輸入模組自己的預設對照表，如此預設鍵位（keymap）就只有一份單一真相來源
     * （source of truth）。這裡把它快照進設定檔，改鍵才能在重啟之後依然存在，「恢復預設
     * 值」也才可能不必重開遊戲就完成。 */
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
/* 套用                                                                                      */
/* -------------------------------------------------------------------------------------- */
void bp_save_apply_settings(void)
{
    const bp_settings *s = &g_save.settings;

    bp_gfx_set_quality(s->quality);
    if (s->resolution_scale > 0.05f) g_gfx.resolution_scale = s->resolution_scale;
    g_gfx.crt_enabled = s->crt_filter;
    bp_gfx_resize();

    bp_camera_set_fov_base((float)BP_CLAMP(s->fov, 70, 120));

    /* raylib 只在視窗建立時讀取 FLAG_VSYNC_HINT，因此切換 vsync 必須重開才會生效。真正能
     * 即時改變的是 EndDrawing() 要不要在事件佇列上阻塞（block）—— 這個決定集中在
     * bp_apply_event_wait()，而它在開機尚未完成時一律拒絕阻塞（bp_save_init 本身就是一個
     * 開機階段，若在這裡阻塞，後面所有階段都不會被執行）。詳見 bp_engine::event_wait_allowed。 */
    g_engine.vsync = s->vsync;
    bp_apply_event_wait();

    bp_audio_set_volumes(s->vol_master, s->vol_sfx, s->vol_music);

    g_input.sensitivity = BP_CLAMP(s->sensitivity, 0.02f, 1.0f);
    g_input.invert_y = s->invert_y;

    /* 在預設值之上重新套用已儲存的鍵位。任何一個儲存代碼全為零的動作會保留預設綁定 ——
     * 這正是「由早於某個新動作的舊版本所寫下的存檔」仍能產生一套可玩操控方案的原因。 */
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
/* 載入                                                                                      */
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
        /* 刻意靜默忽略未知道具：由較新版本寫出的存檔，仍然必須能被舊版本載入，只是會少掉
         * 舊版本並不認識的那些欄位。 */
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

            /* 區段 id 的格式是 "<levelIndex>-<difficulty>"，例如 "2-3" */
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
            /* 檔案比本版本所能理解的還要新。磁碟上的檔案保持原樣、改用預設值執行：覆寫它
             * 等於摧毀較新版本讀得懂、而本版本讀不懂的資料。 */
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
/* 寫入                                                                                      */
/* -------------------------------------------------------------------------------------- */
/*
 * 先序列化進 s_buf，再寫入 "<path>.tmp"，最後以 rename 蓋掉 "<path>"。
 * 成功回傳 true；`len` 用來接收這次寫入的位元組數，供記錄（log）輸出。
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

    /* 先 remove() 再 rename()：在 POSIX 上，覆寫既有檔案的 rename() 是原子的；但在某些
     * Windows/Emscripten 組合下，只要目的地已存在它就會失敗。先刪除會製造出一段「兩份檔案
     * 都不存在」的空窗，這是可接受的，因為載入器把「檔案不存在」視為「改用預設值」，而不
     * 是致命錯誤。 */
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
            /* 跳過完全沒碰過的列：只嘗試過一關的玩家，其進度檔不該長達 64 個空白區塊。 */
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
/* 紀錄                                                                                      */
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

    /* 「最佳」只會變好。一個會往下跌的「最佳」不是紀錄，那只是「最近一次的值」，而玩家
     * 立刻就能分辨這兩者的差別。 */
    if (score > r->best_score) r->best_score = score;
    if (kills > r->best_kills) r->best_kills = kills;
    if (wave > r->best_wave) r->best_wave = wave;
    if (accuracy > r->best_accuracy) r->best_accuracy = BP_CLAMP(accuracy, 0.0f, 1.0f);
    if (perfectWaves > r->perfect_waves) r->perfect_waves = perfectWaves;

    /* 時間的「最佳」＝最快，而且只在完整通關時才算數：40 秒就陣亡並不算一項紀錄。 */
    if (completed && time > 0.0f && (r->best_time <= 0.0f || time < r->best_time)) r->best_time = time;

    g_save.total_runs++;
    g_save.total_kills += kills;
    if (died) g_save.total_deaths++;
    g_save.total_play_time += BP_MAX(0.0f, time);
    if (score > g_save.best_score) g_save.best_score = score;
    if (g_player.streak > g_save.highest_streak) g_save.highest_streak = g_player.streak;

    g_save.settings.last_level = level;
    g_save.dirty = true;

    /* 立刻寫入磁碟。一局的結果正是玩家最痛恨弄丟的東西，而且這次寫入只有幾 KB —— 在這裡
     * 同步完成綽綽有餘。 */
    bp_save_write_progress();
    bp_save_write_settings();
    bp_save_sync_platform();

    BP_LOGI("save: run submitted -- %s %s, score %d, %d kills, %.1fs, acc %d%%",
            g_level_count > level ? g_levels[level].display_name : "?",
            completed ? "CLEARED" : "FAILED", score, kills, (double)time, (int)(accuracy * 100.0f));
}

/*
 * 第 0 關永遠開放。第 N 關在第 N-1 關以「任意」難度通過時解鎖 —— 否則在 VETERAN 上掙扎
 * 的玩家，會被擋在他們本可於 RECRUIT 享受的內容之外；對一款沒有多人連線的遊戲來說，這
 * 是錯誤的誘因設計。
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
/* 初始化                                                                                    */
/* -------------------------------------------------------------------------------------- */
void bp_save_boot(void)
{
    memset(&g_save, 0, sizeof(g_save));
    resolve_paths();
    bp_save_result r = bp_save_load();

    /* 第一次執行會立刻寫出預設設定檔：玩家手上因此有一份可自行編輯的檔案，而唯讀檔案系統
     * 的問題也會在開機時就被發現，而不是等到玩家第一次改選項、發現設定悄悄沒存下來才暴露。 */
    if (r == SAVE_MISSING) bp_save_write_settings();

    /* 注意：設定並不是在這裡套用的。bp_save_apply_settings() 會動到繪圖、音訊裝置與相機，
     * 而這些東西在各自的開機階段跑完之前根本還不存在。 */
}

void bp_save_init(void)
{
    /* 由開機流程呼叫，此時 gfx／audio／input／camera 都已就緒。 */
    bp_save_apply_settings();
    BP_LOGI("save: settings applied (quality %s, fov %d, sens %.2f, difficulty %s)",
            bp_gfx_quality_name(g_gfx.quality), g_save.settings.fov,
            (double)g_save.settings.sensitivity,
            g_difficulties[BP_CLAMP(g_save.settings.difficulty, 0, g_difficulty_count - 1)].name);
}
