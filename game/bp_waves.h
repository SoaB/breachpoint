/******************************************************************************************
 * bp_waves.h -- the encounter director: what spawns, where, when, and how hard.
 *
 * WHY A DIRECTOR INSTEAD OF A SPAWN LIST
 *   A hand-authored spawn list produces a level that is exciting exactly once. The second
 *   playthrough the player has memorised it and the game is over. A director that composes
 *   waves from the level's spawn points, throttled by what is actually on screen and
 *   modulated by how the player is performing, produces a different fight every run out of
 *   the same geometry -- which is the whole economic argument for an arena shooter: one
 *   map, hundreds of hours.
 *
 * THE THREE KNOBS
 *   1. composition -- which archetypes, in what ratio, escalating with wave index.
 *   2. pressure    -- how many enemies may be alive at once, and how fast they trickle in.
 *                      This is capped by MAX_ONSCREEN so a bad composition can never turn
 *                      into a framerate problem: the director is also the perf budget.
 *   3. rubber band -- a hidden skill read (damage taken per second + accuracy) that nudges
 *                      pressure down for a struggling player and up for a dominant one.
 *                      Deliberately subtle: +-18% at most. Players must never feel the
 *                      game deciding to be easy, only that it is always "about right".
 *
 * PLACEMENT RULES (enforced, not suggested)
 *   - never inside the player's view cone if a farther point exists: spawning in front of
 *     the player's face reads as a cheat and removes the value of scanning the arena
 *   - never closer than MIN_SPAWN_DIST: no contact damage on arrival
 *   - prefer points the player has not used recently (round robin over the spawn set)
 *   - a spawn is rejected outright if the point is inside geometry after movers have
 *     shifted, so a closing blast door can never entomb an enemy
 ******************************************************************************************/
#ifndef BP_WAVES_H
#define BP_WAVES_H

#include "bp_common.h"

typedef enum {
    WAVE_IDLE = 0,        /* between levels / before the first wave                 */
    WAVE_INTRO,           /* banner up, no enemies yet, brief breather              */
    WAVE_ACTIVE,          /* spawning + fighting                                    */
    WAVE_CLEARING,        /* budget spent, waiting for the survivors to die         */
    WAVE_INTERMISSION,    /* all dead: resupply, score tally, next-wave countdown   */
    WAVE_COMPLETE         /* every wave in the level finished -> hand off to exit   */
} bp_wave_state;

typedef struct {
    /* level setup */
    int          level_index;
    int          wave_total;         /* from the level table                        */
    int          difficulty;         /* 0 relaxed .. 3 nightmare                    */

    /* current wave */
    int          wave_index;         /* 1-based while running                       */
    bp_wave_state state;
    float        state_time;         /* seconds in the current state                */
    float        level_time;         /* seconds since the level started             */

    int          budget;             /* enemies still to spawn this wave            */
    int          spawned_this_wave;
    int          killed_this_wave;
    int          killed_this_level;

    /* per-archetype remaining counts for the wave being composed */
    int          pending[4];

    /* Difficulty multipliers, resolved once at level start from g_difficulties[].
     * Enemies read these two directly rather than carrying their own copies: difficulty
     * is a property of the run, not of the archetype, and duplicating it per enemy is how
     * a mid-level difficulty change ends up half-applied. */
    float        diff_health;
    float        diff_damage;
    float        diff_score;
    float        diff_resupply;

    /* pressure */
    int          max_onscreen;       /* hard cap on simultaneously alive enemies    */
    float        spawn_interval;     /* seconds between trickle spawns              */
    float        spawn_timer;
    int          spawn_cursor;       /* round-robin index into the spawn point set  */
    float        skill;              /* -1 struggling .. +1 dominant                */
    float        aggression;         /* 0.7 .. 1.3, multiplies pressure             */

    /* intermission */
    float        intermission_time;
    float        intermission_left;
    bool         resupply_given;

    /* stats */
    int          waves_completed;
    int          spawns_rejected;    /* placement refused: inside geometry/too close */
    float        longest_wave;
    float        fastest_wave;
    int          perfect_waves;      /* cleared without taking damage               */
    bool         damage_taken_this_wave;

    bool         initialised;
} bp_wave_sys;

extern bp_wave_sys g_waves;

void  bp_waves_init(void);
void  bp_waves_start_level(int levelIndex);
void  bp_waves_update(float dt);

/* Called by bp_enemies the instant an enemy dies. Declared here so the dependency points
 * one way only (enemies -> waves); waves never needs to know an enemy struct exists. */
void  bp_waves_notify_kill(int enemyType);

bool  bp_waves_is_active(void);
bool  bp_waves_level_complete(void);
int   bp_waves_remaining(void);        /* budget + alive: what the HUD counts down   */
float bp_waves_progress01(void);
const char *bp_waves_state_name(void);

/* Difficulty presets, exposed so the menu can show what a choice actually does. */
typedef struct {
    const char *name;
    const char *blurb;
    float       enemy_health;     /* multiplier applied to archetype health         */
    float       enemy_damage;     /* multiplier applied to attack + projectile dmg  */
    float       pressure;         /* multiplier on max_onscreen + spawn rate        */
    float       score;            /* score multiplier                               */
    float       resupply;         /* intermission ammo/health fraction              */
} bp_difficulty_def;

extern const bp_difficulty_def g_difficulties[];
extern const int g_difficulty_count;

#endif /* BP_WAVES_H */
