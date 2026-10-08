/******************************************************************************************
 * bp_camera.c
 ******************************************************************************************/
#include "bp_camera.h"
#include "bp_gfx.h"
#include "../game/bp_fx.h"

bp_camera g_camera = { 0 };

void bp_camera_init(void)
{
    memset(&g_camera, 0, sizeof(g_camera));
    g_camera.cam.fovy = 90.0f;
    g_camera.cam.projection = CAMERA_PERSPECTIVE;
    g_camera.cam.up = (Vector3){ 0, 1, 0 };
    g_camera.fov_base = 90.0f;
    g_camera.fov_current = 90.0f;
    g_camera.pitch_limit = 88.0f;
    bp_camera_reset((Vector3){ 0, PLAYER_EYE_HEIGHT, 0 });
}

void bp_camera_reset(Vector3 position)
{
    g_camera.yaw = 0.0f;
    g_camera.pitch = 0.0f;
    g_camera.roll = 0.0f;
    g_camera.dip = 0.0f;
    g_camera.recoil_pitch = 0.0f;
    g_camera.recoil_yaw = 0.0f;
    g_camera.shake_roll = 0.0f;
    g_camera.fov_current = g_camera.fov_base;
    g_camera.cam.position = position;
    g_camera.cam.target = Vector3Add(position, (Vector3){ 0, 0, 1 });
    g_camera.render_position = position;
    g_camera.vm_bob = 0.0f;
    g_camera.vm_sway = 0.0f;
    g_camera.vm_recoil = 0.0f;
}

void bp_camera_look(float dx, float dy, float sensitivity)
{
    g_camera.yaw   += dx * sensitivity;
    g_camera.pitch -= dy * sensitivity;

    if (g_camera.yaw > 360.0f) g_camera.yaw -= 360.0f;
    if (g_camera.yaw < 0.0f)   g_camera.yaw += 360.0f;
    g_camera.pitch = BP_CLAMP(g_camera.pitch, -g_camera.pitch_limit, g_camera.pitch_limit);
}

void bp_camera_set_angles(float yaw, float pitch)
{
    g_camera.yaw = yaw;
    g_camera.pitch = BP_CLAMP(pitch, -g_camera.pitch_limit, g_camera.pitch_limit);
}

Vector3 bp_camera_forward(void)
{
    return bp_forward_from_angles(g_camera.yaw, g_camera.pitch + g_camera.recoil_pitch);
}

Vector3 bp_camera_right(void)
{
    Vector3 f = bp_forward_from_angles(g_camera.yaw, 0.0f);
    return (Vector3){ f.z, 0.0f, -f.x };
}

Vector3 bp_camera_eye(void) { return g_camera.render_position; }

void bp_camera_add_recoil(float pitchAmount, float yawAmount)
{
    g_camera.recoil_pitch += pitchAmount;
    g_camera.recoil_yaw   += yawAmount;
    /* Hard cap: recoil must never be able to push the view past the pitch limit in a way
     * the player cannot recover from, or the gun becomes unusable at close range. */
    g_camera.recoil_pitch = BP_CLAMP(g_camera.recoil_pitch, -22.0f, 22.0f);
    g_camera.recoil_yaw   = BP_CLAMP(g_camera.recoil_yaw, -14.0f, 14.0f);
}

void bp_camera_add_dip(float amount) { g_camera.dip -= amount; }
void bp_camera_add_roll(float amount) { g_camera.roll += amount; }
void bp_camera_set_fov_base(float fov) { g_camera.fov_base = BP_CLAMP(fov, 50.0f, 130.0f); }

void bp_camera_simulate(float dt, Vector3 bodyPosition, float speed01, bool onGround,
                        bool crouched, float damageRoll)
{
    /* ---- recoil recovery: fast at first, then slow, so bursts settle predictably ---- */
    g_camera.recoil_pitch = bp_damp(g_camera.recoil_pitch, 0.0f, 9.0f, dt);
    g_camera.recoil_yaw   = bp_damp(g_camera.recoil_yaw,   0.0f, 11.0f, dt);

    /* ---- FOV speed kick ---- */
    float fovTarget = g_camera.fov_base + speed01 * 9.0f + (onGround ? 0.0f : 2.0f);
    g_camera.fov_speed_kick = fovTarget - g_camera.fov_base;
    g_camera.fov_current = bp_damp(g_camera.fov_current, fovTarget, crouched ? 14.0f : 7.0f, dt);

    /* ---- crouch dip ---- */
    float crouchTarget = crouched ? -0.42f : 0.0f;
    g_camera.dip = bp_approach(g_camera.dip, crouchTarget + BP_MIN(g_camera.dip, 0.0f), dt * 2.2f);
    if (g_camera.dip < crouchTarget) g_camera.dip = bp_damp(g_camera.dip, crouchTarget, 10.0f, dt);

    /* ---- damage roll ---- */
    g_camera.roll = bp_damp(g_camera.roll, damageRoll, 7.0f, dt);

    /* ---- head bob: two-sine so the vertical and lateral phases differ like walking ---- */
    float bobAmount = onGround ? speed01 : speed01 * 0.35f;
    g_camera.vm_bob += dt * (6.5f + speed01 * 7.5f);
    float bobY = sinf(g_camera.vm_bob * 2.0f) * 0.028f * bobAmount;
    float bobX = cosf(g_camera.vm_bob) * 0.036f * bobAmount;

    /* ---- shake from the FX system ---- */
    Vector3 shake = bp_fx_shake_offset();
    g_camera.shake_pos = shake;
    g_camera.shake_roll = shake.x * 6.0f;

    /* ---- final eye position ---- */
    Vector3 right = bp_camera_right();
    Vector3 eye = bodyPosition;
    eye.y += PLAYER_EYE_HEIGHT + g_camera.dip + bobY + shake.y;
    eye = Vector3Add(eye, Vector3Scale(right, bobX + shake.x));
    eye.z += shake.z;

    g_camera.render_position = eye;
    g_camera.cam.position = eye;

    float yaw = g_camera.yaw + g_camera.recoil_yaw;
    float pitch = g_camera.pitch + g_camera.recoil_pitch;
    Vector3 fwd = bp_forward_from_angles(yaw, pitch);
    g_camera.cam.target = Vector3Add(eye, fwd);
    g_camera.cam.fovy = g_camera.fov_current;
    g_camera.alpha = 1.0f;
}

Camera3D bp_camera_render(float alpha)
{
    BP_UNUSED(alpha);
    Camera3D c = g_camera.cam;

    /* Roll is not expressible through Camera3D.target alone, so we encode it in `up`.
     * This keeps the projection matrix otherwise untouched (no custom matrix path), which
     * matters because the same code must work under OpenGL 3.3 and WebGL. */
    float rollDeg = g_camera.roll + g_camera.shake_roll;
    if (BP_ABS(rollDeg) > 0.001f)
    {
        float r = rollDeg * BP_DEG2RAD;
        Vector3 fwd = Vector3Normalize(Vector3Subtract(c.target, c.position));
        Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, (Vector3){ 0, 1, 0 }));
        Vector3 up = Vector3CrossProduct(right, fwd);
        Vector3 rolled = Vector3Add(Vector3Scale(up, cosf(r)), Vector3Scale(right, sinf(r)));
        c.up = rolled;
    }
    return c;
}

/* ====================================================================================== */
/* viewmodel                                                                               */
/* ====================================================================================== */
void bp_camera_vm_update(float dt, float speed01, float recoil01, bool moving)
{
    /* Sway: the gun lags behind the look direction. Purely cosmetic but it is the single
     * biggest contributor to "the weapon feels attached to a person". */
    g_camera.vm_sway = bp_damp(g_camera.vm_sway, 0.0f, 6.0f, dt);

    g_camera.vm_recoil = bp_damp(g_camera.vm_recoil, recoil01, 14.0f, dt);

    float bob = g_camera.vm_bob;
    float bobAmp = moving ? (0.5f + speed01 * 0.9f) : 0.10f;

    g_camera.vm_position.x =  cosf(bob) * 0.012f * bobAmp;
    g_camera.vm_position.y = -sinf(bob * 2.0f) * 0.010f * bobAmp - g_camera.vm_recoil * 0.035f;
    g_camera.vm_position.z = -0.030f - g_camera.vm_recoil * 0.110f;

    g_camera.vm_rotation.x = -g_camera.vm_recoil * 13.0f;          /* muzzle climbs */
    g_camera.vm_rotation.y =  sinf(bob * 0.5f) * 1.4f * bobAmp;
    g_camera.vm_rotation.z =  cosf(bob) * 1.8f * bobAmp + g_camera.vm_recoil * 3.0f;
    BP_UNUSED(dt);
}

Matrix bp_camera_vm_matrix(void)
{
    Matrix m = MatrixIdentity();
    m = MatrixMultiply(m, MatrixTranslate(g_camera.vm_position.x, g_camera.vm_position.y,
                                          g_camera.vm_position.z));
    m = MatrixMultiply(m, MatrixRotateXYZ((Vector3){ g_camera.vm_rotation.x * BP_DEG2RAD,
                                                     g_camera.vm_rotation.y * BP_DEG2RAD,
                                                     g_camera.vm_rotation.z * BP_DEG2RAD }));
    return m;
}
