/******************************************************************************************
 * bp_math.h -- collision, intersection and movement helpers used by physics + combat.
 *
 * The whole game is AABB-vs-AABB plus swept ray-vs-AABB. That is a deliberate choice:
 *   - it is branch-predictable and cache friendly (no BVH traversal, no mallocs)
 *   - it gives *exact* answers for axis aligned arena geometry, which is what a
 *     boomer-shooter map is made of
 *   - it behaves identically on desktop GL and WebGL, so hit registration never differs
 *     between platforms (critical: players must not be able to tell which build they are on)
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
 * Ray vs AABB (slab method). Returns true on hit; `t` is the entry distance along `dir`,
 * `normal` is the surface normal of the entered face. `dir` must be normalised.
 * `t_max` limits the ray length.
 * ------------------------------------------------------------------------------------ */
bool bp_ray_aabb(Vector3 origin, Vector3 dir, const bp_aabb *box,
                 float t_max, float *out_t, Vector3 *out_normal);

/* Shortest distance from a segment to a point -- used for explosion falloff & AI hearing. */
float bp_segment_point_distance(Vector3 a, Vector3 b, Vector3 p);

/* --------------------------------------------------------------------------------------
 * Cylinder-vs-AABB overlap test used for character bodies (we model characters as a
 * vertical capsule approximated by an AABB, which keeps resolution cheap and stable).
 * ------------------------------------------------------------------------------------ */
bool bp_body_overlaps_box(Vector3 center, float radius, float height, const bp_aabb *box);

/* Horizontal-only resolution: push `center` out of `box` along the axis of least
 * penetration. Returns true if a correction was applied. */
bool bp_resolve_body_box(Vector3 *center, float radius, float height, const bp_aabb *box);

/* Utility: build a forward vector from yaw/pitch in degrees (raylib camera convention). */
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
