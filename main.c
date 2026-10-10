/******************************************************************************************
 * main.c -- 平台進入點。
 *
 * 什麼屬於這裡、什麼不屬於
 *   這個檔案只擁有四樣東西：GL context、視窗、畫面迴圈，以及桌機與 Web 之間的
 *   差異。它不含任何遊戲規則、不含模擬狀態，而且除了「開機、迴圈、關機」之外
 *   也不決定任何子系統順序。其他一切都住在 bp_game.c 與其下層。
 *
 *   這種分離不是風格偏好。它代表整個遊戲可以由一個測試框架驅動——以迴圈呼叫
 *   bp_game_frame()、餵給它合成的 delta、完全不需要視窗——而這是對固定步進
 *   （fixed timestep）模擬做回歸測試唯一務實的方式。
 *
 * 開機順序是承重結構（load-bearing）
 *   1. bp_log_init()        -- 環形緩衝區（ring buffer）必須在任何東西寫日誌之前
 *                              就先存在，包括 raylib 在建立視窗期間自己的 trace 輸出。
 *   2. bp_game_init()       -- 把遊戲狀態歸零並選擇 SCENE_BOOT。完全不觸碰 GL。
 *   3. bp_save_boot()       -- 讀取玩家設定檔（profile）。視窗大小、vsync 與 MSAA
 *                              都來自它，而這三者都只能在 InitWindow「之前」套用。
 *                              其他設定刻意「不」在此套用：那會動到此刻還不存在
 *                              的算繪器與音訊裝置。
 *   4. InitWindow()
 *   5. 迴圈                -- bp_game_frame() 每個畫面推進一個開機階段，所以載入
 *                              條是真的：它回報的是哪個子系統剛剛上線。
 *
 * WEB 平台
 *   畫面迴圈的主人是瀏覽器，所以 Web 版用 emscripten_set_main_loop 註冊一個回呼，
 *   而且永遠不會從 main() 返回。兩個後果塑造了這個檔案：
 *     - 任何東西都不可以阻塞。一個 while(!WindowShouldClose()) 迴圈會凍住整個分頁。
 *     - 畫布（canvas）的尺寸才是權威。我們從不強制解析度，而是對 IsWindowResized()
 *       做出反應，因為尺寸是由嵌入者（商店頁面、一個 iframe）決定的。
 *   我們也刻意「不」請求指標鎖定（pointer lock）。它需要使用者的手勢，而且在跨
 *   來源（cross-origin）的 iframe 內會被拒絕——而那正是 Web 版被嵌入的典型方式。
 *   只要畫布持有焦點，GetMouseDelta() 依然會送來視角輸入，而且這條程式路徑與桌機
 *   完全相同——這正是 bp_input 裡連一個平台 ifdef 都沒有的原因。
 ******************************************************************************************/

#include "engine/bp_common.h"
#include "engine/bp_input.h"
#include "game/bp_game.h"
#include "game/bp_save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PLATFORM_WEB
#include <emscripten/emscripten.h>
#endif

#include "engine/bp_assets.h"
#include "engine/bp_camera.h"
#include "engine/bp_gfx.h"
#include "game/bp_enemies.h"
#include "game/bp_fx.h"
#include "game/bp_player.h"
#include "game/bp_waves.h"
#include "game/bp_weapons.h"
#include "game/bp_world.h"
#include "game/bp_hud.h"
#include "game/bp_menu.h"

/* -------------------------------------------------------------------------------------- */
/* 自我測試                                                                               */
/* -------------------------------------------------------------------------------------- */
/*
 * --selftest 以合成輸入、沒有真人玩家的方式驅動真正的畫面迴圈，然後回報一份整體
 * 通過／失敗的摘要，並以狀態碼離開。
 *
 * 為什麼這個東西住在 main.c 而不是 tests/：它鍛鍊的是「整條」技術棧，包括 GL
 * context、算繪器、音訊裝置路徑與場景狀態機。單元測試能證明 bp_math 是正確的；
 * 只有它能證明開機、載入關卡、生成一波敵人、開火、畫出 HUD，這些全部能在同一
 * 個行程裡一起運作。這就是「編譯得過」與「出得了貨」之間的差別，也是 CI 第一個
 * 要跑的東西。
 *
 * 它是無條件編譯進去的，而不是躲在 debug 旗標後面：一份能自我驗證的出貨版值得
 * 那幾 KB，而且這代表支援工程師可以請玩家跑一條指令、把輸出貼回來。
 */
/*
 * 固定的合成畫面時間。
 *
 * 自我測試每張畫面都恰好推進 1/60 秒的遊戲時間，完全不管那張畫面實際花了多久
 * 算繪。這是刻意的，而且正是一份「能用的測試」與一份「會依機器快慢默默通過或
 * 失敗的測試」之間的差別：
 *
 *   - 確定性（determinism）。同樣的畫面數永遠涵蓋同樣長的模擬時間，所以「600 張
 *     畫面」在 4090 上與在軟體光柵化器上都代表「十秒的遊戲內容」。
 *   - 覆蓋率。用牆上時間（wall clock）驅動的年代，一台慢的 CI 機器在跑完測試所
 *     花的時間裡只模擬了兩秒遊戲，永遠過不了三秒的關卡開場卡——於是測試回報零
 *     隻敵人、零次開火，看起來像四個各自獨立的遊戲性 bug。其實只是同一個時間
 *     假設錯了。
 *   - 速度。在 llvmpipe 下一張真實畫面要價約 7 ms，把模擬時間與算繪時間解耦，
 *     讓測試能用幾秒的牆上時間跑完二十秒的戰鬥。
 *
 * 這確實代表測試偵測不到「遊戲跑得太慢」——那正是結尾的畫面預算（frame budget）
 * 斷言與 fps 檢查存在的目的。
 */
#define SELFTEST_DT (1.0f / 60.0f)

static int selftest_frames = 0;

/* 迴圈結尾那一幀做一次像素讀回，量測「3D 場景真的有解析到畫面上」。
 * 見 run_selftest() 裡的對應斷言。 */
static bool  s_pixel_capture_wanted = false;
static float s_frame_mean_rgb = -1.0f;
static float s_frame_nonblack = -1.0f;

static void capture_frame_stats(void)
{
    rlDrawRenderBatchActive();          /* 讀回前先把批次清到 GL */
    Image img = LoadImageFromScreen();
    long n = (long)img.width * (long)img.height;
    long sum = 0, nonblack = 0;
    for (long p = 0; p < n; p++)
    {
        const unsigned char *px = (const unsigned char *)img.data + p * 4;
        sum += px[0] + px[1] + px[2];
        if (px[0] + px[1] + px[2] > 24) nonblack++;
    }
    s_frame_mean_rgb = n > 0 ? (float)sum / (float)(n * 3) : 0.0f;
    s_frame_nonblack = n > 0 ? (float)nonblack / (float)n : 0.0f;
    UnloadImage(img);
    BP_LOGI("selftest: framebuffer readback %.0fx%.0f, mean %.1f, non-black %.1f%%",
            (double)img.width, (double)img.height, (double)s_frame_mean_rgb,
            (double)(s_frame_nonblack * 100.0f));
}

static void selftest_frame(void)
{
    selftest_frames++;

    /*
     * 合成輸入一律經過虛擬輸入層，絕不直接寫進 g_input.act[]：bp_input_begin_frame()
     * 會在下一張畫面從實體裝置重新推導每個動作，直接寫入的值會被抹掉。
     * bp_input_set_virtual() 具備邊緣觸發（edge-aware）語意，所以按住 FIRE 一百張
     * 畫面就會開火一百次，而 JUMP 只按一張畫面就只跳一次。
     */
    bool fighting = (g_game.scene == SCENE_PLAYING || g_game.scene == SCENE_LEVEL_INTRO);

    if (g_game.scene == SCENE_TITLE && !fighting)
    {
        /*
         * 這一輪跑程從標題畫面按下 ACCEPT 開始，而不是直接呼叫 bp_game_start_level()。
         *
         * 只多兩行程式碼，卻涵蓋了玩家真正使用的進入路徑：選單列選取、動作分派、
         * 關卡載入的交接與場景轉換。一個直接穿過 API 呼叫的測試，即使選單壞了
         * 也照樣通過。第 150 張畫面已在開機（17 張）之後，而且讓標題橫幅停留得
         * 夠久，在錄影的跑程裡看得到。
         */
        bp_input_clear_virtual();
        if (selftest_frames >= 150) bp_input_set_virtual(ACT_MENU_ACCEPT, true);
    }
    else if (!fighting)
    {
        bp_input_clear_virtual();
    }
    else
    {
        /* 瞄準最近的敵人，戰鬥與波次才會真的推進；一個敵人也沒有時就持續掃視 */
        bp_handle nh = BP_NULL_HANDLE;
        if (bp_enemy_nearest_distance(g_player.pos, &nh) < 120.0f)
        {
            bp_enemy *e = bp_enemy_get(nh);
            if (e && e->state != ES_DYING && e->state != ES_DEAD)
            {
                Vector3 to = Vector3Subtract(e->pos, g_player.pos);
                g_camera.yaw = atan2f(to.x, to.z) * BP_RAD2DEG;
                float dist = sqrtf(to.x * to.x + to.z * to.z);
                float dy = (e->pos.y + 0.9f) - (g_player.pos.y + 1.62f);
                g_camera.pitch = atan2f(dy, BP_MAX(dist, 0.1f)) * BP_RAD2DEG;
            }
            else g_camera.yaw += 2.2f;
        }
        else
        {
            g_camera.yaw += 2.2f;
        }

        /* 以圓形路線移動：鍛鍊加速度、摩擦力、空中控制、牆壁碰撞、踏階（step-up）
         * 求解器與腳步音效。 */
        bp_input_set_virtual(ACT_MOVE_FORWARD, true);
        bp_input_set_virtual(ACT_MOVE_LEFT, (selftest_frames / 40) % 2 == 0);
        bp_input_set_virtual(ACT_JUMP, (selftest_frames % 55) == 0);

        /* 持續開火並定期換槍／裝填，好讓每個武器狀態機、投射物路徑與 hitscan 路徑
         * 都被鍛鍊到。 */
        bp_input_set_virtual(ACT_FIRE, (selftest_frames % 4) < 2);
        bp_input_set_virtual(ACT_RELOAD, (selftest_frames % 240) == 0);
        bp_input_set_virtual(ACT_WEAPON_NEXT, (selftest_frames % 400) == 0);
        bp_input_set_virtual(ACT_SPRINT, (selftest_frames / 90) % 2 == 0);
        bp_input_set_virtual(ACT_CROUCH, (selftest_frames / 150) % 2 == 0);

        if (selftest_frames == 160)
        {
            bp_weapons_give(WPN_ID_SHOTGUN, 40, false);
            bp_weapons_give(WPN_ID_SMG, 300, true);
        }

        /* 自我測試合成玩家的生存機制：持續補足彈藥與血量，讓 1800 張畫面全程都在戰鬥 */
        if (g_weapons.reserve[g_weapons.current] < 30)
            bp_weapons_add_ammo(g_weapons.current, 60);
        if (g_player.health < 40.0f)
            g_player.health = 100.0f;
    }

    BeginDrawing();
    ClearBackground(BLACK);
    bp_game_frame(SELFTEST_DT);
    if (s_pixel_capture_wanted) { s_pixel_capture_wanted = false; capture_frame_stats(); }
    EndDrawing();

}

static int run_selftest(int frames)
{
    BP_LOGI("selftest: %d frames at a fixed %.1f Hz = %.1f s of simulated play",
            frames, 60.0, (double)frames / 60.0);

    /* 每一批 120 張畫面量一次牆上時間（wall clock）：這是發現「某個場景在軟體光柵
     * 化器上一張要算 300 ms」的方法，而不是盯著凍住的畫面乾等五分鐘。 */
    double t0 = GetTime();
    for (int i = 0; i < frames && !g_engine.quit_requested; i++)
    {
        s_pixel_capture_wanted = (i == frames - 1);   /* 最後一幀做像素讀回量測 */
        selftest_frame();
        if ((i % 120) == 119)
        {
            double now = GetTime();
            BP_LOGI("selftest: frame %4d  sim %5.1fs  scene %d  wave %d  %2d alive  %4d particles  %.1f ms/frame",
                    i + 1, (double)g_game.total_time, g_game.scene, g_waves.wave_index,
                    g_enemies.alive_count, g_fx.particle_live, (now - t0) * 1000.0 / 120.0);
            t0 = now;
        }
    }

    /* 在跑完「之後」才記下錯誤計數器的基準：下面的斷言失敗時會以 ERROR 等級寫
     * 日誌，把這些算進既有錯誤，會讓最後那條「全程沒有引發任何錯誤」檢查被自己
     * 的輸出害得失敗。 */
    int errorsBefore = bp_log_error_count();

    int fails = 0;
    #define CHECK(cond, fmt, ...) do { \
        if (cond) BP_LOGI("selftest: PASS  " fmt, __VA_ARGS__); \
        else { BP_LOGE("selftest: FAIL  " fmt, __VA_ARGS__); fails++; } } while (0)

    CHECK(g_game.boot_done,            "boot completed (%d stages)", BOOT_COUNT);
    CHECK(g_assets.ready,              "assets generated (%.0f ms)", (double)g_assets.load_ms);
    CHECK(g_assets.fonts_valid,        "bitmap font atlas usable (%d glyphs)", 0);
    CHECK(g_world.loaded,              "level loaded: %d solids, %d triggers", g_world.solid_count, g_world.trigger_count);
    CHECK(g_world.batch_count > 0,     "static geometry baked into %d batches", g_world.batch_count);
    CHECK(g_enemies.spawned_total > 0, "enemies spawned: %d (%d alive at end)", g_enemies.spawned_total, g_enemies.alive_count);
    CHECK(g_enemies.killed_total > 0,  "combat produced kills: %d", g_enemies.killed_total);
    CHECK(g_weapons.shots_fired > 0,   "weapons fired: %d shots, %d hits (%d%%)",
          g_weapons.shots_fired, g_weapons.shots_hit,
          g_weapons.shots_fired ? (g_weapons.shots_hit * 100) / g_weapons.shots_fired : 0);
    CHECK(g_waves.wave_index >= 2 || (g_waves.wave_index == 1 && g_waves.state == WAVE_INTERMISSION),
          "wave director advanced past wave 1 to %d (%s)", g_waves.wave_index, bp_waves_state_name());
    CHECK(g_player.time_alive > 1.0f,  "player simulated %.1fs alive", (double)g_player.time_alive);
    CHECK(g_player.score >= 0,         "scoring ran: %d points, %d kills, streak %d",
          g_player.score, g_player.kills, g_player.streak);
    CHECK(g_fx.particle_live >= 0,     "particle pool live count %d of %d", g_fx.particle_live, MAX_PARTICLES);
    CHECK(g_game.total_time > 0.0f,    "simulated %.1fs of game time in %d frames", (double)g_game.total_time, selftest_frames);
    CHECK(g_engine.perf.fps > 0,       "frame rate measured at %d fps", g_engine.perf.fps);
    CHECK(g_game.sim_steps > 0,        "fixed-step loop ran (last frame: %d steps)", g_game.sim_steps);

    /* 渲染斷言：場景目標必須真的被解析（resolve）到畫面上。後製著色器的頂點路徑
     * 曾經把 raylib 的「像素座標」直接當成裁剪空間座標輸出，導致整個解析四邊形
     * 落在 NDC 之外——模擬側一切正常、畫面卻全黑。模擬資料看不到這種 bug，
     * 唯一的偵測手段就是讀回真正的像素。 */
    CHECK(s_frame_mean_rgb > 15.0f && s_frame_nonblack > 0.15f,
          "3D world resolved to the framebuffer (mean %.1f, %.0f%% non-black)",
          (double)s_frame_mean_rgb, (double)(s_frame_nonblack * 100.0f));

    /* 碰撞 bug 會先以「掉出世界」的形式現身，遠早於它變成人類會回報的任何症狀，
     * 所以這是此處最便宜、價值也最高的斷言。 */
    CHECK(g_player.pos.y > g_world.bounds_min.y - 1.0f,
          "player stayed inside the level (y=%.2f, floor=%.2f)",
          (double)g_player.pos.y, (double)g_world.bounds_min.y);
    CHECK(g_player.pos.x > g_world.bounds_min.x - 1.0f && g_player.pos.x < g_world.bounds_max.x + 1.0f &&
          g_player.pos.z > g_world.bounds_min.z - 1.0f && g_player.pos.z < g_world.bounds_max.z + 1.0f,
          "player stayed inside the level bounds (%.1f, %.1f)",
          (double)g_player.pos.x, (double)g_player.pos.z);

    /* 存檔系統必須撐過一輪跑程：結算時會寫入設定檔，而毀損或寫不進去的設定檔，
     * 正是那種只會在玩家機器上才爆出來的 bug。 */
    CHECK(g_save.best_score >= 0,      "save system reachable (best score %d, kills %d)",
          g_save.best_score, g_save.total_kills);

    int errorsAfter = bp_log_error_count();
    CHECK(errorsAfter == errorsBefore, "no error-level log lines raised by the run (%d new)", errorsAfter - errorsBefore);
    #undef CHECK

    bp_input_clear_virtual();
    BP_LOGI("selftest: %s -- %d checks failed", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}

/* -------------------------------------------------------------------------------------- */
/* 視窗                                                                                   */
/* -------------------------------------------------------------------------------------- */
/*
 * 視窗旗標必須在 InitWindow「之前」設好：raylib 是在建立 GL context 時讀取它們，
 * 之後再改都會被忽略（同樣的理由，vsync 也沒有執行期的開關）。所以先載入設定檔
 * ——main() 裡的 bp_save_boot()——再於此處套用。
 */
static void apply_window_flags(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);

    /* MSAA 只在 HIGH 等級以上才請求。低於此等級時內部算繪比例已低於 1.0，再對降
     * 解析度的緩衝做超取樣（supersampling）所耗掉的填充率，是玩家明確要求我們
     * 別花的。 */
    if (g_save.settings.quality >= BP_QUALITY_HIGH) SetConfigFlags(FLAG_MSAA_4X_HINT);
    if (g_save.settings.vsync) SetConfigFlags(FLAG_VSYNC_HINT);

    /* 視窗最小化時仍繼續模擬。另一條路是被切到背景的視窗停止計時，等復原時再
     * 一次塞給我們長達數秒的畫面 delta——bp_game_frame() 的累積器夾取應付得了那
     * 種情況，但「沒能乾淨地復原」終究更糟。 */
    SetConfigFlags(FLAG_WINDOW_ALWAYS_RUN);
}

static void create_window(void)
{
    int w = g_save.settings.window_w;
    int h = g_save.settings.window_h;

    /*
     * 把尺寸夾到我們實際會落在的那個螢幕的範圍內。
     *
     * 設定檔帶著「寫下它的那台機器」的視窗大小。把 2560x1440 還原到 1366x768 的
     * 筆電上會讓 HUD 跑出螢幕外，而且在某些驅動程式上會讓 context 建立直接失敗
     * ——在玩家眼中，這就是一款「不肯啟動、又沒有任何錯誤訊息」的遊戲。夾取很
     * 便宜，卻能消滅整類問題。120／80 的留邊是給工作列與標題列的空間。
     */
    if (w <= 0 || h <= 0) { w = 1280; h = 720; }
#if !defined(PLATFORM_WEB)
    int mw = GetMonitorWidth(GetCurrentMonitor());
    int mh = GetMonitorHeight(GetCurrentMonitor());
    if (mw > 320 && mh > 240)
    {
        w = BP_MIN(w, mw - 120);
        h = BP_MIN(h, mh - 120);
    }
#endif

    InitWindow(w, h, BP_GAME_NAME " v" BP_VERSION_STRING "  --  " BP_STUDIO);
    SetWindowMinSize(640, 360);
    SetWindowTitle(BP_GAME_NAME " v" BP_VERSION_STRING);

    /*
     * 事件等待（event-wait）模式從關閉開始，等開機結束後才逐畫面解鎖——至於為什麼
     * 不能在這裡、或在開機流程的結尾直接打開，見 run_frame() 與
     * bp_engine::event_wait_allowed。
     */
    g_engine.event_wait_allowed = false;
    bp_apply_event_wait();
}

/* -------------------------------------------------------------------------------------- */
/* 畫面                                                                                   */
/* -------------------------------------------------------------------------------------- */
/*
 * 算繪一張畫面。
 *
 * BeginDrawing／EndDrawing 框住所有事情：bp_game_frame() 會把離屏場景目標解析
 * （resolve）到後緩衝，接著畫出 HUD 與選單，所以這一切都必須發生在這對呼叫之內。
 * ClearBackground 用 BLACK 而非關卡的環境色，是因為場景解析反正會蓋滿每一個像素，
 * 而清除成黑色能讓任何黑邊縫隙或漏繪的區域，在開發期間立刻現形，而不是躲在一個
 * 「看起來像那麼回事」的顏色後面。
 */
static void run_frame(void)
{
    /* GetFrameTime() 在第一張畫面會回傳 0，分頁被切到背景後則可能回傳荒謬的值。兩
     * 者都在這裡先夾取，這樣 bp_game_frame 自己的夾取就成了第二道防線而不是唯一
     * 的一道——Web 版遲早會踩到這條路徑。 */
    float dt = GetFrameTime();
    if (!(dt > 0.0f)) dt = SIM_DT;
    if (dt > MAX_FRAME_DT) dt = MAX_FRAME_DT;

    BeginDrawing();
    ClearBackground(BLACK);
    bp_game_frame(dt);

    /*
     * 在開機完成後的第一張畫面，把控制權交給事件等待（event-waiting）模式——而且
     * 只在這裡交。
     *
     * bp_game_frame() 在 BeginDrawing() 與 EndDrawing() 之間執行，而 raylib 把事件
     * 等待實作成在 EndDrawing() 內部阻塞、直到作業系統送來事件為止。若由最後一個
     * 開機階段自己去翻這個旗標，同一張畫面就會是第一張有資格「在不產生任何事件
     * 的顯示器上、永遠停在 PollInputEvents 裡」的畫面：無頭的 X server、失去焦點
     * 的視窗、資訊站（kiosk）。症狀是一款完全初始化好的遊戲停止輸出、再也不畫
     * 下一張畫面——看起來就和算繪器當掉一模一樣。
     *
     * 在這裡做，代表檢查發生在這張畫面的工作全部完成之後、阻塞呼叫之前，所以開
     * 機那張畫面永遠會完成，而「下一張」畫面才是第一張被允許阻塞的。代價是每張
     * 畫面一次分支判斷。
     */
    if (!g_engine.event_wait_allowed && g_game.boot_done && !g_engine.quit_requested)
    {
        g_engine.event_wait_allowed = true;
        bp_apply_event_wait();
    }

    EndDrawing();
}

/* -------------------------------------------------------------------------------------- */
/* 進入點                                                                                 */
/* -------------------------------------------------------------------------------------- */
#ifdef PLATFORM_WEB
/* Emscripten 的主迴圈回呼型別是 void(void)，所以每畫面的狀態必須住在各模組裡，
 * 而不是這裡的區域變數。 */
static void web_main_loop(void) { run_frame(); }
#endif

int main(int argc, char **argv)
{
    bool selftest = false;
    int selftestFrameCount = 1800;     /* 以下方固定的 60 Hz 計，約 30 秒的模擬遊玩 */
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--selftest")) selftest = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
        {
            /* 刻意不用 BP_MAX(...)：那個巨集會對參數求值多次，++i 會被執行兩次。
             * 見巨集定義處的警告。 */
            i++;
            selftestFrameCount = atoi(argv[i]);
            if (selftestFrameCount < 1) selftestFrameCount = 1800;
        }
        else if (!strcmp(argv[i], "--help"))
        {
            printf("%s v%s -- %s\n", BP_GAME_NAME, BP_VERSION_STRING, BP_STUDIO);
            printf("  --selftest          run a synthetic play-through and report pass/fail\n");
            printf("  --frames N          self-test length in frames at 60 Hz sim (default 1800)\n");
            return 0;
        }
    }

    bp_log_init();
    BP_LOGI("main: %s v%s -- %s", BP_GAME_NAME, BP_VERSION_STRING, BP_STUDIO);
    BP_LOGI("main: platform %s",
#ifdef PLATFORM_WEB
            "web / WebGL (OpenGL ES 2.0)"
#else
            "desktop / OpenGL 3.3"
#endif
    );

    bp_game_init();
    bp_save_boot();

    apply_window_flags();
    create_window();

    BP_LOGI("main: window %dx%d (monitor %dx%d), vsync %s, event-wait %s",
            GetScreenWidth(), GetScreenHeight(),
            GetMonitorWidth(GetCurrentMonitor()), GetMonitorHeight(GetCurrentMonitor()),
            g_save.settings.vsync ? "on" : "off", g_save.settings.vsync ? "blocking" : "polling");

    /* 在 Web 上，畫布要被點一下才會取得鍵盤焦點。把這句話顯示在畫面上，比再多
     * 的說明文件都值錢：少了它，玩家點了「play」，看到一個不理會鍵盤的選單，就
     * 會認定這個 build 是壞的。 */
#ifdef PLATFORM_WEB
    BP_LOGI("main: web build -- click the canvas once to capture keyboard and mouse");
#endif

    if (selftest)
    {
        /* 整個測試期間強制使用輪詢（polling）。它量的是正確性，不是閒置功耗，
         * 而且在沒有事件到來時阻塞在事件佇列上，會讓測試多花好幾分鐘的牆上時間
         * ——或在無頭顯示器上直接掛死。 */
        g_engine.event_wait_allowed = false;
        bp_apply_event_wait();
        /* 開機是每張畫面一個子系統的分期進行，所以它需要畫面才跑得完。整個測試
         * 期間事件等待都保持停用——見 bp_engine::event_wait_allowed。 */
        int rc = 0;
        while (!g_game.boot_done && rc < 240) { selftest_frame(); rc++; }
        if (!g_game.boot_done) { BP_LOGE("selftest: boot did not complete in %d frames", rc); return 2; }
        int result = run_selftest(selftestFrameCount);
        bp_game_shutdown();
        CloseWindow();
        return result;
    }

#ifdef PLATFORM_WEB
    /* 0 代表「讓瀏覽器以它允許的最快速度跑」，也就是由 requestAnimationFrame 驅動。
     * 在這裡傳固定速率會跟螢幕的更新率打架，在 144 Hz 上就會出現抖動。 */
    emscripten_set_main_loop(web_main_loop, 0, 1);
    /* 在 simulate_infinite_loop=1 之下這個呼叫永遠不返回，所以下面那段關機路徑
     * 在 Web 上是到不了的。這是正確的：瀏覽器會親手把整個實例拆掉，而 IDBFS
     * 早已由 bp_save_sync_platform() 同步完畢。 */
#else
    while (!WindowShouldClose() && !g_engine.quit_requested)
    {
        run_frame();
    }

    BP_LOGI("main: loop exited after %.1fs of game time", (double)g_engine.time_total);
    bp_game_shutdown();
    CloseWindow();
    BP_LOGI("main: clean exit");
#endif

    return 0;
}
