/******************************************************************************************
 * bp_fx.h -- particles, decals, tracers and screen-space combat feedback.
 *
 * Performance contract: the entire particle system is ONE rlgl render batch submission per
 * texture, not one draw call per particle. At 4096 particles that is the difference between
 * ~200 draw calls (unplayable on integrated graphics) and 2.
 *
 * The batch is built manually from camera right/up vectors so particles can carry a roll
 * angle -- DrawBillboardRec cannot, and rotating debris is a large part of why impacts in
 * this genre feel physical.
 ******************************************************************************************/
#ifndef BP_FX_H
#define BP_FX_H

#include "bp_common.h"
#include "../engine/bp_assets.h"   /* bp_texture_id */

typedef enum {
    PTX_SPARK = 0,      /* additive, stretched by velocity */
    PTX_SMOKE,          /* alpha blended, grows and fades  */
    PTX_BLOOD,
    PTX_DEBRIS,         /* gravity-affected, bounces       */
    PTX_SHELL,          /* ejected casing                  */
    PTX_EMBER,
    PTX_FLASH,          /* very short lived, additive      */
    PTX_RING            /* expanding shockwave billboard   */
} bp_particle_kind;

typedef struct {
    Vector3 pos;
    Vector3 vel;
    float   age;
    float   life;
    float   size0;
    float   size1;
    float   roll;
    float   spin;
    Color   color0;
    Color   color1;
    uint8_t kind;
    uint8_t tex;           /* bp_texture_id */
    uint8_t gravity;       /* 0 none, 1 full, 2 half */
    uint8_t additive;
    uint8_t bounce;
    uint8_t alive;
} bp_particle;

typedef struct {
    Vector3 pos;
    Vector3 normal;
    float   size;
    float   age;
    float   life;
    uint8_t tex;
    uint8_t alive;
    float   roll;
    Color   tint;
} bp_decal;

typedef struct {
    Vector3 from;
    Vector3 to;
    float   age;
    float   life;
    float   width;
    Color   color;
    uint8_t alive;
} bp_tracer;

typedef struct {
    bp_particle particles[MAX_PARTICLES];
    int         particle_head;      /* round-robin allocation cursor */
    int         particle_live;

    bp_decal    decals[MAX_DECALS];
    int         decal_head;
    int         decal_live;         /* telemetry for the debug overlay      */

    bp_tracer   tracers[MAX_HITSCAN_TRACERS];
    int         tracer_head;

    /* gameplay feedback (read by the HUD + camera) */
    float       hit_marker;         /* 0..1, spikes on a confirmed hit   */
    float       kill_marker;        /* 0..1, spikes on a kill            */
    float       muzzle_flash;       /* 0..1                              */
    Vector3     muzzle_pos;
    Vector3     muzzle_dir;
    float       shake_amount;
    float       shake_time;
    Vector3     shake_offset;
    float       speedometer;        /* smoothed player speed for post FX */
    int         spawned_this_frame; /* telemetry */
} bp_fx;

extern bp_fx g_fx;

void bp_fx_init(void);
void bp_fx_reset(void);
void bp_fx_update(float dt, Vector3 worldFloorY);
void bp_fx_draw(Camera3D cam);

void bp_fx_particle(bp_particle_kind kind, Vector3 pos, Vector3 vel, float life,
                    float size0, float size1, Color c0, Color c1);

/* Composed emitters -- gameplay code calls these, never raw particles. */
void bp_fx_muzzle_flash(Vector3 pos, Vector3 dir, float intensity);
void bp_fx_impact_wall(Vector3 pos, Vector3 normal, float energy);
void bp_fx_impact_flesh(Vector3 pos, Vector3 dir, float energy);
void bp_fx_impact_metal(Vector3 pos, Vector3 normal, float energy);
void bp_fx_explosion(Vector3 pos, float radius);
void bp_fx_blood_mist(Vector3 pos, Vector3 dir, float amount);
void bp_fx_shell_eject(Vector3 pos, Vector3 right, Vector3 up);
void bp_fx_smoke_trail(Vector3 pos, float amount);
void bp_fx_dust_puff(Vector3 pos, float amount);
void bp_fx_speed_burst(Vector3 pos, Vector3 dir);

void bp_fx_decal(bp_texture_id tex, Vector3 pos, Vector3 normal, float size, float life, Color tint);
void bp_fx_tracer(Vector3 from, Vector3 to, float width, float life, Color c);

void bp_fx_hit_marker(bool kill);
void bp_fx_shake(float amount, float duration);
Vector3 bp_fx_shake_offset(void);

#endif /* BP_FX_H */
