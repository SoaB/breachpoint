/******************************************************************************************
 * bp_input.c
 ******************************************************************************************/
#include "bp_input.h"

bp_input g_input = { 0 };

typedef struct { const char *name; int neg; int pos; int padAxis; float padSign; } bp_default_bind;

/* -------------------------------------------------------------------------------------- */
/* 預設鍵位映射                                                                              */
/* -------------------------------------------------------------------------------------- */
static void set_bind(bp_action a, const char *name, int k0, int k1, int k2,
                     int pad0, int pad1, int axis, float axisSign)
{
    bp_binding *b = &g_input.act[a];
    b->keys[0] = k0; b->keys[1] = k1; b->keys[2] = k2;
    b->padButtons[0] = pad0; b->padButtons[1] = pad1;
    b->padAxis = axis; b->padAxisSign = axisSign;
    snprintf(b->name, sizeof(b->name), "%s", name);
    b->held = b->pressed = b->released = false;
    b->heldTime = 0.0f;
}

void bp_input_reset_binds(void)
{
    set_bind(ACT_MOVE_FORWARD, "Move Forward", KEY_W, KEY_UP, 0,          -1, -1, GAMEPAD_AXIS_LEFT_Y, -1.0f);
    set_bind(ACT_MOVE_BACK,    "Move Back",    KEY_S, KEY_DOWN, 0,        -1, -1, GAMEPAD_AXIS_LEFT_Y,  1.0f);
    set_bind(ACT_MOVE_LEFT,    "Strafe Left",  KEY_A, 0, 0,               -1, -1, GAMEPAD_AXIS_LEFT_X, -1.0f);
    set_bind(ACT_MOVE_RIGHT,   "Strafe Right", KEY_D, 0, 0,               -1, -1, GAMEPAD_AXIS_LEFT_X,  1.0f);
    set_bind(ACT_JUMP,         "Jump",         KEY_SPACE, 0, 0,           GAMEPAD_BUTTON_RIGHT_FACE_DOWN, -1, -1, 0);
    set_bind(ACT_CROUCH,       "Crouch",       KEY_LEFT_CONTROL, KEY_C, 0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, -1, -1, 0);
    set_bind(ACT_SPRINT,       "Sprint",       KEY_LEFT_SHIFT, 0, 0,      GAMEPAD_BUTTON_LEFT_TRIGGER_1, -1, GAMEPAD_AXIS_LEFT_Y, -1.0f);

    set_bind(ACT_FIRE,         "Fire",         BP_BIND_FROM_MOUSE(MOUSE_BUTTON_LEFT), 0, 0,   GAMEPAD_BUTTON_RIGHT_TRIGGER_1, -1, -1, 0);
    set_bind(ACT_ALT_FIRE,     "Alt Fire",     BP_BIND_FROM_MOUSE(MOUSE_BUTTON_RIGHT), 0, 0,  GAMEPAD_BUTTON_LEFT_TRIGGER_1, -1, -1, 0);
    set_bind(ACT_RELOAD,       "Reload",       KEY_R, 0, 0,               GAMEPAD_BUTTON_RIGHT_FACE_LEFT, -1, -1, 0);

    set_bind(ACT_WEAPON_1,     "Weapon 1",     KEY_ONE, 0, 0,             -1, -1, -1, 0);
    set_bind(ACT_WEAPON_2,     "Weapon 2",     KEY_TWO, 0, 0,             -1, -1, -1, 0);
    set_bind(ACT_WEAPON_3,     "Weapon 3",     KEY_THREE, 0, 0,           -1, -1, -1, 0);
    set_bind(ACT_WEAPON_4,     "Weapon 4",     KEY_FOUR, 0, 0,            -1, -1, -1, 0);
    set_bind(ACT_WEAPON_5,     "Weapon 5",     KEY_FIVE, 0, 0,            -1, -1, -1, 0);
    set_bind(ACT_WEAPON_NEXT,  "Next Weapon",  KEY_E, BP_BIND_FROM_MOUSE(MOUSE_BUTTON_SIDE), 0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT, -1, -1, 0);
    set_bind(ACT_WEAPON_PREV,  "Prev Weapon",  KEY_Q, 0, 0,               GAMEPAD_BUTTON_LEFT_FACE_LEFT, -1, -1, 0);

    set_bind(ACT_USE,          "Use",          KEY_F, 0, 0,               GAMEPAD_BUTTON_RIGHT_FACE_UP, -1, -1, 0);
    set_bind(ACT_PAUSE,        "Pause",        KEY_ESCAPE, KEY_P, 0,      GAMEPAD_BUTTON_MIDDLE_RIGHT, -1, -1, 0);
    set_bind(ACT_SCOREBOARD,   "Scoreboard",   KEY_TAB, 0, 0,             -1, -1, -1, 0);
    set_bind(ACT_DEBUG_OVERLAY,"Debug Overlay",KEY_F3, KEY_GRAVE, 0,      -1, -1, -1, 0);

    set_bind(ACT_MENU_UP,      "Menu Up",      KEY_W, KEY_UP, 0,          GAMEPAD_BUTTON_LEFT_FACE_UP, -1, GAMEPAD_AXIS_LEFT_Y, -1.0f);
    set_bind(ACT_MENU_DOWN,    "Menu Down",    KEY_S, KEY_DOWN, 0,        GAMEPAD_BUTTON_LEFT_FACE_DOWN, -1, GAMEPAD_AXIS_LEFT_Y, 1.0f);
    set_bind(ACT_MENU_LEFT,    "Menu Left",    KEY_A, KEY_LEFT, 0,        GAMEPAD_BUTTON_LEFT_FACE_LEFT, -1, GAMEPAD_AXIS_LEFT_X, -1.0f);
    set_bind(ACT_MENU_RIGHT,   "Menu Right",   KEY_D, KEY_RIGHT, 0,       GAMEPAD_BUTTON_LEFT_FACE_RIGHT, -1, GAMEPAD_AXIS_LEFT_X, 1.0f);
    set_bind(ACT_MENU_ACCEPT,  "Accept",       KEY_ENTER, KEY_SPACE, 0,   GAMEPAD_BUTTON_RIGHT_FACE_DOWN, -1, -1, 0);
    set_bind(ACT_MENU_BACK,    "Back",         KEY_ESCAPE, KEY_BACKSPACE, 0, GAMEPAD_BUTTON_MIDDLE_RIGHT, -1, -1, 0);

    g_input.bind_count = ACT_COUNT;
}

void bp_input_init(void)
{
    memset(&g_input, 0, sizeof(g_input));
    g_input.sensitivity = 0.13f;
    g_input.deadzone = 0.22f;
    g_input.captured = false;
    g_input.invert_y = false;
    bp_input_reset_binds();
    BP_LOGD("input: %d actions bound", ACT_COUNT);
}

/* -------------------------------------------------------------------------------------- */
/* 按鍵輪詢                                                                                  */
/* -------------------------------------------------------------------------------------- */
/*
 * 所有綁定代碼都透過下面這三個函式輪詢。
 *
 * 滑鼠判定「必須」是偏移檢查（bias check），不能寫成數值區間。MouseButton 是 0..6，而
 * KeyboardKey 從 32 開始，所以「code >= MOUSE_BUTTON_LEFT」對鍵盤上每一個按鍵都會成立 ——
 * 本檔案較早的版本正是如此，把方向鍵與兩顆 Ctrl 鍵都當成滑鼠鍵在輪詢，結果悄悄地廢掉了蹲
 * 下、跳躍與所有選單綁定。代碼小於 BP_BIND_MOUSE_BASE 者是鍵盤按鍵；等於或大於者才是滑鼠鍵。
 */
static bool is_code_down(int code)
{
    if (code == 0) return false;
    if (BP_BIND_IS_MOUSE(code)) return IsMouseButtonDown((MouseButton)BP_BIND_MOUSE_INDEX(code));
    return IsKeyDown((KeyboardKey)code);
}

static bool is_code_pressed(int code)
{
    if (code == 0) return false;
    if (BP_BIND_IS_MOUSE(code)) return IsMouseButtonPressed((MouseButton)BP_BIND_MOUSE_INDEX(code));
    return IsKeyPressed((KeyboardKey)code);
}

static bool is_code_released(int code)
{
    if (code == 0) return false;
    if (BP_BIND_IS_MOUSE(code)) return IsMouseButtonReleased((MouseButton)BP_BIND_MOUSE_INDEX(code));
    return IsKeyReleased((KeyboardKey)code);
}

void bp_input_begin_frame(void)
{
    g_input.frame++;
    g_input.mouse_delta = GetMouseDelta();
    g_input.mouse_pos = GetMousePosition();
    g_input.wheel = GetMouseWheelMove();

    /* 失去焦點時，絕不能讓遊戲以為某個鍵還被按著 —— 這正是經典的「玩家 Alt-Tab 切走、切回
     * 來之後一路 sprint 撞牆」缺陷。 */
    bool focused = IsWindowFocused();
    g_input.focus_lost_this_frame = (g_input.captured && !focused);
    if (g_input.focus_lost_this_frame) BP_LOGD("input: focus lost, releasing all actions");

    g_input.gamepad_present = IsGamepadAvailable(0);

    for (int i = 0; i < ACT_COUNT; i++)
    {
        bp_binding *b = &g_input.act[i];
        bool held = false, pressed = false, released = false;

        if (focused && !g_input.focus_lost_this_frame)
        {
            for (int s = 0; s < BP_MAX_BINDS_PER_ACTION; s++)
            {
                if (is_code_down(b->keys[s])) held = true;
                if (is_code_pressed(b->keys[s])) pressed = true;
                if (is_code_released(b->keys[s])) released = true;
            }

            if (g_input.gamepad_present)
            {
                for (int s = 0; s < 2; s++)
                {
                    if (b->padButtons[s] >= 0)
                    {
                        if (IsGamepadButtonDown(0, (GamepadButton)b->padButtons[s])) held = true;
                        if (IsGamepadButtonPressed(0, (GamepadButton)b->padButtons[s])) pressed = true;
                        if (IsGamepadButtonReleased(0, (GamepadButton)b->padButtons[s])) released = true;
                    }
                }
                if (b->padAxis >= 0)
                {
                    float v = GetGamepadAxisMovement(0, (GamepadAxis)b->padAxis) * b->padAxisSign;
                    if (v > g_input.deadzone + 0.35f) { held = true; pressed = (b->heldTime <= 0.0f); }
                    else if (v < g_input.deadzone && b->held) released = true;
                }
            }
        }

        b->pressed = pressed && !b->held;
        b->released = released;
        if (held && !b->held) b->pressed = true;
        b->held = held;
        b->heldTime = held ? b->heldTime + GetFrameTime() : 0.0f;
    }
}

void bp_input_end_frame(void)
{
    for (int i = 0; i < ACT_COUNT; i++)
    {
        g_input.act[i].pressed = false;
        g_input.act[i].released = false;
    }
    g_input.wheel = 0.0f;
}

bool bp_down(bp_action a)    { return a >= 0 && a < ACT_COUNT && g_input.act[a].held; }
/* ---- 虛擬輸入 ---- */
static bool s_virtual[ACT_COUNT];
static bool s_virtual_prev[ACT_COUNT];

void bp_input_set_virtual(bp_action a, bool down)
{
    if (a < 0 || a >= ACT_COUNT) return;
    s_virtual[a] = down;
}

void bp_input_clear_virtual(void)
{
    for (int i = 0; i < ACT_COUNT; i++) s_virtual[i] = false;
}

void bp_input_apply_virtual(void)
{
    for (int i = 0; i < ACT_COUNT; i++)
    {
        bp_binding *b = &g_input.act[i];
        bool rising = s_virtual[i] && !s_virtual_prev[i];
        bool falling = !s_virtual[i] && s_virtual_prev[i];

        if (s_virtual[i]) { b->held = true; if (rising) b->pressed = true; }
        if (falling) b->released = true;

        s_virtual_prev[i] = s_virtual[i];
    }
}

bool bp_pressed(bp_action a) { return a >= 0 && a < ACT_COUNT && g_input.act[a].pressed; }
bool bp_released(bp_action a){ return a >= 0 && a < ACT_COUNT && g_input.act[a].released; }

static float axis_from_action(bp_action a)
{
    if (a < 0 || a >= ACT_COUNT) return 0.0f;
    bp_binding *b = &g_input.act[a];
    float v = b->held ? 1.0f : 0.0f;

    /* 手把的類比量貢獻會疊加在數位值之上，兩者取較大者 */
    if (g_input.gamepad_present && b->padAxis >= 0)
    {
        float g = GetGamepadAxisMovement(0, (GamepadAxis)b->padAxis) * b->padAxisSign;
        if (BP_ABS(g) > g_input.deadzone)
        {
            float norm = (BP_ABS(g) - g_input.deadzone) / (1.0f - g_input.deadzone);
            v = BP_MAX(v, BP_CLAMP(norm, 0.0f, 1.0f));
        }
    }
    return v;
}

float bp_axis(bp_action neg, bp_action pos)
{
    return BP_CLAMP(axis_from_action(pos) - axis_from_action(neg), -1.0f, 1.0f);
}

float bp_move_axis_x(void) { return bp_axis(ACT_MOVE_LEFT, ACT_MOVE_RIGHT); }
float bp_move_axis_y(void) { return bp_axis(ACT_MOVE_BACK, ACT_MOVE_FORWARD); }

float bp_input_look_dx(void) { return g_input.mouse_delta.x; }
float bp_input_look_dy(void) { return g_input.mouse_delta.y * (g_input.invert_y ? -1.0f : 1.0f); }

/* -------------------------------------------------------------------------------------- */
/* 滑鼠捕捉                                                                                  */
/* -------------------------------------------------------------------------------------- */
void bp_input_capture(bool enable)
{
    if (enable == g_input.captured) return;
    g_input.captured = enable;

    if (enable)
    {
        DisableCursor();
#if !defined(PLATFORM_WEB)
        /* 只有桌機才有意義：避免我們剛開始捕捉的瞬間，作業系統就沿著視窗邊框塞給我們一次莫
         * 名其妙的點擊。 */
        SetMouseOffset(0, 0);
#endif
        BP_LOGD("input: mouse captured");
    }
    else
    {
        EnableCursor();
        BP_LOGD("input: mouse released");
    }
}

bool bp_input_is_captured(void) { return g_input.captured; }

/* -------------------------------------------------------------------------------------- */
/* 重新綁定                                                                                  */
/* -------------------------------------------------------------------------------------- */
bp_action bp_input_action_for_key(int key)
{
    if (key == 0) return ACT_NONE;
    for (int i = 0; i < ACT_COUNT; i++)
        for (int s = 0; s < BP_MAX_BINDS_PER_ACTION; s++)
            if (g_input.act[i].keys[s] == key) return (bp_action)i;
    return ACT_NONE;
}

bool bp_input_rebind(bp_action a, int slot, int key)
{
    if (a < 0 || a >= ACT_COUNT) return false;
    if (slot < 0 || slot >= BP_MAX_BINDS_PER_ACTION) return false;

    /* 清掉其他用到同一按鍵的動作，讓任何一筆綁定都不會有歧義。 */
    if (key != 0)
    {
        for (int i = 0; i < ACT_COUNT; i++)
            for (int s = 0; s < BP_MAX_BINDS_PER_ACTION; s++)
                if (i != (int)a && g_input.act[i].keys[s] == key) g_input.act[i].keys[s] = 0;
    }
    g_input.act[a].keys[slot] = key;
    BP_LOGD("input: '%s' slot %d -> %s", g_input.act[a].name, slot,
            key ? bp_input_key_name(key) : "(unbound)");
    return true;
}

int bp_input_key_for_action(bp_action a, int slot)
{
    if (a < 0 || a >= ACT_COUNT || slot < 0 || slot >= BP_MAX_BINDS_PER_ACTION) return 0;
    return g_input.act[a].keys[slot];
}

const char *bp_input_action_name(bp_action a)
{
    if (a < 0 || a >= ACT_COUNT) return "?";
    return g_input.act[a].name;
}

const char *bp_input_key_name(int key)
{
    /* raylib 沒有提供 KeyCode -> 字串的函式，而為了重綁畫面去維護一張 300 項的對照表根本不
     * 划算。實際上我們只開放字母、數字、功能鍵、方向鍵與修飾鍵供玩家綁定 —— 所以這裡也只映
     * 射那些按鍵，其餘代碼一律落到下方 switch 的 default 分支。 */
    static char buf[24];

    if (key == 0) return "--";

    /* 滑鼠綁定是以偏移值儲存的（見 BP_BIND_MOUSE_BASE），所以要在這裡先解析掉，而不是留給
     * 下面的 switch —— 它們的原始數值會與 KEY_E、KEY_F 等按鍵撞在一起。 */
    if (BP_BIND_IS_MOUSE(key))
    {
        static const char *mb[] = { "MOUSE 1", "MOUSE 2", "MOUSE 3", "MOUSE 4",
                                    "MOUSE 5", "MOUSE 6", "MOUSE 7", "MOUSE 8" };
        return mb[BP_CLAMP(BP_BIND_MOUSE_INDEX(key), 0, 7)];
    }

    if (key >= KEY_A && key <= KEY_Z) { snprintf(buf, sizeof(buf), "%c", (char)('A' + (key - KEY_A))); return buf; }
    if (key >= KEY_ZERO && key <= KEY_NINE) { snprintf(buf, sizeof(buf), "%d", key - KEY_ZERO); return buf; }
    if (key >= KEY_F1 && key <= KEY_F12) { snprintf(buf, sizeof(buf), "F%d", 1 + key - KEY_F1); return buf; }
    if (key >= KEY_KP_0 && key <= KEY_KP_9) { snprintf(buf, sizeof(buf), "NUM%d", key - KEY_KP_0); return buf; }

    switch (key)
    {
        case KEY_SPACE: return "SPACE";
        case KEY_ESCAPE: return "ESC";
        case KEY_ENTER: return "ENTER";
        case KEY_TAB: return "TAB";
        case KEY_BACKSPACE: return "BACKSPACE";
        case KEY_INSERT: return "INSERT";
        case KEY_DELETE: return "DELETE";
        case KEY_RIGHT: return "RIGHT";
        case KEY_LEFT: return "LEFT";
        case KEY_DOWN: return "DOWN";
        case KEY_UP: return "UP";
        case KEY_PAGE_UP: return "PGUP";
        case KEY_PAGE_DOWN: return "PGDN";
        case KEY_HOME: return "HOME";
        case KEY_END: return "END";
        case KEY_LEFT_CONTROL: return "L-CTRL";
        case KEY_LEFT_SHIFT: return "L-SHIFT";
        case KEY_LEFT_ALT: return "L-ALT";
        case KEY_RIGHT_CONTROL: return "R-CTRL";
        case KEY_RIGHT_SHIFT: return "R-SHIFT";
        case KEY_RIGHT_ALT: return "R-ALT";
        case KEY_GRAVE: return "`";
        case KEY_MINUS: return "-";
        case KEY_EQUAL: return "=";
        case KEY_LEFT_BRACKET: return "[";
        case KEY_RIGHT_BRACKET: return "]";
        case KEY_BACKSLASH: return "\\";
        case KEY_SEMICOLON: return ";";
        case KEY_APOSTROPHE: return "'";
        case KEY_COMMA: return ",";
        case KEY_PERIOD: return ".";
        case KEY_SLASH: return "/";
        case KEY_CAPS_LOCK: return "CAPS";
        default: snprintf(buf, sizeof(buf), "KEY %d", key); return buf;
    }
}
