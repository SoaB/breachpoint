/******************************************************************************************
 * bp_input.h -- 動作映射（action mapping）、滑鼠捕捉（mouse capture）與按鍵重綁（rebinding）。
 *
 * 遊戲邏輯從不詢問「KEY_W 現在有沒有被按住」，它問的是「ACT_MOVE_FORWARD 有沒有被按住」。
 * 正是這一層間接（indirection）讓重綁、手把支援、QWERTY/AZERTY 鍵盤佈局與觸控輸入全部可行，
 * 而且射擊玩法本身的程式碼一行都不必修改。
 *
 * 滑鼠捕捉：桌機上 DisableCursor() 會給一個隱藏且被限制在視窗內的游標；Web 上游標仍可見，
 * 但只要 canvas 持有焦點，滑鼠位移量（mouse delta）一樣會送進來，所以兩邊走的是完全相同的
 * 程式碼路徑 —— 這就是我們不依賴 pointer lock 的原因。（pointer lock 雖然更理想，但它需要
 * 一次使用者手勢，而且在跨來源 iframe 中會被拒絕；偏偏 Web 版正是以 iframe 的形式嵌進商店
 * 頁面。）
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
 * 綁定代碼（bind code）本身就是 raylib 的 KeyboardKey 值，只有滑鼠綁定是例外：它們存成
 * BP_BIND_MOUSE_BASE + MouseButton。
 *
 * 之所以需要這個偏移（bias），是因為兩個 enum 的數值互相重疊：MOUSE_BUTTON_LEFT 是 0，在
 * 我們的表格裡代表「未綁定」；MOUSE_BUTTON_SIDE 是 5，而 5 同時也是 KEY_E。若直接存原始的
 * 滑鼠代碼，「射擊 = 左鍵」就會和「未綁定」無法區分，「下一把武器 = 滑鼠 4」則會被讀成 E
 * 鍵。付出一個常數的代價，就能讓每一筆存起來的綁定都沒有歧義 —— 重綁介面與存檔都需要這點。
 */
#define BP_BIND_MOUSE_BASE 4096
#define BP_BIND_IS_MOUSE(code) ((code) >= BP_BIND_MOUSE_BASE && (code) < BP_BIND_MOUSE_BASE + 8)
#define BP_BIND_MOUSE_INDEX(code) ((code) - BP_BIND_MOUSE_BASE)
#define BP_BIND_FROM_MOUSE(mb) (BP_BIND_MOUSE_BASE + (mb))

typedef struct {
    int  keys[BP_MAX_BINDS_PER_ACTION];   /* raylib KeyboardKey；0 = 未綁定 */
    int  padButtons[2];                   /* raylib GamepadButton；-1 = 未綁定 */
    int  padAxis;                         /* raylib GamepadAxis；-1 = 不使用 */
    float padAxisSign;
    char name[32];
    bool held;
    bool pressed;     /* 僅在單一幀內保持 true（邊緣觸發） */
    bool released;
    float heldTime;
} bp_binding;

typedef struct {
    bp_binding act[ACT_COUNT];
    int        bind_count;

    /* 滑鼠 */
    Vector2    mouse_delta;
    Vector2    mouse_pos;
    float      sensitivity;      /* 每單位滑鼠位移對應的旋轉量（度） */
    bool       captured;
    bool       invert_y;
    float      wheel;

    /* 手把 */
    bool       gamepad_present;
    float      deadzone;

    /* Web 版的捕捉請求握手（capture-request handshake） */
    bool       capture_requested;
    bool       focus_lost_this_frame;

    int        frame;
} bp_input;

extern bp_input g_input;

void  bp_input_init(void);
/* ------------------------------------------------------------------------------------------
 * 虛擬輸入（virtual input）
 * ------------------------------------------------------------------------------------------
 * 一個疊加（OR）在實體輸入之上的第二個、純合成的輸入來源。
 *
 * 它存在的意義，是讓遊戲可以在「沒有玩家」的情況下被驅動：main.c 的自我測試（self-test）
 * 用到它，而回放、示範模式（attract-mode demo）與 CI 冒煙測試也會用到完全相同的接縫
 * （seam）。重點在於它是「疊加」而不是「取代」—— 虛擬輸入作用期間實體輸入依然有效，所以
 * 開發者可以隨時介入正在跑的自我測試，而不必先把行程殺掉。
 *
 * 呼叫順序（ordering）是承重的。bp_input_begin_frame() 是從真實鍵盤、滑鼠與手把推導出
 * held/pressed/released，因此虛擬狀態必須在它「之後」套用，並且在固定步長模擬「消費」輸入
 * 之前完成。bp_game_frame() 正是在這個位置呼叫 bp_input_apply_virtual()，其他任何地方都不
 * 可以呼叫它。
 *
 * bp_input_set_virtual() 具備邊緣感知（edge-aware）：把一個動作按住數幀，會讓 held=true
 * 每一幀都成立，但 pressed=true 只在上升沿（rising edge）那一幀成立 —— 正因如此，一次性
 * 動作（跳躍、換彈、切換武器）在合成輸入驅動下才會表現正確。 */
void  bp_input_set_virtual(bp_action a, bool down);
void  bp_input_clear_virtual(void);
void  bp_input_apply_virtual(void);       /* 在 begin_frame 之後、模擬迴圈之前呼叫 */

void  bp_input_begin_frame(void);        /* 必須是本幀最先呼叫的輸入函式 */
void  bp_input_end_frame(void);          /* 必須是本幀最後呼叫的輸入函式  */

bool  bp_down(bp_action a);
bool  bp_pressed(bp_action a);
bool  bp_released(bp_action a);
float bp_axis(bp_action neg, bp_action pos);   /* -1..1，並支援手把類比軸 */
float bp_move_axis_x(void);
float bp_move_axis_y(void);

/* 本幀的原始視角位移（look delta）。刻意做成函式而非直接暴露結構體欄位，是因為視模型擺動
 * 與鏡頭都要讀取它，兩者必須遵守同一套正負號約定（sign convention），否則轉頭方向會對不上。 */
float bp_input_look_dx(void);
float bp_input_look_dy(void);

void  bp_input_capture(bool enable);
bool  bp_input_is_captured(void);

bool  bp_input_rebind(bp_action a, int slot, int key);
void  bp_input_reset_binds(void);
int   bp_input_key_for_action(bp_action a, int slot);
const char *bp_input_action_name(bp_action a);
const char *bp_input_key_name(int key);

/* 回傳 `key` 目前所綁定的動作，沒有任何綁定則回傳 ACT_NONE。重綁介面會拿它在正式寫入按鍵
 * 之前先偵測衝突並警告玩家，避免一個按鍵同時觸發兩個動作。 */
bp_action bp_input_action_for_key(int key);

#endif /* BP_INPUT_H */
