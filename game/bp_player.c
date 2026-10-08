/******************************************************************************************
 * bp_player.c -- movement, vitals, pickups, trigger contact.
 *
 * WHY THIS FILE IS THE MOST IMPORTANT ONE IN THE PROJECT
 *   In a boomer shooter the player *is* the game. Enemies can be reskinned, levels can be
 *   reblocked, weapons can be retuned -- but if movement does not feel right nothing else
 *   matters. Every constant below is therefore a deliberate, documented decision rather
 *   than a placeholder, and the acceleration model is the Quake/DOOM lineage on purpose:
 *
 *     1. wishdir acceleration with a speed cap applied only along the wish direction.
 *        Consequence: turning while holding strafe converts forward speed into lateral
 *        speed and *adds* to the total, so circle-strafing exceeds the straight-line cap.
 *        That emergent property is the entire skill ceiling of arena movement and it falls
 *        out of ~15 lines of code. We only clamp with a hard ceiling to stop it diverging.
 *
 *     2. friction applied only to the component of velocity *opposing* the input.
 *        Consequence: releasing the keys stops you in about a third of a second (crisp),
 *        but a direction change keeps most of your momentum (flowy). Applying friction to
 *        the whole velocity vector instead -- the naive version -- makes the game feel
 *        like wading through syrup and is the single most common mistake in indie FPS
 *        movement code.
 *
 *     3. generous jump forgiveness: coyote time (jump still works briefly after walking
 *        off a ledge) and jump buffering (pressing jump slightly before landing still
 *        jumps). Both are invisible when they work and infuriating when absent.
 *
 *   The camera is NOT driven from here. We publish body position + speed and
 *   bp_camera_simulate() derives eye height, bob, FOV and roll. Keeping those separate is
 *   what allows a death cam / spectator cam later without touching a line of movement code.
 ******************************************************************************************/
#include "bp_player.h"

#include "../engine/bp_math.h"
#include "../engine/bp_camera.h"
#include "../engine/bp_input.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_gfx.h"
#include "bp_world.h"
#include "bp_fx.h"
#include "bp_weapons.h"
#include "bp_enemies.h"

bp_player g_player = { 0 };

/* -------------------------------------------------------------------------------------- */
/* tuning                                                                                  */
/* -------------------------------------------------------------------------------------- */
/*
 * Jump height is derived, not guessed: h = v^2 / (2 * |GRAVITY|).
 *   JUMP_VELOCITY 8.6 -> 1.32 m standing.
 * The level budget in bp_levels.inc assumes exactly this: 0.55 m walk-up steps, 1.10 m
 * standing-jump platforms, 1.50 m reachable only with a sprint start (which adds height
 * through the air-control projection, not through a higher jump). If you change
 * JUMP_VELOCITY you must re-verify every platform in every map.
 */
#define PLAYER_SPEED_GROUND      8.2f
#define PLAYER_SPEED_SPRINT     11.5f
#define PLAYER_SPEED_CROUCH      3.4f
#define PLAYER_SPEED_AIR         1.35f   /* air-control accel cap, NOT a speed cap */
#define PLAYER_ACCEL_GROUND     72.0f    /* 0 -> top speed in ~0.11 s: instant by design */
#define PLAYER_ACCEL_AIR        16.0f
#define PLAYER_FRICTION_GROUND  13.0f
#define PLAYER_FRICTION_STOP    17.0f    /* higher when no key is held: crisp stop */
#define PLAYER_JUMP_VELOCITY     8.6f
#define PLAYER_COYOTE_TIME       0.12f
#define PLAYER_JUMP_BUFFER       0.14f
#define PLAYER_SPEED_HARD_CAP   24.0f    /* stops circle-strafe diverging to infinity */
#define PLAYER_FOV_MAX_BONUS     9.0f

#define PLAYER_HEIGHT_CROUCH     1.15f   /* collision height while crouched */
#define PLAYER_EYE_CROUCH        0.95f
#define PLAYER_CROUCH_SPEED      12.0f   /* 0->1 blend rate for the crouch transition */
#define PLAYER_HEAD_CLEAR        0.12f   /* headroom required to stand back up */

/* Fall damage. Threshold is expressed as impact speed, not fall height, because impact
 * speed is what we actually have and it composes correctly with jumps off moving lifts.
 *   14 m/s = a drop of 3.5 m. Below that: free. Above: 3.2 damage per m/s of excess.
 * SLAG PIT's 6.9 m catwalk therefore costs ~13 health if you step off -- punishing but
 * never lethal, which is the right trade for a map that wants you to take the stairs. */
#define PLAYER_FALL_SAFE_SPEED  14.0f
#define PLAYER_FALL_DMG_PER_MS   3.2f
#define PLAYER_LAND_SHAKE_MAX    0.55f

#define PLAYER_STEP_LEN_RUN      2.35f   /* metres of travel per footstep */
#define PLAYER_STEP_LEN_CROUCH   1.45f

#define PLAYER_HAZARD_TICK       0.5f    /* seconds between hazard damage applications */
#define PLAYER_INVULN_SPAWN      1.1f
#define PLAYER_DEATH_DURATION    1.75f   /* dying -> dead, the death-cam window */
#define PLAYER_LOW_HP_THRESH    28.0f
#define PLAYER_LOW_HP_INTERVAL   1.15f

#define PLAYER_STREAK_WINDOW     3.5f    /* seconds to keep a kill streak alive */
#define PLAYER_STREAK_MAX        8

#define PICKUP_RADIUS            0.95f   /* horizontal reach for auto-collection */
#define PICKUP_HEIGHT_TOL        1.60f   /* vertical reach: generous so you never miss */
#define PICKUP_BOB_HEIGHT        0.16f
#define PICKUP_SPIN_SPEED       95.0f    /* degrees per second */

/* -------------------------------------------------------------------------------------- */
/* helpers                                                                                 */
/* -------------------------------------------------------------------------------------- */
static inline Vector3 vec3(float x, float y, float z) { return (Vector3){ x, y, z }; }

/* Horizontal speed only. Vertical velocity must never make the player feel fast -- if it
 * did, falling down a shaft would widen the FOV, which reads as a bug. */
static inline float hspeed(const Vector3 v) { return sqrtf(v.x * v.x + v.z * v.z); }

static inline float player_height_now(void)
{
    return BP_LERP(PLAYER_HEIGHT, PLAYER_HEIGHT_CROUCH, g_player.crouch_amount);
}

static inline float player_eye_height(void)
{
    return BP_LERP(PLAYER_EYE_HEIGHT, PLAYER_EYE_CROUCH, g_player.crouch_amount);
}

/* Is there room to stand up at the current horizontal position? Checked before we release
 * the crouch, otherwise the player wedges inside a crate and the resolver ejects them in
 * an arbitrary direction -- the classic "crouch under a ledge and get launched" bug. */
static bool has_stand_headroom(void)
{
    Vector3 feet = g_player.pos;
    bp_aabb standing = bp_aabb_make(
        vec3(feet.x - g_player.radius, feet.y, feet.z - g_player.radius),
        vec3(feet.x + g_player.radius, feet.y + PLAYER_HEIGHT + PLAYER_HEAD_CLEAR,
             feet.z + g_player.radius));

    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed || s->shoot_through) continue;
        if (bp_aabb_overlaps(&standing, &s->box)) return false;
    }
    return true;
}

/* -------------------------------------------------------------------------------------- */
/* init / reset                                                                            */
/* -------------------------------------------------------------------------------------- */
void bp_player_init(void)
{
    memset(&g_player, 0, sizeof(g_player));
    g_player.max_health     = PLAYER_MAX_HEALTH;
    g_player.max_armor      = PLAYER_MAX_ARMOUR;
    g_player.health         = PLAYER_MAX_HEALTH;
    g_player.radius         = PLAYER_RADIUS;
    g_player.height         = PLAYER_HEIGHT;

    g_player.speed_ground   = PLAYER_SPEED_GROUND;
    g_player.speed_sprint   = PLAYER_SPEED_SPRINT;
    g_player.speed_crouch   = PLAYER_SPEED_CROUCH;
    g_player.speed_air      = PLAYER_SPEED_AIR;
    g_player.accel_ground   = PLAYER_ACCEL_GROUND;
    g_player.accel_air      = PLAYER_ACCEL_AIR;
    g_player.friction_ground= PLAYER_FRICTION_GROUND;
    g_player.jump_velocity  = PLAYER_JUMP_VELOCITY;
    g_player.coyote_time    = 0.0f;
    g_player.jump_buffer    = 0.0f;

    g_player.state          = PST_ALIVE;
    g_player.initialised    = true;
    BP_LOGI("player: initialised (ground %.1f m/s, sprint %.1f, jump %.2f m)",
            (double)PLAYER_SPEED_GROUND, (double)PLAYER_SPEED_SPRINT,
            (double)(PLAYER_JUMP_VELOCITY * PLAYER_JUMP_VELOCITY / (2.0f * -GRAVITY)));
}

void bp_player_reset(Vector3 start, float yaw)
{
    int   keepScore   = g_player.score;
    int   keepKills   = g_player.kills;
    int   keepSecrets = g_player.secrets_found;
    int   keepPickups = g_player.pickups_taken;

    memset(&g_player, 0, sizeof(g_player));
    g_player.max_health      = PLAYER_MAX_HEALTH;
    g_player.max_armor       = PLAYER_MAX_ARMOUR;
    g_player.health          = PLAYER_MAX_HEALTH;
    g_player.armor           = 0.0f;
    g_player.radius          = PLAYER_RADIUS;
    g_player.height          = PLAYER_HEIGHT;

    g_player.speed_ground    = PLAYER_SPEED_GROUND;
    g_player.speed_sprint    = PLAYER_SPEED_SPRINT;
    g_player.speed_crouch    = PLAYER_SPEED_CROUCH;
    g_player.speed_air       = PLAYER_SPEED_AIR;
    g_player.accel_ground    = PLAYER_ACCEL_GROUND;
    g_player.accel_air       = PLAYER_ACCEL_AIR;
    g_player.friction_ground = PLAYER_FRICTION_GROUND;
    g_player.jump_velocity   = PLAYER_JUMP_VELOCITY;

    g_player.pos = start;
    g_player.yaw = yaw;
    g_player.vel = vec3(0, 0, 0);
    g_player.state = PST_ALIVE;
    g_player.invuln_time = PLAYER_INVULN_SPAWN;
    g_player.on_ground = true;
    g_player.initialised = true;

    /* Run-to-run totals survive a respawn within a level; only a level load clears them.
     * That is why bp_player_reset preserves them explicitly instead of trusting the caller
     * to re-seed: a death should never cost the player their score. */
    g_player.score          = keepScore;
    g_player.kills          = keepKills;
    g_player.secrets_found  = keepSecrets;
    g_player.pickups_taken  = keepPickups;

    bp_camera_reset(vec3(start.x, start.y + PLAYER_EYE_HEIGHT, start.z));
    bp_camera_set_angles(yaw, 0.0f);
}

/* -------------------------------------------------------------------------------------- */
/* wish direction from input                                                               */
/* -------------------------------------------------------------------------------------- */
/*
 * Movement is always expressed relative to the *yaw only*. Pitching the camera up must not
 * launch the player into the ceiling -- a mistake every first FPS makes at least once.
 */
static Vector3 compute_wish(float *out_strength, bool *sprinting)
{
    /* bp_move_axis_y is +1 forward / -1 back; x is +1 right / -1 left. Both already fold in
     * gamepad analogue sticks and the deadzone, so gameplay never touches a key code. */
    float fwd = bp_move_axis_y();
    float rgt = bp_move_axis_x();

    float len = sqrtf(fwd * fwd + rgt * rgt);
    if (len > 1.0f) { fwd /= len; rgt /= len; len = 1.0f; }   /* normalise digital diagonals */

    float yawRad = g_player.yaw * BP_DEG2RAD;
    float sx = sinf(yawRad), cz = cosf(yawRad);

    /* camera forward projected onto the ground plane is (sin(yaw), 0, cos(yaw)) -- but note
     * the sign convention: yaw 0 looks down +Z. Right is therefore (cos(yaw), 0, -sin(yaw)). */
    Vector3 wish = vec3(fwd * sx + rgt * cz, 0.0f, fwd * cz - rgt * sx);

    *out_strength = len;
    *sprinting = bp_down(ACT_SPRINT) && fwd > 0.35f && !g_player.crouched;
    return wish;
}

/* -------------------------------------------------------------------------------------- */
/* acceleration / friction                                                                 */
/* -------------------------------------------------------------------------------------- */
static void accelerate(Vector3 wish, float wishSpeed, float accel, float dt)
{
    float current = g_player.vel.x * wish.x + g_player.vel.z * wish.z;   /* signed projection */
    float add = wishSpeed - current;
    if (add <= 0.0f) return;              /* already at or above target along this axis */

    float accelSpeed = accel * dt * wishSpeed;
    if (accelSpeed > add) accelSpeed = add;

    g_player.vel.x += wish.x * accelSpeed;
    g_player.vel.z += wish.z * accelSpeed;
}

/*
 * Friction, applied only against the input.
 *
 * `inputStrength` is 0 when no movement key is held. In that case we bleed the whole
 * horizontal velocity (crisp stop). When input *is* held we only bleed the component that
 * opposes the wish direction, so a turn preserves momentum instead of scrubbing speed.
 */
static void apply_friction(Vector3 wish, float inputStrength, float dt)
{
    float speed = hspeed(g_player.vel);
    if (speed < 0.001f) { g_player.vel.x = 0.0f; g_player.vel.z = 0.0f; return; }

    /* Below this speed friction becomes linear instead of exponential, otherwise the
     * velocity decays asymptotically and the player drifts forever at 0.01 m/s. */
    const float CONTROL = 1.6f;
    float drop;

    float friction = (inputStrength < 0.01f) ? PLAYER_FRICTION_STOP : PLAYER_FRICTION_GROUND;

    /* Standing on a low-friction surface (grate, hazard) should slide. */
    const bp_material_def *md = bp_world_material(bp_world_material_at_point(
        vec3(g_player.pos.x, g_player.pos.y - 0.05f, g_player.pos.z)));
    if (md && md->friction > 0.01f) friction /= md->friction;

    if (inputStrength > 0.01f)
    {
        /* Opposing component only: project velocity onto the wish axis and keep the part
         * that points the same way. */
        float along = g_player.vel.x * wish.x + g_player.vel.z * wish.z;
        if (along >= 0.0f) return;                       /* input agrees: no scrubbing */
        float opposing = -along * inputStrength;
        drop = opposing * friction * dt;
        if (drop > -along) drop = -along;
        /* remove exactly the opposing part along the wish axis */
        g_player.vel.x += wish.x * drop;
        g_player.vel.z += wish.z * drop;
        return;
    }

    float control = (speed < CONTROL) ? CONTROL : speed;
    drop = control * friction * dt;
    if (drop >= speed) { g_player.vel.x = 0.0f; g_player.vel.z = 0.0f; return; }

    float scale = (speed - drop) / speed;
    g_player.vel.x *= scale;
    g_player.vel.z *= scale;
}

static void clamp_speed(void)
{
    float s = hspeed(g_player.vel);
    if (s > PLAYER_SPEED_HARD_CAP)
    {
        float k = PLAYER_SPEED_HARD_CAP / s;
        g_player.vel.x *= k;
        g_player.vel.z *= k;
    }
}

/* -------------------------------------------------------------------------------------- */
/* footsteps                                                                               */
/* -------------------------------------------------------------------------------------- */
static void update_footsteps(float travelled, int groundMaterial, float dt)
{
    BP_UNUSED(dt);
    if (!g_player.on_ground || travelled < 0.0001f) return;

    float stride = g_player.crouched ? PLAYER_STEP_LEN_CROUCH
               : (hspeed(g_player.vel) > g_player.speed_ground + 0.5f ? 2.75f : PLAYER_STEP_LEN_RUN);

    g_player.step_phase += travelled / stride;
    if (g_player.step_phase < 1.0f) return;
    g_player.step_phase -= floorf(g_player.step_phase);

    const bp_material_def *md = bp_world_material(groundMaterial);
    int sfx = (md && md->step_sfx >= 0) ? md->step_sfx : SFX_FOOTSTEP;

    /* Crouched footsteps are quieter -- audible feedback that crouching actually helps. */
    float vol = g_player.crouched ? 0.28f : BP_CLAMP(hspeed(g_player.vel) / g_player.speed_ground, 0.4f, 1.0f);
    bp_audio_play((bp_sfx_id)sfx, vol);

    /* A small dust puff at the feet sells contact with the ground far more than the sound
     * does, and it is nearly free: one particle, 0.25 s of life. */
    if (!g_player.crouched)
    {
        Vector3 foot = vec3(g_player.pos.x, g_player.pos.y + 0.04f, g_player.pos.z);
        bp_fx_particle(PTX_SMOKE, foot,
                       vec3(bp_rng_range(&g_engine.rng, -0.4f, 0.4f),
                            bp_rng_range(&g_engine.rng, 0.15f, 0.55f),
                            bp_rng_range(&g_engine.rng, -0.4f, 0.4f)),
                       0.30f, 0.10f, 0.34f,
                       (Color){ 190, 180, 168, 70 }, (Color){ 190, 180, 168, 0 });
    }
}

/* -------------------------------------------------------------------------------------- */
/* landing                                                                                 */
/* -------------------------------------------------------------------------------------- */
static void on_land(float impactSpeed, int groundMaterial)
{
    float severity = BP_CLAMP((impactSpeed - 6.0f) / 16.0f, 0.0f, 1.0f);

    if (impactSpeed > 3.0f)
    {
        const bp_material_def *md = bp_world_material(groundMaterial);
        int sfx = (md && md->step_sfx >= 0) ? md->step_sfx : SFX_LAND;
        bp_audio_play((bp_sfx_id)sfx, 0.35f + severity * 0.65f);
    }
    if (impactSpeed > 8.0f) bp_audio_play(SFX_LAND, 0.5f + severity * 0.5f);

    g_player.land_impact = severity;
    if (severity > 0.05f)
    {
        bp_camera_add_dip(severity * 5.5f);
        bp_fx_shake(severity * PLAYER_LAND_SHAKE_MAX, 0.16f + severity * 0.14f);
    }

    /* dust ring on a hard landing: reads as weight without costing a real shockwave */
    if (severity > 0.25f)
    {
        Vector3 at = vec3(g_player.pos.x, g_player.pos.y + 0.05f, g_player.pos.z);
        for (int i = 0; i < (int)(6 + severity * 10.0f); i++)
        {
            float a = bp_rng_range(&g_engine.rng, 0.0f, BP_TAU);
            float sp = bp_rng_range(&g_engine.rng, 1.2f, 3.4f) * (0.5f + severity);
            bp_fx_particle(PTX_SMOKE, at, vec3(cosf(a) * sp, bp_rng_range(&g_engine.rng, 0.3f, 1.4f), sinf(a) * sp),
                           bp_rng_range(&g_engine.rng, 0.35f, 0.7f), 0.14f, 0.6f,
                           (Color){ 200, 190, 175, 90 }, (Color){ 200, 190, 175, 0 });
        }
    }

    /* Fall damage. Applied after the feedback so the player sees/hears the landing before
     * the health bar drops -- the order matters for reading "that was my fault". */
    if (impactSpeed > PLAYER_FALL_SAFE_SPEED && g_player.state == PST_ALIVE)
    {
        float dmg = (impactSpeed - PLAYER_FALL_SAFE_SPEED) * PLAYER_FALL_DMG_PER_MS;
        /* No armour absorption on fall damage: armour is for gunfire, and letting it tank
         * falls would make armour a movement upgrade, which muddies both systems. */
        bp_player_damage(dmg, g_player.pos, 0.0f);
    }
}

/* -------------------------------------------------------------------------------------- */
/* trigger contact                                                                         */
/* -------------------------------------------------------------------------------------- */
static void pickup_from_trigger(bp_trigger *t)
{
    switch (t->kind)
    {
        case TRIG_HEALTH_SPAWN:
            bp_player_heal(t->param > 0 ? (float)t->param : 25.0f);
            break;
        case TRIG_ARMOR_SPAWN:
            bp_player_add_armor(t->param > 0 ? (float)t->param : 25.0f);
            break;
        case TRIG_AMMO_SPAWN:
        {
            int amount = t->param > 0 ? t->param : 24;
            bp_weapons_add_reserve_all(amount);
            bp_audio_play(SFX_AMMO_PICKUP, 0.9f);
            bp_fx_particle(PTX_FLASH, t->position, vec3(0, 0.4f, 0), 0.20f, 0.3f, 0.9f,
                           g_assets.pal_ammo, (Color){ 240, 208, 92, 0 });
            g_player.pickups_taken++;
            break;
        }
        case TRIG_WEAPON_SPAWN:
        {
            bp_weapon_id wid = (bp_weapon_id)BP_CLAMP(t->param, 0, WPN_ID_COUNT - 1);
            bool firstTime = !bp_weapons_owns(wid);
            int ammo = g_weapon_table[wid].ammo_per_pickup;
            bp_weapons_give(wid, firstTime ? g_weapon_table[wid].mag_size * 2 : ammo, firstTime);
            bp_audio_play(SFX_WEAPON_SWAP, 1.0f);
            bp_fx_shake(0.12f, 0.10f);
            g_player.pickups_taken++;
            if (firstTime)
            {
                /* New weapon: a ring + a burst of sparks so the player cannot miss it. This
                 * is the one pickup worth spending particles on. */
                bp_fx_particle(PTX_RING, t->position, vec3(0, 0, 0), 0.45f, 0.4f, 3.2f,
                               g_assets.pal_accent, (Color){ 255, 138, 32, 0 });
                for (int i = 0; i < 14; i++)
                {
                    float a = bp_rng_range(&g_engine.rng, 0.0f, BP_TAU);
                    bp_fx_particle(PTX_SPARK, t->position,
                                   vec3(cosf(a) * 3.0f, bp_rng_range(&g_engine.rng, 1.0f, 4.0f), sinf(a) * 3.0f),
                                   0.35f, 0.05f, 0.0f, g_assets.pal_accent, (Color){ 255, 220, 150, 0 });
                }
            }
            break;
        }
        case TRIG_EXIT:
            if (!g_player.exit_reached)
            {
                g_player.exit_reached = true;
                g_player.exit_time = g_engine.time_total;
                bp_audio_play(SFX_WAVE_CLEAR, 1.0f);
                BP_LOGI("player: exit reached at %.1fs", (double)g_engine.time_total);
            }
            return;    /* never consumed: the game layer decides what happens next */
        case TRIG_SECRET:
            if (!g_player.secret_reached)
            {
                g_player.secret_reached = true;
                g_player.secrets_found++;
                bp_player_add_score(500);
                bp_audio_play(SFX_SECRET, 1.0f);
                bp_fx_particle(PTX_RING, t->position, vec3(0, 0, 0), 0.6f, 0.3f, 4.0f,
                               g_assets.pal_accent2, (Color){ 92, 226, 196, 0 });
                BP_LOGI("player: secret found (%d total)", g_player.secrets_found);
            }
            break;
        case TRIG_DOOR:
            /* Door pads are proximity-activated, not consumed-on-touch: standing on the pad
             * must keep re-asserting "open" or the door slams on the player mid-stride. */
            bp_world_activate_doors(t->position, t->radius);
            return;
        default:
            return;
    }

    t->consumed = true;
    t->timer = t->respawn;
}

static void update_triggers(void)
{
    Vector3 centre = vec3(g_player.pos.x, g_player.pos.y + g_player.height * 0.5f, g_player.pos.z);

    for (int i = 0; i < g_world.trigger_count; i++)
    {
        bp_trigger *t = &g_world.triggers[i];
        if (t->consumed) continue;
        if (t->kind == TRIG_PLAYER_START || t->kind == TRIG_ENEMY_SPAWN) continue;

        float r = t->radius > 0.05f ? t->radius : 1.2f;
        float dx = centre.x - t->position.x;
        float dz = centre.z - t->position.z;
        float dy = centre.y - t->position.y;
        if (dx * dx + dz * dz > r * r) continue;
        if (BP_ABS(dy) > BP_MAX(r, 1.8f)) continue;

        pickup_from_trigger(t);
    }
}

/* -------------------------------------------------------------------------------------- */
/* pickups (dropped by enemies / placed by the level)                                       */
/* -------------------------------------------------------------------------------------- */
void bp_pickup_clear(void)
{
    for (int i = 0; i < MAX_PICKUPS; i++) g_player.pickups[i].alive = false;
    g_player.pickup_count = 0;
}

void bp_pickup_spawn(bp_pickup_kind kind, Vector3 pos, float amount, int weaponId)
{
    if (kind == PICKUP_NONE) return;

    /* Reuse a dead slot first, then append, then steal the oldest. Dropping a pickup must
     * never fail: a killed brute that drops nothing feels like the game ate the reward. */
    bp_pickup *p = NULL;
    for (int i = 0; i < MAX_PICKUPS; i++)
        if (!g_player.pickups[i].alive) { p = &g_player.pickups[i]; break; }

    if (!p)
    {
        int oldest = 0;
        float best = 1e30f;
        for (int i = 0; i < MAX_PICKUPS; i++)
            if (g_player.pickups[i].age > g_player.pickups[i].respawn_at && g_player.pickups[i].age < best)
            { best = g_player.pickups[i].age; oldest = i; }
        p = &g_player.pickups[oldest];
    }

    memset(p, 0, sizeof(*p));
    p->pos = pos;
    p->kind = kind;
    p->amount = amount;
    p->weapon_id = weaponId;
    p->alive = true;
    p->spinning = true;
    p->age = 0.0f;
    p->bob = bp_rng_range(&g_engine.rng, 0.0f, BP_TAU);   /* desync the bobbing across drops */
    g_player.pickup_count = BP_MIN(g_player.pickup_count + 1, MAX_PICKUPS);
}

/* Can this pickup actually help right now? Returning false leaves it on the floor instead
 * of consuming it -- a health pack you do not need is a health pack you want later, and
 * auto-eating it is the most universally hated pickup behaviour in the genre. */
static bool pickup_is_useful(const bp_pickup *p)
{
    switch (p->kind)
    {
        case PICKUP_HEALTH: return g_player.health < g_player.max_health - 0.5f;
        case PICKUP_ARMOR:  return g_player.armor  < g_player.max_armor  - 0.5f;
        case PICKUP_AMMO:   return true;   /* reserve caps are enforced by the weapon bank */
        case PICKUP_WEAPON: return true;
        default: return false;
    }
}

static void collect_pickup(bp_pickup *p)
{
    switch (p->kind)
    {
        case PICKUP_HEALTH:
            bp_player_heal(p->amount);
            break;
        case PICKUP_ARMOR:
            bp_player_add_armor(p->amount);
            break;
        case PICKUP_AMMO:
            bp_weapons_add_reserve_all((int)p->amount);
            bp_audio_play(SFX_AMMO_PICKUP, 0.8f);
            break;
        case PICKUP_WEAPON:
        {
            bp_weapon_id wid = (bp_weapon_id)BP_CLAMP(p->weapon_id, 0, WPN_ID_COUNT - 1);
            bool firstTime = !bp_weapons_owns(wid);
            bp_weapons_give(wid, firstTime ? g_weapon_table[wid].mag_size * 2 : (int)p->amount, firstTime);
            bp_audio_play(SFX_WEAPON_SWAP, 1.0f);
            break;
        }
        default: return;
    }

    g_player.pickups_taken++;
    p->alive = false;

    bp_fx_particle(PTX_FLASH, p->pos, vec3(0, 0.5f, 0), 0.18f, 0.25f, 0.8f,
                   (Color){ 255, 255, 255, 220 }, (Color){ 255, 255, 255, 0 });
}

void bp_pickup_update(float dt)
{
    int live = 0;
    Vector3 centre = vec3(g_player.pos.x, g_player.pos.y + g_player.height * 0.4f, g_player.pos.z);

    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        bp_pickup *p = &g_player.pickups[i];
        if (!p->alive) continue;
        live++;

        p->age += dt;
        p->bob += dt;

        /* Pickups rest on whatever the floor is under them, so a drop off a catwalk lands
         * instead of hovering at the height it was killed at. */
        float floorY = bp_world_floor_height(p->pos, 0.25f, p->pos.y + 0.05f);
        if (p->pos.y > floorY + 0.001f) p->pos.y = BP_MAX(floorY, p->pos.y - 6.0f * dt);
        else p->pos.y = floorY;

        if (g_player.state != PST_ALIVE) continue;

        float dx = centre.x - p->pos.x;
        float dz = centre.z - p->pos.z;
        float dy = centre.y - p->pos.y;
        if (dx * dx + dz * dz > PICKUP_RADIUS * PICKUP_RADIUS) continue;
        if (BP_ABS(dy) > PICKUP_HEIGHT_TOL) continue;

        if (pickup_is_useful(p)) collect_pickup(p);
    }
    g_player.pickup_count = live;
}

/*
 * Pickups are drawn as billboards rather than as their 3D models.
 *
 * Reason: the models exist (MDL_PICKUP_*) and are used by the world for static placements,
 * but a spinning 3D model at 40 m reads as a speck, while a coloured billboard with a
 * bright core reads instantly at any distance and costs one draw call for all of them.
 * Arena shooters live and die on "can I see the health pack mid-fight".
 */
void bp_pickup_draw(Camera3D cam, Vector3 viewer)
{
    BP_UNUSED(cam);
    Texture2D soft = bp_tex(TEX_PARTICLE_SOFT);
    Texture2D ring = bp_tex(TEX_RING);
    if (!soft.id) return;

    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        const bp_pickup *p = &g_player.pickups[i];
        if (!p->alive) continue;

        float dist = Vector3Distance(viewer, p->pos);
        if (dist > 90.0f) continue;

        Color c;
        float size;
        switch (p->kind)
        {
            case PICKUP_HEALTH: c = g_assets.pal_health; size = 0.42f; break;
            case PICKUP_ARMOR:  c = g_assets.pal_armor;  size = 0.42f; break;
            case PICKUP_AMMO:   c = g_assets.pal_ammo;   size = 0.36f; break;
            case PICKUP_WEAPON: c = g_assets.pal_accent; size = 0.52f; break;
            default: continue;
        }

        /* fade in over the first 0.25 s so a drop pops into existence instead of blinking */
        float appear = BP_CLAMP(p->age * 4.0f, 0.0f, 1.0f);
        float bobY = sinf(p->bob * 2.6f) * PICKUP_BOB_HEIGHT;
        Vector3 at = vec3(p->pos.x, p->pos.y + 0.42f + bobY, p->pos.z);

        /* distance fade keeps far pickups from strobing against the level geometry */
        float farFade = BP_CLAMP((90.0f - dist) / 25.0f, 0.0f, 1.0f);
        int alpha = (int)(230.0f * appear * farFade);
        if (alpha <= 0) continue;

        Color glow = c; glow.a = (unsigned char)BP_CLAMP((int)(alpha * 0.55f), 0, 255);
        Color core = c; core.a = (unsigned char)alpha;

        rlSetBlendMode(RL_BLEND_ADDITIVE);
        DrawBillboard(cam, ring, at, size * 2.1f, glow);
        rlSetBlendMode(RL_BLEND_ALPHA);
        DrawBillboard(cam, soft, at, size, core);

        /* Weapon pickups get a second, taller marker: they are the one thing a player will
         * cross a firefight for, so it must be locatable from the corner of the eye. */
        if (p->kind == PICKUP_WEAPON)
        {
            rlSetBlendMode(RL_BLEND_ADDITIVE);
            DrawBillboard(cam, soft, vec3(at.x, at.y + 0.75f, at.z), size * 0.5f, glow);
            rlSetBlendMode(RL_BLEND_ALPHA);
        }
    }
}

/* -------------------------------------------------------------------------------------- */
/* vitals                                                                                  */
/* -------------------------------------------------------------------------------------- */
void bp_player_heal(float amount)
{
    if (amount <= 0.0f || g_player.state != PST_ALIVE) return;
    float before = g_player.health;
    g_player.health = BP_MIN(g_player.health + amount, g_player.max_health);
    if (g_player.health > before + 0.01f)
    {
        g_player.heal_flash = 1.0f;
        bp_audio_play(SFX_HEAL, 0.85f);
        bp_fx_particle(PTX_RING, vec3(g_player.pos.x, g_player.pos.y + 0.2f, g_player.pos.z),
                       vec3(0, 0, 0), 0.35f, 0.3f, 1.8f,
                       g_assets.pal_health, (Color){ 86, 214, 110, 0 });
    }
}

void bp_player_add_armor(float amount)
{
    if (amount <= 0.0f || g_player.state != PST_ALIVE) return;
    float before = g_player.armor;
    g_player.armor = BP_MIN(g_player.armor + amount, g_player.max_armor);
    if (g_player.armor > before + 0.01f)
    {
        bp_audio_play(SFX_ARMOR, 0.85f);
        g_player.heal_flash = BP_MAX(g_player.heal_flash, 0.5f);
    }
}

void bp_player_damage(float amount, Vector3 sourcePos, float armorAbsorb)
{
    if (amount <= 0.0f || g_player.state != PST_ALIVE) return;
    if (g_player.invuln_time > 0.0f) return;

    /* Armour takes a share first, then depletes. armorAbsorb is the fraction of the hit the
     * armour is allowed to eat (0 for fall damage, ~0.65 for gunfire). Splitting the hit
     * rather than gating it entirely means armour always feels valuable but never makes
     * the player immortal -- a brute still chunks you through full armour. */
    float absorbed = 0.0f;
    if (g_player.armor > 0.0f && armorAbsorb > 0.0f)
    {
        absorbed = BP_MIN(g_player.armor, amount * armorAbsorb);
        g_player.armor -= absorbed;
        amount -= absorbed;
    }

    g_player.health -= amount;
    g_player.damage_flash = BP_MIN(1.0f, g_player.damage_flash + BP_CLAMP(amount / 40.0f, 0.18f, 1.0f));
    g_player.last_damage_source = sourcePos;
    g_player.last_damage_time = g_engine.time_total;

    /* Damage direction relative to the view, so the HUD indicator can point at the shooter.
     * atan2 of (dot with right, dot with forward) gives a signed angle in the ground plane. */
    Vector3 toSrc = Vector3Subtract(sourcePos, g_player.pos);
    toSrc.y = 0.0f;
    if (Vector3LengthSqr(toSrc) > 0.01f)
    {
        toSrc = Vector3Scale(toSrc, 1.0f / Vector3Length(toSrc));
        float yawRad = g_player.yaw * BP_DEG2RAD;
        Vector3 fwd = vec3(sinf(yawRad), 0.0f, cosf(yawRad));
        Vector3 rgt = vec3(cosf(yawRad), 0.0f, -sinf(yawRad));
        g_player.last_damage_dir = atan2f(Vector3DotProduct(toSrc, rgt), Vector3DotProduct(toSrc, fwd));
    }

    bp_audio_play(absorbed > amount ? SFX_HIT_ARMOR : SFX_HURT,
                  BP_CLAMP(0.5f + (absorbed + amount) / 60.0f, 0.5f, 1.0f));
    bp_camera_add_roll((g_player.last_damage_dir > 0.0f ? -1.0f : 1.0f) *
                       BP_CLAMP((absorbed + amount) / 55.0f, 0.0f, 2.6f));
    bp_fx_shake(BP_CLAMP((absorbed + amount) / 90.0f, 0.05f, 0.4f), 0.14f);

    /* Getting shot should never let you keep perfect aim: a brief control penalty that
     * scales with the hit is what makes trading shots a decision rather than a freebie. */
    bp_camera_add_dip(BP_CLAMP(amount / 30.0f, 0.0f, 2.0f));

    if (g_player.health <= 0.0f)
    {
        g_player.health = 0.0f;
        g_player.state = PST_DYING;
        g_player.death_time = 0.0f;
        g_player.streak = 0;
        /* Death is the one moment the game is allowed to be loud: an explosion-grade sting
         * plus the low health loop stopping dead. Volume 1.0 is intentional here. */
        bp_audio_play(SFX_EXPLOSION, 1.0f);
        bp_fx_shake(0.9f, 0.9f);
        BP_LOGI("player: died at %.1fs (score %d, %d kills)",
                (double)g_player.time_alive, g_player.score, g_player.kills);
    }
}

void bp_player_impulse(Vector3 impulse)
{
    g_player.vel.x += impulse.x;
    g_player.vel.y += impulse.y;
    g_player.vel.z += impulse.z;
    /* Any vertical impulse means we are airborne; clearing on_ground stops the friction
     * code from scrubbing the launch in the same tick it happened. */
    if (impulse.y > 0.01f) g_player.on_ground = false;
    clamp_speed();
}

void bp_player_add_score(int amount)
{
    if (amount <= 0) return;
    /* Streak multiplier: consecutive kills inside PLAYER_STREAK_WINDOW compound up to 2x.
     * This is the scoring system's whole reason to exist -- it rewards aggressive play,
     * which is the behaviour a boomer shooter wants to encourage. */
    int mult = 1 + BP_MIN(g_player.streak, PLAYER_STREAK_MAX) / 4;
    g_player.score += amount * mult;
}

void bp_player_register_kill(int scoreValue)
{
    g_player.kills++;
    if (g_player.streak_time > 0.0f) g_player.streak++;
    else g_player.streak = 1;
    g_player.streak_time = PLAYER_STREAK_WINDOW;
    bp_player_add_score(scoreValue > 0 ? scoreValue : 100);
}

/* -------------------------------------------------------------------------------------- */
/* accessors                                                                               */
/* -------------------------------------------------------------------------------------- */
Vector3 bp_player_position(void) { return g_player.pos; }
Vector3 bp_player_eye(void)      { return vec3(g_player.pos.x, g_player.pos.y + player_eye_height(), g_player.pos.z); }
Vector3 bp_player_velocity(void) { return g_player.vel; }
bool    bp_player_alive(void)    { return g_player.state == PST_ALIVE; }
bool    bp_player_on_ground(void){ return g_player.on_ground; }

float bp_player_speed01(void)
{
    /* Normalised against the sprint cap, not the walk cap: the HUD and the FOV kick should
     * both saturate at the fastest the player can legitimately go. */
    return BP_CLAMP(hspeed(g_player.vel) / g_player.speed_sprint, 0.0f, 1.0f);
}

/* -------------------------------------------------------------------------------------- */
/* main update                                                                             */
/* -------------------------------------------------------------------------------------- */
void bp_player_update(float dt)
{
    if (!g_player.initialised || dt <= 0.0f) return;

    /* ---- timers that run in every state ---- */
    if (g_player.invuln_time > 0.0f)  g_player.invuln_time  -= dt;
    if (g_player.streak_time > 0.0f)  { g_player.streak_time -= dt; if (g_player.streak_time <= 0.0f) g_player.streak = 0; }
    g_player.damage_flash = BP_MAX(0.0f, g_player.damage_flash - dt * 2.6f);
    g_player.heal_flash   = BP_MAX(0.0f, g_player.heal_flash   - dt * 2.0f);
    g_player.land_impact  = BP_MAX(0.0f, g_player.land_impact  - dt * 4.0f);

    /* ---- look ---- */
    if (g_player.state == PST_ALIVE)
    {
        /* Sensitivity is applied inside the camera so mouse and gamepad share one curve. */
        bp_camera_look(bp_input_look_dx(), bp_input_look_dy(), g_input.sensitivity);
        g_player.yaw = g_camera.yaw;
    }

    /* ---- death / respawn sequencing ---- */
    if (g_player.state == PST_DYING)
    {
        g_player.death_time += dt;
        /* Bleed velocity so the corpse settles instead of sliding off the platform. */
        g_player.vel.x *= BP_MAX(0.0f, 1.0f - dt * 6.0f);
        g_player.vel.z *= BP_MAX(0.0f, 1.0f - dt * 6.0f);
        g_player.vel.y += GRAVITY * dt;
        g_player.pos = Vector3Add(g_player.pos, Vector3Scale(g_player.vel, dt));
        bool grounded = false;
        int mat = MAT_CONCRETE;
        bp_world_resolve_body(&g_player.pos, g_player.radius, player_height_now(),
                              &g_player.vel, &grounded, &mat);
        if (g_player.death_time >= PLAYER_DEATH_DURATION) g_player.state = PST_DEAD;
        bp_camera_simulate(dt, g_player.pos, 0.0f, grounded, false, 0.0f);
        return;
    }
    if (g_player.state != PST_ALIVE)
    {
        /* DEAD / RESPAWNING are owned by the game layer (it decides when to respawn us).
         * We still tick the camera so the death cam keeps drifting rather than freezing. */
        bp_camera_simulate(dt, g_player.pos, 0.0f, g_player.on_ground, g_player.crouched, 0.0f);
        return;
    }

    g_player.time_alive += dt;

    /* ---- crouch ---- */
    g_player.crouch_held = bp_down(ACT_CROUCH);
    if (g_player.crouch_held) g_player.crouch_amount = BP_MIN(1.0f, g_player.crouch_amount + dt * PLAYER_CROUCH_SPEED);
    else if (has_stand_headroom()) g_player.crouch_amount = BP_MAX(0.0f, g_player.crouch_amount - dt * PLAYER_CROUCH_SPEED);
    /* else: keep crouched -- releasing the key under a ledge must not stand us up into it */
    g_player.crouched = g_player.crouch_amount > 0.6f;
    g_player.height = player_height_now();

    /* ---- wish direction ---- */
    float inputStrength = 0.0f;
    bool sprinting = false;
    Vector3 wish = compute_wish(&inputStrength, &sprinting);

    float targetSpeed = g_player.crouched ? g_player.speed_crouch
                      : (sprinting ? g_player.speed_sprint : g_player.speed_ground);
    g_player.speed_current = targetSpeed;

    /* ---- jump ---- */
    if (g_player.coyote_time > 0.0f) g_player.coyote_time -= dt;
    if (g_player.jump_buffer > 0.0f) g_player.jump_buffer -= dt;

    if (bp_pressed(ACT_JUMP)) g_player.jump_buffer = PLAYER_JUMP_BUFFER;

    /* Variable jump height: releasing early cuts the upward velocity. Without this every
     * jump is the same height and platforming feels like a vending machine. */
    if (!bp_down(ACT_JUMP) && g_player.vel.y > 0.0f && g_player.jump_held)
        g_player.vel.y *= BP_MAX(0.0f, 1.0f - dt * 7.5f);
    g_player.jump_held = bp_down(ACT_JUMP);

    if (g_player.jump_buffer > 0.0f && (g_player.on_ground || g_player.coyote_time > 0.0f))
    {
        g_player.vel.y = g_player.jump_velocity;
        g_player.on_ground = false;
        g_player.coyote_time = 0.0f;
        g_player.jump_buffer = 0.0f;
        bp_audio_play(SFX_JUMP, 0.55f);
        /* Slight forward assist in the wish direction: jumping toward an enemy should feel
         * like a lunge, not like the movement system stopped listening for 0.2 s. */
        if (inputStrength > 0.1f)
        {
            g_player.vel.x += wish.x * targetSpeed * 0.18f;
            g_player.vel.z += wish.z * targetSpeed * 0.18f;
        }
        clamp_speed();
    }

    /* ---- horizontal movement ---- */
    bool wasGrounded = g_player.on_ground;
    if (g_player.on_ground)
    {
        apply_friction(wish, inputStrength, dt);
        accelerate(wish, targetSpeed * inputStrength, g_player.accel_ground, dt);
    }
    else
    {
        /* Air control uses a much smaller cap (PLAYER_SPEED_AIR) but the same projection
         * trick, which is what preserves momentum through a jump while still allowing a
         * mid-air correction. This asymmetry is the single most important line for feel. */
        accelerate(wish, g_player.speed_air * inputStrength, g_player.accel_air * 6.0f, dt);
    }
    clamp_speed();

    /* ---- gravity + integrate ---- */
    g_player.vel.y += GRAVITY * dt;
    if (g_player.vel.y < -60.0f) g_player.vel.y = -60.0f;    /* terminal velocity: no tunnel */

    Vector3 prev = g_player.pos;
    g_player.pos = Vector3Add(g_player.pos, Vector3Scale(g_player.vel, dt));

    /* ---- collide ---- */
    float impactSpeed = -g_player.vel.y;
    bool grounded = false;
    int groundMaterial = MAT_CONCRETE;
    bp_world_resolve_body(&g_player.pos, g_player.radius, g_player.height,
                          &g_player.vel, &grounded, &groundMaterial);

    /* The resolver zeroes velocity.y on landing, so capture the impact speed *before* it
     * ran -- which is why impactSpeed is sampled above from the pre-resolution value. */
    if (grounded && !wasGrounded && impactSpeed > 1.0f) on_land(impactSpeed, groundMaterial);
    g_player.on_ground = grounded;
    g_player.coyote_time = grounded ? PLAYER_COYOTE_TIME : g_player.coyote_time;
    if (!grounded) g_player.air_time += dt; else g_player.air_time = 0.0f;

    /* ---- speed bookkeeping ---- */
    float hs = hspeed(g_player.vel);
    g_player.speed_peak = BP_MAX(g_player.speed_peak, hs);
    if (-g_player.vel.y > g_player.fall_speed_peak) g_player.fall_speed_peak = -g_player.vel.y;

    /* Speed above the walk cap is the game's core expression, so it earns feedback: a
     * burst of streaking particles and (via bp_camera_simulate) a wider FOV. */
    if (hs > g_player.speed_sprint + 1.5f && grounded && bp_rng_int(&g_engine.rng, 3) == 0)
        bp_fx_speed_burst(vec3(g_player.pos.x, g_player.pos.y + 0.3f, g_player.pos.z),
                          vec3(-g_player.vel.x, 0.0f, -g_player.vel.z));

    /* ---- footsteps ---- */
    float travelled = Vector3Distance(prev, g_player.pos);
    /* Ignore the vertical component of travel: landing must not count as a step. */
    if (BP_ABS(g_player.pos.y - prev.y) > travelled * 0.5f) travelled *= 0.5f;
    update_footsteps(travelled, groundMaterial, dt);

    /* ---- hazard floors ---- */
    const bp_material_def *gmd = bp_world_material(groundMaterial);
    if (grounded && gmd && gmd->damage_to_player > 0.01f)
    {
        g_player.hazard_time += dt;
        if (g_player.hazard_time >= PLAYER_HAZARD_TICK)
        {
            g_player.hazard_time -= PLAYER_HAZARD_TICK;
            bp_player_damage(gmd->damage_to_player * PLAYER_HAZARD_TICK, g_player.pos, 0.0f);
            bp_fx_particle(PTX_EMBER, vec3(g_player.pos.x, g_player.pos.y + 0.1f, g_player.pos.z),
                           vec3(bp_rng_range(&g_engine.rng, -0.6f, 0.6f), 1.4f,
                                bp_rng_range(&g_engine.rng, -0.6f, 0.6f)),
                           0.4f, 0.09f, 0.0f, g_assets.pal_danger, (Color){ 232, 52, 60, 0 });
        }
    }
    else g_player.hazard_time = 0.0f;

    /* ---- low health warning ---- */
    if (g_player.health > 0.0f && g_player.health < PLAYER_LOW_HP_THRESH)
    {
        g_player.low_health_beep -= dt;
        if (g_player.low_health_beep <= 0.0f)
        {
            /* Beep faster as health drops: an audio-only urgency ramp that works even when
             * the player is not looking at the HUD. */
            g_player.low_health_beep = PLAYER_LOW_HP_INTERVAL * BP_CLAMP(g_player.health / PLAYER_LOW_HP_THRESH, 0.35f, 1.0f);
            bp_audio_play(SFX_LOW_HEALTH, 0.55f);
        }
    }
    else g_player.low_health_beep = 0.0f;

    /* ---- world interaction ---- */
    update_triggers();
    bp_pickup_update(dt);

    /* ---- camera ----
     * damageRoll pushes the view away from the last hit direction. Sub-2 degrees, under
     * 100 ms: enough to feel, never enough to disturb aim. */
    float damageRoll = g_player.damage_flash * 1.6f * (g_player.last_damage_dir > 0.0f ? -1.0f : 1.0f);
    bp_camera_simulate(dt, g_player.pos, bp_player_speed01(), g_player.on_ground,
                       g_player.crouched, damageRoll);
}

/* -------------------------------------------------------------------------------------- */
/* debug                                                                                   */
/* -------------------------------------------------------------------------------------- */
void bp_player_draw_debug(void)
{
    float h = player_height_now();
    bp_aabb body = bp_aabb_make(
        vec3(g_player.pos.x - g_player.radius, g_player.pos.y, g_player.pos.z - g_player.radius),
        vec3(g_player.pos.x + g_player.radius, g_player.pos.y + h, g_player.pos.z + g_player.radius));

    Color boxCol = g_player.on_ground ? (Color){ 92, 226, 196, 200 } : (Color){ 240, 208, 92, 200 };
    bp_gfx_draw_aabb(body.min, body.max, boxCol);

    /* eye position + view cone */
    Vector3 eye = bp_player_eye();
    DrawSphere(eye, 0.05f, MAGENTA);
    bp_gfx_draw_ray(eye, Vector3Add(eye, Vector3Scale(bp_camera_forward(), 3.0f)), (Color){ 255, 138, 32, 220 });
    bp_gfx_draw_ray(eye, Vector3Add(eye, Vector3Scale(bp_camera_right(), 1.2f)), (Color){ 92, 226, 196, 180 });

    /* velocity vector, scaled so 1 m = 1 m/s */
    bp_gfx_draw_ray(vec3(g_player.pos.x, g_player.pos.y + 0.1f, g_player.pos.z),
                    Vector3Add(vec3(g_player.pos.x, g_player.pos.y + 0.1f, g_player.pos.z), g_player.vel),
                    (Color){ 236, 238, 242, 200 });

    /* pickup volumes */
    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        const bp_pickup *p = &g_player.pickups[i];
        if (!p->alive) continue;
        bp_aabb pb = bp_aabb_make(vec3(p->pos.x - PICKUP_RADIUS, p->pos.y, p->pos.z - PICKUP_RADIUS),
                                  vec3(p->pos.x + PICKUP_RADIUS, p->pos.y + PICKUP_HEIGHT_TOL, p->pos.z + PICKUP_RADIUS));
        bp_gfx_draw_aabb(pb.min, pb.max, (Color){ 86, 214, 110, 90 });
    }
}
