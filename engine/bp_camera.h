/******************************************************************************************
 * bp_camera.h -- first person camera: look, FOV kick, roll, shake, and the viewmodel rig.
 *
 * Separated from the player controller on purpose. The controller owns *where the body is*;
 * the camera owns *how the body is seen*. That split is what lets us add a spectator cam,
 * a death cam and a kill-cam replay later without touching movement code.
 *
 * Feel targets (these numbers are the whole game feel):
 *   - look is 1:1 with the mouse, no smoothing, no acceleration. Anything else and the game
 *     feels like it is fighting the player.
 *   - FOV expands with speed. Sprinting at 14 m/s adds ~9 degrees, which is what makes
 *     movement feel fast without needing a speedometer.
 *   - landing adds a downward pitch dip and a roll wobble; getting hit adds roll in the
 *     direction of the damage. Both are sub-2-degree and sub-100ms -- enough to be felt,
 *     not enough to be aimed through.
 ******************************************************************************************/
#ifndef BP_CAMERA_H
#define BP_CAMERA_H

#include "bp_common.h"
#include "bp_math.h"

typedef struct {
    Camera3D cam;

    float   yaw;            /* degrees */
    float   pitch;          /* degrees, clamped */
    float   pitch_limit;

    /* feel modifiers */
    float   fov_base;
    float   fov_current;
    float   fov_speed_kick;
    float   roll;           /* degrees, applied around the view axis */
    float   dip;            /* vertical offset for crouch/land/recoil */
    float   recoil_pitch;   /* additive, recovers over time */
    float   recoil_yaw;

    /* smoothing state */
    float   shake_roll;
    Vector3 shake_pos;

    /* viewmodel rig: an offset camera space used by bp_weapons to draw the gun */
    Vector3 vm_position;
    Vector3 vm_rotation;    /* degrees */
    float   vm_bob;
    float   vm_sway;
    float   vm_recoil;

    /* interpolated output used for rendering (simulation runs at 120 Hz, render at display Hz) */
    Vector3 render_position;
    float   render_yaw;
    float   render_pitch;
    float   alpha;
} bp_camera;

extern bp_camera g_camera;

void    bp_camera_init(void);
void    bp_camera_reset(Vector3 position);

/* Feed raw mouse/touch look deltas. `sensitivity` is degrees per pixel-equivalent. */
void    bp_camera_look(float dx, float dy, float sensitivity);
void    bp_camera_set_angles(float yaw, float pitch);

/* Called from the fixed-step simulation. */
void    bp_camera_simulate(float dt, Vector3 bodyPosition, float speed01, bool onGround,
                           bool crouched, float damageRoll);

/* Called once per render frame to produce the interpolated Camera3D. */
Camera3D bp_camera_render(float alpha);

Vector3 bp_camera_forward(void);
Vector3 bp_camera_right(void);
Vector3 bp_camera_eye(void);

/* Combat feedback */
void    bp_camera_add_recoil(float pitchAmount, float yawAmount);
void    bp_camera_add_dip(float amount);
void    bp_camera_add_roll(float amount);
void    bp_camera_set_fov_base(float fov);

/* Viewmodel helpers */
void    bp_camera_vm_update(float dt, float speed01, float recoil01, bool moving);
Matrix  bp_camera_vm_matrix(void);

#endif /* BP_CAMERA_H */
