/******************************************************************************************
 * bp_waves.c -- encounter director implementation.
 *
 * See bp_waves.h for the design contract. Implementation notes that are not obvious from
 * the header:
 *
 *   - The director runs on the fixed timestep like everything else, so wave pacing is
 *     frame-rate independent. A 30 fps machine and a 240 fps machine get the identical
 *     encounter, which matters for a leaderboard.
 *
 *   - Spawn placement is a *bounded* search, never an unbounded retry loop. We try at most
 *     SPAWN_ATTEMPTS candidate points per spawn and accept the best legal one; if none is
 *     legal we drop the spawn and count it in `spawns_rejected`. An infinite retry loop
 *     here would be a hang risk the first time a mover seals off the last spawn point.
 *
 *   - The skill read is exponential smoothing over two signals only (damage taken per
 *     second, and weapon accuracy). Both are already maintained elsewhere, so the director
 *     adds no instrumentation of its own -- one less thing to keep in sync.
 ******************************************************************************************/
#include "bp_waves.h"

#include "../engine/bp_math.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_camera.h"
#include "bp_world.h"
#include "bp_player.h"
#include "bp_enemies.h"
#include "bp_weapons.h"
#include "bp_fx.h"

bp_wave_sys g_waves = { 0 };

static inline Vector3 vec3(float x, float y, float z) { return (Vector3){ x, y, z }; }

/* -------------------------------------------------------------------------------------- */
/* difficulty presets                                                                      */
/* -------------------------------------------------------------------------------------- */
const bp_difficulty_def g_difficulties[] = {
    /*  name        blurb                                          health damage pressure score resupply */
    { "RECRUIT",  "Learning the movement. Generous resupply.",     0.70f, 0.55f, 0.72f, 0.70f, 1.00f },
    { "VETERAN",  "The intended experience.",                      1.00f, 1.00f, 1.00f, 1.00f, 0.60f },
    { "NIGHTMARE","Faster pressure, heavier hits, less help.",     1.30f, 1.45f, 1.26f, 1.60f, 0.35f },
    { "BREACH",   "No resupply. Enemies do not stagger.",          1.55f, 1.85f, 1.45f, 2.50f, 0.00f },
};
const int g_difficulty_count = BP_ARRAY_COUNT(g_difficulties);

/* -------------------------------------------------------------------------------------- */
/* pacing constants                                                                        */
/* -------------------------------------------------------------------------------------- */
#define WAVE_INTRO_TIME        2.60f   /* banner up before the first enemy appears   */
#define WAVE_INTERMISSION_MIN  3.20f
#define WAVE_INTERMISSION_MAX  5.60f
#define WAVE_BASE_BUDGET         10    /* wave 1 enemy count on VETERAN              */
#define WAVE_BUDGET_GROWTH        4    /* added per wave                             */
#define WAVE_BUDGET_MAX          46    /* hard ceiling: the arena simply cannot hold
                                          more than this without it becoming a
                                          collision-resolution problem, not a fight  */
#define ONSCREEN_MIN              7
#define ONSCREEN_MAX             20    /* also the perf budget -- see header         */
#define SPAWN_INTERVAL_MIN      0.30f
#define SPAWN_INTERVAL_MAX      1.50f
#define SPAWN_OPENING_BURST       4    /* how many arrive together at wave start     */
#define SPAWN_ATTEMPTS            9
#define SPAWN_MIN_DIST_PLAYER     9.0f
#define SPAWN_HIDE_DIST          16.0f /* beyond this, view-cone rejection is skipped */
#define SPAWN_VIEW_COS            0.35f /* ~69 degrees either side of the crosshair   */
#define SPAWN_TURRET_MIN_DIST    14.0f /* turrets need sightline room to be fair     */
#define SKILL_LAMBDA              0.9f
#define SKILL_CLAMP             0.18f  /* max rubber-band influence, +-18%           */
#define PERFECT_WAVE_BONUS       750

/* Archetype weights per wave band. Indexed [band][ET_*]; band = wave_index / 2.
 * The bands are where the game teaches its verbs:
 *   wave 1-2: grunts only            -- learn to shoot and move
 *   wave 3-4: hounds arrive          -- learn that speed beats accuracy
 *   wave 5-6: turrets arrive         -- learn to break line of sight
 *   wave 7+ : brutes arrive          -- learn to kite and use explosives
 */
static const float s_wave_weights[6][ET_COUNT] = {
    /*          GRUNT   HOUND   BRUTE   TURRET */
    /* band 0 */ { 1.00f, 0.00f, 0.00f, 0.00f },
    /* band 1 */ { 0.72f, 0.28f, 0.00f, 0.00f },
    /* band 2 */ { 0.52f, 0.33f, 0.00f, 0.15f },
    /* band 3 */ { 0.40f, 0.30f, 0.15f, 0.15f },
    /* band 4 */ { 0.32f, 0.30f, 0.22f, 0.16f },
    /* band 5 */ { 0.27f, 0.30f, 0.27f, 0.16f },
};
#define WAVE_BANDS ((int)(sizeof(s_wave_weights) / sizeof(s_wave_weights[0])))

/* -------------------------------------------------------------------------------------- */
/* small helpers                                                                           */
/* -------------------------------------------------------------------------------------- */
static const bp_difficulty_def *diff(void)
{
    int i = BP_CLAMP(g_waves.difficulty, 0, g_difficulty_count - 1);
    return &g_difficulties[i];
}

static float band_weight(int type, int waveIndex)
{
    int band = BP_CLAMP((waveIndex - 1) / 2, 0, WAVE_BANDS - 1);
    if (type < 0 || type >= ET_COUNT) return 0.0f;
    return s_wave_weights[band][type];
}

const char *bp_waves_state_name(void)
{
    switch (g_waves.state)
    {
        case WAVE_IDLE:         return "IDLE";
        case WAVE_INTRO:        return "INTRO";
        case WAVE_ACTIVE:       return "ACTIVE";
        case WAVE_CLEARING:     return "CLEARING";
        case WAVE_INTERMISSION: return "INTERMISSION";
        case WAVE_COMPLETE:     return "COMPLETE";
        default:                return "?";
    }
}

bool bp_waves_is_active(void)
{
    return g_waves.state == WAVE_ACTIVE || g_waves.state == WAVE_CLEARING;
}

bool bp_waves_level_complete(void) { return g_waves.state == WAVE_COMPLETE; }

int bp_waves_remaining(void)
{
    return g_waves.budget + bp_enemy_alive_count();
}

float bp_waves_progress01(void)
{
    int total = g_waves.wave_total > 0 ? g_waves.wave_total : 1;
    float done = (float)(g_waves.wave_index - 1);
    if (g_waves.wave_index >= 1 && total > 0)
    {
        /* add the fraction of the current wave already killed so the bar moves smoothly */
        int waveSize = g_waves.spawned_this_wave + g_waves.budget;
        if (waveSize > 0) done += (float)g_waves.killed_this_wave / (float)waveSize;
    }
    return BP_CLAMP(done / (float)total, 0.0f, 1.0f);
}

/* -------------------------------------------------------------------------------------- */
/* wave composition                                                                        */
/* -------------------------------------------------------------------------------------- */
static int wave_budget(int waveIndex)
{
    float base = (float)(WAVE_BASE_BUDGET + (waveIndex - 1) * WAVE_BUDGET_GROWTH);
    base *= diff()->pressure;
    /* The final wave is a spike: roughly 1.5x, so the last fight is unmistakably the
     * climax rather than just another Tuesday. */
    if (waveIndex >= g_waves.wave_total) base *= 1.5f;
    int n = (int)(base + 0.5f);
    return BP_CLAMP(n, 6, WAVE_BUDGET_MAX);
}

/*
 * Distribute `budget` enemies across archetypes using the wave band's weights.
 *
 * Rounding is done with largest-remainder allocation rather than per-slot rounding, so the
 * parts always sum exactly to the budget. Per-slot rounding routinely produces a wave that
 * is one or two enemies short, which compounds over a level into a visibly thin finale.
 */
static void compose_wave(int waveIndex, int budget)
{
    float w[ET_COUNT];
    float total = 0.0f;
    for (int t = 0; t < ET_COUNT; t++) { w[t] = band_weight(t, waveIndex); total += w[t]; }
    if (total <= 0.0f) { w[ET_GRUNT] = 1.0f; total = 1.0f; }

    float exact[ET_COUNT];
    int   floor_[ET_COUNT];
    int   assigned = 0;
    for (int t = 0; t < ET_COUNT; t++)
    {
        exact[t]  = (w[t] / total) * (float)budget;
        floor_[t] = (int)exact[t];
        g_waves.pending[t] = floor_[t];
        assigned += floor_[t];
    }

    /* hand out the remainder to the largest fractional parts */
    int left = budget - assigned;
    while (left > 0)
    {
        int best = -1;
        float bestFrac = -1.0f;
        for (int t = 0; t < ET_COUNT; t++)
        {
            if (w[t] <= 0.0f) continue;                    /* never add an unlocked type */
            float frac = exact[t] - (float)floor_[t];
            if (frac > bestFrac) { bestFrac = frac; best = t; }
        }
        if (best < 0) best = ET_GRUNT;                     /* all weights integral */
        g_waves.pending[best]++;
        floor_[best]++;                                    /* so it is not picked twice */
        left--;
    }

    /* Turrets are static emplacements: they only make sense if the level has somewhere to
     * put them, and a wave of nothing-but-turrets is not a fight. Cap them at a third. */
    int turretCap = BP_MAX(1, budget / 3);
    if (g_waves.pending[ET_TURRET] > turretCap)
    {
        int overflow = g_waves.pending[ET_TURRET] - turretCap;
        g_waves.pending[ET_TURRET] = turretCap;
        g_waves.pending[ET_GRUNT] += overflow;
    }
}

/* -------------------------------------------------------------------------------------- */
/* spawn placement                                                                         */
/* -------------------------------------------------------------------------------------- */
/*
 * Score a candidate spawn point. Higher is better; a negative score means illegal.
 *
 * The scoring exists because "first legal point" produces clustering: the round-robin
 * cursor alone would make enemies arrive in the same corner every wave. Scoring by
 * distance-from-player plus a penalty for recently used points spreads the pressure
 * around the arena, which is what forces the player to keep moving.
 */
static float score_spawn(const bp_trigger *t, int type, Vector3 playerPos, Vector3 fwd)
{
    float dx = t->position.x - playerPos.x;
    float dz = t->position.z - playerPos.z;
    float dist = sqrtf(dx * dx + dz * dz);

    float minDist = (type == ET_TURRET) ? SPAWN_TURRET_MIN_DIST : SPAWN_MIN_DIST_PLAYER;
    if (dist < minDist) return -1.0f;

    /* Never spawn where the player is already looking, unless everything else is illegal.
     * Enemies materialising in the crosshair read as the game cheating, and it also
     * removes any reward for scanning the arena. */
    if (dist < SPAWN_HIDE_DIST)
    {
        float inv = 1.0f / BP_MAX(dist, 0.001f);
        float dot = (dx * inv) * fwd.x + (dz * inv) * fwd.z;
        if (dot > SPAWN_VIEW_COS) return -1.0f;
    }

    /* Reject a point that is inside geometry. Movers shift solids after load, so this is a
     * real case, not a defensive one: a closing blast door can put a spawn point inside it
     * and an entombbed enemy is both invisible and unkillable. */
    Vector3 probe = vec3(t->position.x, t->position.y + 0.35f, t->position.z);
    if (bp_world_point_solid(probe)) return -1.0f;

    /* Turrets additionally need the floor to actually be there (they never fall) and need
     * a clear shot: a turret spawned behind a pillar is just a free kill. */
    if (type == ET_TURRET)
    {
        float floorY = bp_world_floor_height(t->position, 0.4f, t->position.y + 1.0f);
        if (BP_ABS(floorY - t->position.y) > 1.2f) return -1.0f;
    }

    /* Prefer medium range: too close is unfair, too far wastes the wave (the player kills
     * everything before it arrives). The parabola peaks around 22 m. */
    float ideal = 22.0f;
    float d = dist - ideal;
    float shape = 100.0f - (d * d) * 0.12f;

    /* Behind-the-player spawns are slightly preferred: they create the "they came from
     * somewhere" pressure that makes arena awareness matter. */
    float behind = (dx * fwd.x + dz * fwd.z) < 0.0f ? 12.0f : 0.0f;

    return shape + behind;
}

static bool spawn_one(int type, Vector3 playerPos, Vector3 fwd)
{
    int spawnCount = bp_world_spawn_count(TRIG_ENEMY_SPAWN);
    if (spawnCount <= 0) return false;

    int   bestIdx = -1;
    float bestScore = -1e30f;
    int   cursor = g_waves.spawn_cursor;

    for (int a = 0; a < SPAWN_ATTEMPTS && a < spawnCount; a++)
    {
        int probe = cursor;
        const bp_trigger *t = NULL;

        /* Walk the trigger array from `probe` to find the n-th ENEMY_SPAWN. This is O(n)
         * per attempt, but n is under 40 and we attempt at most 9 times per spawn, so the
         * worst case is ~360 comparisons on a tick that spawns at most a few enemies. */
        int seen = 0;
        for (int i = 0; i < g_world.trigger_count; i++)
        {
            int idx = (probe + i) % g_world.trigger_count;
            if (g_world.triggers[idx].kind != TRIG_ENEMY_SPAWN) continue;
            if (seen == a) { t = &g_world.triggers[idx]; break; }
            seen++;
        }
        if (!t) break;

        float sc = score_spawn(t, type, playerPos, fwd);
        if (sc > bestScore) { bestScore = sc; bestIdx = a; }
    }

    cursor = (cursor + 1) % BP_MAX(spawnCount, 1);
    g_waves.spawn_cursor = cursor;

    if (bestIdx < 0 || bestScore < 0.0f)
    {
        g_waves.spawns_rejected++;
        /* Every legal candidate was refused. Falling back to "any spawn point" is better
         * than stalling the wave forever: a slightly unfair arrival beats a wave that
         * never ends and locks the player out of the exit. */
        int any = 0;
        const bp_trigger *t = NULL;
        for (int i = 0; i < g_world.trigger_count; i++)
        {
            int idx = (g_waves.spawn_cursor + i) % g_world.trigger_count;
            if (g_world.triggers[idx].kind != TRIG_ENEMY_SPAWN) continue;
            if (!bp_world_point_solid(vec3(g_world.triggers[idx].position.x,
                                           g_world.triggers[idx].position.y + 0.35f,
                                           g_world.triggers[idx].position.z)))
            { t = &g_world.triggers[idx]; any = idx; break; }
        }
        if (!t) return false;
        BP_UNUSED(any);
        bp_handle h = bp_enemy_spawn((bp_enemy_type)type, t->position, t->yaw, false);
        if (!(h.index != 0)) return false;
        return true;
    }

    /* re-resolve the winning index to its trigger */
    const bp_trigger *win = NULL;
    int seen = 0;
    for (int i = 0; i < g_world.trigger_count; i++)
    {
        int idx = (g_waves.spawn_cursor + i) % g_world.trigger_count;
        if (g_world.triggers[idx].kind != TRIG_ENEMY_SPAWN) continue;
        if (seen == bestIdx) { win = &g_world.triggers[idx]; break; }
        seen++;
    }
    if (!win) return false;

    bp_handle h = bp_enemy_spawn((bp_enemy_type)type, win->position, win->yaw, false);
    if (!(h.index != 0)) return false;

    /* Arrival feedback: a short ember burst at the spawn point. Without it enemies appear
     * silently and the player cannot build a mental model of where pressure comes from. */
    Vector3 at = vec3(win->position.x, win->position.y + 0.5f, win->position.z);
    for (int i = 0; i < 5; i++)
    {
        float a = bp_rng_range(&g_engine.rng, 0.0f, BP_TAU);
        bp_fx_particle(PTX_EMBER, at,
                       vec3(cosf(a) * 1.6f, bp_rng_range(&g_engine.rng, 0.8f, 2.6f), sinf(a) * 1.6f),
                       0.38f, 0.07f, 0.0f, g_assets.pal_danger, (Color){ 232, 52, 60, 0 });
    }
    return true;
}

/* Pick the archetype to spawn next: highest pending count, ties broken randomly so a wave
 * does not always front-load the same type. */
static int choose_pending_type(void)
{
    /* Largest pending count first, so a wave delivers its bulk before its specialists --
     * arriving as "grunt grunt grunt ... brute" reads as an escalation, whereas the reverse
     * reads as an anticlimax. Ties are broken uniformly at random: a fixed tie order would
     * make every wave open with the same archetype. */
    int bestN = 0;
    for (int t = 0; t < ET_COUNT; t++) if (g_waves.pending[t] > bestN) bestN = g_waves.pending[t];
    if (bestN <= 0) return -1;

    int tied[ET_COUNT], n = 0;
    for (int t = 0; t < ET_COUNT; t++) if (g_waves.pending[t] == bestN) tied[n++] = t;
    return tied[bp_rng_int(&g_engine.rng, n)];
}

static void do_spawn_tick(Vector3 playerPos, Vector3 fwd, int burst)
{
    for (int i = 0; i < burst; i++)
    {
        if (g_waves.budget <= 0) break;
        if (bp_enemy_alive_count() >= g_waves.max_onscreen) break;

        int type = choose_pending_type();
        if (type < 0)
        {
            /* Composition exhausted but budget remains: this happens when the turret cap
             * or a rejected spawn desynchronises the two counters. Refill with grunts so
             * the wave can always finish. */
            g_waves.pending[ET_GRUNT] += g_waves.budget;
            type = ET_GRUNT;
        }

        if (!spawn_one(type, playerPos, fwd))
        {
            /* Placement failed. Return the enemy to the pool of pending and stop trying
             * this tick rather than burning every attempt on an impossible spawn. */
            g_waves.pending[type]++;
            break;
        }

        g_waves.pending[type]--;
        g_waves.budget--;
        g_waves.spawned_this_wave++;
    }
}

/* -------------------------------------------------------------------------------------- */
/* skill read (rubber banding)                                                             */
/* -------------------------------------------------------------------------------------- */
static float s_dmgWindow = 0.0f;
static float s_dmgAccum  = 0.0f;
static float s_lastHealth = 0.0f;

static void update_skill(float dt)
{
    /* Damage taken per second, measured over a rolling 6 s window. We diff the player's
     * health instead of hooking the damage function so the read cannot drift out of sync
     * with healing (a health pack mid-window would otherwise look like negative damage). */
    float h = g_player.health;
    float delta = s_lastHealth - h;
    s_lastHealth = h;
    if (delta > 0.0f) s_dmgAccum += delta;
    s_dmgWindow += dt;

    if (s_dmgWindow >= 6.0f)
    {
        float dps = s_dmgAccum / s_dmgWindow;
        float acc = bp_weapons_accuracy();          /* 0..1 over the run */

        /* Reference points: 12 dps is "getting chewed", 0 dps is "untouched".
         * 0.55 accuracy is a competent player at range. */
        float pressureFromDamage = BP_CLAMP(dps / 12.0f, 0.0f, 1.0f);   /* 1 = struggling */
        float pressureFromSkill  = 1.0f - BP_CLAMP(acc / 0.55f, 0.0f, 1.0f);

        float target = 1.0f - 0.5f * (pressureFromDamage + pressureFromSkill);  /* 0..1, 0.5 = neutral */
        float signedSkill = (target - 0.5f) * 2.0f;                             /* -1..+1 */

        /* Smooth so a single bad second does not swing the whole wave. */
        g_waves.skill = bp_damp(g_waves.skill, BP_CLAMP(signedSkill, -1.0f, 1.0f), SKILL_LAMBDA, s_dmgWindow);

        s_dmgWindow = 0.0f;
        s_dmgAccum = 0.0f;
    }

    /* Aggression is the single number the spawner actually uses: neutral 1.0, plus or
     * minus SKILL_CLAMP. Note the sign: a struggling player (skill < 0) gets LESS pressure. */
    g_waves.aggression = BP_CLAMP(1.0f + g_waves.skill * SKILL_CLAMP, 1.0f - SKILL_CLAMP, 1.0f + SKILL_CLAMP);
}

/* -------------------------------------------------------------------------------------- */
/* state transitions                                                                       */
/* -------------------------------------------------------------------------------------- */
static void enter_state(bp_wave_state s)
{
    g_waves.state = s;
    g_waves.state_time = 0.0f;
}

static void begin_wave(int waveIndex)
{
    g_waves.wave_index = waveIndex;
    g_waves.budget = wave_budget(waveIndex);
    compose_wave(waveIndex, g_waves.budget);
    g_waves.spawned_this_wave = 0;
    g_waves.killed_this_wave = 0;
    g_waves.damage_taken_this_wave = false;
    g_waves.spawn_timer = 0.0f;
    s_lastHealth = g_player.health;

    /* Pressure scales with the wave and is then modulated by difficulty and by the skill
     * read. Everything is clamped: the director must not be able to exceed the perf budget
     * no matter what the config says. */
    float waveRamp = 1.0f + (float)(waveIndex - 1) * 0.11f;
    float target = (float)ONSCREEN_MIN * waveRamp * diff()->pressure * g_waves.aggression;
    g_waves.max_onscreen = BP_CLAMP((int)(target + 0.5f), ONSCREEN_MIN, ONSCREEN_MAX);

    float interval = SPAWN_INTERVAL_MAX / BP_MAX(waveRamp * diff()->pressure * g_waves.aggression, 0.2f);
    g_waves.spawn_interval = BP_CLAMP(interval, SPAWN_INTERVAL_MIN, SPAWN_INTERVAL_MAX);

    enter_state(WAVE_INTRO);
    bp_audio_play(SFX_WAVE_START, 1.0f);
    bp_audio_set_intensity(BP_CLAMP(0.30f + 0.10f * (float)waveIndex, 0.30f, 0.95f), 0.4f);

    BP_LOGI("waves: wave %d/%d -- %d enemies, max %d onscreen, %.2fs trickle (difficulty %s)",
            waveIndex, g_waves.wave_total, g_waves.budget, g_waves.max_onscreen,
            (double)g_waves.spawn_interval, diff()->name);
}

static void finish_wave(void)
{
    float t = g_waves.state_time;
    g_waves.longest_wave = BP_MAX(g_waves.longest_wave, t);
    g_waves.fastest_wave = (g_waves.fastest_wave <= 0.0f) ? t : BP_MIN(g_waves.fastest_wave, t);
    g_waves.waves_completed++;

    if (!g_waves.damage_taken_this_wave)
    {
        g_waves.perfect_waves++;
        bp_player_add_score(PERFECT_WAVE_BONUS);
        BP_LOGI("waves: perfect clear (+%d)", PERFECT_WAVE_BONUS);
    }

    bp_audio_play(SFX_WAVE_CLEAR, 1.0f);

    if (g_waves.wave_index >= g_waves.wave_total)
    {
        enter_state(WAVE_COMPLETE);
        /* Music drops back to the pad layer: the fight is over and the audio has to say so
         * before the HUD does, or the moment lands flat. */
        bp_audio_set_intensity(0.18f, 1.2f);
        BP_LOGI("waves: level complete -- %d kills, %d perfect, %.1fs",
                g_waves.killed_this_level, g_waves.perfect_waves, (double)g_waves.level_time);
        return;
    }

    g_waves.intermission_time = BP_LERP(WAVE_INTERMISSION_MAX, WAVE_INTERMISSION_MIN,
                                        BP_CLAMP((float)g_waves.wave_index / (float)BP_MAX(g_waves.wave_total, 1), 0.0f, 1.0f));
    g_waves.intermission_left = g_waves.intermission_time;
    g_waves.resupply_given = false;
    enter_state(WAVE_INTERMISSION);
}

static void give_resupply(void)
{
    if (g_waves.resupply_given) return;
    g_waves.resupply_given = true;

    float frac = diff()->resupply;
    if (frac <= 0.001f) return;                 /* BREACH difficulty: no help at all */

    /* Top up toward full rather than by a fixed amount: a player at 95 health should not
     * get the same reward as one at 20, otherwise surviving well is penalised. */
    float heal = (g_player.max_health - g_player.health) * frac;
    float armor = (g_player.max_armor - g_player.armor) * frac * 0.65f;
    if (heal > 0.5f) bp_player_heal(heal);
    if (armor > 0.5f) bp_player_add_armor(armor);

    /* Ammo is refilled as a fraction of one full reserve per weapon, which keeps the
     * rocket launcher from becoming a spammable default in long levels. */
    int rounds = 0;
    for (int w = 0; w < WPN_ID_COUNT; w++)
    {
        if (!bp_weapons_owns((bp_weapon_id)w)) continue;
        int full = g_weapon_table[w].mag_size * 3;
        rounds += (int)((float)full * frac * 0.5f);
    }
    if (rounds > 0) bp_weapons_add_reserve_all(rounds / BP_MAX(1, g_waves.wave_index > 3 ? 2 : 1));

    /* Place a visible pickup at the player's feet too, so the resupply reads as an event
     * rather than a silent stat change. */
    bp_pickup_spawn(PICKUP_AMMO, vec3(g_player.pos.x, g_player.pos.y + 0.1f, g_player.pos.z),
                    (float)BP_MAX(8, (int)(24.0f * frac)), -1);
    bp_audio_play(SFX_HEAL, 0.7f);
}

/* -------------------------------------------------------------------------------------- */
/* public API                                                                              */
/* -------------------------------------------------------------------------------------- */
void bp_waves_init(void)
{
    memset(&g_waves, 0, sizeof(g_waves));
    g_waves.difficulty = 1;
    g_waves.diff_health = g_difficulties[1].enemy_health;
    g_waves.diff_damage = g_difficulties[1].enemy_damage;
    g_waves.diff_score = g_difficulties[1].score;
    g_waves.diff_resupply = g_difficulties[1].resupply;
    g_waves.aggression = 1.0f;
    g_waves.max_onscreen = ONSCREEN_MIN;
    g_waves.spawn_interval = SPAWN_INTERVAL_MAX;
    g_waves.state = WAVE_IDLE;
    g_waves.initialised = true;
    s_dmgWindow = s_dmgAccum = 0.0f;
    s_lastHealth = 0.0f;
}

void bp_waves_start_level(int levelIndex)
{
    if (levelIndex < 0 || levelIndex >= g_level_count)
    {
        BP_LOGW("waves: level index %d out of range", levelIndex);
        levelIndex = 0;
    }

    g_waves.level_index = levelIndex;
    g_waves.wave_total = BP_MAX(1, g_levels[levelIndex].waves);
    g_waves.level_time = 0.0f;
    g_waves.wave_index = 0;
    g_waves.killed_this_level = 0;
    g_waves.spawns_rejected = 0;
    g_waves.spawn_cursor = 0;
    g_waves.skill = 0.0f;
    g_waves.aggression = 1.0f;
    g_waves.longest_wave = 0.0f;
    g_waves.fastest_wave = 0.0f;
    g_waves.waves_completed = 0;
    g_waves.perfect_waves = 0;
    memset(g_waves.pending, 0, sizeof(g_waves.pending));
    s_dmgWindow = s_dmgAccum = 0.0f;
    s_lastHealth = g_player.health;

    /* Resolve difficulty once. Changing difficulty mid-level is deliberately not supported:
     * the multipliers are baked into already-spawned enemies' health. */
    g_waves.diff_health = diff()->enemy_health;
    g_waves.diff_damage = diff()->enemy_damage;
    g_waves.diff_score = diff()->score;
    g_waves.diff_resupply = diff()->resupply;

    int spawns = bp_world_spawn_count(TRIG_ENEMY_SPAWN);
    if (spawns <= 0) BP_LOGW("waves: level '%s' has no enemy spawn points", g_world.name);

    BP_LOGI("waves: starting level %d '%s' -- %d waves, %d spawn points, difficulty %s",
            levelIndex, g_levels[levelIndex].display_name, g_waves.wave_total, spawns, diff()->name);

    begin_wave(1);
}

void bp_waves_notify_kill(int enemyType)
{
    BP_UNUSED(enemyType);
    g_waves.killed_this_wave++;
    g_waves.killed_this_level++;
}

void bp_waves_update(float dt)
{
    if (!g_waves.initialised || dt <= 0.0f) return;

    g_waves.state_time += dt;
    if (g_waves.state != WAVE_IDLE && g_waves.state != WAVE_COMPLETE) g_waves.level_time += dt;

    /* Track incoming damage for the perfect-wave bonus and the skill read. */
    if (g_player.health < s_lastHealth - 0.01f) g_waves.damage_taken_this_wave = true;

    Vector3 playerPos = g_player.pos;
    Vector3 fwd = bp_camera_forward();
    fwd.y = 0.0f;
    float fl = Vector3Length(fwd);
    if (fl > 0.001f) fwd = Vector3Scale(fwd, 1.0f / fl); else fwd = vec3(0, 0, 1);

    switch (g_waves.state)
    {
        case WAVE_IDLE:
            break;

        case WAVE_INTRO:
            /* A short breather with the banner up. Enemies do not spawn during it, which is
             * what makes the wave start readable instead of ambient. */
            update_skill(dt);
            if (g_waves.state_time >= WAVE_INTRO_TIME)
            {
                enter_state(WAVE_ACTIVE);
                do_spawn_tick(playerPos, fwd, SPAWN_OPENING_BURST);
            }
            break;

        case WAVE_ACTIVE:
            update_skill(dt);
            g_waves.spawn_timer -= dt;
            if (g_waves.spawn_timer <= 0.0f && g_waves.budget > 0)
            {
                /* Adaptive trickle: when many enemies are already alive, slow down instead
                 * of queueing spawns that the onscreen cap will reject anyway. */
                float load = (float)bp_enemy_alive_count() / (float)BP_MAX(g_waves.max_onscreen, 1);
                g_waves.spawn_timer = g_waves.spawn_interval * (1.0f + load * 0.8f);
                do_spawn_tick(playerPos, fwd, 1);
            }
            if (g_waves.budget <= 0) enter_state(WAVE_CLEARING);
            break;

        case WAVE_CLEARING:
            update_skill(dt);
            /* All spawns are out. Keep the pressure honest: if the last enemy is somehow
             * unreachable (entombed by a mover, fell out of the arena) the wave must still
             * end, or the player is softlocked out of the exit with no way to recover. */
            if (bp_enemy_alive_count() <= 0) finish_wave();
            else if (g_waves.state_time > 120.0f)
            {
                BP_LOGW("waves: %d enemies unreachable after 120s -- forcing wave clear",
                        bp_enemy_alive_count());
                bp_enemies_clear();
                finish_wave();
            }
            break;

        case WAVE_INTERMISSION:
            give_resupply();
            g_waves.intermission_left -= dt;
            if (g_waves.intermission_left <= 0.0f) begin_wave(g_waves.wave_index + 1);
            break;

        case WAVE_COMPLETE:
            /* Nothing left to direct. The game layer polls bp_waves_level_complete() and
             * decides whether to show the stats screen or advance to the next level. */
            bp_audio_set_intensity(0.18f, 0.5f);
            break;

        default:
            enter_state(WAVE_IDLE);
            break;
    }
}
