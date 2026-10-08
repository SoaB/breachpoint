/******************************************************************************************
 * bp_player.h -- the player body: movement, health/armour, pickups, and the game's feel.
 *
 * MOVEMENT MODEL
 *   Acceleration-based with separate ground and air constants, plus friction that is applied
 *   only to the component of velocity *opposing* the input. Consequences the player feels:
 *     - releasing keys stops you fast (friction), but you keep momentum through a turn
 *     - air control is limited but real, so a mid-air correction is possible but cannot
 *       reverse a jump
 *     - strafing in a circle builds speed above the straight-line cap (the classic
 *       "circle-strafe is faster" property that makes arena movement a skill). We allow a
 *       hard speed cap far above the ground cap so this is expressive, not broken.
 *
 *   Crouch is a height change plus a speed penalty, NOT a separate collision shape. One
 *   shape means one resolution path means no crouch-jump-through-ceiling exploits.
 ******************************************************************************************/
#ifndef BP_PLAYER_H
#define BP_PLAYER_H

#include "bp_common.h"
#include "../engine/bp_entity.h"

typedef enum {
    PICKUP_NONE = 0,
    PICKUP_HEALTH,
    PICKUP_ARMOR,
    PICKUP_AMMO,
    PICKUP_WEAPON
} bp_pickup_kind;

typedef struct {
    Vector3       pos;
    bp_pickup_kind kind;
    float         amount;
    int           weapon_id;
    float         age;
    float         respawn_at;
    bool          alive;
    bool          spinning;
    float         bob;
} bp_pickup;

typedef enum {
    PST_ALIVE = 0,
    PST_DYING,
    PST_DEAD,
    PST_RESPAWNING
} bp_player_state;

typedef struct {
    /* body */
    Vector3 pos;          /* feet */
    Vector3 vel;
    float   yaw;
    bool    on_ground;
    bool    crouched;
    bool    crouch_held;
    float   crouch_amount;   /* 0 standing .. 1 crouched, drives eye height + hitbox */
    float   height;
    float   radius;

    /* vitals */
    float   health;
    float   armor;
    float   max_health;
    float   max_armor;
    bp_player_state state;
    float   death_time;
    float   invuln_time;
    float   hazard_time;

    /* movement feel */
    float   speed_ground;
    float   speed_sprint;
    float   speed_crouch;
    float   speed_air;
    float   accel_ground;
    float   accel_air;
    float   friction_ground;
    float   jump_velocity;
    float   coyote_time;
    float   jump_buffer;
    bool    jump_held;
    float   step_phase;
    float   land_impact;
    float   air_time;
    float   speed_current;
    float   speed_peak;

    /* feedback */
    float   damage_flash;
    float   heal_flash;
    float   last_damage_dir;     /* radians relative to view yaw, for the damage indicator */
    float   last_damage_time;
    Vector3 last_damage_source;
    float   low_health_beep;

    /* score */
    int     score;
    int     kills;
    int     streak;
    float   streak_time;
    int     secrets_found;
    int     pickups_taken;
    float   time_alive;

    /* level progression. The player controller owns trigger contact, so it is the one
     * place that can legitimately raise these; the game layer polls them and decides
     * what a level exit or a secret actually means. Both are sticky per level. */
    bool    exit_reached;
    float   exit_time;          /* g_engine.time_total when the exit was touched */
    bool    secret_reached;
    float   fall_speed_peak;    /* fastest downward speed this level, for the stats screen */

    /* pickups */
    bp_pickup pickups[MAX_PICKUPS];
    int       pickup_count;

    bool      initialised;
} bp_player;

extern bp_player g_player;

void    bp_player_init(void);
void    bp_player_reset(Vector3 start, float yaw);
void    bp_player_update(float dt);
void    bp_player_draw_debug(void);

void    bp_player_damage(float amount, Vector3 sourcePos, float armorAbsorb);
void    bp_player_heal(float amount);
void    bp_player_add_armor(float amount);
void    bp_player_impulse(Vector3 impulse);
Vector3 bp_player_position(void);
Vector3 bp_player_eye(void);
Vector3 bp_player_velocity(void);
bool    bp_player_alive(void);
float   bp_player_speed01(void);
bool    bp_player_on_ground(void);

/* pickups */
void    bp_pickup_spawn(bp_pickup_kind kind, Vector3 pos, float amount, int weaponId);
void    bp_pickup_update(float dt);
void    bp_pickup_draw(Camera3D cam, Vector3 viewer);
void    bp_pickup_clear(void);

void    bp_player_add_score(int amount);
void    bp_player_register_kill(int scoreValue);

#endif /* BP_PLAYER_H */
