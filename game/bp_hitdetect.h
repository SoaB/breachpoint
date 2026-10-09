/******************************************************************************************
 * bp_hitdetect.h -- 「這一槍到底打中了什麼」的唯一事實來源（single source of truth）。
 *
 * 遊戲中每一條傷害路徑（hitscan 彈丸、投射物、爆炸、敵人近戰、油桶、軌道炮的貫穿）
 * 都會經過這個模組。這是刻意的約束：命中判定是射擊遊戲裡唯一絕不容許在兩條程式
 * 路徑之間有不同行為的東西，否則玩家遲早會找到這種不一致並加以濫用。
 *
 * 解析順序至關重要，而且固定不變：
 *   1. 敵人命中框（圓柱 -> AABB 近似；頭部框優先檢查）
 *   2. 可破壞物（油桶）
 *   3. 世界幾何（除非特別要求，否則略過可射穿的表面）
 * 三類命中之中距離最近者勝出。「先判定頭部、再判定身體」正是爆頭倍率吃起來依然
 * 公平的原因——即使頭部框就位於身體框的內部。
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
    int         material;        /* 僅世界命中時有效 */
    bp_handle   entity;          /* 敵人／油桶／拾取物 */
    bool        headshot;
    int         solid_index;     /* 僅世界命中時有效：供貼花（decal）與破壞使用 */
} bp_hit;

typedef struct {
    bool ignore_world;
    bool ignore_enemies;
    bool ignore_barrels;
    bool ignore_pickups;
    bool penetrate_shoot_through;
    bp_handle ignore_entity;     /* 讓敵人自己的投射物不會打中自己 */
    float   max_distance;
} bp_trace_filter;

bp_trace_filter bp_filter_default(void);
bp_trace_filter bp_filter_projectile(bp_handle owner);

/* 球體掃掠（sphere-swept）追蹤。`radius` 很重要：火箭有彈體，子彈沒有。 */
bp_hit bp_trace_ray(Vector3 origin, Vector3 dir, float maxDistance, const bp_trace_filter *filter);
bp_hit bp_trace_sphere(Vector3 origin, Vector3 dir, float radius, float maxDistance,
                       const bp_trace_filter *filter);

/* 供 AI 與拾取物使用的點查詢。 */
bool  bp_point_hits_enemy(Vector3 p);
bool  bp_sphere_hits_world(Vector3 p, float radius);

/* 傷害施加。所有戰鬥都經過這兩個函式，好讓擊退、命中標記、血液方向、分數與音效
 * 維持一致。 */
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

/* 對可破壞的世界幾何（油桶）造成傷害。若已引爆則回傳 true。 */
bool  bp_barrel_damage(int solidIndex, float amount);

/* 附帶視線（line-of-sight）檢查的徑向傷害，如此躲進掩體才真的有用。 */
int   bp_apply_explosion(Vector3 center, float radius, float maxDamage, float knockback,
                         bool damagesPlayer, Vector3 playerPos, float *playerDamageOut);

#endif /* BP_HITDETECT_H */
