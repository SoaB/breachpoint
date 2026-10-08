/******************************************************************************************
 * bp_save.h -- settings profile + run records + platform-aware persistence.
 *
 * FORMAT: the same line-oriented key/value syntax as the files in assets/cfg, parsed by
 * engine/bp_config.c. Three reasons this is the right call for a game this size:
 *   1. One parser, one grammar, one set of bugs. A binary format would need a second
 *      reader, a second fuzzer and a second migration path.
 *   2. Saves are human-readable and hand-editable, which is what players of this genre
 *      expect (and which makes support tickets answerable in seconds).
 *   3. It round-trips through the web build unchanged: localStorage stores strings.
 *
 * DURABILITY CONTRACT
 *   - Every write is atomic: serialise to a temp buffer, write "<name>.tmp", fsync-free
 *     replace via rename(). A crash mid-write can never leave a truncated save that the
 *     next boot then trusts. (Rename is atomic on both POSIX and Emscripten's MEMFS/IDBFS.)
 *   - A missing or corrupt file is not an error: bp_save_load() falls back to the
 *     compiled-in default profile and logs a warning. The game must always boot.
 *   - `version` is checked. A newer file than we understand is preserved untouched rather
 *     than overwritten, so a player who downgrades a build does not lose their records.
 *
 * PLATFORMS
 *   Desktop: <user data dir>/BreachPoint/settings.cfg and progress.cfg.
 *   Web:     the same two paths inside Emscripten's virtual FS, mounted to IndexedDB via
 *            IDBFS and synced with FS.syncfs(). Without the explicit sync the browser
 *            discards everything on reload, which is the single most common web-save bug.
 ******************************************************************************************/
#ifndef BP_SAVE_H
#define BP_SAVE_H

#include "bp_common.h"
#include "../engine/bp_gfx.h"
#include "../engine/bp_input.h"

#define SAVE_VERSION        3
#define SAVE_MAX_LEVELS    16
#define SAVE_NAME_LEN      32

typedef enum {
    SAVE_OK = 0,
    SAVE_MISSING,        /* no file yet: first run                          */
    SAVE_CORRUPT,        /* unparseable: defaults used, file left alone     */
    SAVE_OLD_VERSION,    /* older than SAVE_VERSION: migrated               */
    SAVE_NEW_VERSION,    /* newer than we understand: preserved, not loaded */
    SAVE_IO_ERROR
} bp_save_result;

/* -------------------------------------------------------------------------------------- */
/* settings profile -- everything the options menu can change                              */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    /* video */
    bp_quality quality;
    float      resolution_scale;   /* override; <=0 means "use the quality preset" */
    int        fov;                /* 70..120 degrees                              */
    bool       vsync;
    bool       crt_filter;
    bool       fullscreen;
    int        window_w, window_h;

    /* audio */
    float      vol_master;
    float      vol_sfx;
    float      vol_music;

    /* input */
    float      sensitivity;
    bool       invert_y;
    int        binds[ACT_COUNT][BP_MAX_BINDS_PER_ACTION];   /* key codes */
    int        pad_binds[ACT_COUNT][2];

    /* hud */
    bool       hud_visible;
    bool       hud_minimal;
    bool       show_fps;

    /* game */
    int        difficulty;
    int        last_level;

    char       profile_name[SAVE_NAME_LEN];
} bp_settings;

/* -------------------------------------------------------------------------------------- */
/* run records -- one row per level, per difficulty                                        */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    bool  completed;
    bool  secret_found;
    int   best_score;
    int   best_kills;
    float best_time;        /* 0 = never finished */
    float best_accuracy;    /* 0..1 */
    int   best_wave;
    int   attempts;
    int   perfect_waves;
    int   total_deaths;
} bp_level_record;

typedef struct {
    int             version;
    bp_settings     settings;
    bp_level_record levels[SAVE_MAX_LEVELS][4];   /* [level][difficulty] */

    /* global totals, for the stats screen */
    int             total_runs;
    int             total_kills;
    int             total_deaths;
    float           total_play_time;
    int             highest_streak;
    int             best_score;

    /* runtime, never persisted */
    char            path_settings[256];
    char            path_progress[256];
    bp_save_result  last_result;
    bool            dirty;
    bool            loaded;
} bp_save;

extern bp_save g_save;

/* lifecycle
 *
 * bp_save_boot() and bp_save_init() are deliberately two functions. Window creation needs the
 * profile (size, vsync, MSAA) but the renderer, audio device and input table do not exist yet,
 * so applying settings at that point would dereference uninitialised subsystems. Boot reads
 * the file and nothing else; init is called later, once those subsystems are up. */
void           bp_save_boot(void);               /* safe before InitWindow: paths + load    */
void           bp_save_init(void);               /* after gfx/audio/input exist: re-apply   */
bp_save_result bp_save_load(void);
bp_save_result bp_save_write_settings(void);
bp_save_result bp_save_write_progress(void);
void           bp_save_apply_settings(void);     /* push settings into the live subsystems */
void           bp_save_defaults(void);

/* progress */
bp_level_record *bp_save_record(int level, int difficulty);
void             bp_save_submit_run(int level, int difficulty, bool completed, int score,
                                    int kills, float time, float accuracy, int wave,
                                    int perfectWaves, bool secret, bool died);
bool             bp_save_level_unlocked(int level);
int              bp_save_completion_percent(void);

/* platform */
void           bp_save_sync_platform(void);      /* web: FS.syncfs; desktop: no-op */
const char    *bp_save_result_name(bp_save_result r);

#endif /* BP_SAVE_H */
