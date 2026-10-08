/******************************************************************************************
 * bp_weapons.h -- the weapon bank: stats, fire/reload state machine, hitscan, projectiles
 *                 and the first person viewmodel.
 *
 * DATA DRIVEN BY DESIGN
 *   Every number a designer would want to tune lives in assets/cfg/weapons.cfg. The compiled
 *   in table in bp_weapons.c is the *fallback* used when the file is missing, so the game
 *   never fails to boot because an asset did not ship. Rebalancing a weapon is a text edit
 *   and a page reload in the web build -- no rebuild.
 *
 * WHY BOTH HITSCAN AND PROJECTILES
 *   Hitscan is the correct model for fast weapons: the player's intent at the instant of the
 *   click is what matters, and any travel time reads as input lag. Projectiles are the
 *   correct model for the rocket launcher: leading a moving target is a skill, and splash
 *   damage needs a position to arrive at. The bank therefore contains both, and the combat
 *   resolution is shared (see bp_hitdetect.h) so the two can never disagree about a hit.
 ******************************************************************************************/
#ifndef BP_WEAPONS_H
#define BP_WEAPONS_H

#include "bp_common.h"
#include "../engine/bp_assets.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_camera.h"
#include "bp_hitdetect.h"

#define WPN_MAX_SLOTS 5

typedef enum {
    WPN_ID_PISTOL = 0,
    WPN_ID_SHOTGUN,
    WPN_ID_SMG,
    WPN_ID_RAIL,
    WPN_ID_ROCKET,
    WPN_ID_COUNT
} bp_weapon_id;

typedef enum {
    WCATEGORY_HITSCAN = 0,
    WCATEGORY_PROJECTILE,
    WCATEGORY_BEAM
} bp_weapon_category;

typedef struct {
    bp_weapon_id     id;
    char             cfg_id[16];
    char             display_name[32];
    int              slot;
    bp_weapon_category category;

    float            damage;
    int              pellets;
    float            spread_hip;
    float            spread_ads;
    float            spread_move;
    float            fire_rate;
    bool             full_auto;
    int              mag_size;
    int              reserve_max;
    float            reload_time;
    float            swap_time;

    float            recoil_pitch;
    float            recoil_yaw;
    float            recoil_kick;
    float            shake;

    bp_sfx_id        sfx_fire;
    bp_sfx_id        sfx_reload;
    float            tracer_width;
    float            tracer_life;
    float            range;
    float            knockback;
    int              penetration;
    int              ammo_per_pickup;
    float            headshot_mult;
    Color            color;

    /* projectile only */
    float            proj_speed;
    float            proj_radius;
    float            proj_gravity;
    float            blast_radius;
    float            blast_damage;
    float            blast_self;
    float            blast_knockback;

    /* viewmodel */
    bp_model_id      vm_body;
    bp_model_id      vm_part_b;
    bp_model_id      vm_part_c;
    Vector3          vm_offset;
    Vector3          vm_scale;
    Vector3          vm_muzzle;      /* muzzle position in viewmodel space */
} bp_weapon_def;

typedef struct {
    Vector3  pos;
    Vector3  vel;
    float    age;
    float    life;
    float    damage;
    float    radius;         /* blast radius */
    float    body_radius;
    float    gravity;
    float    knockback;
    float    self_scale;
    bool     from_player;
    bool     alive;
    bool     trail;
    Color    color;
    bp_model_id model;
    bp_handle owner;
} bp_projectile;

typedef enum {
    WSTATE_IDLE = 0,
    WSTATE_FIRING,
    WSTATE_RELOADING,
    WSTATE_SWITCHING_OUT,
    WSTATE_SWITCHING_IN,
    WSTATE_EMPTY
} bp_weapon_state;

typedef struct {
    bp_weapon_def def[WPN_ID_COUNT];
    bool          def_loaded;

    bool          owned[WPN_ID_COUNT];
    int           mag[WPN_ID_COUNT];
    int           reserve[WPN_ID_COUNT];

    bp_weapon_id  current;
    bp_weapon_id  pending;          /* weapon being switched to */
    bp_weapon_state state;
    float         state_time;
    float         cooldown;
    float         reload_progress;
    float         switch_progress;

    /* feel */
    float         spread_current;
    float         recoil_viewmodel;
    float         ads;              /* 0 hip .. 1 aimed */
    bool          ads_held;
    float         sway_x, sway_y;
    float         bob_phase;
    float         dry_fire_time;

    /* stats */
    int           shots_fired;
    int           shots_hit;
    int           headshots;
    float         damage_dealt;

    bp_projectile projectiles[MAX_PROJECTILES];
    int           projectile_count;

    bool          initialised;
} bp_weapon_sys;

extern bp_weapon_sys g_weapons;
extern const bp_weapon_def *g_weapon_table;

void  bp_weapons_init(void);
bool  bp_weapons_load_config(const char *path);
void  bp_weapons_reset_loadout(bool fullArsenal);

void  bp_weapons_update(float dt, Camera3D cam, Vector3 velocity, bool onGround);
void  bp_weapons_try_fire(bool triggerHeld, bool triggerPressed);
void  bp_weapons_try_reload(void);
bool  bp_weapons_try_select(bp_weapon_id id);
void  bp_weapons_cycle(int direction);

void  bp_weapons_give(bp_weapon_id id, int ammo, bool switchTo);
void  bp_weapons_add_ammo(bp_weapon_id id, int amount);
void  bp_weapons_add_reserve_all(int amount);

void  bp_weapons_draw_viewmodel(Camera3D cam, float speed01);
void  bp_weapons_draw_projectiles(Camera3D cam);

/* current-state queries used by the HUD and the AI (they need to know if the player is
 * mid-reload to decide whether to push) */
const bp_weapon_def *bp_weapons_current_def(void);
bool   bp_weapons_busy(void);
float  bp_weapons_reload_fraction(void);
bool   bp_weapons_can_fire(void);
int    bp_weapons_total_reserve(void);
float  bp_weapons_accuracy(void);
bool   bp_weapons_owns(bp_weapon_id id);

/* projectiles can also be spawned by enemies */
void   bp_projectile_spawn(Vector3 pos, Vector3 dir, float speed, float damage,
                           float radius, float blastRadius, bool fromPlayer, Color color,
                           bp_model_id model, bp_handle owner);
void   bp_projectile_explode(bp_projectile *p, Vector3 at);

#endif /* BP_WEAPONS_H */
