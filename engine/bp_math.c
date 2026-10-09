/******************************************************************************************
 * bp_math.c
 ******************************************************************************************/
#include "bp_math.h"

bool bp_ray_aabb(Vector3 origin, Vector3 dir, const bp_aabb *box,
                 float t_max, float *out_t, Vector3 *out_normal)
{
    float tmin = 0.0f;
    float tmax = t_max;
    Vector3 normal = { 0, 0, 0 };

    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { dir.x, dir.y, dir.z };
    const float bmin[3] = { box->min.x, box->min.y, box->min.z };
    const float bmax[3] = { box->max.x, box->max.y, box->max.z };

    for (int i = 0; i < 3; i++)
    {
        if (BP_ABS(d[i]) < 1e-8f)
        {
            /* 射線與此 slab 平行：原點若落在 slab 之外即判定未命中。 */
            if (o[i] < bmin[i] || o[i] > bmax[i]) return false;
        }
        else
        {
            float inv = 1.0f / d[i];
            float t1 = (bmin[i] - o[i]) * inv;
            float t2 = (bmax[i] - o[i]) * inv;
            float sign = -1.0f;

            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; sign = 1.0f; }

            if (t1 > tmin)
            {
                tmin = t1;
                normal = (Vector3){ 0, 0, 0 };
                if (i == 0) normal.x = sign;
                if (i == 1) normal.y = sign;
                if (i == 2) normal.z = sign;
            }
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }

    if (out_t) *out_t = tmin;
    if (out_normal) *out_normal = normal;
    return true;
}

float bp_segment_point_distance(Vector3 a, Vector3 b, Vector3 p)
{
    Vector3 ab = Vector3Subtract(b, a);
    float len2 = Vector3LengthSqr(ab);
    if (len2 < 1e-8f) return Vector3Distance(a, p);

    float t = Vector3DotProduct(Vector3Subtract(p, a), ab) / len2;
    t = BP_CLAMP(t, 0.0f, 1.0f);
    Vector3 closest = Vector3Add(a, Vector3Scale(ab, t));
    return Vector3Distance(closest, p);
}

bool bp_body_overlaps_box(Vector3 center, float radius, float height, const bp_aabb *box)
{
    /* 角色身體以 `center` 為中心，涵蓋 [center.y - height/2, center.y + height/2]；而這個
     * `center` 在本引擎中代表的是「腳底」位置（見 bp_fps_controller.h）。 */
    float halfH = height * 0.5f;
    float cy = center.y + halfH;

    float dx = BP_MAX(box->min.x - center.x, BP_MAX(0.0f, center.x - box->max.x));
    float dz = BP_MAX(box->min.z - center.z, BP_MAX(0.0f, center.z - box->max.z));
    float dy = BP_MAX(box->min.y - cy, BP_MAX(0.0f, cy - box->max.y));

    /* 盒子上最接近膠囊線段的點（這裡以身體中心來近似）。 */
    float feetY = center.y;
    float dyFeet = BP_MAX(box->min.y - feetY, BP_MAX(0.0f, feetY - box->max.y));
    dy = BP_MIN(dy, dyFeet);

    float horiz2 = dx * dx + dz * dz;
    if (horiz2 > radius * radius) return false;
    if (dy > halfH) return false;
    return true;
}

bool bp_resolve_body_box(Vector3 *center, float radius, float height, const bp_aabb *box)
{
    float halfH = height * 0.5f;
    float bodyMinY = center->y;
    float bodyMaxY = center->y + height;

    /* 完全沒有垂直重疊 -> 無事可做（此時我們在盒子的上方或下方）。 */
    if (bodyMaxY <= box->min.y || bodyMinY >= box->max.y) return false;

    float cx = BP_CLAMP(center->x, box->min.x, box->max.x);
    float cz = BP_CLAMP(center->z, box->min.z, box->max.z);

    float dx = center->x - cx;
    float dz = center->z - cz;
    float dist2 = dx * dx + dz * dz;

    if (dist2 > radius * radius) return false;

    if (dist2 > 1e-8f)
    {
        float dist = sqrtf(dist2);
        float push = radius - dist;
        center->x += (dx / dist) * push;
        center->z += (dz / dist) * push;
    }
    else
    {
        /* 中心點落在盒子的水平輪廓內：沿穿透最淺的水平軸把身體彈出去。 */
        float penXmin = center->x - box->min.x;
        float penXmax = box->max.x - center->x;
        float penZmin = center->z - box->min.z;
        float penZmax = box->max.z - center->z;
        float minPen = BP_MIN(BP_MIN(penXmin, penXmax), BP_MIN(penZmin, penZmax));

        if (minPen == penXmin)      center->x = box->min.x - radius;
        else if (minPen == penXmax) center->x = box->max.x + radius;
        else if (minPen == penZmin) center->z = box->min.z - radius;
        else                        center->z = box->max.z + radius;
    }

    BP_UNUSED(halfH);
    return true;
}
