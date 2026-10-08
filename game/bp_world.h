/******************************************************************************************
 * bp_world.h -- level data, surface materials, collision and world rendering.
 *
 * COLLISION
 *   An array of axis-aligned solid boxes plus an early-out distance reject. No BSP, no
 *   octree, no navmesh. Arena maps are 60-160 boxes; a full sweep costs single-digit
 *   microseconds, so the frame budget goes to gameplay and rendering instead of traversal.
 *   Everything is AABB, which also means hit registration is bit-identical on desktop GL
 *   and WebGL -- players can never tell which build they are on.
 *
 * RENDERING
 *   Static geometry is merged at level load into ONE mesh per material, with per-face UVs
 *   scaled to world size. That gives a constant texel density across every surface (the
 *   single biggest tell of amateur level geometry) at ~14 draw calls for the whole map.
 *   Dynamic geometry (doors, lifts, exploding barrels) is drawn per box.
 *
 * MATERIALS
 *   Data, not code. A box carries a material index; the table decides texture, impact FX,
 *   footstep sound, friction and penetrability. Adding "wet concrete" is a table row.
 ******************************************************************************************/
#ifndef BP_WORLD_H
#define BP_WORLD_H

#include "bp_common.h"
#include "../engine/bp_assets.h"
#include "../engine/bp_math.h"
#include "../engine/bp_audio.h"

#define MAX_SOLIDS    512
#define MAX_TRIGGERS  192
#define MAX_MOVERS     32

/* -------------------------------------------------------------------------------------- */
/* materials                                                                               */
/* -------------------------------------------------------------------------------------- */
typedef enum {
    MAT_CONCRETE = 0,
    MAT_BRICK,
    MAT_METAL,
    MAT_PANEL,
    MAT_RUST,
    MAT_GRATE,
    MAT_TILE,
    MAT_HAZARD,
    MAT_CRATE,
    MAT_BARREL,
    MAT_TRIM,
    MAT_PILLAR,
    MAT_LIGHT,
    MAT_SCREEN,
    MAT_VENT,
    MAT_PIPE,
    MAT_FORCEFIELD,
    MAT_COUNT
} bp_material;

typedef struct {
    const char   *name;
    bp_texture_id texture;
    int           impact_sfx;      /* bp_sfx_id as int: keeps audio out of this header */
    int           step_sfx;
    float         uv_scale;        /* world units per full texture repeat              */
    float         friction;        /* 1.0 = normal ground                              */
    float         hardness;        /* 0..1, scales debris + spark counts               */
    bool          penetrable;      /* projectiles/hitscan pass through                 */
    bool          emissive;        /* drawn unlit                                      */
    bool          decals_allowed;
    float         damage_to_player;/* per second while standing in it (hazard floors)  */
    Color         tint;
} bp_material_def;

/* -------------------------------------------------------------------------------------- */
/* geometry                                                                                */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    bp_aabb box;
    int     material;
    bool    dynamic;       /* re-drawn every frame, not baked into the static mesh */
    bool    shoot_through; /* hitscan passes (grate, forcefield) but bodies do not */
    bool    explode;       /* barrel: destroyed by damage, hurts nearby */
    float   health;        /* only used when explode == true */
    bool    destroyed;
} bp_solid;

typedef struct {
    bp_solid *solid;       /* points into g_world.solids */
    Vector3   from;
    Vector3   to;
    float     duration;
    float     phase;       /* 0 closed .. 1 open */
    bool      open;
    bool      triggered;
    float     cooldown;
    float     hold;          /* seconds spent fully open before auto-closing */
    float     auto_close;    /* 0 = stays open until re-triggered */
    int       sfx;
} bp_mover;

/* -------------------------------------------------------------------------------------- */
/* triggers / spawn points                                                                 */
/* -------------------------------------------------------------------------------------- */
typedef enum {
    TRIG_PLAYER_START = 0,
    TRIG_ENEMY_SPAWN,
    TRIG_HEALTH_SPAWN,
    TRIG_ARMOR_SPAWN,
    TRIG_AMMO_SPAWN,
    TRIG_WEAPON_SPAWN,
    TRIG_EXIT,
    TRIG_SECRET,
    TRIG_DOOR,
    TRIG_COUNT
} bp_trigger_kind;

typedef struct {
    int     kind;          /* bp_trigger_kind */
    Vector3 position;
    float   yaw;
    int     param;         /* weapon id / pickup amount / mover index */
    float   respawn;       /* seconds; 0 = one shot */
    float   timer;
    bool    consumed;
    float   radius;
} bp_trigger;

/* -------------------------------------------------------------------------------------- */
/* baked static mesh                                                                       */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    Mesh      mesh;
    Model     model;
    Material  material;
    int       material_index;
    int       box_count;
    int       tri_count;
    bool      valid;
} bp_static_batch;

/* -------------------------------------------------------------------------------------- */
/* world                                                                                   */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    char    id[32];
    char    name[48];
    char    subtitle[72];
    int     level_index;

    bp_solid   solids[MAX_SOLIDS];
    int        solid_count;

    bp_trigger triggers[MAX_TRIGGERS];
    int        trigger_count;

    bp_mover   movers[MAX_MOVERS];
    int        mover_count;

    bp_static_batch batch[MAT_COUNT];
    int        batch_count;

    Vector3    bounds_min;
    Vector3    bounds_max;
    Vector3    player_start;
    float      player_start_yaw;

    Color      ambient;
    Color      fog_color;
    float      fog_density;
    float      sky_yaw;

    float      build_ms;
    int        spawn_enemy_count;
    bool       loaded;
} bp_world;

extern bp_world g_world;

/* -------------------------------------------------------------------------------------- */
/* level table                                                                             */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    const char *id;
    const char *display_name;
    const char *subtitle;
    int         waves;
    float       par_time;
    int         difficulty;
} bp_level_def;

extern const bp_level_def g_levels[];
extern const int g_level_count;

/* -------------------------------------------------------------------------------------- */
/* API                                                                                     */
/* -------------------------------------------------------------------------------------- */
void  bp_world_init(void);
void  bp_world_shutdown(void);
bool  bp_world_load(int levelIndex);
void  bp_world_unload(void);
void  bp_world_update(float dt);

/* collision */
bool  bp_world_sweep(Vector3 from, Vector3 to, float radius,
                     float *out_t, Vector3 *out_normal, const bp_solid **out_solid);
bool  bp_world_resolve_body(Vector3 *feet, float radius, float height,
                            Vector3 *velocity, bool *on_ground, int *out_material);
float bp_world_floor_height(Vector3 pos, float radius, float max_y);
bool  bp_world_point_solid(Vector3 p);
bool  bp_world_line_of_sight(Vector3 from, Vector3 to);
int   bp_world_material_at_point(Vector3 pos);
int   bp_world_count_solids(void);
const bp_solid *bp_world_solid(int index);

/* triggers */
int     bp_world_next_spawn(int kind, int *cursor);
Vector3 bp_world_random_spawn(int kind, Vector3 avoid, float min_distance);
int     bp_world_spawn_count(int kind);
Vector3 bp_world_player_start(float *out_yaw);
void    bp_world_activate_doors(Vector3 pos, float radius);

/* materials */
const bp_material_def *bp_world_material(int index);

/* rendering */
void bp_world_draw_sky(Camera3D cam);
void bp_world_draw(Camera3D cam, Vector3 viewer);
void bp_world_draw_debug(void);

#endif /* BP_WORLD_H */
