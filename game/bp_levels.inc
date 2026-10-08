/******************************************************************************************
 * bp_levels.inc -- hand-authored level geometry, included by bp_world.c
 *
 * Space: right-handed, metric. 1 unit = 1 metre, y = 0 is the walkable floor plane, the
 * player's feet are at y and their eye at y + 1.62.
 *
 *   ADD_SOLID(x, y, z, w, h, d, mat)   box with MIN corner (x,y,z) and size (w,h,d)
 *   ADD_SOLID_EXPLODE(..., health)     destructible (barrel)
 *   ADD_TRIGGER(kind, x, y, z, yaw, param, respawn, radius)
 *   ADD_MOVER(solidIndex, fx,fy,fz, tx,ty,tz, seconds, sfx)
 *
 * Traversal budget every map must respect:
 *   - auto step-up is 0.55 m, so any ledge <= 0.55 m is walkable and needs no jump
 *   - a standing jump raises the feet ~1.3 m, a running jump ~1.5 m
 *   - therefore every height band in a map must be reachable by a chain of <= 1.4 m steps
 * Each level below lists its height bands so the invariant can be checked by reading it.
 ******************************************************************************************/
#ifndef BP_LEVELS_INC
#define BP_LEVELS_INC

#define ARENA_MIN_X  (-34.0f)
#define ARENA_MAX_X  ( 34.0f)
#define ARENA_MIN_Z  (-34.0f)
#define ARENA_MAX_Z  ( 34.0f)
#define WALL_T       (1.0f)

/* -------------------------------------------------------------------------------------- */
/* Shared shell: floor, four perimeter walls, skirting trim and ceiling light strips.       */
/* Reused by all three maps so they differ in interior rhythm, not in boilerplate.          */
/* -------------------------------------------------------------------------------------- */
static void level_shell(float height, int lightStrips)
{
    const float mnx = ARENA_MIN_X, mxx = ARENA_MAX_X;
    const float mnz = ARENA_MIN_Z, mxz = ARENA_MAX_Z;
    const float w = mxx - mnx, d = mxz - mnz;

    ADD_SOLID(mnx, -1.0f, mnz, w, 1.0f, d, MAT_TILE);

    ADD_SOLID(mnx, 0.0f, mnz - WALL_T, w, height, WALL_T, MAT_CONCRETE);  /* north */
    ADD_SOLID(mnx, 0.0f, mxz,          w, height, WALL_T, MAT_CONCRETE);  /* south */
    ADD_SOLID(mnx - WALL_T, 0.0f, mnz, WALL_T, height, d, MAT_CONCRETE);  /* west  */
    ADD_SOLID(mxx, 0.0f, mnz,          WALL_T, height, d, MAT_CONCRETE);  /* east  */

    /* Skirting: a dark trim band at the base of every wall. It gives the eye a continuous
     * horizontal reference at ankle height, which is what lets a player judge a ledge
     * without looking down -- small detail, large effect on movement confidence. */
    ADD_SOLID(mnx, 0.0f, mnz - 0.14f, w, 0.50f, 0.14f, MAT_TRIM);
    ADD_SOLID(mnx, 0.0f, mxz,         w, 0.50f, 0.14f, MAT_TRIM);
    ADD_SOLID(mnx - 0.14f, 0.0f, mnz, 0.14f, 0.50f, d, MAT_TRIM);
    ADD_SOLID(mxx, 0.0f, mnz,         0.14f, 0.50f, d, MAT_TRIM);

    for (int i = 0; i < lightStrips; i++)
    {
        float t = lightStrips > 1 ? (float)i / (float)(lightStrips - 1) : 0.5f;
        float z = mnz + 5.0f + t * (d - 10.0f);
        ADD_SOLID(mnx + 7.0f, height - 0.30f, z - 1.5f, w - 14.0f, 0.30f, 3.0f, MAT_LIGHT);
    }
}

/* ====================================================================================== */
/* LEVEL 0 :: FOUNDRY                                                                      */
/* ---------------------------------------------------------------------------------------- */
/* Height bands:  floor 0.0 -> pyramid 0.55 -> 1.10 -> 1.65 (all steps <= 0.55, walkable)
 *                crate stacks reach 2.20 from the floor by jumping.
 *
 * Design goals
 *   - symmetrical so neither spawn has an advantage in a versus-style score attack
 *   - the stepped pyramid in the centre is walk-up from all four sides: it is a movement
 *     reward, never a free perch
 *   - sightlines are broken every 12-16 m so hitscan cannot dominate
 *   - barrels sit adjacent to cover so baiting an enemy past one is a real play
 * ====================================================================================== */
static void level_foundry(void)
{
    level_shell(11.0f, 5);

    /* --- central stepped pyramid (three walk-up tiers) --- */
    ADD_SOLID(-7.0f, 0.0f,  -7.0f, 14.0f, 0.55f, 14.0f, MAT_METAL);
    ADD_SOLID(-5.2f, 0.55f, -5.2f, 10.4f, 0.55f, 10.4f, MAT_PANEL);
    ADD_SOLID(-3.4f, 1.10f, -3.4f,  6.8f, 0.55f,  6.8f, MAT_METAL);
    ADD_SOLID(-3.4f, 1.65f, -3.4f,  6.8f, 0.10f,  0.30f, MAT_TRIM);
    ADD_SOLID(-3.4f, 1.65f,  3.10f, 6.8f, 0.10f,  0.30f, MAT_TRIM);
    ADD_SOLID(-3.4f, 1.65f, -3.4f,  0.30f, 0.10f, 6.8f, MAT_TRIM);
    ADD_SOLID( 3.10f, 1.65f,-3.4f,  0.30f, 0.10f, 6.8f, MAT_TRIM);

    /* --- four corner pillars: the primary cover rhythm --- */
    ADD_SOLID(-16.0f, 0.0f, -16.0f, 3.0f, 5.20f, 3.0f, MAT_PILLAR);
    ADD_SOLID( 13.0f, 0.0f, -16.0f, 3.0f, 5.20f, 3.0f, MAT_PILLAR);
    ADD_SOLID(-16.0f, 0.0f,  13.0f, 3.0f, 5.20f, 3.0f, MAT_PILLAR);
    ADD_SOLID( 13.0f, 0.0f,  13.0f, 3.0f, 5.20f, 3.0f, MAT_PILLAR);

    /* --- crate stacks: two high (2.2 m), reachable from the floor with one jump --- */
    ADD_SOLID(-22.0f, 0.00f, -2.0f, 2.2f, 1.10f, 2.2f, MAT_CRATE);
    ADD_SOLID(-22.0f, 1.10f, -2.0f, 2.2f, 1.10f, 2.2f, MAT_CRATE);
    ADD_SOLID( 19.8f, 0.00f, -2.0f, 2.2f, 1.10f, 2.2f, MAT_CRATE);
    ADD_SOLID( 19.8f, 1.10f, -2.0f, 2.2f, 1.10f, 2.2f, MAT_CRATE);
    ADD_SOLID( -2.0f, 0.00f, -24.0f, 2.2f, 1.10f, 2.2f, MAT_CRATE);
    ADD_SOLID( -2.0f, 0.00f,  21.8f, 2.2f, 1.10f, 2.2f, MAT_CRATE);

    /* --- explosive barrels --- */
    ADD_SOLID_EXPLODE(-12.4f, 0.0f, -12.4f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 28.0f);
    ADD_SOLID_EXPLODE( 11.6f, 0.0f, -12.4f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 28.0f);
    ADD_SOLID_EXPLODE(-12.4f, 0.0f,  11.6f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 28.0f);
    ADD_SOLID_EXPLODE( 11.6f, 0.0f,  11.6f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 28.0f);
    ADD_SOLID_EXPLODE(-27.0f, 0.0f,  26.0f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 28.0f);
    ADD_SOLID_EXPLODE( 26.2f, 0.0f, -27.2f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 28.0f);

    /* --- hazard strips: punish the greedy straight line across the middle --- */
    ADD_SOLID(-30.0f, 0.0f, -1.0f, 8.0f, 0.06f, 2.0f, MAT_HAZARD);
    ADD_SOLID( 22.0f, 0.0f, -1.0f, 8.0f, 0.06f, 2.0f, MAT_HAZARD);

    /* --- wall dressing --- */
    ADD_SOLID(-33.0f,  3.4f, -6.0f, 0.16f, 2.4f, 4.6f, MAT_SCREEN);
    ADD_SOLID(-33.0f,  3.4f,  1.6f, 0.16f, 2.4f, 4.6f, MAT_SCREEN);
    ADD_SOLID( 32.84f, 3.4f, -6.0f, 0.16f, 2.4f, 4.6f, MAT_SCREEN);
    ADD_SOLID( 32.84f, 3.4f,  1.6f, 0.16f, 2.4f, 4.6f, MAT_SCREEN);
    ADD_SOLID(-8.0f, 4.2f, -33.0f, 4.0f, 2.0f, 0.16f, MAT_VENT);
    ADD_SOLID( 4.0f, 4.2f, -33.0f, 4.0f, 2.0f, 0.16f, MAT_VENT);
    ADD_SOLID(-8.0f, 4.2f, 32.84f, 4.0f, 2.0f, 0.16f, MAT_VENT);
    ADD_SOLID( 4.0f, 4.2f, 32.84f, 4.0f, 2.0f, 0.16f, MAT_VENT);
    ADD_SOLID(-33.0f, 8.0f, -30.0f, 0.4f, 0.4f, 60.0f, MAT_PIPE);
    ADD_SOLID( 32.6f, 8.0f, -30.0f, 0.4f, 0.4f, 60.0f, MAT_PIPE);

    /* --- blast door in the south-east corner, opened by a floor pad --- */
    int door = g_world.solid_count;
    ADD_SOLID(28.0f, 0.0f, 24.0f, 0.6f, 4.20f, 5.0f, MAT_RUST);
    g_world.solids[door].dynamic = true;
    ADD_MOVER(door, 28.0f, 0.0f, 24.0f, 28.0f, 4.30f, 24.0f, 1.10f, SFX_DOOR);
    ADD_TRIGGER(TRIG_DOOR, 24.0f, 0.05f, 26.5f, 0.0f, 0, 0.0f, 3.0f);

    /* --- triggers --- */
    ADD_TRIGGER(TRIG_PLAYER_START, 0.0f, 0.05f, 27.0f, 180.0f, 0, 0.0f, 1.0f);
    ADD_TRIGGER(TRIG_EXIT,         0.0f, 1.80f, 0.0f,   0.0f, 0, 0.0f, 2.4f);

    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -27.0f, 0.05f, -27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  27.0f, 0.05f, -27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -27.0f, 0.05f,  27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  27.0f, 0.05f,  27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 0.05f, -28.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 0.05f,  28.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -28.0f, 0.05f,   0.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  28.0f, 0.05f,   0.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -20.0f, 2.25f, -20.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  20.0f, 2.25f,  20.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 1.80f,   0.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -14.0f, 0.05f,  20.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  14.0f, 0.05f, -20.0f, 0.0f, 0, 0.0f, 1.2f);

    ADD_TRIGGER(TRIG_HEALTH_SPAWN, -24.0f, 0.10f,  -8.0f, 0.0f, 25, 22.0f, 0.9f);
    ADD_TRIGGER(TRIG_HEALTH_SPAWN,  24.0f, 0.10f,   8.0f, 0.0f, 25, 22.0f, 0.9f);
    ADD_TRIGGER(TRIG_HEALTH_SPAWN,   0.0f, 0.10f, -20.0f, 0.0f, 50, 40.0f, 0.9f);
    ADD_TRIGGER(TRIG_ARMOR_SPAWN,    0.0f, 0.10f,  20.0f, 0.0f, 50, 35.0f, 0.9f);
    ADD_TRIGGER(TRIG_AMMO_SPAWN,   -18.0f, 0.10f,  18.0f, 0.0f, 40, 16.0f, 0.9f);
    ADD_TRIGGER(TRIG_AMMO_SPAWN,    18.0f, 0.10f, -18.0f, 0.0f, 40, 16.0f, 0.9f);
    ADD_TRIGGER(TRIG_AMMO_SPAWN,   -30.0f, 0.10f,  30.0f, 0.0f, 30, 14.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN,  10.0f, 0.10f,  22.0f, 0.0f, WPN_SHOTGUN, 30.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN, -10.0f, 0.10f, -22.0f, 0.0f, WPN_SMG, 45.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN,  28.0f, 0.10f,   0.0f, 0.0f, WPN_RAIL, 60.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN, -28.0f, 0.10f,   0.0f, 0.0f, WPN_ROCKET, 60.0f, 0.9f);

    ADD_TRIGGER(TRIG_SECRET, -31.0f, 0.05f, -31.0f, 0.0f, 100, 0.0f, 1.5f);
}

/* ====================================================================================== */
/* LEVEL 1 :: SLAG PIT                                                                     */
/* ---------------------------------------------------------------------------------------- */
/* Height bands:  pit floor 0.0 -> deck 2.2 -> catwalk 6.9
 *                pit -> deck : 4 steps of 0.55 (walk-up, no jump)
 *                deck/pit -> catwalk : 10-step towers of 0.69 (needs small hops -- this is
 *                the intended skill gate for the map's best sightline)
 *
 * Design goals
 *   - the catwalk owns the only long sightline, so it is the contested objective
 *   - the pit floor is fast to cross but has no cover and a hazard grate: risk vs time
 *   - barrels clustered in the pit can be detonated from the catwalk -- the signature play
 * ====================================================================================== */
static void level_slagpit(void)
{
    level_shell(15.0f, 6);

    /* --- pit floor: hazard grate over the whole 32x32 centre --- */
    ADD_SOLID(-16.0f, 0.00f, -16.0f, 32.0f, 0.05f, 32.0f, MAT_HAZARD);
    ADD_SOLID(-16.0f, 0.05f, -16.0f, 32.0f, 0.01f, 32.0f, MAT_GRATE);

    /* --- main deck: four land masses around the pit, top surface at y = 2.2 --- */
    ADD_SOLID(-34.0f, 1.8f, -34.0f, 18.0f, 0.4f, 68.0f, MAT_METAL);  /* west  */
    ADD_SOLID( 16.0f, 1.8f, -34.0f, 18.0f, 0.4f, 68.0f, MAT_METAL);  /* east  */
    ADD_SOLID(-16.0f, 1.8f, -34.0f, 32.0f, 0.4f, 18.0f, MAT_METAL);  /* north */
    ADD_SOLID(-16.0f, 1.8f,  16.0f, 32.0f, 0.4f, 18.0f, MAT_METAL);  /* south */

    /* deck edge rails: read the drop without falling in by accident */
    ADD_SOLID(-16.30f, 2.2f, -16.0f, 0.30f, 1.05f, 32.0f, MAT_TRIM);
    ADD_SOLID( 16.00f, 2.2f, -16.0f, 0.30f, 1.05f, 32.0f, MAT_TRIM);
    ADD_SOLID(-16.0f, 2.2f, -16.30f, 32.0f, 1.05f, 0.30f, MAT_TRIM);
    ADD_SOLID(-16.0f, 2.2f,  16.00f, 32.0f, 1.05f, 0.30f, MAT_TRIM);

    /* --- pit -> deck: four 0.55 m walk-up steps on each of two faces --- */
    for (int side = 0; side < 2; side++)
    {
        float zBase = (side == 0) ? -14.0f : 10.0f;   /* north face / south face */
        for (int i = 0; i < 4; i++)
        {
            float h = 0.55f * (float)(i + 1);
            ADD_SOLID(-15.0f + (float)i * 3.0f, 0.0f, zBase, 3.0f, h, 4.0f, MAT_RUST);
        }
    }

    /* --- catwalk at 6.9, spanning the pit north and south --- */
    ADD_SOLID(-7.0f, 6.6f, -16.0f, 14.0f, 0.30f, 7.0f, MAT_GRATE);
    ADD_SOLID(-7.0f, 6.6f,   9.0f, 14.0f, 0.30f, 7.0f, MAT_GRATE);
    ADD_SOLID(-7.30f, 6.90f, -16.0f, 0.30f, 1.05f, 7.0f, MAT_TRIM);
    ADD_SOLID( 6.70f, 6.90f, -16.0f, 0.30f, 1.05f, 7.0f, MAT_TRIM);
    ADD_SOLID(-7.30f, 6.90f,   9.0f, 0.30f, 1.05f, 7.0f, MAT_TRIM);
    ADD_SOLID( 6.70f, 6.90f,   9.0f, 0.30f, 1.05f, 7.0f, MAT_TRIM);

    /* --- stair towers from the pit floor all the way to the catwalk.
     *        10 risers of 0.69 m: above the 0.55 step-up limit, so the climb demands
     *        rhythm. Two towers (NW and SE) keep the catwalk approachable from either side. */
    for (int i = 0; i < 10; i++)
    {
        float h = 0.69f * (float)(i + 1);
        ADD_SOLID(-15.5f, 0.0f, -15.5f + (float)i * 1.20f, 2.4f, h, 1.20f, MAT_METAL);
        ADD_SOLID( 13.1f, 0.0f,  15.5f - (float)i * 1.20f, 2.4f, h, 1.20f, MAT_METAL);
    }

    /* --- support columns from pit floor to ceiling --- */
    ADD_SOLID(-9.0f, 0.06f, -9.0f, 1.6f, 14.0f, 1.6f, MAT_PILLAR);
    ADD_SOLID( 7.4f, 0.06f, -9.0f, 1.6f, 14.0f, 1.6f, MAT_PILLAR);
    ADD_SOLID(-9.0f, 0.06f,  7.4f, 1.6f, 14.0f, 1.6f, MAT_PILLAR);
    ADD_SOLID( 7.4f, 0.06f,  7.4f, 1.6f, 14.0f, 1.6f, MAT_PILLAR);

    /* --- machinery blocks on the deck --- */
    ADD_SOLID(-31.0f, 2.2f, -31.0f, 5.0f, 2.40f, 5.0f, MAT_PANEL);
    ADD_SOLID( 26.0f, 2.2f, -31.0f, 5.0f, 2.40f, 5.0f, MAT_PANEL);
    ADD_SOLID(-31.0f, 2.2f,  26.0f, 5.0f, 2.40f, 5.0f, MAT_PANEL);
    ADD_SOLID( 26.0f, 2.2f,  26.0f, 5.0f, 2.40f, 5.0f, MAT_PANEL);
    ADD_SOLID(-26.0f, 2.2f,  -3.0f, 3.4f, 1.50f, 6.0f, MAT_RUST);
    ADD_SOLID( 22.6f, 2.2f,  -3.0f, 3.4f, 1.50f, 6.0f, MAT_RUST);
    ADD_SOLID( -3.0f, 2.2f, -31.0f, 6.0f, 1.50f, 3.4f, MAT_RUST);
    ADD_SOLID( -3.0f, 2.2f,  27.6f, 6.0f, 1.50f, 3.4f, MAT_RUST);

    /* --- barrels clustered in the pit: the map's signature play is sniping these from
     *        the catwalk to clear everything standing on the grate --- */
    ADD_SOLID_EXPLODE(-10.4f, 0.06f, -10.4f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 30.0f);
    ADD_SOLID_EXPLODE( -8.8f, 0.06f, -10.4f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 30.0f);
    ADD_SOLID_EXPLODE( -9.6f, 0.06f,  -8.8f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 30.0f);
    ADD_SOLID_EXPLODE(  9.6f, 0.06f,   9.6f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 30.0f);
    ADD_SOLID_EXPLODE(  8.0f, 0.06f,   9.6f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 30.0f);
    ADD_SOLID_EXPLODE(  8.8f, 0.06f,  11.2f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 30.0f);
    ADD_SOLID_EXPLODE(  0.0f, 0.06f,   0.0f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 30.0f);
    ADD_SOLID_EXPLODE(-28.0f, 2.20f,   8.0f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 30.0f);
    ADD_SOLID_EXPLODE( 27.2f, 2.20f,  -8.8f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 30.0f);

    /* --- wall dressing --- */
    ADD_SOLID(-33.0f,  5.0f, -12.0f, 0.16f, 3.0f, 6.0f, MAT_SCREEN);
    ADD_SOLID(-33.0f,  5.0f,   6.0f, 0.16f, 3.0f, 6.0f, MAT_SCREEN);
    ADD_SOLID( 32.84f, 5.0f, -12.0f, 0.16f, 3.0f, 6.0f, MAT_SCREEN);
    ADD_SOLID( 32.84f, 5.0f,   6.0f, 0.16f, 3.0f, 6.0f, MAT_SCREEN);
    ADD_SOLID(-12.0f, 9.5f, -33.0f, 6.0f, 2.4f, 0.16f, MAT_VENT);
    ADD_SOLID(  6.0f, 9.5f, -33.0f, 6.0f, 2.4f, 0.16f, MAT_VENT);
    ADD_SOLID(-12.0f, 9.5f, 32.84f, 6.0f, 2.4f, 0.16f, MAT_VENT);
    ADD_SOLID(-33.0f, 11.5f, -30.0f, 0.4f, 0.4f, 60.0f, MAT_PIPE);
    ADD_SOLID( 32.6f, 11.5f, -30.0f, 0.4f, 0.4f, 60.0f, MAT_PIPE);

    /* --- freight lift: a mover that carries the player from the deck to the catwalk --- */
    int lift = g_world.solid_count;
    ADD_SOLID(-2.0f, 2.2f, 12.0f, 4.0f, 0.30f, 4.0f, MAT_METAL);
    g_world.solids[lift].dynamic = true;
    ADD_MOVER(lift, -2.0f, 2.2f, 12.0f, -2.0f, 6.6f, 12.0f, 1.60f, SFX_DOOR);
    ADD_TRIGGER(TRIG_DOOR, 0.0f, 2.30f, 13.5f, 0.0f, 0, 0.0f, 2.6f);

    /* --- triggers --- */
    ADD_TRIGGER(TRIG_PLAYER_START, -27.0f, 2.25f,  27.0f,  45.0f, 0, 0.0f, 1.0f);
    ADD_TRIGGER(TRIG_EXIT,           0.0f, 7.00f, -12.5f, 180.0f, 0, 0.0f, 2.4f);

    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  27.0f, 2.25f,  27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  27.0f, 2.25f, -27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -27.0f, 2.25f, -27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 0.10f,   0.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -12.0f, 0.10f,  12.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  12.0f, 0.10f, -12.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 7.00f,  12.5f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 7.00f, -12.5f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -28.0f, 2.25f,   0.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  28.0f, 2.25f,   0.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 2.25f, -28.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 2.25f,  28.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -22.0f, 0.10f, -22.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  22.0f, 0.10f,  22.0f, 0.0f, 0, 0.0f, 1.2f);

    ADD_TRIGGER(TRIG_HEALTH_SPAWN, -28.0f, 2.30f, -28.0f, 0.0f, 25, 20.0f, 0.9f);
    ADD_TRIGGER(TRIG_HEALTH_SPAWN,  28.0f, 2.30f,  28.0f, 0.0f, 25, 20.0f, 0.9f);
    ADD_TRIGGER(TRIG_HEALTH_SPAWN,   0.0f, 0.10f,   0.0f, 0.0f, 50, 30.0f, 0.9f);
    ADD_TRIGGER(TRIG_ARMOR_SPAWN,    0.0f, 7.05f,  12.5f, 0.0f, 50, 32.0f, 0.9f);
    ADD_TRIGGER(TRIG_ARMOR_SPAWN,  -28.0f, 2.30f,  10.0f, 0.0f, 25, 30.0f, 0.9f);
    ADD_TRIGGER(TRIG_AMMO_SPAWN,   -22.0f, 2.30f, -22.0f, 0.0f, 40, 15.0f, 0.9f);
    ADD_TRIGGER(TRIG_AMMO_SPAWN,    22.0f, 2.30f,  22.0f, 0.0f, 40, 15.0f, 0.9f);
    ADD_TRIGGER(TRIG_AMMO_SPAWN,     0.0f, 0.10f, -14.0f, 0.0f, 35, 15.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN,   0.0f, 7.05f, -12.5f, 0.0f, WPN_RAIL, 55.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN, -28.0f, 2.30f,  28.0f, 0.0f, WPN_SHOTGUN, 28.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN,  28.0f, 2.30f, -28.0f, 0.0f, WPN_SMG, 40.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN,  12.0f, 0.10f,  12.0f, 0.0f, WPN_ROCKET, 60.0f, 0.9f);

    ADD_TRIGGER(TRIG_SECRET, -31.5f, 0.10f, 31.5f, 0.0f, 100, 0.0f, 1.6f);
}

/* ====================================================================================== */
/* LEVEL 2 :: CONDUIT                                                                      */
/* ---------------------------------------------------------------------------------------- */
/* Height bands:  floor 0.0 -> corner-block cap 3.6 -> core balcony 7.1
 *                crate stairs give a walk-up chain 0.6 -> 1.2 -> ... -> 6.0 to the core
 *
 * Design goals
 *   - no sightline longer than 22 m, so the shotgun and SMG are the dominant tools
 *   - a ring corridor around a solid core: you can always loop, never get cornered
 *   - one exposed "duel lane" overlooked from two balconies, so committing to it is a
 *     decision with a cost
 * ====================================================================================== */
static void level_conduit(void)
{
    level_shell(9.0f, 4);

    /* --- solid core with a walkable balcony on top --- */
    ADD_SOLID(-9.0f, 0.0f, -9.0f, 18.0f, 6.5f, 18.0f, MAT_PANEL);
    ADD_SOLID(-9.6f, 6.5f, -9.6f, 19.2f, 0.6f, 19.2f, MAT_TRIM);

    /* --- crate stairs up the west and east faces of the core: six 0.6 m risers each.
     *        0.6 > 0.55 step-up so the player hops up them; that rhythm is deliberate, it
     *        makes reaching the balcony an audible, committable action --- */
    for (int i = 0; i < 6; i++)
    {
        ADD_SOLID(-12.4f, 0.0f, -3.0f + (float)i * 2.10f, 2.2f, 0.60f * (float)(i + 1), 2.10f, MAT_CRATE);
        ADD_SOLID( 10.2f, 0.0f,  3.0f - (float)i * 2.10f, 2.2f, 0.60f * (float)(i + 1), 2.10f, MAT_CRATE);
    }

    /* --- corner blocks with jump-up caps --- */
    ADD_SOLID(-25.0f, 0.0f, -25.0f, 7.0f, 3.20f, 7.0f, MAT_BRICK);
    ADD_SOLID( 18.0f, 0.0f, -25.0f, 7.0f, 3.20f, 7.0f, MAT_BRICK);
    ADD_SOLID(-25.0f, 0.0f,  18.0f, 7.0f, 3.20f, 7.0f, MAT_BRICK);
    ADD_SOLID( 18.0f, 0.0f,  18.0f, 7.0f, 3.20f, 7.0f, MAT_BRICK);
    ADD_SOLID(-24.0f, 3.20f, -24.0f, 5.0f, 0.40f, 5.0f, MAT_TRIM);
    ADD_SOLID( 19.0f, 3.20f, -24.0f, 5.0f, 0.40f, 5.0f, MAT_TRIM);
    ADD_SOLID(-24.0f, 3.20f,  19.0f, 5.0f, 0.40f, 5.0f, MAT_TRIM);
    ADD_SOLID( 19.0f, 3.20f,  19.0f, 5.0f, 0.40f, 5.0f, MAT_TRIM);
    /* ramps onto the corner blocks so they are not pure jump-checks */
    ADD_SOLID(-25.0f, 0.0f, -18.0f, 3.0f, 0.55f, 3.0f, MAT_METAL);
    ADD_SOLID(-25.0f, 0.55f, -21.0f, 3.0f, 0.55f, 3.0f, MAT_METAL);
    ADD_SOLID(-25.0f, 1.10f, -24.0f, 3.0f, 0.55f, 3.0f, MAT_METAL);
    ADD_SOLID(-25.0f, 1.65f, -25.0f, 3.0f, 0.55f, 1.00f, MAT_METAL);
    ADD_SOLID( 22.0f, 0.0f,  15.0f, 3.0f, 0.55f, 3.0f, MAT_METAL);
    ADD_SOLID( 22.0f, 0.55f,  18.0f, 3.0f, 0.55f, 3.0f, MAT_METAL);
    ADD_SOLID( 22.0f, 1.10f,  21.0f, 3.0f, 0.55f, 3.0f, MAT_METAL);
    ADD_SOLID( 22.0f, 1.65f,  24.0f, 3.0f, 0.55f, 1.00f, MAT_METAL);

    /* --- duel lane: two low walls flanking the north corridor --- */
    ADD_SOLID(-6.0f, 0.0f, -21.0f, 12.0f, 1.25f, 0.90f, MAT_METAL);
    ADD_SOLID(-6.0f, 0.0f, -15.0f, 12.0f, 1.25f, 0.90f, MAT_METAL);

    /* --- ring corridor pillars --- */
    ADD_SOLID(-17.0f, 0.0f, -2.0f, 1.8f, 7.0f, 1.8f, MAT_PILLAR);
    ADD_SOLID( 15.2f, 0.0f, -2.0f, 1.8f, 7.0f, 1.8f, MAT_PILLAR);
    ADD_SOLID( -2.0f, 0.0f, 15.0f, 1.8f, 7.0f, 1.8f, MAT_PILLAR);
    ADD_SOLID( -2.0f, 0.0f,-19.0f, 1.8f, 7.0f, 1.8f, MAT_PILLAR);

    /* --- barrels --- */
    ADD_SOLID_EXPLODE(-19.6f, 0.0f,  10.0f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 26.0f);
    ADD_SOLID_EXPLODE( 18.8f, 0.0f, -10.8f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 26.0f);
    ADD_SOLID_EXPLODE(  4.0f, 0.0f,  21.0f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 26.0f);
    ADD_SOLID_EXPLODE( -5.2f, 0.0f, -23.0f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 26.0f);
    ADD_SOLID_EXPLODE(-21.8f, 3.60f, -21.8f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 26.0f);
    ADD_SOLID_EXPLODE( 21.0f, 3.60f,  21.0f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 26.0f);
    ADD_SOLID_EXPLODE(  6.0f, 7.10f,   6.0f, 0.75f, 1.15f, 0.75f, MAT_BARREL, 26.0f);

    /* --- hazard grating across the south corridor --- */
    ADD_SOLID(-14.0f, 0.0f, 22.0f, 28.0f, 0.05f, 3.0f, MAT_HAZARD);

    /* --- wall dressing --- */
    ADD_SOLID(-31.0f,  3.0f, -4.0f, 0.16f, 2.2f, 5.0f, MAT_SCREEN);
    ADD_SOLID( 30.84f, 3.0f, -4.0f, 0.16f, 2.2f, 5.0f, MAT_SCREEN);
    ADD_SOLID( -4.0f, 3.0f, -31.0f, 5.0f, 2.2f, 0.16f, MAT_VENT);
    ADD_SOLID( -4.0f, 3.0f, 30.84f, 5.0f, 2.2f, 0.16f, MAT_VENT);
    ADD_SOLID(-31.0f, 6.2f, -28.0f, 0.4f, 0.4f, 56.0f, MAT_PIPE);
    ADD_SOLID( 30.6f, 6.2f, -28.0f, 0.4f, 0.4f, 56.0f, MAT_PIPE);

    /* --- sliding shutter guarding the exit on the core balcony --- */
    int shutter = g_world.solid_count;
    ADD_SOLID(-3.0f, 7.10f, -9.6f, 6.0f, 2.20f, 0.40f, MAT_RUST);
    g_world.solids[shutter].dynamic = true;
    ADD_MOVER(shutter, -3.0f, 7.10f, -9.6f, -3.0f, 9.30f, -9.6f, 1.40f, SFX_DOOR);
    ADD_TRIGGER(TRIG_DOOR, 0.0f, 0.05f, -24.0f, 0.0f, 0, 0.0f, 2.4f);

    /* --- triggers --- */
    ADD_TRIGGER(TRIG_PLAYER_START, -27.0f, 0.05f,  27.0f,  45.0f, 0, 0.0f, 1.0f);
    ADD_TRIGGER(TRIG_EXIT,           0.0f, 7.15f,  -6.0f, 180.0f, 0, 0.0f, 2.2f);

    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  27.0f, 0.05f, -27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  27.0f, 0.05f,  27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -27.0f, 0.05f, -27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 0.05f, -27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 0.05f,  27.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -27.0f, 0.05f,   0.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  27.0f, 0.05f,   0.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -21.0f, 3.65f, -21.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  21.0f, 3.65f, -21.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  21.0f, 3.65f,  21.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN, -21.0f, 3.65f,  21.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 7.15f,   6.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   0.0f, 7.15f,  -6.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,  -6.0f, 0.05f,  19.0f, 0.0f, 0, 0.0f, 1.2f);
    ADD_TRIGGER(TRIG_ENEMY_SPAWN,   6.0f, 0.05f, -18.0f, 0.0f, 0, 0.0f, 1.2f);

    ADD_TRIGGER(TRIG_HEALTH_SPAWN,  27.0f, 0.10f,  27.0f, 0.0f, 25, 18.0f, 0.9f);
    ADD_TRIGGER(TRIG_HEALTH_SPAWN, -27.0f, 0.10f, -27.0f, 0.0f, 25, 18.0f, 0.9f);
    ADD_TRIGGER(TRIG_HEALTH_SPAWN,   0.0f, 7.20f,   0.0f, 0.0f, 50, 35.0f, 0.9f);
    ADD_TRIGGER(TRIG_ARMOR_SPAWN,   21.0f, 3.70f,  21.0f, 0.0f, 50, 30.0f, 0.9f);
    ADD_TRIGGER(TRIG_ARMOR_SPAWN,  -21.0f, 3.70f, -21.0f, 0.0f, 25, 30.0f, 0.9f);
    ADD_TRIGGER(TRIG_AMMO_SPAWN,   -27.0f, 0.10f,   0.0f, 0.0f, 40, 13.0f, 0.9f);
    ADD_TRIGGER(TRIG_AMMO_SPAWN,    27.0f, 0.10f,   0.0f, 0.0f, 40, 13.0f, 0.9f);
    ADD_TRIGGER(TRIG_AMMO_SPAWN,     0.0f, 0.10f, -27.0f, 0.0f, 30, 13.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN,   0.0f, 7.20f,   8.0f, 0.0f, WPN_SHOTGUN, 25.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN, -21.0f, 3.70f, -21.0f, 0.0f, WPN_SMG, 38.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN,  21.0f, 3.70f, -21.0f, 0.0f, WPN_RAIL, 55.0f, 0.9f);
    ADD_TRIGGER(TRIG_WEAPON_SPAWN,  21.0f, 3.70f,  21.0f, 0.0f, WPN_ROCKET, 60.0f, 0.9f);

    ADD_TRIGGER(TRIG_SECRET, 31.0f, 0.05f, 31.0f, 0.0f, 100, 0.0f, 1.5f);
}

static void (*const s_level_builders[])(void) = { level_foundry, level_slagpit, level_conduit };

#endif /* BP_LEVELS_INC */
