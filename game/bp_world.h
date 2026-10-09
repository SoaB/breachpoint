/******************************************************************************************
 * bp_world.h -- 關卡資料、表面材質、碰撞與世界繪製。
 *
 * 碰撞
 *   一組軸對齊（axis-aligned）的實心方塊，外加一個「先用距離就直接排除」的早退檢查。沒有
 *   BSP、沒有 octree、沒有 navmesh。競技場地圖大約 60–160 個方塊；把整張地圖掃過一遍只要
 *   個位數微秒，於是每幀的預算留給了玩法與繪製，而不是花在走訪資料結構上。一切都是 AABB，
 *   這也意味著命中判定在桌機 OpenGL 與 WebGL 上是逐位元一致的 —— 玩家永遠猜不出自己跑的
 *   是哪個版本。
 *
 * 繪製
 *   靜態幾何在載入關卡時，依材質合併成「每種材質一個」mesh，每個面的 UV 再依世界尺寸縮放。
 *   這讓所有表面都有恆定的 texel 密度（業餘關卡幾何最容易露餡之處），整張地圖只要約 14 個
 *   draw call。動態幾何（門、升降機、會爆炸的油桶）則逐方塊繪製。
 *
 * 材質
 *   這是資料，不是程式碼。方塊只攜帶一個材質索引，由表格決定貼圖、受擊特效、腳步聲、摩擦
 *   與能否被射穿。要新增「濕水泥」這種材質，只需加一列表格資料。
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
/* 材質                                                                                       */
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
    int           impact_sfx;      /* 以 int 存放的 bp_sfx_id：讓 audio 標頭檔不必出現在這裡 */
    int           step_sfx;
    float         uv_scale;        /* 一整次貼圖重複所佔的世界單位數                   */
    float         friction;        /* 1.0 = 一般地面                                   */
    float         hardness;        /* 0..1，用來縮放碎片與火花的數量                    */
    bool          penetrable;      /* 子彈與 hitscan 可直接穿過                        */
    bool          emissive;        /* 不受光照、直接繪製（unlit）                       */
    bool          decals_allowed;
    float         damage_to_player;/* 站在其上時每秒造成的傷害（危險地板）               */
    Color         tint;
} bp_material_def;

/* -------------------------------------------------------------------------------------- */
/* 幾何                                                                                       */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    bp_aabb box;
    int     material;
    bool    dynamic;       /* 每幀重新繪製，不會被烘進靜態 mesh */
    bool    shoot_through; /* hitscan 可穿過（鐵格板、力場），但軀體不行 */
    bool    explode;       /* 油桶：可被傷害摧毀，並波及附近 */
    float   health;        /* 只在 explode == true 時才會用到 */
    bool    destroyed;
} bp_solid;

typedef struct {
    bp_solid *solid;       /* 指向 g_world.solids 內部的元素 */
    Vector3   from;
    Vector3   to;
    float     duration;
    float     phase;       /* 0 = 全關 .. 1 = 全開 */
    bool      open;
    bool      triggered;
    float     cooldown;
    float     hold;          /* 完全打開後停留幾秒才自動闔上 */
    float     auto_close;    /* 0 = 保持開啟，直到再次被觸發 */
    int       sfx;
} bp_mover;

/* -------------------------------------------------------------------------------------- */
/* 觸發器與出生點                                                                             */
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
    int     kind;          /* bp_trigger_kind 列舉值 */
    Vector3 position;
    float   yaw;
    int     param;         /* 武器 id / 拾取數量 / mover 索引 */
    float   respawn;       /* 幾秒；0 表示只觸發一次 */
    float   timer;
    bool    consumed;
    float   radius;
} bp_trigger;

/* -------------------------------------------------------------------------------------- */
/* 烘焙的靜態 mesh                                                                            */
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
/* 世界                                                                                       */
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
/* 關卡表                                                                                     */
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
/* 對外介面（API）                                                                            */
/* -------------------------------------------------------------------------------------- */
void  bp_world_init(void);
void  bp_world_shutdown(void);
bool  bp_world_load(int levelIndex);
void  bp_world_unload(void);
void  bp_world_update(float dt);

/* 碰撞 */
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

/* 觸發器 */
int     bp_world_next_spawn(int kind, int *cursor);
Vector3 bp_world_random_spawn(int kind, Vector3 avoid, float min_distance);
int     bp_world_spawn_count(int kind);
Vector3 bp_world_player_start(float *out_yaw);
void    bp_world_activate_doors(Vector3 pos, float radius);

/* 材質 */
const bp_material_def *bp_world_material(int index);

/* 繪製 */
void bp_world_draw_sky(Camera3D cam);
void bp_world_draw(Camera3D cam, Vector3 viewer);
void bp_world_draw_debug(void);

#endif /* BP_WORLD_H */
