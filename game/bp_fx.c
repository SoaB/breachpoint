/******************************************************************************************
 * bp_fx.c -- 特效系統：粒子（particle）、貼花（decal）、曳光彈（tracer）與畫面震動。
 *
 * 全部以固定大小的池（pool）實作，執行期不做動態配置：池滿時以環狀方式回收最舊的槽位，
 * 寧可讓一團早已存在的煙提早消失，也不要放棄一記剛發生的彈著火花 —— 在射擊遊戲裡，後者
 * 顯眼得多。繪製時不走 raylib 的 3D 圖元，而是把每個 billboard 手動送進 rlgl 批次
 * （batch），好讓成千上萬個粒子只需極少的 draw call。
 ******************************************************************************************/
#include "bp_fx.h"
#include "../engine/bp_assets.h"

bp_fx g_fx = { 0 };

/* ====================================================================================== */
/* 生命週期                                                                                  */
/* ====================================================================================== */
void bp_fx_init(void) { bp_fx_reset(); }

void bp_fx_reset(void)
{
    memset(&g_fx, 0, sizeof(g_fx));
}

/* 環狀（round-robin）配置：池子滿了就回收最舊的槽位，而不是拒絕生成。在射擊遊戲裡，漏掉
 * 一記剛發生的彈著火花，遠比縮短一團 800 ms 前就存在的煙霧來得難看。 */
static bp_particle *alloc_particle(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        int idx = (g_fx.particle_head + i) % MAX_PARTICLES;
        if (!g_fx.particles[idx].alive)
        {
            g_fx.particle_head = (idx + 1) % MAX_PARTICLES;
            g_fx.particle_live++;
            return &g_fx.particles[idx];
        }
    }
    int idx = g_fx.particle_head;
    g_fx.particle_head = (idx + 1) % MAX_PARTICLES;
    return &g_fx.particles[idx];
}

void bp_fx_particle(bp_particle_kind kind, Vector3 pos, Vector3 vel, float life,
                    float size0, float size1, Color c0, Color c1)
{
    bp_particle *p = alloc_particle();
    memset(p, 0, sizeof(*p));
    p->pos = pos;
    p->vel = vel;
    p->life = BP_MAX(life, 0.01f);
    p->size0 = size0;
    p->size1 = size1;
    p->color0 = c0;
    p->color1 = c1;
    p->kind = (uint8_t)kind;
    p->alive = 1;
    p->roll = bp_rng_range(&g_engine.rng, 0.0f, 360.0f);
    p->spin = bp_rng_range(&g_engine.rng, -160.0f, 160.0f);

    switch (kind)
    {
        case PTX_SPARK:
            p->tex = TEX_SPARK; p->additive = 1; p->gravity = 1; break;
        case PTX_SMOKE:
            p->tex = TEX_SMOKE; p->additive = 0; p->gravity = 2; p->spin *= 0.2f; break;
        case PTX_BLOOD:
            p->tex = TEX_PARTICLE_SOFT; p->additive = 0; p->gravity = 1; break;
        case PTX_DEBRIS:
            p->tex = TEX_PARTICLE_HARD; p->additive = 0; p->gravity = 1; p->bounce = 1; break;
        case PTX_SHELL:
            p->tex = TEX_PARTICLE_HARD; p->additive = 0; p->gravity = 1; p->bounce = 2; break;
        case PTX_EMBER:
            p->tex = TEX_PARTICLE_SOFT; p->additive = 1; p->gravity = 2; break;
        case PTX_FLASH:
            p->tex = TEX_PARTICLE_SOFT; p->additive = 1; p->gravity = 0; break;
        case PTX_RING:
            p->tex = TEX_RING; p->additive = 1; p->gravity = 0; p->spin = 0.0f; break;
        default:
            p->tex = TEX_PARTICLE_SOFT; p->gravity = 1; break;
    }
    g_fx.spawned_this_frame++;
}

/* ====================================================================================== */
/* 更新                                                                                      */
/* ====================================================================================== */
void bp_fx_update(float dt, Vector3 worldFloorY)
{
    BP_UNUSED(worldFloorY);
    g_fx.spawned_this_frame = 0;

    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        bp_particle *p = &g_fx.particles[i];
        if (!p->alive) continue;

        p->age += dt;
        if (p->age >= p->life) { p->alive = 0; g_fx.particle_live--; continue; }

        float g = (p->gravity == 1) ? GRAVITY : (p->gravity == 2 ? GRAVITY * 0.18f : 0.0f);
        p->vel.y += g * dt;

        /* 空氣阻力：煙劇烈減速，火花幾乎不受影響 */
        float drag = (p->kind == PTX_SMOKE) ? 2.6f : (p->kind == PTX_SPARK ? 0.9f : 0.35f);
        p->vel = Vector3Scale(p->vel, 1.0f / (1.0f + drag * dt));

        p->pos = Vector3Add(p->pos, Vector3Scale(p->vel, dt));
        p->roll += p->spin * dt;

        /* 煙在冷卻時會略微上飄 */
        if (p->kind == PTX_SMOKE) p->pos.y += 0.55f * dt;

        /* 地面反彈 */
        if (p->bounce && p->pos.y < 0.06f && p->vel.y < 0.0f)
        {
            p->pos.y = 0.06f;
            p->vel.y = -p->vel.y * (p->bounce == 1 ? 0.34f : 0.42f);
            p->vel.x *= 0.72f;
            p->vel.z *= 0.72f;
            p->spin *= 0.5f;
        }
    }

    for (int i = 0; i < MAX_DECALS; i++)
    {
        bp_decal *d = &g_fx.decals[i];
        if (!d->alive) continue;
        d->age += dt;
        if (d->age >= d->life) { d->alive = 0; g_fx.decal_live--; }
    }

    for (int i = 0; i < MAX_HITSCAN_TRACERS; i++)
    {
        bp_tracer *t = &g_fx.tracers[i];
        if (!t->alive) continue;
        t->age += dt;
        if (t->age >= t->life) t->alive = 0;
    }

    /* 回饋效果的衰減 */
    g_fx.hit_marker    = BP_MAX(0.0f, g_fx.hit_marker - dt * 5.5f);
    g_fx.kill_marker   = BP_MAX(0.0f, g_fx.kill_marker - dt * 3.0f);
    g_fx.muzzle_flash  = BP_MAX(0.0f, g_fx.muzzle_flash - dt * 18.0f);

    /* 畫面震動：三個軸上逐漸衰減的雜訊，Y 軸的振幅較大（感覺更像被撞了一下） */
    if (g_fx.shake_amount > 0.001f)
    {
        g_fx.shake_time += dt;
        g_fx.shake_amount = bp_damp(g_fx.shake_amount, 0.0f, 5.0f, dt);
        float a = g_fx.shake_amount;
        float t = g_fx.shake_time * 34.0f;
        g_fx.shake_offset.x = sinf(t * 1.7f) * a * 0.10f + sinf(t * 5.3f) * a * 0.045f;
        g_fx.shake_offset.y = sinf(t * 2.3f + 1.1f) * a * 0.13f + sinf(t * 6.1f) * a * 0.05f;
        g_fx.shake_offset.z = sinf(t * 1.3f + 2.4f) * a * 0.06f;
    }
    else
    {
        g_fx.shake_amount = 0.0f;
        g_fx.shake_offset = (Vector3){ 0, 0, 0 };
    }
}

void bp_fx_shake(float amount, float duration)
{
    BP_UNUSED(duration);
    g_fx.shake_amount = BP_MIN(1.6f, g_fx.shake_amount + amount);
}

Vector3 bp_fx_shake_offset(void) { return g_fx.shake_offset; }

void bp_fx_hit_marker(bool kill)
{
    g_fx.hit_marker = 1.0f;
    if (kill) g_fx.kill_marker = 1.0f;
}

/* ====================================================================================== */
/* 貼花 / 曳光                                                                               */
/* ====================================================================================== */
void bp_fx_decal(bp_texture_id tex, Vector3 pos, Vector3 normal, float size, float life, Color tint)
{
    bp_decal *d = NULL;
    bool recycled = false;
    for (int i = 0; i < MAX_DECALS; i++)
    {
        int idx = (g_fx.decal_head + i) % MAX_DECALS;
        if (!g_fx.decals[idx].alive) { d = &g_fx.decals[idx]; g_fx.decal_head = (idx + 1) % MAX_DECALS; break; }
    }
    if (!d)
    {
        /* 所有槽位都在使用中：直接偷走游標所指的那一個。decal_live 在這裡絕對不能改變，因
         * 為我們是替換掉一個早已計入統計的貼花，而不是新增一個。 */
        d = &g_fx.decals[g_fx.decal_head];
        g_fx.decal_head = (g_fx.decal_head + 1) % MAX_DECALS;
        recycled = true;
    }

    memset(d, 0, sizeof(*d));
    /* 把貼花推離表面一丁點，讓它永遠不會與牆面發生 z-fighting */
    d->pos = Vector3Add(pos, Vector3Scale(normal, 0.02f));
    d->normal = normal;
    d->size = size;
    d->life = life;
    d->tex = (uint8_t)tex;
    d->tint = tint;
    d->alive = 1;
    d->roll = bp_rng_range(&g_engine.rng, 0.0f, 360.0f);
    if (!recycled) g_fx.decal_live++;
}

void bp_fx_tracer(Vector3 from, Vector3 to, float width, float life, Color c)
{
    bp_tracer *t = NULL;
    for (int i = 0; i < MAX_HITSCAN_TRACERS; i++)
    {
        int idx = (g_fx.tracer_head + i) % MAX_HITSCAN_TRACERS;
        if (!g_fx.tracers[idx].alive) { t = &g_fx.tracers[idx]; g_fx.tracer_head = (idx + 1) % MAX_HITSCAN_TRACERS; break; }
    }
    if (!t)
    {
        t = &g_fx.tracers[g_fx.tracer_head];
        g_fx.tracer_head = (g_fx.tracer_head + 1) % MAX_HITSCAN_TRACERS;
    }
    t->from = from; t->to = to; t->width = width;
    t->life = life; t->color = c; t->age = 0.0f; t->alive = 1;
}

/* ====================================================================================== */
/* 組合式發射器                                                                              */
/* ====================================================================================== */
void bp_fx_muzzle_flash(Vector3 pos, Vector3 dir, float intensity)
{
    g_fx.muzzle_flash = BP_MIN(1.0f, intensity);
    g_fx.muzzle_pos = pos;
    g_fx.muzzle_dir = dir;

    Color hot = (Color){ 255, 246, 200, 255 };
    Color cool = (Color){ 255, 150, 40, 0 };

    bp_fx_particle(PTX_FLASH, pos, Vector3Scale(dir, 1.5f), 0.055f,
                   0.55f * intensity, 0.10f, hot, cool);

    for (int i = 0; i < (int)(5.0f * intensity); i++)
    {
        Vector3 v = (Vector3){
            dir.x * 6.0f + bp_rng_range(&g_engine.rng, -2.5f, 2.5f),
            dir.y * 6.0f + bp_rng_range(&g_engine.rng, -1.5f, 3.0f),
            dir.z * 6.0f + bp_rng_range(&g_engine.rng, -2.5f, 2.5f)
        };
        bp_fx_particle(PTX_SPARK, pos, v, bp_rng_range(&g_engine.rng, 0.10f, 0.26f),
                       0.09f, 0.01f, (Color){ 255, 220, 130, 255 }, (Color){ 255, 90, 20, 0 });
    }
    for (int i = 0; i < (int)(2.0f * intensity); i++)
    {
        Vector3 v = Vector3Add(Vector3Scale(dir, bp_rng_range(&g_engine.rng, 1.0f, 2.5f)),
                               (Vector3){ bp_rng_range(&g_engine.rng, -0.6f, 0.6f),
                                          bp_rng_range(&g_engine.rng, 0.2f, 1.0f),
                                          bp_rng_range(&g_engine.rng, -0.6f, 0.6f) });
        bp_fx_particle(PTX_SMOKE, pos, v, bp_rng_range(&g_engine.rng, 0.40f, 0.85f),
                       0.14f, 0.62f, (Color){ 150, 148, 146, 120 }, (Color){ 90, 90, 92, 0 });
    }
}

void bp_fx_impact_wall(Vector3 pos, Vector3 normal, float energy)
{
    bp_fx_decal(TEX_BULLET_HOLE, pos, normal, bp_rng_range(&g_engine.rng, 0.16f, 0.26f) * (0.7f + energy),
                14.0f, (Color){ 255, 255, 255, 210 });

    int n = (int)(4.0f + energy * 8.0f);
    for (int i = 0; i < n; i++)
    {
        Vector3 v = Vector3Add(Vector3Scale(normal, bp_rng_range(&g_engine.rng, 2.0f, 6.5f)),
                               (Vector3){ bp_rng_range(&g_engine.rng, -3.0f, 3.0f),
                                          bp_rng_range(&g_engine.rng, -1.0f, 3.5f),
                                          bp_rng_range(&g_engine.rng, -3.0f, 3.0f) });
        bp_fx_particle(PTX_SPARK, pos, v, bp_rng_range(&g_engine.rng, 0.12f, 0.34f),
                       0.10f, 0.005f, (Color){ 255, 232, 170, 255 }, (Color){ 255, 120, 30, 0 });
    }
    for (int i = 0; i < 3; i++)
    {
        Vector3 v = Vector3Add(Vector3Scale(normal, bp_rng_range(&g_engine.rng, 0.6f, 1.6f)),
                               (Vector3){ bp_rng_range(&g_engine.rng, -0.8f, 0.8f),
                                          bp_rng_range(&g_engine.rng, 0.2f, 1.2f),
                                          bp_rng_range(&g_engine.rng, -0.8f, 0.8f) });
        bp_fx_particle(PTX_SMOKE, pos, v, bp_rng_range(&g_engine.rng, 0.35f, 0.70f),
                       0.16f, 0.55f, (Color){ 140, 136, 130, 105 }, (Color){ 80, 78, 76, 0 });
    }
    for (int i = 0; i < (int)(2.0f + energy * 3.0f); i++)
    {
        Vector3 v = Vector3Add(Vector3Scale(normal, bp_rng_range(&g_engine.rng, 1.5f, 4.0f)),
                               (Vector3){ bp_rng_range(&g_engine.rng, -2.5f, 2.5f),
                                          bp_rng_range(&g_engine.rng, 0.5f, 3.0f),
                                          bp_rng_range(&g_engine.rng, -2.5f, 2.5f) });
        bp_fx_particle(PTX_DEBRIS, pos, v, bp_rng_range(&g_engine.rng, 0.40f, 0.90f),
                       0.055f, 0.035f, (Color){ 168, 158, 146, 255 }, (Color){ 110, 104, 98, 0 });
    }
}

void bp_fx_impact_metal(Vector3 pos, Vector3 normal, float energy)
{
    bp_fx_decal(TEX_SCORCH, pos, normal, 0.20f + energy * 0.12f, 10.0f, (Color){ 255, 255, 255, 130 });
    for (int i = 0; i < (int)(6.0f + energy * 10.0f); i++)
    {
        Vector3 v = Vector3Add(Vector3Scale(normal, bp_rng_range(&g_engine.rng, 4.0f, 9.0f)),
                               (Vector3){ bp_rng_range(&g_engine.rng, -3.5f, 3.5f),
                                          bp_rng_range(&g_engine.rng, -1.0f, 4.0f),
                                          bp_rng_range(&g_engine.rng, -3.5f, 3.5f) });
        bp_fx_particle(PTX_SPARK, pos, v, bp_rng_range(&g_engine.rng, 0.18f, 0.45f),
                       0.075f, 0.004f, (Color){ 255, 250, 220, 255 }, (Color){ 255, 160, 40, 0 });
    }
}

void bp_fx_impact_flesh(Vector3 pos, Vector3 dir, float energy)
{
    bp_fx_blood_mist(pos, dir, energy);
}

void bp_fx_blood_mist(Vector3 pos, Vector3 dir, float amount)
{
    int n = (int)(5.0f + amount * 12.0f);
    for (int i = 0; i < n; i++)
    {
        Vector3 v = Vector3Add(Vector3Scale(dir, bp_rng_range(&g_engine.rng, 1.5f, 5.0f)),
                               (Vector3){ bp_rng_range(&g_engine.rng, -2.6f, 2.6f),
                                          bp_rng_range(&g_engine.rng, -0.5f, 3.2f),
                                          bp_rng_range(&g_engine.rng, -2.6f, 2.6f) });
        bp_fx_particle(PTX_BLOOD, pos, v, bp_rng_range(&g_engine.rng, 0.28f, 0.72f),
                       bp_rng_range(&g_engine.rng, 0.10f, 0.22f), 0.02f,
                       (Color){ 176, 18, 28, 235 }, (Color){ 96, 6, 14, 0 });
    }
    bp_fx_decal(TEX_BLOOD_SPLAT, (Vector3){ pos.x, 0.03f, pos.z }, (Vector3){ 0, 1, 0 },
                0.5f + amount * 0.45f, 22.0f, (Color){ 255, 255, 255, 190 });
}

void bp_fx_explosion(Vector3 pos, float radius)
{
    float r = BP_CLAMP(radius, 0.5f, 12.0f);

    bp_fx_particle(PTX_FLASH, pos, (Vector3){ 0, 0, 0 }, 0.10f,
                   r * 1.6f, r * 0.2f, (Color){ 255, 255, 235, 255 }, (Color){ 255, 160, 40, 0 });
    bp_fx_particle(PTX_RING, pos, (Vector3){ 0, 0, 0 }, 0.34f,
                   r * 0.5f, r * 3.4f, (Color){ 255, 210, 130, 220 }, (Color){ 255, 110, 40, 0 });

    int sparks = (int)(18.0f + r * 9.0f);
    for (int i = 0; i < sparks; i++)
    {
        Vector3 v = (Vector3){ bp_rng_range(&g_engine.rng, -1.0f, 1.0f),
                               bp_rng_range(&g_engine.rng, -0.3f, 1.3f),
                               bp_rng_range(&g_engine.rng, -1.0f, 1.0f) };
        v = Vector3Scale(Vector3Normalize(v), bp_rng_range(&g_engine.rng, 4.0f, 14.0f) * (0.5f + r * 0.16f));
        bp_fx_particle(PTX_SPARK, pos, v, bp_rng_range(&g_engine.rng, 0.25f, 0.75f),
                       0.16f, 0.01f, (Color){ 255, 236, 170, 255 }, (Color){ 255, 90, 20, 0 });
    }
    int smoke = (int)(10.0f + r * 5.0f);
    for (int i = 0; i < smoke; i++)
    {
        Vector3 v = (Vector3){ bp_rng_range(&g_engine.rng, -3.5f, 3.5f),
                               bp_rng_range(&g_engine.rng, 0.6f, 5.0f),
                               bp_rng_range(&g_engine.rng, -3.5f, 3.5f) };
        bp_fx_particle(PTX_SMOKE, pos, v, bp_rng_range(&g_engine.rng, 0.85f, 2.0f),
                       0.5f, r * 1.5f, (Color){ 96, 88, 84, 165 }, (Color){ 44, 42, 42, 0 });
    }
    for (int i = 0; i < (int)(6.0f + r * 3.0f); i++)
    {
        Vector3 v = (Vector3){ bp_rng_range(&g_engine.rng, -7.0f, 7.0f),
                               bp_rng_range(&g_engine.rng, 2.0f, 9.0f),
                               bp_rng_range(&g_engine.rng, -7.0f, 7.0f) };
        bp_fx_particle(PTX_DEBRIS, pos, v, bp_rng_range(&g_engine.rng, 0.7f, 1.6f),
                       0.10f, 0.05f, (Color){ 120, 112, 104, 255 }, (Color){ 70, 66, 62, 0 });
    }
    for (int i = 0; i < (int)(8.0f + r * 4.0f); i++)
    {
        Vector3 v = (Vector3){ bp_rng_range(&g_engine.rng, -1.8f, 1.8f),
                               bp_rng_range(&g_engine.rng, 0.4f, 2.6f),
                               bp_rng_range(&g_engine.rng, -1.8f, 1.8f) };
        bp_fx_particle(PTX_EMBER, pos, v, bp_rng_range(&g_engine.rng, 0.8f, 1.9f),
                       0.09f, 0.005f, (Color){ 255, 170, 60, 220 }, (Color){ 200, 40, 10, 0 });
    }
    bp_fx_decal(TEX_SCORCH, (Vector3){ pos.x, 0.04f, pos.z }, (Vector3){ 0, 1, 0 },
                r * 1.1f, 30.0f, (Color){ 255, 255, 255, 200 });
    bp_fx_shake(BP_MIN(1.2f, 0.20f + r * 0.10f), 0.5f);
}

void bp_fx_shell_eject(Vector3 pos, Vector3 right, Vector3 up)
{
    Vector3 v = Vector3Add(Vector3Add(Vector3Scale(right, bp_rng_range(&g_engine.rng, 1.8f, 3.2f)),
                                      Vector3Scale(up, bp_rng_range(&g_engine.rng, 1.6f, 2.8f))),
                           (Vector3){ 0, bp_rng_range(&g_engine.rng, 0.2f, 0.9f), 0 });
    bp_particle *p = alloc_particle();
    memset(p, 0, sizeof(*p));
    p->pos = pos; p->vel = v; p->life = 2.2f;
    p->size0 = 0.045f; p->size1 = 0.045f;
    p->color0 = (Color){ 226, 190, 96, 255 };
    p->color1 = (Color){ 178, 142, 60, 255 };
    p->kind = PTX_SHELL; p->tex = TEX_PARTICLE_HARD;
    p->gravity = 1; p->bounce = 2; p->alive = 1;
    p->roll = bp_rng_range(&g_engine.rng, 0.0f, 360.0f);
    p->spin = bp_rng_range(&g_engine.rng, -520.0f, 520.0f);
    g_fx.particle_live++;
}

void bp_fx_smoke_trail(Vector3 pos, float amount)
{
    Vector3 v = (Vector3){ bp_rng_range(&g_engine.rng, -0.5f, 0.5f),
                           bp_rng_range(&g_engine.rng, 0.1f, 0.8f),
                           bp_rng_range(&g_engine.rng, -0.5f, 0.5f) };
    bp_fx_particle(PTX_SMOKE, pos, v, bp_rng_range(&g_engine.rng, 0.5f, 1.1f) * (0.6f + amount),
                   0.16f * (0.6f + amount), 0.95f * (0.6f + amount),
                   (Color){ 176, 172, 168, 120 }, (Color){ 96, 94, 92, 0 });
}

void bp_fx_dust_puff(Vector3 pos, float amount)
{
    for (int i = 0; i < (int)(3.0f + amount * 5.0f); i++)
    {
        Vector3 v = (Vector3){ bp_rng_range(&g_engine.rng, -1.4f, 1.4f) * amount,
                               bp_rng_range(&g_engine.rng, 0.2f, 1.1f),
                               bp_rng_range(&g_engine.rng, -1.4f, 1.4f) * amount };
        bp_fx_particle(PTX_SMOKE, pos, v, bp_rng_range(&g_engine.rng, 0.30f, 0.65f),
                       0.20f, 0.72f, (Color){ 150, 140, 126, 90 }, (Color){ 110, 102, 92, 0 });
    }
}

void bp_fx_speed_burst(Vector3 pos, Vector3 dir)
{
    for (int i = 0; i < 10; i++)
    {
        Vector3 v = Vector3Add(Vector3Scale(dir, bp_rng_range(&g_engine.rng, 6.0f, 14.0f)),
                               (Vector3){ bp_rng_range(&g_engine.rng, -2.0f, 2.0f),
                                          bp_rng_range(&g_engine.rng, -1.0f, 2.0f),
                                          bp_rng_range(&g_engine.rng, -2.0f, 2.0f) });
        bp_fx_particle(PTX_SPARK, pos, v, bp_rng_range(&g_engine.rng, 0.16f, 0.36f),
                       0.07f, 0.005f, (Color){ 255, 214, 150, 220 }, (Color){ 255, 120, 40, 0 });
    }
}

/* ====================================================================================== */
/* 繪製：手動 billboard 與單一 rlgl 批次                                                     */
/* ====================================================================================== */
static Color fade_color(Color a, Color b, float t)
{
    t = BP_CLAMP(t, 0.0f, 1.0f);
    return (Color){
        (unsigned char)((float)a.r + ((float)b.r - (float)a.r) * t),
        (unsigned char)((float)a.g + ((float)b.g - (float)a.g) * t),
        (unsigned char)((float)a.b + ((float)b.b - (float)a.b) * t),
        (unsigned char)((float)a.a + ((float)b.a - (float)a.a) * t)
    };
}

/* 把一個面向相機、帶有滾轉角（roll）的四邊形直接推進目前的 rlgl 批次（batch）。 */
static void emit_quad(Camera3D cam, Texture2D tex, Vector3 center, float sizeX, float sizeY,
                      float rollDeg, Color color)
{
    if (!tex.id || sizeX <= 0.0f || sizeY <= 0.0f) return;

    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, cam.up));
    Vector3 up = Vector3CrossProduct(right, fwd);

    float cr = cosf(rollDeg * BP_DEG2RAD);
    float sr = sinf(rollDeg * BP_DEG2RAD);
    Vector3 r = Vector3Add(Vector3Scale(right, cr * sizeX * 0.5f), Vector3Scale(up, sr * sizeX * 0.5f));
    Vector3 u = Vector3Add(Vector3Scale(up, cr * sizeY * 0.5f), Vector3Scale(right, -sr * sizeY * 0.5f));

    Vector3 v0 = Vector3Subtract(Vector3Subtract(center, r), u);
    Vector3 v1 = Vector3Add(Vector3Subtract(center, r), u);
    Vector3 v2 = Vector3Add(Vector3Add(center, r), u);
    Vector3 v3 = Vector3Subtract(Vector3Add(center, r), u);

    /* 每個四邊形由兩個三角形組成。批次滿時 rl_check_render_batch 會自動清空送出，正是這
     * 一點讓成本保持在 O(particles)，而不是 O(draw calls)。 */
    rlCheckRenderBatchLimit(12);
    rlSetTexture(tex.id);

    rlColor4ub(color.r, color.g, color.b, color.a);
    rlNormal3f(0.0f, 0.0f, 1.0f);

    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(v0.x, v0.y, v0.z);
    rlTexCoord2f(0.0f, 1.0f); rlVertex3f(v1.x, v1.y, v1.z);
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(v2.x, v2.y, v2.z);

    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(v0.x, v0.y, v0.z);
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(v2.x, v2.y, v2.z);
    rlTexCoord2f(1.0f, 0.0f); rlVertex3f(v3.x, v3.y, v3.z);

    rlSetTexture(0);
}

void bp_fx_draw(Camera3D cam)
{
    /* ---- 貼花（decal）：以 alpha 混合繪製，而且畫在最底層，好讓粒子疊在它上面 ---- */
    for (int i = 0; i < MAX_DECALS; i++)
    {
        bp_decal *d = &g_fx.decals[i];
        if (!d->alive) continue;

        Texture2D t = bp_tex((bp_texture_id)d->tex);
        if (!t.id) continue;

        float k = d->age / d->life;
        float fade = (k > 0.72f) ? 1.0f - (k - 0.72f) / 0.28f : 1.0f;
        Color c = d->tint;
        c.a = (unsigned char)((float)c.a * BP_CLAMP(fade, 0.0f, 1.0f));

        /* 由表面法線建出一組基底向量（basis） */
        Vector3 n = Vector3Normalize(d->normal);
        Vector3 ref = (BP_ABS(n.y) > 0.9f) ? (Vector3){ 1, 0, 0 } : (Vector3){ 0, 1, 0 };
        Vector3 tan1 = Vector3Normalize(Vector3CrossProduct(n, ref));
        Vector3 tan2 = Vector3CrossProduct(n, tan1);

        float cr = cosf(d->roll * BP_DEG2RAD), sr = sinf(d->roll * BP_DEG2RAD);
        Vector3 r = Vector3Add(Vector3Scale(tan1, cr), Vector3Scale(tan2, sr));
        Vector3 u = Vector3Add(Vector3Scale(tan2, cr), Vector3Scale(tan1, -sr));
        float hs = d->size * 0.5f;

        Vector3 v0 = Vector3Subtract(Vector3Subtract(d->pos, Vector3Scale(r, hs)), Vector3Scale(u, hs));
        Vector3 v1 = Vector3Add(Vector3Subtract(d->pos, Vector3Scale(r, hs)), Vector3Scale(u, hs));
        Vector3 v2 = Vector3Add(Vector3Add(d->pos, Vector3Scale(r, hs)), Vector3Scale(u, hs));
        Vector3 v3 = Vector3Subtract(Vector3Add(d->pos, Vector3Scale(r, hs)), Vector3Scale(u, hs));

        rlCheckRenderBatchLimit(12);
        rlSetTexture(t.id);
        rlColor4ub(c.r, c.g, c.b, c.a);
        rlNormal3f(n.x, n.y, n.z);
        rlTexCoord2f(0, 0); rlVertex3f(v0.x, v0.y, v0.z);
        rlTexCoord2f(0, 1); rlVertex3f(v1.x, v1.y, v1.z);
        rlTexCoord2f(1, 1); rlVertex3f(v2.x, v2.y, v2.z);
        rlTexCoord2f(0, 0); rlVertex3f(v0.x, v0.y, v0.z);
        rlTexCoord2f(1, 1); rlVertex3f(v2.x, v2.y, v2.z);
        rlTexCoord2f(1, 0); rlVertex3f(v3.x, v3.y, v3.z);
        rlSetTexture(0);
    }

    /* ---- 曳光彈（tracer） ---- */
    for (int i = 0; i < MAX_HITSCAN_TRACERS; i++)
    {
        bp_tracer *t = &g_fx.tracers[i];
        if (!t->alive) continue;
        float k = t->age / t->life;
        Color c = t->color;
        c.a = (unsigned char)((float)c.a * (1.0f - k));
        /* 曳光彈是一個沿自身方向拉伸的廣告牌（billboard）。 */
        Vector3 dir = Vector3Subtract(t->to, t->from);
        float len = Vector3Length(dir);
        if (len < 0.001f) continue;
        Vector3 dn = Vector3Scale(dir, 1.0f / len);
        Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        Vector3 side = Vector3CrossProduct(dn, fwd);
        float sl = Vector3Length(side);
        if (sl < 0.0001f) continue;
        side = Vector3Scale(side, t->width * 0.5f / sl);

        Vector3 a = Vector3Add(t->from, side), b = Vector3Subtract(t->from, side);
        Vector3 cc = Vector3Add(t->to, side),  d = Vector3Subtract(t->to, side);

        Texture2D grad = bp_tex(TEX_PARTICLE_SOFT);
        rlCheckRenderBatchLimit(12);
        rlSetTexture(grad.id);
        rlColor4ub(c.r, c.g, c.b, c.a);
        rlNormal3f(fwd.x, fwd.y, fwd.z);
        rlTexCoord2f(0, 0); rlVertex3f(a.x, a.y, a.z);
        rlTexCoord2f(0, 1); rlVertex3f(b.x, b.y, b.z);
        rlTexCoord2f(1, 1); rlVertex3f(cc.x, cc.y, cc.z);
        rlTexCoord2f(0, 0); rlVertex3f(a.x, a.y, a.z);
        rlTexCoord2f(1, 1); rlVertex3f(cc.x, cc.y, cc.z);
        rlTexCoord2f(1, 0); rlVertex3f(d.x, d.y, d.z);
        rlSetTexture(0);
    }

    /* ---- 粒子：不需要由遠到近排序，因為採 additive 混合的粒子佔多數；先畫 alpha 混合的
     *      煙，再把 additive 的疊上去。 ---- */
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < MAX_PARTICLES; i++)
        {
            bp_particle *p = &g_fx.particles[i];
            if (!p->alive) continue;
            if ((pass == 0) == (p->additive != 0)) continue;

            float k = p->age / p->life;
            float size = p->size0 + (p->size1 - p->size0) * k;
            Color c = fade_color(p->color0, p->color1, k);
            if (c.a == 0 || size <= 0.0f) continue;

            Texture2D t = bp_tex((bp_texture_id)p->tex);

            if (p->kind == PTX_SPARK || p->kind == PTX_SHELL)
            {
                /* 沿速度方向拉伸：免費得到運動模糊 */
                float sp = Vector3Length(p->vel);
                float stretch = BP_CLAMP(sp * 0.035f, 1.0f, 4.5f);
                Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
                Vector3 vel = (sp > 0.2f) ? Vector3Scale(p->vel, 1.0f / sp) : fwd;
                Vector3 side = Vector3CrossProduct(vel, fwd);
                float sl = Vector3Length(side);
                if (sl > 0.0001f)
                {
                    side = Vector3Scale(side, size * 0.5f / sl);
                    Vector3 along = Vector3Scale(vel, size * 0.5f * stretch);
                    Vector3 a = Vector3Add(Vector3Add(p->pos, side), along);
                    Vector3 b = Vector3Add(Vector3Subtract(p->pos, side), along);
                    Vector3 cc = Vector3Subtract(Vector3Subtract(p->pos, side), along);
                    Vector3 d = Vector3Subtract(Vector3Add(p->pos, side), along);
                    rlCheckRenderBatchLimit(12);
                    rlSetTexture(t.id);
                    rlColor4ub(c.r, c.g, c.b, c.a);
                    rlNormal3f(fwd.x, fwd.y, fwd.z);
                    rlTexCoord2f(0, 0); rlVertex3f(a.x, a.y, a.z);
                    rlTexCoord2f(0, 1); rlVertex3f(b.x, b.y, b.z);
                    rlTexCoord2f(1, 1); rlVertex3f(cc.x, cc.y, cc.z);
                    rlTexCoord2f(0, 0); rlVertex3f(a.x, a.y, a.z);
                    rlTexCoord2f(1, 1); rlVertex3f(cc.x, cc.y, cc.z);
                    rlTexCoord2f(1, 0); rlVertex3f(d.x, d.y, d.z);
                    rlSetTexture(0);
                    continue;
                }
            }

            emit_quad(cam, t, p->pos, size, size, p->roll, c);
        }
    }

    /* ---- 槍口燈光：一個短命的 additive 光球，讓房間也跟著有反應 ---- */
    if (g_fx.muzzle_flash > 0.02f)
    {
        Texture2D soft = bp_tex(TEX_PARTICLE_SOFT);
        Color c = (Color){ 255, 214, 150, (unsigned char)(g_fx.muzzle_flash * 150.0f) };
        emit_quad(cam, soft, g_fx.muzzle_pos, 2.6f * g_fx.muzzle_flash, 2.6f * g_fx.muzzle_flash, 0.0f, c);
    }
}
