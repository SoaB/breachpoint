/******************************************************************************************
 * bp_menu.h -- 所有非遊戲進行中的畫面：標題、關卡選擇、選項、暫停、結算。
 *
 * 所有畫面共用同一套導覽模型
 *   每個選單都由 `bp_menu_list` 建立 —— 這是一個「列堆疊（row stack）」，負責管理：目前
 *   選取的是哪一列、鍵盤／手把／滑鼠導覽、啟動（activation），以及每一列對應的音效回饋。
 *   各畫面彼此唯一的差別，只在於它們推入哪些列，以及列被啟動時要執行什麼。
 *
 *   這不只是程式碼比較少而已，這正是遊戲操作感之所以一致的原因：到處都是相同的高亮、
 *   相同的滑入時序、相同的點擊音效。若每個畫面各寫一套導覽程式碼，最後就會變成上市遊戲
 *   中那種「一個選單能用滑鼠、另一個卻不行」的情況。
 *
 * 滑鼠支援是必要條件，而非選項
 *   Web 版的玩家可能從未碰過鍵盤映射。因此每一列都有各自的命中矩形（hit rectangle），
 *   游標懸停（hover）由虛擬空間的滑鼠座標判定，而且懸停會「直接移動選取」（而不是另外
 *   維持一個獨立的視覺狀態），如此一來就永遠只有一個「目前列」，鍵盤與滑鼠也不可能對不上。
 ******************************************************************************************/
#ifndef BP_MENU_H
#define BP_MENU_H

#include "../engine/bp_common.h"

#define MENU_MAX_ROWS 24
#define MENU_LABEL_LEN 40
#define MENU_VALUE_LEN 32

typedef enum {
    MENU_SCREEN_NONE = 0,
    MENU_SCREEN_TITLE,
    MENU_SCREEN_LEVELS,
    MENU_SCREEN_OPTIONS,
    MENU_SCREEN_CONTROLS,
    MENU_SCREEN_CREDITS,
    MENU_SCREEN_PAUSE,
    MENU_SCREEN_RESULTS,
    MENU_SCREEN_QUIT
} bp_menu_screen;

typedef enum {
    MROW_ACTION = 0, /* 啟動某個動作                                */
    MROW_TOGGLE,     /* 布林值，切換開／關                          */
    MROW_CYCLE,      /* 在 choices[] 之間循環切換                    */
    MROW_SLIDER,     /* [min,max] 之間的浮點數，以左右鍵調整          */
    MROW_LABEL,      /* 不可互動的標題列                            */
    MROW_SPACER
} bp_menu_row_kind;

typedef struct {
    char label[MENU_LABEL_LEN];
    char value[MENU_VALUE_LEN];
    bp_menu_row_kind kind;
    int id; /* 啟動時回傳的動作 id                     */
    bool* bval; /* MROW_TOGGLE 綁定的布林來源            */
    int* ival; /* MROW_CYCLE 綁定的整數索引             */
    float* fval; /* MROW_SLIDER 綁定的浮點來源            */
    const char** choices;
    int choice_count;
    float vmin, vmax, vstep;
    bool enabled;
    char hint[64];
} bp_menu_row;

typedef struct {
    bp_menu_row rows[MENU_MAX_ROWS];
    int count;
    int selected;
    int hovered;
    float scroll;
    float x, y, w; /* 此清單在虛擬空間中的版面配置 */
    float row_h;

    /* 欄位流動（column flow）。高 360 單位的虛擬畫布最多只能容納約 15 列，而選項畫面有
     * 20 列。與其自己發明捲動機制（那需要捲軸、裁剪，以及第二個導覽軸），超過
     * `column_rows` 的列會流向第二欄。選取仍嚴格地上下移動、再換到下一欄，因此玩家心中
     * 的模型依然是「一份清單」。 */
    int column_rows; /* 0 表示單欄 */
    float column_gap;
    float open_time; /* 用於滑入動畫 */
    bool nav_up, nav_down, nav_left, nav_right, nav_accept, nav_back;
    bool mouse_used;
} bp_menu_list;

typedef struct {
    bp_menu_screen screen;
    bp_menu_screen return_to; /* 按下 BACK 時要返回的畫面 */
    bp_menu_list list;

    /* 標題畫面的氛圍動畫 */
    float time;
    float logo_pulse;
    int logo_frame;

    /* 結算資料內容，由 bp_game 在切換到 MENU_SCREEN_RESULTS 之前填入 */
    bool result_victory;
    char result_title[48];
    char result_lines[8][64];
    int result_line_count;
    int result_score;
    int result_accuracy;
    float result_time;
    int result_kills;
    int result_wave;

    /* 控制畫面：正在等待新按鍵的動作（-1 表示沒有） */
    int capturing_action;

    bool initialised;
} bp_menu;

extern bp_menu g_menu;

/* bp_menu_update 回傳的動作 id。任何 >= 0 的值都是某一列自己的 id；以下這些負值則是由
 * 框架本身發出的。 */
enum {
    MENU_ACT_NONE = -1,
    MENU_ACT_BACK = -2,
    MENU_ACT_ACCEPT = -3,
    MENU_ACT_UP = -4,
    MENU_ACT_DOWN = -5
};

void bp_menu_init(void);
void bp_menu_open(bp_menu_screen screen);
void bp_menu_close(void);
bool bp_menu_is_open(void);

/* 回傳一個 MENU_ACT_* 值或某一列的 id。每個「畫面（frame）」呼叫一次（而非每個模擬步）：
 * 選單由輸入驅動，若以 120 Hz 執行會重複計算邊緣觸發（edge trigger）。 */
int bp_menu_update(float dt);
void bp_menu_draw(void);

/* 結算畫面的資料內容 */
void bp_menu_set_results(bool victory, const char* title, int score, int kills,
    float time, float accuracy01, int wave);
void bp_menu_add_result_line(const char* fmt, ...);

/* 當選項畫面確定套用時，bp_game 所使用的存取子（accessor） */
float bp_menu_fov_mirror(void);
void bp_menu_rebuild_current(void);

/* 控制畫面的按鍵擷取（key capture） */
void bp_menu_capture_key_for(int action);
bool bp_menu_is_capturing(void);

#endif /* BP_MENU_H */
