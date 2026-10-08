/******************************************************************************************
 * bp_hitdetect.h -- the single source of truth for "what did this shot hit".
 *
 * Every damage path in the game (hitscan pellets, projectiles, explosions, enemy melee,
 * barrels, the railgun's pierce) goes through this module. That is a deliberate constraint:
 * hit registration is the one thing in a shooter that must never behave differently between
 * two code paths, or players will find and abuse the inconsistency.
 *
 * Order of resolution matters and is fixed:
 *   1. enemy hitboxes  (cylinder -> AABB approximation, head box checked first)
 *   2. destructibles   (barrels)
 *   3. world geometry  (skips shoot-through surfaces unless asked not to)
 * The nearest hit across all three wins. Head-before-body is what makes headshot multipliers
 * feel fair even when the head box is inside the body box.
 ******************************************************************************************/
#ifndef BP_HITDETECT_H
#define BP_HITDETECT_H

#include "bp_common.h"
#include "../engine/bp_entity.h"

typedef enum {
    HIT_NONE = 0,
    HIT_WORLD,
    HIT_ENEMY,
    HIT_BARREL,
    HIT_PICKUP
} bp_hit_kind;

typedef struct {
    bp_hit_kind kind;
    float       distance;
    Vector3     point;
    Vector3     normal;
    int         material;        /* world only */
    bp_handle   entity;          /* enemy / barrel / pickup */
    bool        headshot;
    int         solid_index;     /* world only, for decals and destruction */
} bp_hit;

typedef struct {
    bool ignore_world;
    bool ignore_enemies;
    bool ignore_barrels;
    bool ignore_pickups;
    bool penetrate_shoot_through;
    bp_handle ignore_entity;     /* so an enemy's own projectile cannot hit itself */
    float   max_distance;
} bp_trace_filter;

bp_trace_filter bp_filter_default(void);
bp_trace_filter bp_filter_projectile(bp_handle owner);

/* Sphere-swept trace. `radius` matters: a rocket has a body, a bullet does not. */
bp_hit bp_trace_ray(Vector3 origin, Vector3 dir, float maxDistance, const bp_trace_filter *filter);
bp_hit bp_trace_sphere(Vector3 origin, Vector3 dir, float radius, float maxDistance,
                       const bp_trace_filter *filter);

/* Point queries used by AI and pickups. */
bool  bp_point_hits_enemy(Vector3 p);
bool  bp_sphere_hits_world(Vector3 p, float radius);

/* Damage application. All combat goes through these two so knockback, hit markers, blood
 * direction, score and sound stay consistent. */
typedef struct {
    float   amount;
    Vector3 point;
    Vector3 direction;
    float   knockback;
    bool    from_player;
    bp_handle source;
    bool    explosive;
    float   headshot_multiplier;
} bp_damage_info;

float bp_apply_damage(bp_handle target, const bp_damage_info *info, bool *out_killed);

/* Destructible world geometry (barrels). Returns true if it detonated. */
bool  bp_barrel_damage(int solidIndex, float amount);

/* Radial damage with line-of-sight checks so hiding behind cover actually works. */
int   bp_apply_explosion(Vector3 center, float radius, float maxDamage, float knockback,
                         bool damagesPlayer, Vector3 playerPos, float *playerDamageOut);

#endif /* BP_HITDETECT_H */
