/******************************************************************************************
 * bp_enemies.h -- enemy archetypes, AI state machine, animation and hitboxes.
 *
 * AI ARCHITECTURE
 *   A finite state machine over six states (DORMANT / IDLE / PURSUE / ATTACK / STAGGER /
 *   DYING) plus a "desire" layer that picks strafing direction. No behaviour trees, no
 *   GOAP, no utility scores. For an arena shooter the correct AI is *legible*: the player
 *   must be able to look at an enemy and know what it is about to do within 150 ms. Every
 *   state has a tell (sound, pose, colour) and every transition has a minimum dwell time so
 *   enemies never strobe between two behaviours.
 *
 *   Pathing is steering, not pathfinding: seek + obstacle avoidance + separation. On an
 *   arena map with sparse convex cover, a well-tuned wall-follow avoid beats an A* grid in
 *   both CPU cost and perceived intelligence, because it never produces the "stuck in a
 *   doorway" failure mode that players notice immediately.
 *
 * HITBOXES
 *   A body cylinder (as an AABB) plus a smaller head box on top. Headshots exist because a
 *   game with no precision reward trains players to spray. The head box is generous (0.62x
 *   the body radius) because punishing near-misses feels worse than rewarding them.
 ******************************************************************************************/
#ifndef BP_ENEMIES_H
#define BP_ENEMIES_H

#include "bp_common.h"
#include "../engine/bp_entity.h"
#include "../engine/bp_assets.h"
#include "../engine/bp_math.h"

typedef enum {
    ET_GRUNT = 0,
    ET_HOUND,
    ET_BRUTE,
    ET_TURRET,
    ET_COUNT
} bp_enemy_type;

typedef enum {
    ES_DORMANT = 0,     /* spawned but not yet activated */
    ES_IDLE,            /* patrolling / looking for the player */
    ES_PURSUE,          /* has LOS or heard the player, closing in */
    ES_ATTACK,          /* in range, attacking */
    ES_STAGGER,         /* pain reaction, brief */
    ES_DYING,           /* death animation, no collision */
    ES_DEAD
} bp_enemy_state;

#define ENEMY_MAX_ANIM_PARTS 6

typedef struct {
    char    cfg_id[16];
    char    display_name[32];
    float   health;
    float   armor;
    float   speed_walk;
    float   speed_run;
    float   radius;
    float   height;
    float   scale;
    float   sight_range;
    float   hear_range;
    float   attack_range;
    float   attack_damage;
    float   attack_cooldown;
    int     attack_sfx;
    float   pain_chance;
    int     score;
    float   drop_health;
    float   drop_ammo;
    float   drop_armor;
    Color   color_body;
    Color   color_trim;
    Color   eye_color;
    float   strafe;
    float   aggression;
    bool    projectile;
    float   proj_speed;
    float   proj_radius;
    float   proj_damage;
    bool    is_static;
    float   knockback_scale;
    int     body_set;         /* 0 humanoid, 1 hound, 2 brute, 3 turret */
    int     sfx_sight, sfx_pain, sfx_die;
} bp_enemy_def;

typedef struct {
    bp_enemy_type type;
    bp_enemy_state state;
    float   state_time;

    Vector3 pos;          /* feet position */
    Vector3 vel;
    float   yaw;          /* degrees, facing */
    bool    on_ground;

    float   health;
    float   armor;
    float   max_health;

    float   attack_timer;
    float   stagger_timer;
    float   pain_scale;     /* 0 on the hardest difficulty: no flinch, by design */
    float   die_timer;
    float   pain_flash;
    float   activation_delay;

    /* perception */
    bool    can_see;
    float   last_known_dist;
    Vector3 last_known_pos;
    float   memory_time;
    bool    alerted_others;

    /* steering */
    float   strafe_dir;
    float   strafe_timer;
    float   avoid_timer;
    Vector3 avoid_dir;
    float   wander_yaw;

    /* animation */
    float   anim_time;
    float   leg_phase;
    float   arm_phase;
    float   bob;
    float   attack_wind;    /* 0..1 telegraph */
    float   lunge;

    /* bookkeeping */
    float   damage_dealt_to_player;
    int     hits_taken;
    bool    counted_for_wave;
    bool    dropped_pickup;
} bp_enemy;

typedef struct {
    bp_enemy_def def[ET_COUNT];
    bool         def_loaded;

    bp_pool      pool;
    bp_enemy     storage[MAX_ENEMIES];
    uint32_t     generations[MAX_ENEMIES];
    uint8_t      alive_flags[MAX_ENEMIES];
    uint32_t     free_stack[MAX_ENEMIES];
    uint32_t     iter_list[MAX_ENEMIES];

    int          alive_count;
    int          killed_total;
    int          spawned_total;
    float        damage_received_total;

    bool         initialised;
} bp_enemy_sys;

extern bp_enemy_sys g_enemies;
extern const bp_enemy_def *g_enemy_table;

void   bp_enemies_init(void);
bool   bp_enemies_load_config(const char *path);
void   bp_enemies_clear(void);

bp_handle bp_enemy_spawn(bp_enemy_type type, Vector3 pos, float yaw, bool dormant);
void   bp_enemy_update(float dt, Vector3 playerPos, Vector3 playerEye, bool playerAlive);
void   bp_enemy_draw(Camera3D cam, Vector3 viewer);
void   bp_enemy_draw_debug(void);

/* damage entry point used by bp_hitdetect */
float  bp_enemy_take_damage(bp_handle h, float amount, Vector3 point, Vector3 dir,
                            bool headshot, float knockback, bool *out_killed);

/* queries */
bp_enemy *bp_enemy_get(bp_handle h);
int       bp_enemy_alive_count(void);
int       bp_enemy_count_type(bp_enemy_type t);
bool      bp_enemy_head_box(bp_enemy *e, bp_aabb *out);
bool      bp_enemy_body_box(bp_enemy *e, bp_aabb *out);
Vector3   bp_enemy_center(bp_enemy *e);
bp_enemy_type bp_enemy_type_from_cfg(const char *id);
float     bp_enemy_nearest_distance(Vector3 from, bp_handle *out);
void      bp_enemy_alert_nearby(Vector3 pos, float radius);

#endif /* BP_ENEMIES_H */
