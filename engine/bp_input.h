/******************************************************************************************
 * bp_input.h -- action mapping, mouse capture, rebinding.
 *
 * Gameplay code never asks "is KEY_W down". It asks "is ACT_MOVE_FORWARD held". That single
 * indirection is what makes rebinding, gamepad support, QWERTY/AZERTY layouts and touch
 * input all possible without a single change to the shooter code.
 *
 * Mouse capture: on desktop DisableCursor() gives a hidden, confined cursor. On the web the
 * cursor stays visible but mouse deltas are still delivered while the canvas is focused, so
 * the exact same code path works in both -- which is why we do NOT depend on pointer lock.
 * (Pointer lock would be nicer, but it requires a user gesture and is refused inside
 * cross-origin iframes, which is exactly how a web build gets embedded on a store page.)
 ******************************************************************************************/
#ifndef BP_INPUT_H
#define BP_INPUT_H

#include "bp_common.h"

typedef enum {
    ACT_NONE = -1,

    ACT_MOVE_FORWARD, ACT_MOVE_BACK, ACT_MOVE_LEFT, ACT_MOVE_RIGHT,
    ACT_JUMP, ACT_CROUCH, ACT_SPRINT,
    ACT_FIRE, ACT_ALT_FIRE, ACT_RELOAD,
    ACT_WEAPON_1, ACT_WEAPON_2, ACT_WEAPON_3, ACT_WEAPON_4, ACT_WEAPON_5,
    ACT_WEAPON_NEXT, ACT_WEAPON_PREV,
    ACT_USE,
    ACT_PAUSE, ACT_SCOREBOARD, ACT_DEBUG_OVERLAY,
    ACT_MENU_UP, ACT_MENU_DOWN, ACT_MENU_LEFT, ACT_MENU_RIGHT,
    ACT_MENU_ACCEPT, ACT_MENU_BACK,

    ACT_COUNT
} bp_action;

#define BP_MAX_BINDS_PER_ACTION 3

/*
 * Binding codes are raylib KeyboardKey values, except that mouse binds are stored as
 * BP_BIND_MOUSE_BASE + MouseButton.
 *
 * The bias exists because the two enums overlap: MOUSE_BUTTON_LEFT is 0 (meaning "unbound"
 * in our table) and MOUSE_BUTTON_SIDE is 5, which is also KEY_E. Storing a raw mouse code
 * would make "fire = left mouse" indistinguishable from "unbound", and "weapon next = mouse
 * 4" would read as the E key. Biasing costs one constant and makes every stored bind
 * unambiguous, which is what a rebinding UI and a save file both require.
 */
#define BP_BIND_MOUSE_BASE 4096
#define BP_BIND_IS_MOUSE(code) ((code) >= BP_BIND_MOUSE_BASE && (code) < BP_BIND_MOUSE_BASE + 8)
#define BP_BIND_MOUSE_INDEX(code) ((code) - BP_BIND_MOUSE_BASE)
#define BP_BIND_FROM_MOUSE(mb) (BP_BIND_MOUSE_BASE + (mb))

typedef struct {
    int  keys[BP_MAX_BINDS_PER_ACTION];   /* raylib KeyboardKey, 0 = unbound */
    int  padButtons[2];                   /* raylib GamepadButton, -1 = unbound */
    int  padAxis;                         /* raylib GamepadAxis, -1 = none */
    float padAxisSign;
    char name[32];
    bool held;
    bool pressed;     /* true for exactly one frame */
    bool released;
    float heldTime;
} bp_binding;

typedef struct {
    bp_binding act[ACT_COUNT];
    int        bind_count;

    /* mouse */
    Vector2    mouse_delta;
    Vector2    mouse_pos;
    float      sensitivity;      /* degrees per unit of mouse delta */
    bool       captured;
    bool       invert_y;
    float      wheel;

    /* gamepad */
    bool       gamepad_present;
    float      deadzone;

    /* capture-request handshake for the web build */
    bool       capture_requested;
    bool       focus_lost_this_frame;

    int        frame;
} bp_input;

extern bp_input g_input;

void  bp_input_init(void);
/* ------------------------------------------------------------------------------------------
 * Virtual input
 * ------------------------------------------------------------------------------------------
 * A second, synthetic input source OR'd on top of the physical one.
 *
 * This exists so the game can be driven without a player: the self-test in main.c uses it,
 * and a replay, an attract-mode demo or a CI smoke test would use exactly the same seam. The
 * important property is that it is layered, never substituted -- physical input keeps working
 * while virtual input is active, so a developer can intervene in a running self-test instead
 * of having to kill the process.
 *
 * Ordering is load-bearing. bp_input_begin_frame() derives held/pressed/released from the
 * real keyboard, mouse and gamepad, so virtual state must be applied *after* it and *before*
 * the fixed-step simulation consumes it. bp_game_frame() calls bp_input_apply_virtual() in
 * exactly that position, and nothing else may call it.
 *
 * bp_input_set_virtual() is edge-aware: holding an action for several frames yields
 * held=true every frame but pressed=true only on the rising edge, which is what makes
 * one-shot actions (jump, reload, weapon cycle) behave correctly under synthetic control. */
void  bp_input_set_virtual(bp_action a, bool down);
void  bp_input_clear_virtual(void);
void  bp_input_apply_virtual(void);       /* call after begin_frame, before the sim loop */

void  bp_input_begin_frame(void);        /* call FIRST in the frame */
void  bp_input_end_frame(void);          /* call LAST in the frame  */

bool  bp_down(bp_action a);
bool  bp_pressed(bp_action a);
bool  bp_released(bp_action a);
float bp_axis(bp_action neg, bp_action pos);   /* -1..1 with gamepad analogue support */
float bp_move_axis_x(void);
float bp_move_axis_y(void);

/* Raw look delta for this frame. Exposed as functions (not the struct) because the
 * viewmodel sway and the camera both consume it and must agree on sign convention. */
float bp_input_look_dx(void);
float bp_input_look_dy(void);

void  bp_input_capture(bool enable);
bool  bp_input_is_captured(void);

bool  bp_input_rebind(bp_action a, int slot, int key);
void  bp_input_reset_binds(void);
int   bp_input_key_for_action(bp_action a, int slot);
const char *bp_input_action_name(bp_action a);
const char *bp_input_key_name(int key);

/* Returns the action that `key` is currently bound to, or ACT_NONE. Used by the rebinding
 * UI to warn about conflicts before committing. */
bp_action bp_input_action_for_key(int key);

#endif /* BP_INPUT_H */
