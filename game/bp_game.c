/******************************************************************************************
 * bp_game.c -- 場景狀態機、固定步進迴圈、關卡生命週期。
 *
 * 開機流程分階段進行，每個畫面只初始化一個子系統
 *   Asset、音訊與繪圖各自都耗時到不容忽視（程序化材質生成要數十毫秒；世界烘焙則更久）。
 *   若把這些全塞進同一個函式、並在第一次 EndDrawing() 之前完成，玩家會看到一個凍結的
 *   黑色視窗 —— 桌機上還附帶作業系統的「沒有回應」提示，Web 上則是一片空白畫布。因此
 *   bp_game 每個畫面只推進一個開機階段，讓載入條是真的：它確實在回報哪個子系統剛剛啟動。
 *
 * 誰呼叫誰
 *   main.c 每個繪製畫面呼叫一次 bp_game_frame(frameDt)，除此之外別無其他。底下所有事情
 *   —— 要跑幾個模擬步、哪個場景要更新、UI 何時繪製 —— 都在這裡決定。正因為只有這一個
 *   進入點，整個遊戲才能從測試框架中被無頭（headless）驅動。
 ******************************************************************************************/
#include "bp_game.h"

#include "bp_ui.h"
#include "bp_hud.h"
#include "bp_menu.h"
#include "bp_save.h"
#include "bp_world.h"
#include "bp_waves.h"
#include "bp_player.h"
#include "bp_weapons.h"
#include "bp_enemies.h"
#include "bp_fx.h"
#include "bp_hitdetect.h"
#include "../engine/bp_assets.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_camera.h"
#include "../engine/bp_gfx.h"
#include "../engine/bp_input.h"
#include "../engine/bp_math.h"

#include <stdio.h>
#include <string.h>

bp_game g_game = { 0 };

static inline Vector3 vec3y(float y) { return (Vector3){ 0.0f, y, 0.0f }; }

static inline Color mc_game(float r, float g, float b, float a)
{
    return (Color){ (unsigned char)BP_CLAMP((int)r, 0, 255), (unsigned char)BP_CLAMP((int)g, 0, 255),
                    (unsigned char)BP_CLAMP((int)b, 0, 255), (unsigned char)BP_CLAMP((int)a, 0, 255) };
}

/* 選單動作 id。刻意與 bp_menu.c 重複一份：bp_menu 負責建構列，bp_game 負責對列做出反應，
 * 若共用同一個 enum，會讓選單的標頭檔變成遊戲控制流程的依賴。這些值在開機時會做斷言檢查
 * （見 sanity_check_ids）。 */
enum {
    GA_PLAY = 1, GA_LEVELS, GA_OPTIONS, GA_CONTROLS, GA_CREDITS, GA_QUIT,
    GA_RESUME, GA_RESTART, GA_ABORT,
    GA_RETRY, GA_NEXT_LEVEL, GA_TO_MENU,
    GA_BACK, GA_RESET_BINDS, GA_RESET_SETTINGS, GA_APPLY,
    GA_LEVEL_BASE = 100,
    GA_BIND_BASE  = 200
};

/* 開機階段：因為自我測試（self-test）需要回報階段數量，所以宣告於標頭檔中 */

#define INTRO_CARD_TIME   3.0f
#define DEAD_TO_RESULTS   2.2f
#define ORBIT_SPEED       6.5f      /* 選單環繞相機的角速度（度／秒） */

/* -------------------------------------------------------------------------------------- */
/* 前置宣告                                                                                  */
/* -------------------------------------------------------------------------------------- */
static void simulate_step(float dt);
static void render_frame(void);
static void enter_scene(bp_scene s);
static void update_attract_camera(float dt);

/* 實作於 bp_menu.c。選項畫面透過浮點鏡像變數來編輯 FOV，因為滑桿列型別以浮點運作，而玩家
 * 設定儲存的是整數；這兩個轉接函式（thunk）讓 bp_game 不必伸手去碰另一個編譯單元的 static
 * 變數。 */
float bp_menu_fov_mirror(void);
void  bp_menu_rebuild_current(void);

/* -------------------------------------------------------------------------------------- */
/* 場景輔助函式                                                                              */
/* -------------------------------------------------------------------------------------- */
static inline bool scene_is_menu(bp_scene s)
{
    return s == SCENE_TITLE || s == SCENE_RESULTS || s == SCENE_PAUSED || s == SCENE_GAME_COMPLETE;
}

void bp_game_set_scene(bp_scene s) { enter_scene(s); }
bp_scene bp_game_scene(void) { return g_game.scene; }
bool bp_game_is_playing(void) { return g_game.scene == SCENE_PLAYING; }

static void banner_for_scene(bp_scene s)
{
    switch (s)
    {
        case SCENE_LEVEL_INTRO:
        {
            int i = BP_CLAMP(g_game.level_index, 0, BP_MAX(g_level_count - 1, 0));
            bp_menu_open(MENU_SCREEN_NONE);      /* 關閉任何已開啟的選單 */
            g_menu.screen = MENU_SCREEN_NONE;
            bp_hud_banner(g_levels[i].display_name, g_levels[i].subtitle, INTRO_CARD_TIME);
            bp_hud_toast(TextFormat("DIFFICULTY: %s",
                         g_difficulties[BP_CLAMP(g_game.difficulty, 0, g_difficulty_count - 1)].name),
                         TOAST_INFO, INTRO_CARD_TIME);
            break;
        }
        case SCENE_LEVEL_COMPLETE:
            bp_hud_banner("AREA CLEAR", g_world.name, 2.4f);
            break;
        case SCENE_GAME_COMPLETE:
            bp_hud_banner("ALL AREAS CLEAR", "Nothing left to breach.", 3.0f);
            break;
        default: break;
    }
}

static void enter_scene(bp_scene s)
{
    if (s == SCENE_PAUSED) g_game.scene_before_pause = g_game.scene;
    g_game.scene = s;
    g_game.scene_time = 0.0f;
    banner_for_scene(s);

    switch (s)
    {
        case SCENE_TITLE:
            bp_menu_open(MENU_SCREEN_TITLE);
            bp_input_capture(false);
            bp_audio_set_intensity(0.20f, 1.0f);
            break;

        case SCENE_LEVEL_INTRO:
            bp_menu_close();
            /* 游標在開場資訊卡期間維持被擷取（captured）的狀態，如此第一波開始時，玩家才
             * 不會還得先去找視窗。 */
            bp_input_capture(true);
            bp_audio_set_intensity(0.30f, 0.8f);
            break;

        case SCENE_PLAYING:
            bp_menu_close();
            bp_input_capture(true);
            break;

        case SCENE_PAUSED:
            bp_menu_open(MENU_SCREEN_PAUSE);
            bp_input_capture(false);
            bp_audio_set_intensity(0.14f, 0.4f);
            break;

        case SCENE_DEAD:
            bp_menu_close();
            bp_input_capture(false);
            bp_audio_set_intensity(0.10f, 1.2f);
            break;

        case SCENE_RESULTS:
        case SCENE_LEVEL_COMPLETE:
        case SCENE_GAME_COMPLETE:
            bp_menu_open(MENU_SCREEN_RESULTS);
            bp_input_capture(false);
            bp_audio_set_intensity(0.16f, 1.0f);
            break;

        default: break;
    }
    BP_LOGI("game: scene -> %d", (int)s);
}

/* -------------------------------------------------------------------------------------- */
/* 關卡生命週期                                                                              */
/* -------------------------------------------------------------------------------------- */
static void reset_run_stats(void)
{
    g_game.damage_taken = 0.0f;
    g_game.headshots_this_level = 0;
    g_game.best_streak = 0.0f;
    g_game.secret_this_level = false;
    g_game.submitted = false;
    g_game.level_start_time = g_engine.time_total;
    g_game.score_at_level_start = g_player.score;
    g_game.kills_at_level_start = g_player.kills;
}

void bp_game_start_level(int levelIndex)
{
    if (levelIndex < 0 || levelIndex >= g_level_count) levelIndex = 0;
    g_game.level_index = levelIndex;
    g_game.difficulty = BP_CLAMP(g_save.settings.difficulty, 0, g_difficulty_count - 1);

    if (!bp_world_load(levelIndex))
    {
        BP_LOGE("game: failed to load level %d", levelIndex);
        enter_scene(SCENE_TITLE);
        return;
    }

    bp_fx_reset();
    bp_enemies_clear();
    bp_pickup_clear();
    bp_weapons_reset_loadout(false);
    bp_hud_reset();

    float startYaw = 0.0f;
    Vector3 start = bp_world_player_start(&startYaw);
    bp_player_reset(start, startYaw);

    /* 起始裝備：隨身副武器，再加上關卡中第一個武器觸發器所給的東西。若在第一波就給滿整座
     * 軍火庫，就會抹去為了撿拾而戰的理由。 */
    bp_weapons_give(WPN_ID_PISTOL, g_weapon_table[WPN_ID_PISTOL].mag_size * 2, true);

    g_save.settings.last_level = levelIndex;
    g_save.dirty = true;

    reset_run_stats();
    g_waves.difficulty = g_game.difficulty;
    bp_waves_start_level(levelIndex);

    bp_hud_toast(g_world.name, TOAST_BIG, INTRO_CARD_TIME);
    enter_scene(SCENE_LEVEL_INTRO);

    BP_LOGI("game: level %d '%s' started on %s", levelIndex, g_world.name,
            g_difficulties[g_game.difficulty].name);
}

void bp_game_restart_level(void)
{
    /* 重新開始會保留這一輪的分數：那是玩家自己挣來的，若清掉會讓「重試」感覺像是在為死亡
     * 受罰，而不是獲得第二次機會。 */
    int keepScore = g_player.score;
    int keepKills = g_player.kills;
    g_game.deaths_this_run = 0;
    bp_game_start_level(g_game.level_index);
    g_player.score = keepScore;
    g_player.kills = keepKills;
}

void bp_game_next_level(void)
{
    int next = g_game.level_index + 1;
    if (next >= g_level_count) { enter_scene(SCENE_GAME_COMPLETE); return; }
    bp_game_start_level(next);
}

void bp_game_return_to_title(void)
{
    if (!g_game.submitted && g_world.loaded) bp_game_submit_run(false);
    g_game.deaths_this_run = 0;
    bp_pickup_clear();
    bp_enemies_clear();
    bp_fx_reset();
    bp_audio_set_intensity(0.20f, 1.0f);

    /* 保留最後一個關卡作為「展示模式（attract mode）」的背景：重新載入只會造成一次畫面
     * 頓挫，卻沒有任何好處，因為選單相機只會環繞現有的場景。 */
    enter_scene(SCENE_TITLE);
}

void bp_game_request_quit(void)
{
    if (!g_game.submitted && g_world.loaded && g_game.scene == SCENE_PLAYING)
        bp_game_submit_run(false);
    bp_save_write_settings();
    bp_save_write_progress();
    bp_save_sync_platform();
    g_game.quit_requested = true;
}

/* -------------------------------------------------------------------------------------- */
/* 結算                                                                                      */
/* -------------------------------------------------------------------------------------- */
void bp_game_submit_run(bool victory)
{
    if (g_game.submitted) return;      /* 每個關卡只結算一次：重試不能重複計分 */
    g_game.submitted = true;

    float time = g_engine.time_total - g_game.level_start_time;
    float acc = bp_weapons_accuracy();
    int score = g_player.score - g_game.score_at_level_start;
    int kills = g_player.kills - g_game.kills_at_level_start;

    bp_save_submit_run(g_game.level_index, g_game.difficulty, victory, score, kills,
                       time, acc, g_waves.wave_index, g_waves.perfect_waves,
                       g_player.secret_reached, g_player.state != PST_ALIVE);

    bp_menu_set_results(victory, victory ? "AREA CLEAR" : "RUN OVER",
                        score, kills, time, acc, g_waves.wave_index);

    bp_menu_add_result_line("%s ON %s", g_world.loaded ? g_world.name : "AREA",
                            g_difficulties[BP_CLAMP(g_game.difficulty, 0, g_difficulty_count - 1)].name);
    bp_menu_add_result_line("WAVES CLEARED %d / %d", g_waves.waves_completed, BP_MAX(1, g_waves.wave_total));
    bp_menu_add_result_line("PERFECT WAVES %d", g_waves.perfect_waves);
    bp_menu_add_result_line("HEADSHOTS %d", g_weapons.headshots);
    bp_menu_add_result_line("DAMAGE DEALT %.0f", (double)g_weapons.damage_dealt);
    bp_menu_add_result_line("DAMAGE TAKEN %.0f", (double)g_game.damage_taken);
    bp_menu_add_result_line("BEST STREAK x%d", g_player.streak);
    bp_menu_add_result_line(g_player.secret_reached ? "SECRET FOUND" : "SECRET MISSED");
}

/* -------------------------------------------------------------------------------------- */
/* 選單動作                                                                                  */
/* -------------------------------------------------------------------------------------- */
void bp_game_apply_menu_action(int action)
{
    bp_menu_screen from = g_menu.screen;

    switch (action)
    {
        case GA_PLAY:
            bp_game_start_level(BP_CLAMP(g_save.settings.last_level, 0, BP_MAX(g_level_count - 1, 0)));
            break;

        case GA_LEVELS:   bp_menu_open(MENU_SCREEN_LEVELS); break;
        case GA_OPTIONS:  bp_menu_open(MENU_SCREEN_OPTIONS); break;
        case GA_CONTROLS: bp_menu_open(MENU_SCREEN_CONTROLS); break;
        case GA_CREDITS:  bp_menu_open(MENU_SCREEN_CREDITS); break;
        case GA_QUIT:
            if (from == MENU_SCREEN_TITLE) bp_menu_open(MENU_SCREEN_QUIT);
            else bp_game_request_quit();
            break;

        case GA_RESUME:
            enter_scene(g_game.scene_before_pause == SCENE_PLAYING ? SCENE_PLAYING : SCENE_LEVEL_INTRO);
            break;

        case GA_RESTART:
            bp_game_restart_level();
            break;

        case GA_ABORT:
            bp_game_submit_run(false);
            bp_game_return_to_title();
            break;

        case GA_RETRY:
            bp_game_restart_level();
            break;

        case GA_NEXT_LEVEL:
            bp_game_next_level();
            break;

        case GA_TO_MENU:
            bp_game_return_to_title();
            break;

        case GA_APPLY:
        {
            /* 把 FOV 鏡像變數寫回玩家設定，然後即時套用所有設定。 */
            g_save.settings.fov = BP_CLAMP((int)(bp_menu_fov_mirror() + 0.5f), 70, 120);
            bp_save_apply_settings();
            bp_save_write_settings();
            bp_save_sync_platform();
            bp_hud_toast("SETTINGS APPLIED", TOAST_GOOD, 1.6f);
            bp_menu_open(from == MENU_SCREEN_PAUSE ? MENU_SCREEN_PAUSE : MENU_SCREEN_TITLE);
            break;
        }

        case GA_RESET_SETTINGS:
        {
            int keepScore = g_save.best_score;
            bp_level_record keep[SAVE_MAX_LEVELS][4];
            memcpy(keep, g_save.levels, sizeof(keep));

            bp_save_defaults();
            memcpy(g_save.levels, keep, sizeof(g_save.levels));
            g_save.best_score = keepScore;
            bp_save_apply_settings();
            bp_save_write_settings();
            bp_hud_toast("SETTINGS RESET -- RECORDS KEPT", TOAST_INFO, 2.0f);
            bp_menu_rebuild_current();
            break;
        }

        case GA_RESET_BINDS:
            bp_input_reset_binds();
            for (int a = 0; a < ACT_COUNT; a++)
                for (int k = 0; k < BP_MAX_BINDS_PER_ACTION; k++)
                    g_save.settings.binds[a][k] = g_input.act[a].keys[k];
            g_save.dirty = true;
            bp_save_write_settings();
            bp_hud_toast("DEFAULT KEYMAP RESTORED", TOAST_INFO, 2.0f);
            bp_menu_open(MENU_SCREEN_CONTROLS);
            break;

        case GA_BACK:
            if (from == MENU_SCREEN_LEVELS || from == MENU_SCREEN_CREDITS ||
                from == MENU_SCREEN_CONTROLS)
            {
                bp_menu_open(g_menu.return_to == MENU_SCREEN_NONE ? MENU_SCREEN_TITLE : g_menu.return_to);
            }
            else if (from == MENU_SCREEN_OPTIONS)
            {
                bp_menu_open(g_menu.return_to == MENU_SCREEN_NONE ? MENU_SCREEN_TITLE : g_menu.return_to);
            }
            else if (from == MENU_SCREEN_QUIT) bp_menu_open(MENU_SCREEN_TITLE);
            else if (from == MENU_SCREEN_PAUSE) enter_scene(SCENE_PLAYING);
            else if (from == MENU_SCREEN_RESULTS) { /* 結算畫面是終端畫面 */ }
            break;

        default:
            if (action >= GA_LEVEL_BASE && action < GA_LEVEL_BASE + SAVE_MAX_LEVELS)
                bp_game_start_level(action - GA_LEVEL_BASE);
            /* GA_BIND_BASE 開頭的列由 bp_menu 內部（按鍵擷取）消耗，不會進到這裡。 */
            break;
    }
}

/* -------------------------------------------------------------------------------------- */
/* 在選單之外處理的暫停／除錯切換                                                             */
/* -------------------------------------------------------------------------------------- */
static void handle_global_input(void)
{
    if (bp_pressed(ACT_DEBUG_OVERLAY))
    {
        g_engine.perf.show_overlay = !g_engine.perf.show_overlay;
        BP_LOGI("game: debug overlay %s", g_engine.perf.show_overlay ? "on" : "off");
    }

    if (g_game.scene == SCENE_PLAYING || g_game.scene == SCENE_LEVEL_INTRO)
    {
        if (bp_pressed(ACT_PAUSE))
        {
            enter_scene(SCENE_PAUSED);
            return;
        }
        /* 類 F1 的 HUD 切換放在計分板動作的隔壁：用長按 SCOREBOARD 來觸發太取巧了，因此把
         * 它綁在除錯覆蓋層的第二個按鍵欄位。 */
        if (IsKeyPressed(KEY_H))
        {
            g_hud.visible = !g_hud.visible;
            g_save.settings.hud_visible = g_hud.visible;
            g_save.dirty = true;
        }
        if (IsKeyPressed(KEY_G))
        {
            g_hud.minimal = !g_hud.minimal;
            g_save.settings.hud_minimal = g_hud.minimal;
            g_save.dirty = true;
            bp_hud_toast(g_hud.minimal ? "MINIMAL HUD" : "FULL HUD", TOAST_INFO, 1.2f);
        }
    }
    else if (g_game.scene == SCENE_PAUSED)
    {
        if (bp_pressed(ACT_PAUSE)) enter_scene(SCENE_PLAYING);
    }
    else if (g_game.scene == SCENE_TITLE)
    {
        if (bp_pressed(ACT_PAUSE) && g_menu.screen == MENU_SCREEN_TITLE)
            bp_menu_open(MENU_SCREEN_QUIT);
    }
}

/* -------------------------------------------------------------------------------------- */
/* 模擬                                                                                      */
/* -------------------------------------------------------------------------------------- */
static void handle_combat_input(void)
{
    /* ADS（瞄準）狀態在此發布，而非在 bp_weapons 內部，好讓「瞄準」是什麼只由一個地方決定；
     * 武器程式碼只負責消費這個旗標。 */
    g_weapons.ads_held = bp_down(ACT_ALT_FIRE);

    if (bp_pressed(ACT_RELOAD)) bp_weapons_try_reload();

    /* 先處理直接選擇武器欄位，再處理循環切換。欄位鍵優先於滾輪，如此綁定 1～5 的玩家，其
     * 意圖永遠不會被意外滾動滑鼠滾輪所覆蓋。 */
    bool slotPressed = false;
    for (int i = 0; i < WPN_ID_COUNT; i++)
    {
        if (bp_pressed((bp_action)(ACT_WEAPON_1 + i)))
        {
            bp_weapons_try_select((bp_weapon_id)i);
            slotPressed = true;
        }
    }
    if (!slotPressed)
    {
        if (bp_pressed(ACT_WEAPON_NEXT)) bp_weapons_cycle(+1);
        if (bp_pressed(ACT_WEAPON_PREV)) bp_weapons_cycle(-1);
        float wheel = GetMouseWheelMove();
        if (wheel > 0.05f) bp_weapons_cycle(+1);
        else if (wheel < -0.05f) bp_weapons_cycle(-1);
    }

    /* 開火。全自動武器讀取「持續按住」狀態；半自動武器讀取「本畫面剛按下」的邊緣。兩者都
     * 傳入，交由 bp_weapons 決定，因為 full_auto 旗標本來就住在那裡。 */
    bp_weapons_try_fire(bp_down(ACT_FIRE), bp_pressed(ACT_FIRE));
}

static void simulate_step(float dt)
{
    g_game.total_time += dt;
    g_game.sim_steps++;

    switch (g_game.scene)
    {
        case SCENE_PLAYING:
        {
            handle_combat_input();

            float healthBefore = g_player.health;
            bp_player_update(dt);
            if (g_player.health < healthBefore)
            {
                float dmg = healthBefore - g_player.health;
                g_game.damage_taken += dmg;
                bp_hud_notify_damage(g_player.last_damage_dir, BP_CLAMP(dmg / 30.0f, 0.25f, 1.0f));
            }

            Camera3D cam = g_camera.cam;
            bp_weapons_update(dt, cam, g_player.vel, g_player.on_ground);
            bp_enemy_update(dt, g_player.pos, bp_player_eye(), bp_player_alive());
            bp_world_update(dt);
            bp_fx_update(dt, vec3y(g_world.bounds_min.y));
            bp_waves_update(dt);
            bp_audio_update(dt);

            /* HUD 想立刻得知的拾取回饋 */
            if (g_player.secret_reached && !g_game.secret_this_level)
            {
                g_game.secret_this_level = true;
                bp_hud_toast("SECRET FOUND  +500", TOAST_GOOD, 2.4f);
            }

            /* 狀態轉換 */
            if (g_player.state == PST_DYING || g_player.state == PST_DEAD)
            {
                if (g_player.state == PST_DEAD) enter_scene(SCENE_DEAD);
            }
            else if (g_waves.state == WAVE_COMPLETE && g_player.exit_reached)
            {
                bp_game_submit_run(true);
                enter_scene(SCENE_LEVEL_COMPLETE);
            }
            break;
        }

        case SCENE_LEVEL_INTRO:
            /* 世界持續運作（移動平台、觸發器、粒子），但不會生成任何東西，玩家也不會移動：
             * 開場資訊卡是讓玩家閱讀場地的一個片刻。 */
            bp_world_update(dt);
            bp_fx_update(dt, vec3y(g_world.bounds_min.y));
            bp_audio_update(dt);
            bp_camera_simulate(dt, g_player.pos, 0.0f, true, false, 0.0f);
            if (g_game.scene_time >= INTRO_CARD_TIME) enter_scene(SCENE_PLAYING);
            /* 或者讓沒耐心的玩家跳過。跳過時絕不能連帶跳過該波次的開始。 */
            else if (bp_pressed(ACT_MENU_ACCEPT) || bp_pressed(ACT_FIRE))
                enter_scene(SCENE_PLAYING);
            break;

        case SCENE_DEAD:
            bp_player_update(dt);
            bp_world_update(dt);
            bp_fx_update(dt, vec3y(g_world.bounds_min.y));
            bp_audio_update(dt);
            /* 死亡運鏡持續緩慢環繞而非凍結：靜止的畫面讀起來像是當機，緩慢漂移的則像是
             * 一台攝影機。 */
            g_camera.yaw += dt * 9.0f;
            if (g_game.scene_time >= DEAD_TO_RESULTS)
            {
                bp_game_submit_run(false);
                enter_scene(SCENE_RESULTS);
            }
            break;

        case SCENE_RESULTS:
        case SCENE_LEVEL_COMPLETE:
            bp_fx_update(dt, vec3y(g_world.bounds_min.y));
            bp_audio_update(dt);
            update_attract_camera(dt);
            /* LEVEL_COMPLETE 在玩家看過結算統計後就前進；RESULTS 則會等待玩家明確的選單
             * 選擇。 */
            if (g_game.scene == SCENE_LEVEL_COMPLETE && g_game.scene_time > 1.2f &&
                bp_pressed(ACT_MENU_ACCEPT))
                bp_game_next_level();
            break;

        case SCENE_GAME_COMPLETE:
            bp_audio_update(dt);
            update_attract_camera(dt);
            if (g_game.scene_time > 2.0f && bp_pressed(ACT_MENU_ACCEPT)) bp_game_return_to_title();
            break;

        case SCENE_PAUSED:
            /* 模擬被凍結 —— 這就是暫停的定義 —— 視角也一樣。暫停時還讓相機環繞，會看起來
             * 像遊戲仍在運行。 */
            bp_audio_update(dt);
            break;

        case SCENE_TITLE:
            /* 標題畫面環繞著目前已載入的關卡，因此選單背後永遠是一座真實的競技場，而不是
             * 一片單色。 */
            bp_audio_update(dt);
            update_attract_camera(dt);
            break;

        default: break;
    }

    g_game.scene_time += dt;
}

/*
 * 選單背景相機：繞著競技場中心緩慢環繞，高度足以看清整體佈局。
 *
 * 讓真正的相機走它正常的流程（而不是直接指派 g_camera.cam）很重要：bp_camera_render()
 * 會套用震動（shake）、側傾（roll）與 FOV 平滑，因此選單背景與遊戲進行會繼承完全相同的
 * 渲染契約，當這些系統變動時，兩者也不可能失去同步。
 */
static void update_attract_camera(float dt)
{
    if (!g_world.loaded) return;

    g_game.orbit_yaw += ORBIT_SPEED * dt;
    if (g_game.orbit_yaw > 360.0f) g_game.orbit_yaw -= 360.0f;

    Vector3 c = Vector3Scale(Vector3Add(g_world.bounds_min, g_world.bounds_max), 0.5f);
    c.y = g_world.bounds_min.y;
    g_game.orbit_center = c;

    float radius = 26.0f + sinf(g_game.orbit_yaw * BP_DEG2RAD * 0.5f) * 6.0f;
    float yawRad = g_game.orbit_yaw * BP_DEG2RAD;
    Vector3 pos = { c.x + sinf(yawRad) * radius, c.y + 11.0f, c.z + cosf(yawRad) * radius };

    /* 相機從外部看向中心，因此 yaw 指向內側：在環繞角度上 +180 度。pitch 是由高度差推導
     * 而來，而非手動指定的，所以就算調整環繞高度，也不可能不小心讓相機朝向地板。 */
    g_camera.yaw = g_game.orbit_yaw + 180.0f;
    float dy = c.y + 1.5f - pos.y;
    float hd = BP_MAX(radius, 0.001f);
    g_camera.pitch = BP_CLAMP(atan2f(dy, hd) * BP_RAD2DEG, -g_camera.pitch_limit, g_camera.pitch_limit);

    bp_camera_simulate(dt, pos, 0.0f, true, false, 0.0f);
}

/* -------------------------------------------------------------------------------------- */
/* 繪製                                                                                      */
/* -------------------------------------------------------------------------------------- */
static void draw_boot_screen(void)
{
    bp_ui_begin_frame();
    bp_ui_dim(1.0f);

    float cx = (float)UI_VW * 0.5f;
    bp_ui_text_shadow(cx, 132.0f, UI_FS_LARGE, UI_ALIGN_CENTER, g_assets.pal_accent,
                      (Color){ 0, 0, 0, 230 }, "%s", BP_GAME_NAME);

    float bw = 260.0f, bx = cx - bw * 0.5f, by = 186.0f;
    bp_ui_bar(bx, by, bw, 8.0f, g_game.boot_progress, g_assets.pal_accent2,
              (Color){ 14, 16, 20, 255 }, (Color){ 92, 226, 196, 90 });
    bp_ui_text_align(cx, by + 14.0f, UI_FS_TINY, UI_ALIGN_CENTER, g_assets.pal_text_dim, "%s",
               g_game.boot_status[0] ? g_game.boot_status : "STARTING");
    bp_ui_text_align(cx, (float)UI_VH - 20.0f, UI_FS_TINY, UI_ALIGN_CENTER, (Color){ 96, 102, 114, 220 },
               "v%s   %s", BP_VERSION_STRING, BP_STUDIO);
    bp_ui_end_frame();
}

static void draw_intro_card(void)
{
    /* 名稱資訊卡是由 HUD 橫幅繪製的；這裡只負責在資訊卡顯示夠久、足以被閱讀之後，補上
     * 難度那行，以及一句「按開火鍵跳過」的提示。 */
    if (g_game.scene_time > 1.0f)
        bp_ui_text_align((float)UI_VW * 0.5f, (float)UI_VH - 44.0f, UI_FS_TINY, UI_ALIGN_CENTER,
                   g_assets.pal_text_dim, "CLICK TO DEPLOY");
}

static void render_frame(void)
{
    if (!g_game.boot_done) { draw_boot_screen(); return; }

    bp_gfx_begin_scene();

    /* 整個畫面只使用一台經過插值的相機。底下每一次繪製都使用這同一個值，因此世界與武器
     * 視角模型（viewmodel）不可能出現相差一個畫面的不一致。 */
    Camera3D cam = bp_camera_render(g_game.alpha);

    if (g_world.loaded)
    {
        BeginMode3D(cam);

        bp_world_draw_sky(cam);
        bp_world_draw(cam, cam.position);

        Vector3 viewer = cam.position;
        bp_enemy_draw(cam, viewer);
        bp_weapons_draw_projectiles(cam);
        bp_pickup_draw(cam, viewer);
        bp_fx_draw(cam);

        if (g_engine.perf.show_overlay)
        {
            bp_world_draw_debug();
            bp_enemy_draw_debug();
            bp_player_draw_debug();
        }

        EndMode3D();

        /* 武器視角模型是一道獨立的正交（orthographic）繪製流程，刻意放在 EndMode3D 之後，
         * 這樣它的自訂投影才不會受到世界 3D 相機影響。它必須最後繪製，才不會被世界幾何
         * 遮擋。 */
        if (g_game.scene == SCENE_PLAYING || g_game.scene == SCENE_LEVEL_INTRO ||
            g_game.scene == SCENE_PAUSED)
            bp_weapons_draw_viewmodel(cam, bp_player_speed01());
    }

    bp_gfx_end_scene();

    /* raylib 5.5 刻意不透過公開 API 提供繪製呼叫次數（draw-call counter）的計數器
     * （rlgl 內部有保留一個，但沒有公開存取子），因此這個覆蓋層只回報真正能測量到的東西：
     * FPS、每畫面的實際耗時，以及我們自己的模擬／繪製計時器。回報一個捏造出來的繪製呼叫
     * 次數，會比乾脆不回報更糟。 */

    /* ---- UI：在場景解析（resolve）之後，以原生解析度繪製 ---- */
    bp_ui_begin_frame();

    switch (g_game.scene)
    {
        case SCENE_PLAYING:
        case SCENE_LEVEL_INTRO:
            bp_hud_draw();
            if (g_game.scene == SCENE_LEVEL_INTRO) draw_intro_card();
            break;

        case SCENE_PAUSED:
            bp_hud_draw();
            bp_menu_draw();
            break;

        case SCENE_DEAD:
        {
            /* 死亡：去飽和的暗角 + 玩家唯一需要的那一行（「你死了，代價是這些」）。
             * 不放選單 —— 這個轉場是自動且短暫的。 */
            float k = BP_CLAMP(g_game.scene_time / 0.6f, 0.0f, 1.0f);
            bp_ui_dim(0.30f * k);
            bp_ui_vgradient(0.0f, 0.0f, (float)UI_VW, (float)UI_VH,
                            (Color){ 60, 6, 10, (unsigned char)(110 * k) },
                            (Color){ 10, 2, 4, (unsigned char)(150 * k) });
            bp_ui_text_shadow((float)UI_VW * 0.5f, (float)UI_VH * 0.5f - 10.0f, UI_FS_LARGE,
                              UI_ALIGN_CENTER, mc_game(232, 52, 60, 255 * k), (Color){ 0, 0, 0, 220 },
                              "YOU DIED");
            bp_ui_text_align((float)UI_VW * 0.5f, (float)UI_VH * 0.5f + 18.0f, UI_FS_TINY, UI_ALIGN_CENTER,
                       mc_game(200, 206, 216, 220 * k), "SCORE %d   KILLS %d   WAVE %d/%d",
                       g_player.score, g_player.kills, BP_MAX(1, g_waves.wave_index),
                       BP_MAX(1, g_waves.wave_total));
            break;
        }

        case SCENE_RESULTS:
        case SCENE_LEVEL_COMPLETE:
        case SCENE_GAME_COMPLETE:
            bp_hud_draw();
            bp_menu_draw();
            break;

        case SCENE_TITLE:
            bp_menu_draw();
            break;

        default: break;
    }

    bp_ui_end_frame();
}

/* -------------------------------------------------------------------------------------- */
/* 開機                                                                                      */
/* -------------------------------------------------------------------------------------- */
static void boot_stage(int stage)
{
    switch (stage)
    {
        case BOOT_LOG:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "INITIALISING");
            BP_LOGI("game: %s v%s -- %s", BP_GAME_NAME, BP_VERSION_STRING, BP_STUDIO);
            break;
        case BOOT_INPUT:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "INPUT");
            bp_input_init();
            break;
        case BOOT_ASSETS:
            /* 整個開機流程中最慢的單一階段：遊戲中每一張材質、每一個模型與字型圖集
             * （glyph atlas）都在此生成。它會記錄各分組的耗時，因此就算沒有效能分析器，
             * 也能從主控台診斷出是哪台機器太慢。 */
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "GENERATING TEXTURES");
            bp_assets_init();
            break;
        case BOOT_AUDIO:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "SYNTHESISING AUDIO BANK");
            bp_audio_init();
            break;
        case BOOT_GFX:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "RENDERER");
            bp_gfx_init();
            break;
        case BOOT_FX:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "PARTICLE SYSTEM");
            bp_fx_init();
            break;
        case BOOT_WORLD:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "WORLD SYSTEMS");
            bp_world_init();
            break;
        case BOOT_WEAPONS:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "WEAPONS");
            bp_weapons_init();
            break;
        case BOOT_ENEMIES:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "ENEMIES");
            bp_enemies_init();
            break;
        case BOOT_PLAYER:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "PLAYER CONTROLLER");
            bp_player_init();
            bp_camera_init();
            break;
        case BOOT_WAVES:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "ENCOUNTER DIRECTOR");
            bp_waves_init();
            break;
        case BOOT_SAVE:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "LOADING PROFILE");
            bp_save_init();
            break;
        case BOOT_UI:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "INTERFACE");
            bp_ui_init();
            break;
        case BOOT_HUD:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "HUD");
            bp_hud_init();
            /* 除錯覆蓋層旗標是一項設定，而非 HUD 的預設值：在這裡還原它（在
             * bp_save_init() 執行之後），好讓開發者儲存過的偏好設定能撐過重新啟動。 */
            g_engine.perf.show_overlay = g_save.settings.show_fps;
            break;
        case BOOT_MENU:
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "MENUS");
            bp_menu_init();
            break;
        case BOOT_LEVEL:
            /* 載入第 0 關作為展示模式的背景。這是（幾何烘焙中）最慢的單一階段，因此排
             * 在最後，並獨占一個畫面。 */
            snprintf(g_game.boot_status, sizeof(g_game.boot_status), "BUILDING %s",
                     g_level_count > 0 ? g_levels[0].display_name : "ARENA");
            bp_world_load(0);
            g_game.level_index = 0;
            float y = 0.0f;
            Vector3 start = bp_world_player_start(&y);
            g_game.orbit_center = start;
            g_game.orbit_yaw = 0.0f;
            bp_player_reset(start, y);
            bp_weapons_reset_loadout(true);
            break;
        default: break;
    }
}

static int s_boot_stage = 0;

static void boot_tick(void)
{
    if (s_boot_stage < BOOT_COUNT)
    {
        boot_stage(s_boot_stage);
        s_boot_stage++;
        g_game.boot_progress = (float)s_boot_stage / (float)BOOT_COUNT;
        return;
    }

    /* 只會執行一次。少了這道防護，這個尾段會在之後每一個畫面重新執行：它會永遠重新進入
     * SCENE_TITLE（導致展示相機與橫幅永遠無法推進）、每個畫面都重新記錄一次「boot
     * complete」，而且最糟的是，它會重新啟用事件等待（event waiting），覆蓋掉任何刻意
     * 要求輪詢（polling）的呼叫端，例如 --selftest。 */
    g_game.boot_done = true;
    g_game.boot_progress = 1.0f;

    /*
     * 這裡刻意「不」啟用事件等待。
     *
     * bp_game_frame() 是在 BeginDrawing() 與 EndDrawing() 之間被呼叫的，因此它對視窗
     * 阻塞模式（blocking mode）所做的任何事，都會在「本畫面」的 EndDrawing() 中生效。
     * 在此啟用它的話，完成開機的那一個畫面就會成為第一個可能在 PollInputEvents 中永遠
     * 停駐的畫面（在一個不遞送任何事件的顯示器上）—— 這正是無頭視窗或失去焦點的視窗，
     * 在遊戲已完整初始化卻毫無輸出的情況下會卡死的原因。
     *
     * 在事件佇列上阻塞屬於「呈現（presentation）」的範疇，因此交由 main() 決定：開機完成
     * 後它會翻轉 g_engine.event_wait_allowed，並在畫面區段之外呼叫 bp_apply_event_wait()。
     * 詳見 bp_engine::event_wait_allowed。
     */
    enter_scene(SCENE_TITLE);
    BP_LOGI("game: boot complete in %.2fs", (double)g_engine.time_total);
}

/* -------------------------------------------------------------------------------------- */
/* 畫面（frame）                                                                             */
/* -------------------------------------------------------------------------------------- */
void bp_game_frame(float frameDt)
{
    /* 夾限兩次：呼叫端本來就應該夾限，而且被切到背景的分頁無論如何都會丟給我們一個好幾秒
     * 的 delta。少了這道手續，累加器會試圖追趕數百個模擬步，遊戲就會在玩家切回來的那一刻
     * 卡死。 */
    if (!(frameDt > 0.0f)) frameDt = SIM_DT;
    if (frameDt > MAX_FRAME_DT) frameDt = MAX_FRAME_DT;

    g_engine.time_total += frameDt;
    g_engine.perf.frame_ms = bp_damp(g_engine.perf.frame_ms, frameDt * 1000.0f, 8.0f, frameDt);
    g_engine.perf.fps = g_engine.perf.frame_ms > 0.01f ? (int)(1000.0f / g_engine.perf.frame_ms) : 0;
    if (IsWindowResized()) { bp_gfx_resize(); bp_ui_resize(); }
    if (WindowShouldClose() || g_game.quit_requested) g_engine.quit_requested = true;

    bp_input_begin_frame();

    /* 虛擬輸入是在真實輪詢「之後」、模擬步「之前」疊加上去的，因此自我測試或重播可以
     * 驅動遊戲，而不必讓作業系統的鍵盤與之一致。begin_frame() 會從實體狀態推導出
     * held/pressed，若沒有這道疊加，它會抹除任何從畫面外部注入的輸入。詳見 bp_input.h。 */
    bp_input_apply_virtual();

    if (!g_game.boot_done)
    {
        boot_tick();
        /* 仍要讓渲染器喘口氣：開始／結束畫面，讓載入條真的能畫出來，而不是等到開機完成前
         * 視窗都維持空白。 */
        bp_gfx_begin_scene();
        bp_gfx_end_scene();
        render_frame();
        bp_input_end_frame();
        return;
    }

    handle_global_input();

    /* 選單由畫面更新率驅動，而非由模擬驅動：詳見 bp_menu.c。 */
    if (bp_menu_is_open())
    {
        int action = bp_menu_update(frameDt);
        if (action == MENU_ACT_BACK) bp_game_apply_menu_action(GA_BACK);
        else if (action > 0) bp_game_apply_menu_action(action);
    }

    /* ---- 固定時間步進（fixed timestep） ---- */
    float timeScale = BP_CLAMP(g_engine.time_scale, 0.0f, 2.0f);
    g_game.accumulator += frameDt * timeScale;
    g_game.sim_steps = 0;

    int steps = 0;
    while (g_game.accumulator >= SIM_DT && steps < MAX_SIM_STEPS_PER_FRAME)
    {
        simulate_step(SIM_DT);
        g_game.accumulator -= SIM_DT;
        steps++;
    }
    /* 若達到步數上限，就丟棄積壓的工作，而不是愈落愈後。這是「死亡螺旋（spiral of
     * death）」的防護：跟不上速度的機器會直接捨棄模擬時間，而不是把每個畫面都用來償還
     * 一筆它還不起的債。 */
    if (g_game.accumulator > SIM_DT * (float)MAX_SIM_STEPS_PER_FRAME) g_game.accumulator = 0.0f;
    g_game.alpha = BP_CLAMP(g_game.accumulator / SIM_DT, 0.0f, 1.0f);

    g_engine.perf.sim_steps = steps;

    /* ---- HUD 由畫面驅動：它的平滑處理必須配合顯示更新率，而非模擬更新率，否則 240 Hz
     * 的玩家會看到比 60 Hz 玩家更抖動的血條。 ---- */
    if (g_game.scene == SCENE_PLAYING || g_game.scene == SCENE_LEVEL_INTRO ||
        g_game.scene == SCENE_PAUSED || g_game.scene == SCENE_DEAD)
        bp_hud_update(frameDt);

    render_frame();
    bp_input_end_frame();
}

/* -------------------------------------------------------------------------------------- */
/* 初始化 / 關閉                                                                            */
/* -------------------------------------------------------------------------------------- */
void bp_game_init(void)
{
    memset(&g_game, 0, sizeof(g_game));
    g_game.scene = SCENE_BOOT;
    g_game.difficulty = 1;
    g_game.level_index = 0;
    g_game.orbit_radius = 26.0f;
    g_engine.time_scale = 1.0f;
    s_boot_stage = 0;
    BP_LOGI("game: bp_game_init (boot is staged, %d stages)", BOOT_COUNT);
}

void bp_game_shutdown(void)
{
    if (g_save.dirty)
    {
        bp_save_write_settings();
        bp_save_write_progress();
    }
    bp_save_sync_platform();
    bp_world_unload();
    bp_world_shutdown();
    bp_audio_shutdown();
    bp_gfx_shutdown();
    bp_assets_shutdown();
    BP_LOGI("game: shutdown complete");
}
