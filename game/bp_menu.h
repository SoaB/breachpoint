/******************************************************************************************
 * bp_menu.h -- every non-gameplay screen: title, level select, options, pause, results.
 *
 * ONE NAVIGATION MODEL FOR EVERY SCREEN
 *   All menus are built from `bp_menu_list`, a row stack that owns: which row is selected,
 *   keyboard/gamepad/mouse navigation, activation, and the sound feedback for each. Screens
 *   differ only in what rows they push and what they do on activation.
 *
 *   That is not just less code. It is the reason the game feels consistent: the same
 *   highlight, the same slide-in timing, the same click sound, everywhere. Per-screen
 *   navigation code is how a shipped game ends up with one menu that responds to the mouse
 *   and another that does not.
 *
 * MOUSE SUPPORT IS MANDATORY, NOT OPTIONAL
 *   The web build will be played by people who never touch a keyboard mapping. Every row
 *   therefore has a hit rectangle, hover is resolved from the virtual-space mouse position,
 *   and hover *moves* the selection (rather than being a separate visual state) so there is
 *   exactly one "current row" and no way for keyboard and mouse to disagree.
 ******************************************************************************************/
#ifndef BP_MENU_H
#define BP_MENU_H

#include "bp_common.h"

#define MENU_MAX_ROWS     24
#define MENU_LABEL_LEN    40
#define MENU_VALUE_LEN    28

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
    MROW_ACTION = 0,     /* activates something                       */
    MROW_TOGGLE,         /* bool, cycles on/off                       */
    MROW_CYCLE,          /* cycles through choices[]                   */
    MROW_SLIDER,         /* float in [min,max], left/right adjusts    */
    MROW_LABEL,          /* non-interactive heading                   */
    MROW_SPACER
} bp_menu_row_kind;

typedef struct {
    char            label[MENU_LABEL_LEN];
    char            value[MENU_VALUE_LEN];
    bp_menu_row_kind kind;
    int             id;           /* action id returned on activation */
    bool           *bval;         /* MROW_TOGGLE                      */
    int            *ival;         /* MROW_CYCLE                       */
    float          *fval;         /* MROW_SLIDER                      */
    const char    **choices;
    int             choice_count;
    float           vmin, vmax, vstep;
    bool            enabled;
    char            hint[64];
} bp_menu_row;

typedef struct {
    bp_menu_row rows[MENU_MAX_ROWS];
    int         count;
    int         selected;
    int         hovered;
    float       scroll;
    float       x, y, w;          /* virtual-space layout of the list */
    float       row_h;

    /* Column flow. A 360-unit-tall virtual canvas fits at most ~15 rows, and the options
     * screen has 20. Rather than invent scrolling (which needs a scrollbar, clipping and a
     * second navigation axis), rows past `column_rows` flow into a second column. Selection
     * still moves strictly up/down then wraps to the next column, so the mental model stays
     * "a list". */
    int         column_rows;      /* 0 = single column                  */
    float       column_gap;
    float       open_time;        /* for the slide-in animation       */
    bool        nav_up, nav_down, nav_left, nav_right, nav_accept, nav_back;
    bool        mouse_used;
} bp_menu_list;

typedef struct {
    bp_menu_screen screen;
    bp_menu_screen return_to;      /* where BACK goes                  */
    bp_menu_list   list;

    /* title screen ambience */
    float          time;
    float          logo_pulse;
    int            logo_frame;

    /* results payload, filled by bp_game before switching to MENU_SCREEN_RESULTS */
    bool           result_victory;
    char           result_title[48];
    char           result_lines[8][64];
    int            result_line_count;
    int            result_score;
    int            result_accuracy;
    float          result_time;
    int            result_kills;
    int            result_wave;

    /* controls screen: which action is waiting for a new key (-1 = none) */
    int            capturing_action;

    bool           initialised;
} bp_menu;

extern bp_menu g_menu;

/* Action ids returned by bp_menu_update. Anything >= 0 is a row's own id; the negatives
 * below are emitted by the framework itself. */
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

/* Returns a MENU_ACT_* value or a row id. Called once per *frame* (not per sim step):
 * menus are input-driven, and running them at 120 Hz would double-count edge triggers. */
int  bp_menu_update(float dt);
void bp_menu_draw(void);

/* results screen payload */
void bp_menu_set_results(bool victory, const char *title, int score, int kills,
                         float time, float accuracy01, int wave);
void bp_menu_add_result_line(const char *fmt, ...);

/* accessors used by bp_game when the options screen commits */
float bp_menu_fov_mirror(void);
void  bp_menu_rebuild_current(void);

/* key capture for the controls screen */
void bp_menu_capture_key_for(int action);
bool bp_menu_is_capturing(void);

#endif /* BP_MENU_H */
