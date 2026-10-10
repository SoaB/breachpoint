/******************************************************************************************
 * bp_game.h -- 場景狀態機、固定步進迴圈、關卡生命週期、單局戰績統計。
 *
 * 職責切分（這是架構本身，而不只是一份檔案的分工）
 *   main.c            視窗、GL context、真正的 while(!WindowShouldClose()) 迴圈、各平台
 *                     的怪癖。這裡不含任何遊戲規則。
 *   bp_game.c         哪個場景正在進行、關卡何時載入／卸載、一局何時結束，以及所有子系統
 *                     據以推進的固定步進累加器（accumulator）。
 *   其餘模組          各司其職。bp_game 底下的任何東西都不知道「場景」的存在。
 *
 *   因此依賴圖是一棵嚴格的單向樹：
 *       main -> game -> {menu, hud, save, waves} -> {player, weapons, enemies} ->
 *              {world, hitdetect, fx} -> engine
 *   沒有模組會向上呼叫。正因為如此，才能只連結三個檔案就把 bp_math 或 bp_entity 單獨拿來
 *   做單元測試；也正因為如此，一個「暫時」的跨模組呼叫才不會慢慢結成解不開的死結。
 *
 * 固定步進（fixed timestep）
 *   模擬以 SIM_HZ（120 Hz）連同累加器執行；繪製跟著顯示更新率跑，兩者之間做插值。以下
 *   這些後果是支撐玩法的，而非純粹的美學選擇：
 *     - 命中判定在 30 fps 筆電與 240 fps 螢幕上完全一致。一個準度會隨幀率漂移的 hitscan
 *       遊戲，稱不上是競技遊戲。
 *     - 敵人 AI 與移動手感全部以「秒」為單位表達，日後永遠不必因為硬體不同而重新調校。
 *     - Web 版不會因為分頁被切到背景而壞掉：過長的單幀會被夾到 MAX_FRAME_DT，累加器也
 *       不會陷入追不上的惡性循環（spiral of death）。
 *   相機每個模擬步推進、每個繪製幀以 alpha 插值呈現，這正是它拆成 _simulate() 與
 *   _render(alpha) 而不是單一 update() 的原因。
 ******************************************************************************************/
#ifndef BP_GAME_H
#define BP_GAME_H

#include "../engine/bp_common.h"

typedef enum {
    SCENE_BOOT = 0, /* 只佔一幀：各子系統在此啟動                     */
    SCENE_TITLE, /* 主選單，背後是持續漂移的即時相機               */
    SCENE_LEVEL_INTRO, /* 區域名稱卡，世界已載入，玩家原地待命           */
    SCENE_PLAYING, /* 遊戲本體                                      */
    SCENE_PAUSED, /* 模擬凍結，選單仍可操作                        */
    SCENE_DEAD, /* 死亡鏡頭，接著進入結算                        */
    SCENE_RESULTS, /* 本局戰績總結                                  */
    SCENE_LEVEL_COMPLETE, /* 抵達出口：統計，然後前往下一區或回選單        */
    SCENE_GAME_COMPLETE /* 全部區域均已通關                              */
} bp_scene;

/* 開機階段，依序執行。每一幀只跑一個階段，好讓載入條回報的是真實進度、視窗也維持可回應；
 * 詳見 bp_game.c。 */
enum {
    BOOT_LOG = 0,
    BOOT_INPUT,
    BOOT_ASSETS,
    BOOT_AUDIO,
    BOOT_GFX,
    BOOT_FX,
    BOOT_WORLD,
    BOOT_WEAPONS,
    BOOT_ENEMIES,
    BOOT_PLAYER,
    BOOT_WAVES,
    BOOT_SAVE,
    BOOT_UI,
    BOOT_HUD,
    BOOT_MENU,
    BOOT_LEVEL,
    BOOT_COUNT
};

typedef struct {
    /* 單局記帳 */
    int level_index;
    int difficulty;
    int score_at_level_start;
    int kills_at_level_start;
    float level_start_time;
    int deaths_this_run;

    /* 本局表現指標，每個關卡重置 */
    float damage_taken;
    int headshots_this_level;
    float best_streak;
    bool secret_this_level;
    bool submitted; /* 防護：一局只能提交（submit）一次 */

    /* 場景計時 */
    bp_scene scene;
    bp_scene scene_before_pause;
    float scene_time;
    float accumulator; /* 固定步進剩下的零頭時間                */
    int sim_steps;
    float alpha; /* 繪製插值係數 0..1                     */

    /* 選單用的吸引模式（attract-mode）相機 */
    float orbit_yaw;
    float orbit_pitch;
    float orbit_radius;
    Vector3 orbit_center;

    /* 開機 */
    float boot_progress; /* 0..1，供載入條使用                     */
    char boot_status[64];
    bool boot_done;
    bool quit_requested;
    float total_time;

    bool initialised;
} bp_game;

extern bp_game g_game;

/* 生命週期 */
void bp_game_init(void);
void bp_game_shutdown(void);

/* 由 main() 每個繪製幀呼叫一次。累加器由這裡持有，所以一次可能跑零個、也可能跑好幾個模擬
 * 步。`frameDt` 理應已由呼叫端夾到 MAX_FRAME_DT —— bp_game 仍會再防禦性地夾一次，因為被切
 * 到背景的瀏覽器分頁可能丟給我們長達六十秒的 delta，而累加器絕不能試圖把這段時間全追回來。 */
void bp_game_frame(float frameDt);

/* 場景控制 */
void bp_game_set_scene(bp_scene s);
bp_scene bp_game_scene(void);
bool bp_game_is_playing(void);

void bp_game_start_level(int levelIndex);
void bp_game_restart_level(void);
void bp_game_next_level(void);
void bp_game_return_to_title(void);
void bp_game_request_quit(void);

/* 由選單層呼叫，處理那些需要產生遊戲側副作用的動作 */
void bp_game_apply_menu_action(int action);

/* 本局統計，供結算畫面與存檔使用 */
void bp_game_submit_run(bool victory);

#endif /* BP_GAME_H */
