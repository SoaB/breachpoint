/******************************************************************************************
 * bp_assets.h -- procedural asset bank.
 *
 * EVERY visual and typographic asset in the game is generated in code at boot. There are no
 * .png, .ttf, .obj or .wav files in the repository. Rationale:
 *
 *   - Shipping size. The whole Web build (engine + game + assets) is a few hundred KB.
 *   - Art-direction consistency. Palette, texel density and bevel rules live in one place,
 *     so every surface in the game reads as part of the same world.
 *   - Zero licensing surface for a commercial release.
 *   - Iteration speed: a texture change is a code change, reviewable in the same diff as
 *     the gameplay code that depends on it.
 *
 * The bank is a flat struct of handles. Assets are created once in bp_assets_init() and
 * destroyed in bp_assets_shutdown(); nothing here is reloaded mid-game.
 ******************************************************************************************/
#ifndef BP_ASSETS_H
#define BP_ASSETS_H

#include "bp_common.h"

/* -------------------------------------------------------------------------------------- */
/* Texture ids                                                                             */
/* -------------------------------------------------------------------------------------- */
typedef enum {
    TEX_NONE = -1,

    /* architecture */
    TEX_WALL_BRICK, TEX_WALL_METAL, TEX_WALL_CONCRETE, TEX_WALL_PANEL, TEX_WALL_RUSTY,
    TEX_FLOOR_GRATE, TEX_FLOOR_TILE, TEX_FLOOR_CONCRETE, TEX_FLOOR_HAZARD,
    TEX_CEILING, TEX_TRIM, TEX_CRATE, TEX_BARREL, TEX_PILLAR,

    /* emissive / detail */
    TEX_LIGHT_PANEL, TEX_SCREEN, TEX_VENT, TEX_PIPE, TEX_BLOOD_SPLAT, TEX_SCORCH,
    TEX_BULLET_HOLE, TEX_LOGO,

    /* billboards & fx */
    TEX_PARTICLE_SOFT, TEX_PARTICLE_HARD, TEX_SPARK, TEX_SMOKE, TEX_RING,
    TEX_CROSSHAIR, TEX_GRADIENT, TEX_NOISE,

    TEX_COUNT
} bp_texture_id;

/* -------------------------------------------------------------------------------------- */
/* Model ids                                                                               */
/* -------------------------------------------------------------------------------------- */
typedef enum {
    MDL_NONE = -1,

    /* enemy body parts (drawn with per-part matrices so limbs can be animated) */
    MDL_ENEMY_TORSO, MDL_ENEMY_HEAD, MDL_ENEMY_ARM, MDL_ENEMY_LEG,
    MDL_HOUND_BODY, MDL_HOUND_HEAD, MDL_HOUND_LEG,
    MDL_BRUTE_TORSO, MDL_BRUTE_ARM, MDL_BRUTE_LEG, MDL_BRUTE_HEAD,
    MDL_TURRET_BASE, MDL_TURRET_BARREL,

    /* viewmodel parts */
    MDL_GUN_PISTOL_BODY, MDL_GUN_PISTOL_SLIDE, MDL_GUN_HAND,
    MDL_GUN_SHOTGUN_BODY, MDL_GUN_SHOTGUN_PUMP, MDL_GUN_SHOTGUN_BARREL,
    MDL_GUN_SMG_BODY, MDL_GUN_SMG_MAG, MDL_GUN_SMG_STOCK,
    MDL_GUN_RAIL_BODY, MDL_GUN_RAIL_COIL, MDL_GUN_RAIL_CORE,
    MDL_GUN_ROCKET_TUBE, MDL_GUN_ROCKET_SIGHT,

    /* world props */
    MDL_PICKUP_HEALTH, MDL_PICKUP_ARMOR, MDL_PICKUP_AMMO, MDL_PICKUP_WEAPON,
    MDL_PROJECTILE_ROCKET, MDL_PROJECTILE_PLASMA,
    MDL_BARREL, MDL_CRATE,

    MDL_COUNT
} bp_model_id;

/* -------------------------------------------------------------------------------------- */
/* Per-model render metadata                                                               */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    Model   model;
    Vector3 origin;        /* where the "socket" sits relative to model space              */
    Vector3 half_extent;     /* for cheap shadow / bounds                                  */
    Color   tint;
    bool    valid;
} bp_model;

typedef struct {
    Texture2D tex[TEX_COUNT];
    bp_model  mdl[MDL_COUNT];

    Font      font_small;    /* 8px  -- HUD labels, debug          */
    Font      font_main;     /* 16px -- body text, menus           */
    Font      font_big;      /* 32px -- headings                   */
    Font      font_huge;     /* 64px -- title, kill counter        */
    bool      fonts_valid;

    /* palette (used by HUD, particles, level lighting) */
    Color     pal_accent;
    Color     pal_accent2;
    Color     pal_danger;
    Color     pal_health;
    Color     pal_armor;
    Color     pal_ammo;
    Color     pal_bg;
    Color     pal_panel;
    Color     pal_text;
    Color     pal_text_dim;

    bool      ready;
    float     load_ms;
} bp_assets;

extern bp_assets g_assets;

void       bp_assets_init(void);
void       bp_assets_shutdown(void);

Texture2D  bp_tex(bp_texture_id id);
bool       bp_model_valid(bp_model_id id);
Model      bp_model_get(bp_model_id id);
Color      bp_pal(int index);          /* 0..N indexed palette ramp for lighting */

/* Convenience: draw a model part with a transform built from position/rotation/scale. */
void bp_draw_model(bp_model_id id, Vector3 position, Vector3 axis, float angleDeg,
                   Vector3 scale, Color tint);

#endif /* BP_ASSETS_H */
