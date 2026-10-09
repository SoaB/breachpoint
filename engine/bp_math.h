/******************************************************************************************
 * bp_math.h -- 物理與戰鬥使用的碰撞、相交與移動輔助函式。
 *
 * 整個遊戲的碰撞都是 AABB 對 AABB，再加上掃掠線段（swept ray）對 AABB。這是刻意的選擇：
 *   - 分支可預測、快取友善（不遍歷 BVH，也不 malloc）
 *   - 對軸對齊的競技場幾何給出「精確」答案，而 boomer-shooter 的地圖正是由這種幾何構成
 *   - 在桌機 GL 與 WebGL 上行為完全一致，命中判定因此永遠不會因平台而異
 *     （關鍵：玩家必須無法分辨自己正在玩的是哪一個版本）
 ******************************************************************************************/
#ifndef BP_MATH_H
#define BP_MATH_H

#include "bp_common.h"

typedef struct {
    Vector3 min;
    Vector3 max;
} bp_aabb;

static inline bp_aabb bp_aabb_make(Vector3 min, Vector3 max) { bp_aabb b = { min, max }; return b; }

static inline bp_aabb bp_aabb_from_center(Vector3 c, Vector3 half)
{
    bp_aabb b;
    b.min = Vector3Subtract(c, half);
    b.max = Vector3Add(c, half);
    return b;
}

static inline Vector3 bp_aabb_center(const bp_aabb *b)
{
    return Vector3Scale(Vector3Add(b->min, b->max), 0.5f);
}

static inline Vector3 bp_aabb_size(const bp_aabb *b) { return Vector3Subtract(b->max, b->min); }

static inline bool bp_aabb_contains_point(const bp_aabb *b, Vector3 p)
{
    return p.x >= b->min.x && p.x <= b->max.x &&
           p.y >= b->min.y && p.y <= b->max.y &&
           p.z >= b->min.z && p.z <= b->max.z;
}

static inline bool bp_aabb_overlaps(const bp_aabb *a, const bp_aabb *b)
{
    return a->min.x <= b->max.x && a->max.x >= b->min.x &&
           a->min.y <= b->max.y && a->max.y >= b->min.y &&
           a->min.z <= b->max.z && a->max.z >= b->min.z;
}

static inline bp_aabb bp_aabb_grow(const bp_aabb *b, float m)
{
    bp_aabb r;
    r.min = (Vector3){ b->min.x - m, b->min.y - m, b->min.z - m };
    r.max = (Vector3){ b->max.x + m, b->max.y + m, b->max.z + m };
    return r;
}

static inline bp_aabb bp_aabb_merge(const bp_aabb *a, const bp_aabb *b)
{
    bp_aabb r;
    r.min.x = BP_MIN(a->min.x, b->min.x);
    r.min.y = BP_MIN(a->min.y, b->min.y);
    r.min.z = BP_MIN(a->min.z, b->min.z);
    r.max.x = BP_MAX(a->max.x, b->max.x);
    r.max.y = BP_MAX(a->max.y, b->max.y);
    r.max.z = BP_MAX(a->max.z, b->max.z);
    return r;
}

/* --------------------------------------------------------------------------------------
 * Ray vs AABB（slab 法）。命中回傳 true；`t` 是沿 `dir` 方向的進入距離，`normal` 是所進入
 * 那一面的表面法線。`dir` 必須事先正規化（normalised），`t_max` 則限制線段的最大長度。
 * ------------------------------------------------------------------------------------ */
bool bp_ray_aabb(Vector3 origin, Vector3 dir, const bp_aabb *box,
                 float t_max, float *out_t, Vector3 *out_normal);

/* 線段與一點之間的最短距離 —— 用於爆炸傷害衰減與 AI 的聽覺判定。 */
float bp_segment_point_distance(Vector3 a, Vector3 b, Vector3 p);

/* --------------------------------------------------------------------------------------
 * 角色身體使用的「圓柱體 vs AABB」重疊測試（我們把角色建成一個以 AABB 近似的垂直膠囊，
 * 如此一來解算（resolution）既便宜又穩定）。
 * ------------------------------------------------------------------------------------ */
bool bp_body_overlaps_box(Vector3 center, float radius, float height, const bp_aabb *box);

/* 只在水平面解算：沿穿透最淺的那一條軸把 `center` 推出 `box`。若確實施加了修正則回傳 true。 */
bool bp_resolve_body_box(Vector3 *center, float radius, float height, const bp_aabb *box);

/* 工具函式：由 yaw／pitch（單位為度）建立前向量（raylib 攝影機慣例）。 */
static inline Vector3 bp_forward_from_angles(float yawDeg, float pitchDeg)
{
    float cp = cosf(pitchDeg * BP_DEG2RAD);
    Vector3 v;
    v.x = cp * sinf(yawDeg * BP_DEG2RAD);
    v.y = sinf(pitchDeg * BP_DEG2RAD);
    v.z = cp * cosf(yawDeg * BP_DEG2RAD);
    return v;
}

static inline float bp_yaw_to(Vector3 from, Vector3 to)
{
    Vector3 d = Vector3Subtract(to, from);
    return atan2f(d.x, d.z) * BP_RAD2DEG;
}

#endif /* BP_MATH_H */
