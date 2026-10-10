# BREACH POINT — 架構文件

> 這份文件說明**為什麼**程式碼是這個形狀。API 清單在 [`DEVELOPMENT.md`](DEVELOPMENT.md)，數值在 [`GAMEPLAY_DATA.md`](GAMEPLAY_DATA.md)。

---

## 目錄

1. [設計目標與約束](#1-設計目標與約束)
2. [分層架構與相依規則](#2-分層架構與相依規則)
3. [一帧的解剖](#3-一帧的解剖)
4. [開機順序：為什麼是 16 個階段](#4-開機順序為什麼是-16-個階段)
5. [固定時間步長與渲染插值](#5-固定時間步長與渲染插值)
6. [記憶體策略：啟動後零配置](#6-記憶體策略啟動後零配置)
7. [程序化素材管線](#7-程序化素材管線)
8. [渲染器](#8-渲染器)
9. [世界與碰撞](#9-世界與碰撞)
10. [輸入層與虛擬輸入](#10-輸入層與虛擬輸入)
11. [音訊](#11-音訊)
12. [存檔](#12-存檔)
13. [UI 與選單框架](#13-ui-與選單框架)
14. [場景機](#14-場景機)
15. [可測性](#15-可測性)
16. [平台抽象](#16-平台抽象)

---

## 1. 設計目標與約束

這些約束不是事後歸納的，它們是**在寫第一行程式之前**就定下的，之後每個設計決策都由它們推導出來。

| # | 約束 | 為什麼 | 它強迫了什麼 |
|---|---|---|---|
| C1 | **純 C11，零 C++** | 編譯快、ABI 穩定、可移植到任何平台、無例外處理的隱性成本 | 沒有 RAII → 必須有明確的 `init/shutdown` 配對與池化 |
| C2 | **零外部素材二進位檔** | repo 小、無授權問題、無「素材遺失」這一類錯誤、可完全重現 | 所有貼圖／模型／字型／音效必須程序化產生 |
| C3 | **啟動後零動態配置** | 消除執行期 OOM、消除碎片、讓幀時間可預測、web 上不需 GC 停頓 | 所有容量必須在 `bp_common.h` 用 `MAX_*` 常數宣告 |
| C4 | **固定步長模擬** | 手感必須與幀率無關；boomer shooter 的空氣加速對 dt 極敏感 | 累加器 + 螺旋死亡守衛 + 渲染插值 |
| C5 | **單向相依樹** | 22 個編譯單元若互相 include 就成了無法重構的泥球 | `engine/` 絕不知道 `game/` 的存在 |
| C6 | **資料驅動調參** | 平衡調整是設計工作，不該需要重新編譯 | 文字設定檔解析器 + 缺失鍵降級到內建預設 |
| C7 | **同一份程式碼跑桌面與 web** | 兩條產線等於兩倍的 bug | `#ifdef PLATFORM_WEB` 只准出現在邊界（`main.c`、存檔同步、滑鼠捕捉） |

### 刻意**不**做的事

- **不用 ECS。** 對於 128 個敵人上限的遊戲，完整的 archetype ECS 是過度工程。這裡用的是「**結構化陣列 + 世代式 handle**」（`bp_entity.c`），拿到 ECS 的主要好處（穩定 handle、緊湊儲存、無懸空指標）而沒有其實體查詢的複雜度。
- **不用指標鎖定（pointer lock）。** Web 上用 `DisableCursor()` + `GetMouseDelta()`。理由：跨來源 iframe 會**拒絕** pointer lock 請求，而這個遊戲的 web 預覽正是跑在 iframe 裡。
- **不用執行期腳本語言。** Lua 會帶來第二個工具鏈、第二套除錯器、以及一個新的攻擊面。設定檔 + 程序化產生已經覆蓋了「不重編就能調」的需求。
- **不自己寫 GL。** raylib/rlgl 已經是完整的抽象層；只在它缺功能時（天穹、後製）用 `rlgl` 直接下指令。

---

## 2. 分層架構與相依規則

```
┌─────────────────────────────────────────────────────────────┐
│  main.c          平台入口：視窗、帧迴圈、--selftest          │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  game/bp_game    頂層協調者：場景機、開機順序、固定步長       │
└───┬──────────┬──────────┬──────────┬────────────────────────┘
    │          │          │          │
┌───▼───┐  ┌───▼───┐  ┌───▼───┐  ┌───▼────┐
│ menu  │  │ hud   │  │ save  │  │ waves  │   表現層 / 持久層 / 導演層
└───┬───┘  └───┬───┘  └───┬───┘  └───┬────┘
    │          │          │          │
┌───▼──────────▼──────────▼──────────▼────────────────────────┐
│  player      weapons      enemies                            │  規則層
└───┬──────────────┬────────────┬──────────────────────────────┘
    │              │            │
┌───▼──────────────▼────────────▼──────────────────────────────┐
│  world      hitdetect      fx                                │  模擬層
└───────────────────────────┬──────────────────────────────────┘
                            │
┌───────────────────────────▼──────────────────────────────────┐
│  engine/  log · math · entity · config · assets · audio      │
│           gfx · camera · input                               │  引擎層
└──────────────────────────────────────────────────────────────┘
```

### 規則

**R1. 上層可以呼叫下層；下層絕不 include 上層。**
`engine/bp_input.c` 完全不知道 `ACT_FIRE` 會觸發什麼。它只知道「有一個叫 ACT_FIRE 的動作，它現在是 held」。

**R2. 遊戲層的 header 必須顯式 include 它需要的引擎 header。**
`bp_fx.h` 自己 include `bp_assets.h`，因為它的 API 出現 `bp_texture_id`。不能依賴「呼叫者大概已經 include 了」——那是隱性耦合，會在重構時炸開。

**R3. 跨層的狀態透過「發布」而非「讀取」。**
`g_weapons.ads_held` 由 `bp_game.c` 的 `handle_combat_input()` 寫入，`bp_weapons` 只讀它。武器系統不去問輸入層「玩家有沒有按住右鍵」，因為那是場景層的職責（暫停時不該讀輸入）。

**R4. 唯一例外的向上通道是回呼與發布旗標。**
`bp_save.c` 需要知道「開機完成了嗎」才能決定要不要啟用事件等待，但它**不**直接讀 `g_game.boot_done`。它讀 `g_engine.event_wait_allowed`——一個由 `bp_game`／`main.c` 發布的引擎層旗標。這樣 `bp_save` 就不需要知道「開機階段」這個概念存在。

**R5. 沒有循環 include。** 用 `grep '#include "' game/*.h engine/*.h` 可以驗證：`game/` 的 header 只會指向 `engine/`，`engine/` 的 header 只會指向 `engine/` 與 raylib。

### 為什麼 `g_engine` 定義在 `bp_log.c`

`bp_engine g_engine` 在 `bp_common.h` 用 `extern` 宣告，**定義在 `engine/bp_log.c`**。

這看起來很怪——日誌模組為什麼要擁有引擎單例？理由是：`bp_log.c` 是**相依樹最底層的編譯單元**（它只 include `bp_common.h`）。如果把定義放在 `bp_gfx.c`，那麼連結器就要透過一個葉子子系統去解析全引擎狀態，任何用到 `g_engine` 的 TU 都會間接依賴圖形層。放在最底層，依賴方向才是對的。

代價：`bp_log_init()` **絕對不能碰 GL**。它只初始化純資料欄位（`time_scale = 1.0f`、RNG 種子、`event_wait_allowed = false`）。這條約束在程式碼裡有註解，因為違反它的症狀（開機時 segfault）與原因（日誌模組呼叫了 GL）距離很遠。

---

## 3. 一帧的解剖

`main.c` 的 `run_frame()` 是唯一被允許呼叫 `BeginDrawing`／`EndDrawing` 的地方。

```
run_frame()                                     main.c
│
├─ dt = clamp(GetFrameTime(), SIM_DT, MAX_FRAME_DT)
│     GetFrameTime() 第一帧回 0，背景分頁回來時可能回幾秒。
│     這裡夾一次，bp_game_frame 內部再夾一次（第二道防線）。
│
├─ BeginDrawing()
├─ ClearBackground(BLACK)
│     為什麼是黑色而不是關卡的環境色：場景 resolve 會覆蓋每個像素，
│     所以清成黑色能讓任何 letterbox 缺口或漏畫區域**立刻可見**，
│     而不是躲在一個看起來合理的顏色後面。
│
├─ bp_game_frame(dt)                            game/bp_game.c
│   │
│   ├─ 若 !boot_done → boot_tick()，然後直接 return（見 §4）
│   │
│   ├─ 更新 g_engine.time_total、perf.frame_ms、perf.fps
│   ├─ if (IsWindowResized()) → bp_gfx_resize(); bp_ui_resize();
│   ├─ if (WindowShouldClose()) → g_engine.quit_requested = true
│   │
│   ├─ bp_input_begin_frame()          ← 從實體裝置推導 held/pressed/released
│   ├─ bp_input_apply_virtual()        ← 疊上合成輸入（見 §10）
│   │
│   ├─ handle_scene_input()            ← 暫停、偵錯覆疊、HUD 切換
│   │
│   ├─ if (bp_menu_is_open())          ← 選單是「帧驅動」不是「模擬驅動」
│   │     action = bp_menu_update(dt)     理由：選單吃 bp_pressed() 邊緣，
│   │     bp_game_apply_menu_action()     若跑在 120 Hz 會雙重消費同一個邊緣
│   │
│   ├─ ── 固定時間步長 ──              （見 §5）
│   │   accumulator += dt * time_scale
│   │   while (accumulator >= SIM_DT && steps < 8)
│   │       simulate_step(SIM_DT)
│   │   alpha = accumulator / SIM_DT   ← 渲染插值係數
│   │
│   ├─ bp_hud_update(dt)               ← 同樣是帧驅動：HUD 的平滑必須
│   │                                     匹配顯示率，否則 240 Hz 玩家的
│   │                                     血條會比 60 Hz 的更抖
│   │
│   ├─ render_frame()                  ← 用 alpha 插值，見 §8
│   │
│   └─ bp_input_end_frame()            ← 清掉 pressed/released 邊緣
│
├─ if (!event_wait_allowed && boot_done) → 啟用事件等待   ← 見下方警告
│
└─ EndDrawing()
```

### ⚠️ 事件等待的位置是**載入性**的

raylib 的 `EnableEventWaiting()` 會讓 `EndDrawing()` 內的 `PollInputEvents()` **阻塞直到 OS 送來事件**。

如果最後一個開機階段自己把旗標翻起來，那麼**完成開機的那一帧**就會是第一個可能永遠停在 `PollInputEvents` 的帧——在任何不產生事件的顯示環境下（無頭 X server、失去焦點的視窗、kiosk）。症狀是：一個完全初始化好的遊戲停止輸出、不再畫任何一帧，**看起來完全像渲染器崩潰**。

所以規則是一條：**只有 `run_frame()` 可以在 `bp_game_frame()` 回傳之後、`EndDrawing()` 之前翻這個旗標。** 這樣開機帧本身一定會完成，下一帧才是第一個允許阻塞的帧。

這個決策集中在一個函式 `bp_apply_event_wait()`（`engine/bp_log.c`），全專案只有它呼叫 `EnableEventWaiting`／`DisableEventWaiting`。要改這個行為，只有一個地方可以改錯。

> 這個 bug 實際發生過兩次，兩次都表現為「開機卡住、無任何錯誤訊息」。

### 為什麼選單與 HUD 是帧驅動、模擬是固定步長

| | 頻率 | 理由 |
|---|---|---|
| 模擬（物理、AI、武器） | 固定 120 Hz | 手感必須與幀率無關 |
| 選單 | 每渲染帧一次 | 消費 `bp_pressed()` 邊緣；跑 120 Hz 會雙重消費 |
| HUD 平滑 | 每渲染帧一次 | 平滑要匹配**顯示**率，不是模擬率 |
| 相機渲染 | 每渲染帧一次 | 用 `alpha` 插值，一帧只能有一個相機 |

---

## 4. 開機順序：為什麼是 16 個階段

```c
enum {
    BOOT_LOG = 0, BOOT_INPUT, BOOT_ASSETS, BOOT_AUDIO, BOOT_GFX,
    BOOT_FX, BOOT_WORLD, BOOT_WEAPONS, BOOT_ENEMIES, BOOT_PLAYER,
    BOOT_WAVES, BOOT_SAVE, BOOT_UI, BOOT_HUD, BOOT_MENU, BOOT_LEVEL,
    BOOT_COUNT
};
```

`boot_tick()` **每帧只跑一個階段**，然後 `bp_game_frame()` 直接 return。

### 為什麼不是一次全部初始化

三個理由，每個都是真實的：

1. **載入進度條是真的。** 如果所有初始化在一帧內完成，進度條只能假裝。分階段之後，`boot_progress = stage / BOOT_COUNT` 反映實際工作量。素材生成實測 ~330–420 ms——在慢機器上這是可感的時間，玩家應該看到它在做什麼。

2. **視窗不會顯示「未回應」。** Windows 與 X11 都會在視窗長時間不處理事件時把它標記為 hung。每帧回傳一次，事件佇列就有被處理。

3. **失敗可以定位。** 每個階段結束時都會印一行日誌。開機卡住時，最後一行日誌就是罪魁禍首。這個特性在開發期救了非常多次——見 §15 的 llvmpipe 事件。

### 開機順序為什麼是這個順序

順序不是任意的，它由**「誰需要誰已經存在」**推導：

```
LOG      最先。之後所有東西才能印日誌。也定義 g_engine。
INPUT    需要 LOG（印出綁定表）。不需要 GL。
ASSETS   需要 GL context 才能上傳貼圖。**最慢的階段**，自己印分組計時。
AUDIO    需要 LOG。裝置打不開時降級為靜音，**不該讓開機失敗**。
GFX      需要 ASSETS（後製著色器要用調色盤）。建離屏渲染目標。
FX       需要 ASSETS（粒子貼圖）+ GFX。
WORLD    需要 ASSETS（材質貼圖）。烘焙靜態批次。
WEAPONS  需要 ASSETS（視模型）+ 讀 weapons.cfg。
ENEMIES  需要 ASSETS（身體部件）+ 讀 enemies.cfg。
PLAYER   需要 WORLD（出生點）+ WEAPONS（初始裝備）。
WAVES    需要 ENEMIES + PLAYER。
SAVE     需要 INPUT（重綁）+ GFX（品質）+ AUDIO（音量）才能 apply。
UI       需要 ASSETS（字型）+ GFX（要知道視窗尺寸）。
HUD      需要 UI + ASSETS。
MENU     需要 UI + HUD + SAVE（選項畫面讀設定）。
LEVEL    最後。需要以上全部。
```

### `bp_save_boot()` vs `bp_save_init()`：為什麼存檔要拆成兩個函式

這是一個**被迫**的拆分，不是風格選擇。

建立視窗需要存檔裡的資料（視窗尺寸、vsync、MSAA、品質）。但是把設定**套用**到子系統需要那些子系統已經存在——而它們在 `InitWindow` 之前都不存在。

```
bp_save_boot()   InitWindow 之前：只解析路徑 + 讀檔 + 首次執行時寫預設值。
                 絕不觸碰 gfx / audio / camera / input。
bp_save_init()   開機 BOOT_SAVE 階段：只 apply。
```

如果在 `InitWindow` 之前呼叫 `bp_save_init()`，它會去解參考未初始化的子系統 → segfault，而且**崩潰點離原因很遠**。這條在 `bp_save.h` 的註解裡有寫明。

`main.c` 的開機順序因此是載入性的：

```c
bp_log_init();        // 定義 g_engine，此後才能印日誌
bp_game_init();       // 只清狀態，不碰 GL
bp_save_boot();       // 讀存檔，只碰檔案系統
apply_window_flags(); // SetConfigFlags 必須在 InitWindow 之前
create_window();      // InitWindow
// → 帧迴圈，開機階段逐帧推進
```

### `boot_tick()` 必須是冪等的

`boot_tick()` 在 `s_boot_stage >= BOOT_COUNT` 時進入尾段：設 `boot_done = true`、進 `SCENE_TITLE`、印日誌。

**尾段必須只跑一次。** 少了這個守衛，尾段會在之後每一帧重新執行：

- 每帧重新 `enter_scene(SCENE_TITLE)` → 吸引模式相機與橫幅永遠不會推進
- 每帧重印「boot complete」
- 每帧重新啟用事件等待 → **覆蓋任何明確要求輪詢的呼叫者**（例如 `--selftest`）

現在的寫法是 `if (g_game.boot_done) return;` 在最前面。

---

## 5. 固定時間步長與渲染插值

```c
#define SIM_HZ                  120.0f
#define SIM_DT              (1.0f / SIM_HZ)      // 8.333 ms
#define MAX_FRAME_DT            0.1f             // 幀時間上限
#define MAX_SIM_STEPS_PER_FRAME   8              // 每帧最多補 8 步
```

```c
float timeScale = BP_CLAMP(g_engine.time_scale, 0.0f, 2.0f);
g_game.accumulator += frameDt * timeScale;

int steps = 0;
while (g_game.accumulator >= SIM_DT && steps < MAX_SIM_STEPS_PER_FRAME)
{
    simulate_step(SIM_DT);
    g_game.accumulator -= SIM_DT;
    steps++;
}

/* 螺旋死亡守衛：撞到步數上限就丟掉積欠，而不是繼續追。
   追不上的機器若堅持還債，會把每一帧都花在模擬上，然後更追不上。 */
if (g_game.accumulator > SIM_DT * MAX_SIM_STEPS_PER_FRAME) g_game.accumulator = 0.0f;

g_game.alpha = BP_CLAMP(g_game.accumulator / SIM_DT, 0.0f, 1.0f);
```

### 為什麼是 120 Hz 而不是 60

boomer shooter 的空氣加速（air-strafing）對 `dt` 極度敏感：投影速度上限的計算每步都在跑，步長越長，可累積的速度越高。60 Hz 與 120 Hz 的圓弧走位上限**不一樣**，而 120 Hz 的玩家會比 60 Hz 的快。

固定在 120 Hz 讓所有幀率下手感一致。代價是模擬成本翻倍——但模擬只占幀時間的一小部分（實測 `perf.sim_ms`），渲染才是瓶頸。

### `time_scale` 的用途

`g_engine.time_scale` 乘在累加器上，所以它可以做慢動作（死亡回放）或加速（自我測試）。它被夾在 `[0, 2]`，因為 `time_scale = 0` 會凍結遊戲而 `> 2` 會撞到步數上限並開始丟時間。

### 渲染插值

`simulate_step` 只寫「模擬位置」。渲染讀的是 `bp_camera_render(alpha)`，它在上一個模擬狀態與當前狀態之間插值，**每帧只算一次**，存進 `g_camera.render_position/yaw/pitch`。所有繪圖都用這一份相機，所以一帧之內不會出現「敵人用 t 的相機畫、世界用 t+1 的相機畫」的撕裂。

---

## 6. 記憶體策略：啟動後零配置

`engine/bp_common.h` 是唯一允許定義容量的地方：

```c
#define MAX_ENEMIES              128
#define MAX_PROJECTILES          256
#define MAX_HITSCAN_TRACERS       32
#define MAX_PARTICLES           4096
#define MAX_DECALS               256
#define MAX_PICKUPS              128
#define MAX_DAMAGE_NUMBERS       128
#define MAX_SFX_VOICES            32
#define MAX_MAP_CELLS         128 * 128
#define MAX_MAP_SPAWNS            64
#define MAX_WEAPON_DEFS           12
#define MAX_ENEMY_DEFS            16
#define MAX_LEVELS                 8
#define MAX_MENU_ITEMS            24
#define MAX_LOG_LINES            256
#define MAX_SAVE_SLOTS             4
```

### 例外（三個，都是刻意的）

1. **開機時的 `Image` 暫存**（素材生成）。程序化貼圖在 CPU 算出像素，上傳 GPU 後立刻 `UnloadImage`。只發生在 `BOOT_ASSETS`。
2. **音訊開機時的 Wave 暫存**。同上。
3. **世界靜態網格烘焙**（`BOOT_WORLD` / `bp_world_load`）。`UploadMesh` 之後 raylib 接管所有權。

這三個例外都在開機或關卡載入時，**不在帧迴圈裡**。這是這條規則的真正含義：帧迴圈裡不准出現 `malloc`。

### 世代式 handle（`bp_entity.{h,c}`）

```c
bp_handle h = bp_pool_alloc(&pool);      // {index, generation}
bp_enemy *e = bp_pool_get(&pool, h);     // generation 不符 → 回 NULL
bp_pool_free(&pool, h);                  // generation++，slot 進 free list
```

為什麼需要世代：敵人死掉之後，`bp_fx` 可能還持有一個指向它的 handle（例如「這個粒子的目標」）。沒有世代，那個 handle 會 silently 指向**回收後的新敵人**，產生「粒子追蹤錯誤目標」這種極難重現的 bug。有了世代，`bp_pool_get` 回 NULL，呼叫者走「目標已消失」分支。

`bp_pool_compact()` 存在但帧迴圈不用它——它會使所有 handle 失效，只適合在關卡切換時呼叫。

---

## 7. 程序化素材管線

`engine/bp_assets.c`（1,265 行）是全專案最大的單一檔案，因為它要產生：

| 類別 | 數量 | 尺寸 | 手法 |
|---|---|---|---|
| 建築貼圖 | 17 | 256² | `fbm()` 分形雜訊 + `surface_pass()` 材質特化 |
| FX／貼花貼圖 | 13 | 128² | 解析式（放射衰減、環形、雜訊） |
| 3D 模型 | 35 | — | `model_simple()`：盒／柱／球 + `par_shapes` 風格程序化 |
| 點陣字型 | 4 | 8/16/32/64 px | 手寫 5×7 位元組表 → `GenImageFontAtlas` |

### 雜訊基底

```c
static uint32_t hash2i(int x, int y, uint32_t seed);  // 整數雜湊，無浮點
static float    hashf2(int x, int y, uint32_t seed);  // [0,1)
static float    fbm(float x, float y, int octaves, float lacunarity, float gain);
```

**全部用整數座標雜湊，不用 Perlin 的梯度表。** 理由：可重現（同一 seed 永遠同一結果，跨平台一致）、無查表、無分支。對 256² 的建築貼圖，品質差異肉眼不可見。

### 字型為什麼是手寫的

`bp_assets_font.inc` 是一张 5×7 的點陣字模表，每個字元 `[7]` 個 byte，每 byte 的低 5 位元是一列。

> ⚠️ **踩過的坑**：寫成 8 個 byte 是硬編譯錯誤。列數是 7。

為什麼不用 raylib 的 `LoadFont`：那需要外部 `.ttf`／`.fnt` 檔，違反 C2。為什麼不用 `GetFontDefault`：它的字形在低解析度下會糊，而 boomer shooter 的 HUD 需要銳利的像素感。

建構路徑是 `GlyphInfo[]` → `GenImageFontAtlas()` → `LoadTextureFromImage()`。**絕不是** `LoadFontFromImage()`——後者的行為與我們要的 atlas 佈局不符。`glyphPadding = 2` 防止相鄰字形滲色，過濾設為 POINT（最近鄰）保持像素銳利。

### 分組計時

`bp_assets_init()` 內部有 `stage_begin()`／`stage_end()`，每個素材群組結束時印一行：

```
[INFO ] assets: architecture textures   407.2 ms
[INFO ] assets: fx and decal textures    11.8 ms
[INFO ] assets: procedural models         1.0 ms
[INFO ] assets: bitmap fonts              4.0 ms
[INFO ] assets: 30/30 textures, 35/35 models, fonts ok -- 424 ms
```

**這是除錯必需品，不是裝飾。** 當開機在 llvmpipe 下變慢時，這幾行直接指出是 17 張 256² 含 mipmap + 各向異性過濾的貼圖在上傳時被軟體光柵化拖住（407 ms），而不是模型或字型。

### `bp_pal(index)`：調色盤

```c
pal_accent   (255,138, 32)   琥珀    主強調色
pal_accent2  ( 92,226,196)   青      次強調色
pal_danger   (232, 52, 60)   紅      傷害、敵人
pal_health   ( 86,214,110)   綠      血量
pal_armor    ( 96,168,255)   藍      護甲
pal_ammo     (240,208, 92)   黃      彈藥
pal_bg       ( 12, 13, 17)           背景
pal_panel    ( 24, 26, 33,220)       面板（半透明）
pal_text     (236,238,242)           文字
pal_text_dim (138,144,156)           次要文字
```

調色盤集中在 `g_assets`，HUD、粒子、關卡燈光、選單全部引用它。**改一次，全遊戲的色調一起變。**

---

## 8. 渲染器

### 管線

```
bp_gfx_begin_scene()
  → 綁定離屏 RenderTexture2D（尺寸 = 視窗 × resolution_scale）
  → BeginMode3D(camera)
       bp_world_draw_sky(cam)      天穹（見下）
       bp_world_draw(cam, viewer)  靜態批次 + 移動件
       bp_enemy_draw(cam, viewer)
       bp_pickup_draw(cam, viewer)
       bp_fx_draw(cam)             粒子、貼花、曳光彈
       bp_weapons_draw_projectiles(cam)
  → EndMode3D()
bp_gfx_end_scene()
  → 用後製著色器把離屏目標 resolve 到 backbuffer
  → bp_weapons_draw_viewmodel()   視模型畫在 resolve **之後**，不受後製影響
  → bp_hud_draw() / bp_menu_draw()
```

### 品質预设

| 品質 | `resolution_scale` | 後製 | MSAA |
|---|---|---|---|
| `BP_QUALITY_LOW` | 0.50× | 無 | 無 |
| `BP_QUALITY_MEDIUM` | 0.75× | 顆粒 + 暗角 | 無 |
| `BP_QUALITY_HIGH` | 1.00× | 完整 | 4× |
| `BP_QUALITY_ULTRA` | 1.25×（超取樣） | 完整 | 4× |

MSAA 只在 HIGH 以上請求，且**必須在 `InitWindow` 之前** `SetConfigFlags(FLAG_MSAA_4X_HINT)`——raylib 在建 GL context 時讀取旗標，之後忽略。

低於 HIGH 時內部解析度已經 <1.0，再對一個降採樣的緩衝做 MSAA 是白燒 fill rate。

### 天穹：為什麼是反轉的頂點著色球體

**死路**：用 `rlMatrixMode`／`rlMultMatrixf` 在 `BeginMode3D` 內畫等距柱狀投影 skybox。`DrawMesh` 會從 `RLGL.State.currentMatrix` 重載 modelview，所以你手動設的矩陣被覆蓋。

**可行解**：`build_sky_dome()` 產生一個半徑 1000 的球體，**法線朝內**、頂點色渐变，配合 `rlDisableBackfaceCulling()`。實測 779 頂點／1440 三角形。

這也順便解決了另一個問題：raylib 5.5 **沒有內建霧**。`bp_world` 的 `fog_color`／`fog_density` 目前是純資料欄位，天穹渐变承擔了大部分「遠處淡出」的視覺工作，直到有自訂著色器。

### 靜態批次烘焙

`bp_world_load()` 把所有靜態 AABB **依材質合併**成 mesh，實測 FOUNDRY 是 12 個批次／516 三角形。

為什麼：一張地圖有 50 個 solid，逐個 `DrawCube` 是 50+ 個 draw call。合併後每材質一個，約 14 個 draw call 就能畫完整張地圖。

每面的 UV 以**公尺**為單位除以材質的 `uv_scale`，所以 2m 的牆與 8m 的牆貼圖密度一致。

### 假 Lambert 面光

```c
static const float s_face_light[6] = { 1.00, 0.30, 0.78, 0.62, 0.86, 0.55 };
                              /*  +Y    -Y    +X    -X    +Z    -Z  */
```

烘焙時直接把這六個值乘進頂點色。沒有光源、沒有法線貼圖、沒有 shadow map——但幾何體立刻有了體積感。這是 boomer shooter 的正確取捨：**讀取性 > 真實感**。

### 剔除

`CULL_DISTANCE 170`。`bp_world_draw` 與所有實體繪圖都先算到 viewer 的距離，超過就直接跳過。搭配 `bp_world` 的 AABB 距離早退，這是「沒有空間分割也能跑 128 個敵人」的原因。

### 視模型的矩陣鏈

**死路**：`DrawModelEx(model, pos, axis, angle, scale, tint)` 無法表達組合式關節鏈（槍身 → 滑套 → 手，各有各的偏移與旋轉）。

**可行解**：自己組 `Matrix`（ZXY 順序），然後

```c
rlSetMatrixModelview(m);
DrawMesh(mesh, material, m);
```

逐個 mesh 畫。色調折進 `materials[0].maps[MATERIAL_MAP_DIFFUSE].color`，畫完還原。

### 正交投影覆寫（除錯繪圖）

在 `BeginMode3D` 內畫 2D 除錯資訊：

```c
rlMatrixMode(RL_PROJECTION); rlPushMatrix(); rlLoadIdentity();
rlOrtho(...);
rlMatrixMode(RL_MODELVIEW);  rlPushMatrix(); rlLoadIdentity();
   /* 畫 */
rlPopMatrix(); rlMatrixMode(RL_PROJECTION); rlPopMatrix();
```

**兩個 `rlPopMatrix()` 都必须在 `EndMode3D()` 之前。**

### raylib 5.5 **沒有**的東西

這份清單是踩坑踩出來的，寫在這裡避免重蹈：

| 不存在 | 實際可用的替代 |
|---|---|
| `GetKeyName()` / `GetCharName()` | 自己維護 code→label 表（`bp_input_key_name`） |
| `GetFrameStats()` | 自己測（`g_engine.perf`） |
| `rlRenderBatch.drawCounter` 的公開存取器 | **無法取得 draw call 數** → HUD 與自我測試都不回報它 |
| `LoadSoundFromSample()` | `LoadSoundFromWave(Wave)`（會複製，呼叫端 `UnloadWave`） |
| `DrawCubeTexture()` | 只有 `DrawCube`／`DrawCubeV`／`DrawCubeWires`／`DrawCubeWiresV` |
| `DrawText3D()` | 用 `DrawBillboard` 或自己排 |
| `DrawModel(model, matrix, color)` | `DrawModel(model,pos,scale,tint)` / `DrawModelEx(...)` |
| `GetShaderDefault()` | `rlGetShaderIdDefault()` |
| `rlSetTextureAnisotropy()` | `SetTextureFilter(t, TEXTURE_FILTER_ANISOTROPIC_4X\|8X\|16X)` |
| `rlEnableBlendMode()` / `rlDisableBlendMode()` | `rlSetBlendMode(int)` + `RL_BLEND_ALPHA/ADDITIVE/ADD_COLORS` |
| `rl_check_render_batch()` | `rlCheckRenderBatchLimit(int)` |
| 內建霧 | 無。天穹渐变 + 距離剔除 |
| `GAMEPAD_BUTTON_LEFT_FACE_1` | dpad 沒有 `_1` 後綴 |

---

## 9. 世界與碰撞

### 資料表示

世界是**一個扁平的 AABB 陣列**，沒有 BSP、沒有 octree、沒有網格。

```c
typedef struct {
    bp_aabb box;
    int     material;      // → s_materials[]
    bool    dynamic;       // 移動件
    bool    shoot_through; // 子彈可穿透
    bool    explode;       // 油桶
    float   health;
    bool    destroyed;
} bp_solid;
```

為什麼夠用：50 個 solid 的線性掃描 + 距離早退，比維護空間結構的複雜度便宜得多。**在 50 個 AABB 的規模上，任何空間分割都是負最佳化。**

如果關卡規模成長到 500+ solids，`MAX_MAP_CELLS (128×128)` 的網格欄位已經預留了，屆時填它。

### 17 種材質

每個材質是一個 `bp_material_def`：

```c
{ "grate", TEX_FLOOR_GRATE, SFX_HIT_ARMOR, SFX_FOOTSTEP,
  1.80f,   /* uv_scale */
  0.88f,   /* friction */
  0.90f,   /* 反射／光澤 */
  true,    /* penetrable：子彈可穿過 */
  false,   /* emissive */
  false,   /* solid for movement */
  0.0f,    /* hazard_dps */
  {255,255,255,255} },
```

`concrete brick metal panel rust grate tile hazard crate barrel trim pillar light screen vent pipe field` = 17 種。

`hazard` 的 `hazard_dps = 9.0`，`field` 是 `penetrable + emissive`（能量護盾）。

**材質同時驅動五件事**：貼圖、撞擊音效、腳步音效、摩擦力、UV 密度。這是「一個資料結構統一表現與物理」的刻意設計——加一種材質就同時得到正確的視覺與正確的手感。

### 碰撞解算

```c
bool bp_body_overlaps_box(Vector3 center, float radius, float height, const bp_aabb *box);
bool bp_resolve_body_box (Vector3 *center, float radius, float height, const bp_aabb *box);
```

玩家是**膠囊近似為「圓柱 = 圓 + 高度」**，逐軸推出（先 Y 再 XZ）。逐軸的好處是「沿牆滑行」自然成立，不需要額外的投影數學。

`bp_world_sweep()` 給彈體用：回傳 `t`、法線、命中的 solid。

### ⚠️ 關卡作者必讀：`ADD_SOLID` 吃的是**最小角**

```c
ADD_SOLID(x, y, z, w, h, d, material);
//        ↑min corner    ↑size
```

`box.min = (x,y,z)`，`box.max = (x+w, y+h, z+d)`。

**每一個高度帶都必須可以用 ≤1.4 m 的台階抵達，且單一階梯 ≤0.55 m**（0.55 m 是玩家不用跳就能走上的高度）。

> 手寫的關卡高度曾經默默破壞了可 traversal 性：某個平台的高度差是 0.72 m，玩家永遠上不去，而**沒有任何錯誤訊息**。這是最難發現的一類 bug，因為它看起來像「關卡設計不好」而不是「程式壞了」。

三張地圖的高度預算：

| 高度差 | 可達方式 |
|---|---|
| ≤ 0.55 m | 直接走上去 |
| ≤ 1.10 m | 站立跳（跳躍高度 1.32 m，留餘量） |
| ≤ 1.50 m | 衝刺跳 |
| > 1.50 m | **必須有樓梯／斜坡／升降機** |

SLAG PIT 的三個高度帶（0.0 / 2.2 / 6.9）用 10 階、每階 0.69 m 的樓梯塔連接——注意 0.69 > 0.55，所以那些樓梯**需要跳**，這是刻意的：棧道應該是要努力才能拿到的位置。

### 移動件與觸發器

- **移動件（mover）**：門、升降機、快門。有起點／終點／速度／等待時間。`bp_world_activate_doors(pos, radius)` 由玩家的 `ACT_USE` 或 proximity 觸發。
- **觸發器（trigger）**：`PLAYER_START`（1 個）、`EXIT`（1 個，**永不消耗**）、`SECRET`（1 個，+500 分）、`HEALTH`/`ARMOR`/`AMMO`/`WEAPON`（定時重生）。

EXIT 永不消耗是刻意的：玩家可能在波次未完成時踩到它，那時不該觸發過場；等 `g_waves.state == WAVE_COMPLETE && g_player.exit_reached` 才進 `SCENE_LEVEL_COMPLETE`。

---

## 10. 輸入層與虛擬輸入

### 27 個動作

```c
ACT_MOVE_FORWARD BACK LEFT RIGHT   ACT_JUMP CROUCH SPRINT
ACT_FIRE ALT_FIRE RELOAD
ACT_WEAPON_1..5  ACT_WEAPON_NEXT PREV
ACT_USE  ACT_PAUSE SCOREBOARD DEBUG_OVERLAY
ACT_MENU_UP DOWN LEFT RIGHT ACCEPT BACK
```

每個動作有 `keys[2]`（兩個綁定槽）+ `padButtons[2]` + `padAxis`/`padAxisSign` + `name`。

### ⚠️ 滑鼠按鈕與鍵盤按鍵的編碼空間**重疊**

這是整個專案最陰險的坑，值得完整記錄。

raylib 的 `MouseButton` 是 `0..6`，`KeyboardKey` 是 `32..`。而且：

```c
MOUSE_BUTTON_LEFT == 0      // ← 0 同時是我們的「未綁定」哨兵值！
MOUSE_BUTTON_FORWARD == 5 == KEY_E
MOUSE_BUTTON_BACK    == 6 == KEY_F
```

後果：

1. **混合 `switch` 會出現 `duplicate case value`** 編譯錯誤。
2. **範圍測試會靜默錯誤。** `if (code >= MOUSE_BUTTON_LEFT && code <= 10)` 會匹配**方向鍵與左右 Ctrl**，因為 `KEY_LEFT_CONTROL == 341`…不對，實際上它會匹配所有 32 以下的東西，而因為 `MOUSE_BUTTON_LEFT == 0`，這個條件對**幾乎所有按鍵**都成立。
3. 這**曾經是已上線的 bug**：`bp_input.c` 的滑鼠範圍判斷把方向鍵當成滑鼠按鈕。

**唯一正確的方案**：偏置。

```c
#define BP_BIND_MOUSE_BASE   4096
#define BP_BIND_FROM_MOUSE(mb)  (BP_BIND_MOUSE_BASE + (mb))
#define BP_BIND_IS_MOUSE(code)  ((code) >= BP_BIND_MOUSE_BASE)
#define BP_BIND_INDEX(code)     ((code) - BP_BIND_MOUSE_BASE)
```

4096 遠高於任何 `KeyboardKey`，所以兩個空間不再重疊，`0` 可以安全地表示「未綁定」。

### 焦點遺失必須釋放所有按鍵

`bp_input_begin_frame()` 第一件事：

```c
bool focused = IsWindowFocused();
g_input.focus_lost_this_frame = (g_input.captured && !focused);
```

若焦點遺失，所有動作的 `held` 強制為 false。

這是經典的「玩家 alt-tab 回來發現角色一直往前跑」bug。沒有這個守衛，`IsKeyDown()` 在視窗失焦時回傳的最後一個狀態會被凍結。

### 虛擬輸入：讓遊戲可以被程式驅動

```c
void bp_input_set_virtual(bp_action a, bool down);
void bp_input_clear_virtual(void);
void bp_input_apply_virtual(void);   // 只在 bp_game_frame 內、begin_frame 之後呼叫
```

**這是「疊加」而不是「取代」**：虛擬輸入用 OR 併入實體輸入，所以開發者可以在自我測試執行中直接介入，不用砍掉 process。

**順序是載入性的。** `bp_input_begin_frame()` 從實體裝置**推導** `held/pressed/released`，所以虛擬狀態必須在它**之後**套用，否則會被抹掉。`bp_game_frame()` 在正確的位置呼叫 `bp_input_apply_virtual()`，別的地方都不准呼叫。

`set_virtual` 是**邊緣感知**的：連續多帧設為 down 會產生 `held = true` 每帧、但 `pressed = true` 只在上升沿。這讓一次性動作（跳躍、換彈、切槍）在合成控制下行為正確。

> **踩過的坑**：自我測試最初直接寫 `g_input.act[ACT_FIRE].held = true`，在 `bp_game_frame()` **之前**。下一帧的 `begin_frame()` 把它抹掉了，所以測試永遠沒有開火——而症狀看起來像武器系統壞了。

---

## 11. 音訊

### 全程序化，零 `.wav`

44 種音效（`bp_sfx_id`）全部在 `bp_audio_init()` 由合成器算出：振盪器 + 包絡 + 單極低通 + 雜訊。

`bp_onepole` 濾波器有一個坑：**型別雙關 `(bp_onepole *)v` 會撞到 `v->phase`**，必須用 `&v->lp`。

### 聲部管理

```c
#define MAX_SFX_VOICES 32
```

加上 **3 聲部 alias bank**：同一個音效在短時間內重複觸發時，共用一個 voice 並重置包絡，而不是搶新 voice。這是「機槍聲不會變成白噪聲」的關鍵。

### 音樂：串流回呼，不是檔案

`LoadAudioStream(rate, 32, 2)` + `SetAudioStreamCallback()`。180 BPM，程序化產生。

回呼**在音訊執行緒上執行**，所以它只能碰檔案靜態的合成器狀態，**絕不能**碰任何 GL、任何 `g_*` 遊戲狀態、任何會 allocate 的東西。

> **絕對不要**用 `LoadMusicStreamFromMemory("wav", NULL, 0)` — 那是 UB。

`music_voice.freq` **必須在觸發時鎖存**，不能在回呼中重讀 pattern——否則音符進行到一半時 bass 會改變音高。

### 裝置打不開時降級

```
WARNING: AUDIO: Failed to initialize playback device
[WARN ] audio: device failed to open -- running silent
```

`bp_audio_init()` 失敗時設一個旗標，所有 `bp_audio_play()` 變成 no-op。**開機不該因為沒有音效卡而失敗**——這對 CI 與無頭環境是必需的。

### 動態強度

`bp_audio_set_intensity(target01, dt)`：標題畫面 0.20、關卡開場 0.30、戰鬥中依敵人數量上升、暫停 0.14、死亡 0.10。音樂層（鼓、bass、lead）依強度漸進加入。

---

## 12. 存檔

### 格式：人類可讀的文字

```
# BREACH POINT -- player settings (version 3)
# Hand-editable. Unknown keys are ignored, missing keys keep their default.
# Delete this file to reset everything to factory defaults.

settings profile
    version            = 3
    profile_name       = PLAYER
    quality            = 2
    resolution_scale   = 1.0000
    fov                = 95
    vsync              = true
    ...
    # key bindings: bind_<action>_<slot> = raylib key code
    bind_0_0           = 87
    bind_0_1           = 265
```

**與 `assets/cfg/*.cfg` 是同一套語法**，由同一個解析器（`engine/bp_config.c`）讀取。這是一個刻意的決定：玩家用記事本改存檔時，用的是設計者改武器數值時的同一個格式。

`progress.cfg` 則是：

```
totals all
    total_runs = 12
    ...

level 0-1          # <level>-<difficulty>
    completed = true
    best_score = 48200
    ...
```

### 原子寫入

```c
static bool atomic_write(const char *path, const char *data, int len)
{
    // 寫到 path.tmp
    // fflush + fsync
    // remove(path)
    // rename(tmp, path)   ← 原子
}
```

**為什麼**：寫到一半時斷電／當機，會讓玩家失去整個存檔。`rename` 在同一檔案系統上是原子的，所以舊檔與新檔之間沒有中間狀態。

### 序列化：一個 buffer，一次 `fwrite`

整個設定檔用 `snprintf` 累加進一個檔案靜態 buffer，最後一次寫出。理由：

1. 避免幾百次小 write 的 syscall 開銷
2. 保證「寫出的內容」與「buffer 裡的內容」完全一致（沒有部分寫入）
3. 有 `SAVE_BUF_SIZE - 512` 的剩餘空間檢查，滿了會印警告而不是溢位

### 版本遷移

```c
#define SAVE_VERSION 3
```

- 檔案版本 **>** `SAVE_VERSION`：保留檔案不动，用預設值執行，印警告。（不覆寫一個比我們新的存檔——那可能是玩家用了更新版本。）
- 檔案版本 **<** `SAVE_VERSION`：遷移，然後重寫。
- 檔案損壞／不存在：預設值 + 警告。**絕不讓開機失敗。**

### 平台路徑

| 平台 | 路徑 |
|---|---|
| 桌面 | `$XDG_DATA_HOME/BreachPoint/` 或 `~/.local/share/BreachPoint/` |
| Web | `/bpdata/`（emscripten IDBFS，映射到 IndexedDB） |

Web 上每次寫入後必須 `FS.syncfs(false, callback)`，否則資料留在 MEMFS，重新整理就消失。`bp_save_sync_platform()` 封裝了這個差異（桌面上是 no-op）。

### 解鎖規則

關卡 N 在 N−1 於**任何難度**完成後解鎖。最佳成績只在**改進時**更新（不會因為一次爛run而降低）。

`bp_save_completion_percent()` = 已完成的 (關卡 × 難度) 組合 / 總組合數。

---

## 13. UI 與選單框架

### 640×360 虛擬畫布

所有 UI 座標都在一個固定的 640×360 虛擬空間裡，`bp_ui_resize()` 算出整數倍縮放與 letterbox 偏移。

```
[INFO ] ui: virtual canvas 640x360 at 2x scale (window 1280x720, letterbox 0,0)
```

**整數倍**縮放是關鍵：點陣字型在非整數縮放下會糊。1280×720 → 恰好 2×。

### 文字 API 的兩個版本

```c
void bp_ui_text      (float x, float y, float size, Color c, const char *fmt, ...);
void bp_ui_text_align(float x, float y, float size, bp_ui_align a, Color c, const char *fmt, ...);
void bp_ui_text_shadow(float x, float y, float size, bp_ui_align a, Color c, Color shadow, const char *fmt, ...);
```

> ⚠️ **踩過的坑**：`bp_ui_text` **沒有** align 參數。把 `UI_ALIGN_LEFT` 傳給它是型別錯誤。這個錯誤在正則批次替換時很容易產生——曾經用 regex 把所有 `bp_ui_text(` 換成 `bp_ui_text_align(`，結果把不需要 align 的呼叫也換了。

### 選單：一套列堆疊驅動全部 8 個畫面

```c
MENU_SCREEN_NONE TITLE LEVELS OPTIONS CONTROLS CREDITS PAUSE RESULTS QUIT
```

每個畫面由 `build_*()` 產生一個 `bp_menu_row[]`：

| 列類型 | 行為 |
|---|---|
| `MROW_ACTION` | 啟動時回傳 `r->id` |
| `MROW_TOGGLE` | bool，循環 on/off |
| `MROW_CYCLE` | 在 `choices[]` 中循環 |
| `MROW_SLIDER` | `[vmin,vmax]` 浮點，`vstep` |
| `MROW_LABEL` | 不可互動的標題 |
| `MROW_SPACER` | 空白 |

**動作 id 的編碼**：正數 = 遊戲動作（`A_PLAY = 1`…），負數 = 框架自己（`MENU_ACT_BACK = -2` 等）。`bp_game.c` 用一組**本地複製**的 `GA_PLAY = 1 … GA_LEVEL_BASE = 100, GA_BIND_BASE = 200` 常數，**靠約定**與 `bp_menu.c` 保持同步。

> 這是一個已知的脆弱點。正確做法是把 id 抽到共用 header（`bp_menu_ids.h`），見 ROADMAP。

### 欄位流：為什麼不做捲動

360 单位的虛擬高度最多放 ~15 列，但選項畫面有 21 列。

**不做捲動**，因為捲動需要捲動條、裁剪區、以及第二個導覽軸——對一個點陣字型 UI 來說是過度的複雜度。

**改成欄位流**：超過 `column_rows` 的列流進第二欄（`column_gap` 間距）。選擇仍然是嚴格上下移動然後換欄，所以心智模型還是「一個清單」。

### 滑鼠互動與 ⚠️ 遊標守衛

列可以用滑鼠點擊。滑動器／循環／開關列的**右三分之一**點擊是 +1、中三分之一是 −1——這給了滑桿可用的滑鼠互動而不需要拖曳 widget。

```c
Vector2 msp = GetMousePosition();
bool mouseInside = msp.x >= 0 && msp.y >= 0 &&
                   msp.x < GetScreenWidth() && msp.y < GetScreenHeight();

for (int i = 0; mouseInside && i < g_menu.list.count; i++) { ... }
```

**為什麼需要 `mouseInside` 守衛**：`bp_ui_mouse_virtual()` 把螢幕座標映射進 640×360 畫布並**夾限**。一個在視窗外的遊標（因為還沒被移動过而在原點、或在另一個螢幕上）會被夾到一個**看起來合理的畫布座標**。沒有守衛時，每個選單的第一帧就會 hover-select 那個座標上的列，然後**靜默地把 ACCEPT 重新導向**：玩家在 NEW RUN 上按 Enter，結果開了 OPTIONS。

這個 bug 在桌面上看不見（按鍵之前遊標已經在視窗內了），在無頭／kiosk 環境下則是永久的。

### 按鍵捕捉

CONTROLS 畫面可以重新綁定任何動作：

```c
g_menu.capturing_action = r->id - A_BIND_BASE;
```

捕捉模式下 `bp_menu_update` **吞掉所有其他導覽**。`GetKeyPressed()` 填 slot 0，滑鼠按鈕 0–2 存為 `BP_BIND_FROM_MOUSE(mb)`。`ESC` 取消而不是綁定——因為 ESC 同時是 `ACT_PAUSE`，把 pause 綁到自己會是一個看起來像失敗的 no-op。

---

## 14. 場景機

```c
typedef enum {
    SCENE_BOOT = 0,       // 開機帧
    SCENE_TITLE,          // 主選單 + 即時漂移相機
    SCENE_LEVEL_INTRO,    // 區域名稱卡，世界已載入，玩家靜止
    SCENE_PLAYING,        // 遊戲本體
    SCENE_PAUSED,         // 模擬凍結，選單活著
    SCENE_DEAD,           // 死亡運鏡 → 結算
    SCENE_RESULTS,        // 單次 run 結算
    SCENE_LEVEL_COMPLETE, // 抵達出口：統計 → 下一區或選單
    SCENE_GAME_COMPLETE   // 全部通關
} bp_scene;
```

### 每個場景模擬什麼

| 場景 | 玩家 | 敵人 | 世界 | 相機 |
|---|---|---|---|---|
| `SCENE_TITLE` | — | — | — | **吸引模式軌道**（見下） |
| `SCENE_LEVEL_INTRO` | 靜止 | 不生成 | tick（移動件、觸發器） | 模擬但不动 |
| `SCENE_PLAYING` | 全部 | 全部 | 全部 | 玩家控制 |
| `SCENE_PAUSED` | **凍結** | **凍結** | **凍結** | **凍結** |
| `SCENE_DEAD` | 死亡動畫 | tick | tick | 慢速漂移 + 紅暗角 |
| `SCENE_RESULTS` / `LEVEL_COMPLETE` | — | — | — | 吸引模式軌道 |

**`SCENE_PAUSED` 凍結相機是刻意的**：暫停時讓相機繼續軌道會讓畫面看起來還在跑。「暫停」的定義就是模擬**與視角**都停止。

### 吸引模式相機

標題畫面與結算畫面不是靜態背景——相機在**已載入的關卡**裡軌道飛行：

```
yaw    += 6.5°/s
radius  = 26 ± 6
pitch   = 由高度推導
```

而且它**走真的 `bp_camera_simulate()`**，不是自己算一個 `Camera3D`。這樣它自動繼承畫面震動、FOV 呼吸、後座力衰減——選單背後的畫面與實際遊玩用的是同一套相機程式碼。

「選單永遠有真實競技場在後面」比一個平色背景好太多，而且成本是零（世界已經載入了）。

### 關卡生命週期

```
SCENE_TITLE
   │ bp_game_start_level(i)
   ▼
SCENE_LEVEL_INTRO  ── INTRO_CARD_TIME = 3.0s ──┐
   │  （開火或 Accept 可跳過，但跳過不能跳過波次開始）
   ▼                                            │
SCENE_PLAYING                                   │
   │                                            │
   ├─ 玩家死亡 → SCENE_DEAD ── 2.2s ─→ SCENE_RESULTS
   │                                      │
   └─ WAVE_COMPLETE && exit_reached       │
        → bp_game_submit_run(true)        │
        → SCENE_LEVEL_COMPLETE ───────────┤
                                          ▼
                              bp_game_next_level() 或回 TITLE
```

**關卡開始只給手槍 + 2 個彈匣。** 重啟保留該 run 的分數與擊殺數。

`bp_game_submit_run()` 是**冪等**的（靠 `g_game.submitted` 旗標）——死亡路徑與通關路徑都可能觸發它，不能送兩次。

`SCENE_DEAD` 跑 2.2 秒：慢速漂移 + 紅暗角，然後自動提交並開 RESULTS。這段延遲是刻意的——立刻切到結算畫面會讓死亡失去重量。

### 波次導演

```
WAVE_IDLE → WAVE_INTRO → WAVE_ACTIVE → WAVE_CLEARING → WAVE_INTERMISSION → (下一波)
                                                            ↓
                                                      WAVE_COMPLETE
```

| 常數 | 值 | 意義 |
|---|---|---|
| `WAVE_INTRO_TIME` | 2.60s | 橫幅顯示、**不生成敵人** |
| `WAVE_INTERMISSION_MIN/MAX` | 3.20 / 5.60s | 補給與倒數 |
| `WAVE_BASE_BUDGET` | 10 | 第 1 波敵人數（VETERAN） |
| `WAVE_BUDGET_GROWTH` | 4 | 每波增加 |
| `WAVE_BUDGET_MAX` | 46 | **硬上限** |
| `ONSCREEN_MIN/MAX` | 7 / 20 | 同時在場上限 = 效能預算 |
| `SPAWN_INTERVAL_MIN/MAX` | 0.30 / 1.50s | |
| `SPAWN_OPENING_BURST` | 4 | 波次開始時同時抵達 |
| `SPAWN_MIN_DIST_PLAYER` | 9.0m | 不能在玩家臉上生成 |
| `SPAWN_HIDE_DIST` | 16.0m | 超過此距離跳過視錐檢查 |
| `SPAWN_VIEW_COS` | 0.35 | ~69° 視線外才生成 |
| `SPAWN_TURRET_MIN_DIST` | 14.0m | 砲塔需要視線空間才公平 |
| `SKILL_LAMBDA` | 0.9 | 技術評估的指數平滑 |
| `SKILL_CLAMP` | 0.18 | 橡皮筋影響上限 ±18% |
| `PERFECT_WAVE_BONUS` | 750 | 無傷過波 |

`WAVE_BUDGET_MAX = 46` 的註解值得複製：**「競技場根本裝不下更多，超過就變成碰撞解算問題而不是戰鬥問題。」** 這是把設計限制寫進程式碼常數的例子。

### 軟鎖防護

```c
case WAVE_CLEARING:
    if (bp_enemy_alive_count() <= 0) finish_wave();
    else if (g_waves.state_time > 120.0f)
    {
        BP_LOGW("waves: %d enemies unreachable after 120s -- forcing wave clear", ...);
        bp_enemies_clear();
        finish_wave();
    }
```

如果最後一個敵人因為某種原因不可達（被移動件埋住、掉出競技場），波次**必須**還是會結束，否則玩家被軟鎖在出口之外，沒有任何回復手段。120 秒的強制清除 + 警告日誌是這個保證。

---

## 15. 可測性

### 為什麼自我測試住在 `main.c` 而不是 `tests/`

`--selftest` 驅動**真實的帧迴圈**，包含 GL context、渲染器、音訊裝置路徑、場景機。

單元測試可以證明 `bp_math` 是正確的；**只有這個能證明開機、載入關卡、生成波次、開火、畫 HUD 全部在一個 process 裡一起運作。** 這是「它能編譯」與「它能出貨」之間的差別。

它被**無條件編譯進去**，不藏在 debug flag 後面：一個能自我驗證的出貨版值那幾 KB，而且支援工程師可以叫玩家跑一條指令然後貼出輸出。

### 固定合成帧時間

```c
#define SELFTEST_DT (1.0f / 60.0f)
bp_game_frame(SELFTEST_DT);
```

**不**用 `GetFrameTime()`。理由：

| | 用真實帧時間 | 用固定 SELFTEST_DT |
|---|---|---|
| 确定性 | ❌ 依機器速度 | ✅ 同样帧数 = 同样模擬時間 |
| 覆蓋率 | ❌ 慢機器上 600 帧可能只模擬 2 秒 | ✅ 600 帧 = 正好 10 秒 |
| 速度 | ❌ 受渲染速度限制 | ✅ llvmpipe 下 20 秒模擬跑 ~12 秒 |

> **實際踩過的坑**：自我測試最初用 `GetFrameTime()`。在 llvmpipe（軟體光柵化）下每帧 ~9 ms，600 帧只模擬了 4 秒——**永遠走不完 3 秒的關卡開場卡**。測試回報「0 個敵人生成、0 發子彈、玩家存活 0 秒」，看起來像四個獨立的遊戲 bug。實際上是**一個**計時假設。

代價：自我測試**不能**偵測「遊戲跑太慢」。那由帧預算與結尾的 fps 檢查負責。

### 18 項斷言

```
PASS  boot completed (16 stages)
PASS  assets generated (330 ms)
PASS  bitmap font atlas usable
PASS  level loaded: 50 solids, 28 triggers
PASS  static geometry baked into 12 batches
FAIL  enemies spawned: 0 (0 alive at end)
...
PASS  3D world resolved to the framebuffer (mean 43.2, 78% non-black)
PASS  player stayed inside the level (y=0.05, floor=-1.00)
PASS  player stayed inside the level bounds (0.0, 27.0)
PASS  save system reachable (best score 0, kills 0)
FAIL  no error-level log lines raised by the run (5 new)
```

幾個值得注意的：

- **「玩家留在關卡內」**：碰撞 bug 在人類能回報任何症狀之前，會先表現為「穿過世界掉下去」。這是最便宜的高價值斷言。
- **「3D 場景解析到畫面上」**：模擬側的斷言看不到渲染管線。後製著色器曾經把 raylib 的像素座標直接當 NDC 輸出，解析四邊形整個落到裁剪體外——模擬全過、畫面全黑。這條在最後一幀讀回framebuffer 像素統計（亮度／非黑比例），是唯一能抓到這類 bug 的斷言。
- **「沒有 ERROR 級日誌」**：用 `bp_log_error_count()` 計數器，而不是抓 stdout。基線在**迴圈結束後**才取——因為斷言本身失敗時會用 `BP_LOGE` 印出，把自己的輸出算進去會讓最後這個檢查因自己的輸出而失敗。
- **逐批計時**：每 120 帧印一行 `sim Xs / scene / wave / alive / particles / ms per frame`。這是「發現某個場景在軟體光柵化下要 300 ms」的方法，而不是盯著凍結的 console 五分鐘。

### 結束碼

| 碼 | 意義 |
|---|---|
| 0 | 全部通過 |
| 1 | 有檢查失敗 |
| 2 | 開機在 240 帧內未完成 |

CI 可以直接用這個。

---

## 16. 平台抽象

### `#ifdef PLATFORM_WEB` 只准出現在三個地方

1. **`main.c`** — 帧迴圈驅動方式
2. **`bp_save.c`** — IDBFS 同步
3. **`bp_input.c`** — 滑鼠捕捉策略

其他 20 個編譯單元**完全不知道平台存在**。這是 C7 的執行方式。

### 帧迴圈

**桌面**：
```c
while (!g_engine.quit_requested) run_frame();
```
搭配 `SetTargetFPS(0)`（vsync 控制節奏）或 `SetTargetFPS(240)`。

**Web**：
```c
emscripten_set_main_loop(web_main_loop, 0, 1);
```
`0` 表示「瀏覽器允許多快就跑多快」，即 requestAnimationFrame 驅動。**web 上不能有 `while` 迴圈**——那會凍結整個分頁。

`web_main_loop` 是 `static void (void)`，emscripten 要求的簽名，所以每帧狀態都住在模組裡而不是區域變數。

### 滑鼠

| 平台 | 策略 |
|---|---|
| 桌面 | `DisableCursor()` + `GetMouseDelta()` |
| Web | 同上，**不用 pointer lock** |

不用 pointer lock 的理由見 §1：跨來源 iframe 會拒絕請求。`GetMouseDelta()` 在游標隱藏時照常運作，而且不需要任何權限。

代價：web 上玩家必須先**點一下 canvas** 才能拿到鍵盤焦點。這在啟動時明確印出來：

```
[INFO ] main: web build -- click the canvas once to capture keyboard and mouse
```

**在螢幕上說出來，勝過任何數量的文件。** 沒有它，玩家點「Play」、看到一個不理鍵盤的選單、然後結論是「這個建置壞了」。

### 視窗尺寸夾限

```c
int mw = GetMonitorWidth(GetCurrentMonitor());
int mh = GetMonitorHeight(GetCurrentMonitor());
if (mw > 320 && mh > 240) {
    w = BP_MIN(w, mw - 120);
    h = BP_MIN(h, mh - 120);
}
```

存檔帶著**寫入它的那台機器**的視窗尺寸。把 2560×1440 還原到 1366×768 的筆電上，會把 HUD 推到螢幕外，在某些驅動上還會讓 **GL context 建立直接失敗**——對玩家表現為「遊戲 refusing to start，沒有任何錯誤訊息」。

120 的邊界留給工作列與標題列。`mw > 320` 的守衛是因為無頭環境下 `GetMonitorWidth` 可能回傳 0 或垃圾值（實測 Xvfb 會印 `GLFW: Failed to find selected monitor`）。

---

## 附：`-W` 旗標為什麼是這一組

```
-Wall -Wextra -Wshadow -Wdouble-promotion -Wformat=2
-Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function
```

| 旗標 | 抓到了什麼真實 bug |
|---|---|
| `-Wdouble-promotion` | **每一個**傳給 variadic 日誌巨集的 float 都會被提升為 double。強制在呼叫點寫 `(double)`，讓「這個值是 float 還是 double」變成顯式決定。raylib 的 `Lerp` 是 double-based，所以專案用自己的 float `bp_lerp`。 |
| `-Wshadow` | 巢狀迴圈用同一個字母（外層 `int n`、內層 `Vector3 n`）。內層被重新命名為 `fn`。 |
| `-Wformat=2` | 拒絕把純字串當 format 參數傳。標籤必須走明確的 `"%s"`。 |
| `-Wextra` | 未初始化的欄位、簽名不符。 |
| `-Wno-unused-parameter` | raylib 回呼與模組 shim 合法地忽略參數。 |
| `-Wno-missing-field-initializers` | `= { 0 }` 初始化大量欄位的結構是刻意的。 |

**零警告通過是建置健康訊號，不是噪音。** 一旦允許「已知的幾個警告」，新警告就會被淹沒。

### ⚠️ `-Wformat-truncation`

`snprintf("%s/settings.cfg", dir)` 當 `dir` 與目的地一样大時會觸發。解法：把來源緩衝縮短（`char dir[200]` → `char path[256]`），或把 `%d` 參數夾到可證明的範圍（`BP_CLAMP(...,1,999)`）。
