# BREACH POINT — 開發指南

> 給要動手改這份程式碼的人。架構理由在 [`ARCHITECTURE.md`](ARCHITECTURE.md)，數值在 [`GAMEPLAY_DATA.md`](GAMEPLAY_DATA.md)。

---

## 目錄

1. [環境建置](#1-環境建置)
2. [日常開發迴圈](#2-日常開發迴圈)
3. [無頭執行與 CI](#3-無頭執行與-ci)
4. [除錯手冊](#4-除錯手冊)
5. [程式碼慣例](#5-程式碼慣例)
6. [How-to：新增一把武器](#6-how-to新增一把武器)
7. [How-to：新增一種敵人](#7-how-to新增一種敵人)
8. [How-to：新增一張地圖](#8-how-to新增一張地圖)
9. [How-to：新增一種材質](#9-how-to新增一種材質)
10. [How-to：新增一個輸入動作](#10-how-to新增一個輸入動作)
11. [How-to：新增一個選單畫面](#11-how-to新增一個選單畫面)
12. [How-to：新增一個音效](#12-how-to新增一個音效)
13. [Web / WebAssembly 建置](#13-web--webassembly-建置)
14. [已知陷阱總表](#14-已知陷阱總表)
15. [模組 API 索引](#15-模組-api-索引)

---

## 1. 環境建置

### 1.1 Linux（主要開發平台）

```bash
# 系統套件
sudo apt-get install -y build-essential git cmake \
    libasound2-dev libx11-dev libxrandr-dev libxi-dev \
    libgl1-mesa-dev libglu1-mesa-dev libxcursor-dev \
    libxinerama-dev libwayland-dev libxkbcommon-dev \
    xvfb gdb

# raylib 5.5（必須是 5.5，本專案對 API 有精確假設，見 §14）
git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git ~/tools/raylib

# 建置靜態庫（約 1–2 分鐘）
cd breachpoint && make raylib
```

> ⚠️ **缺 X11 開發標頭會失敗兩次**，而且錯誤訊息不直觀：先是 `X11/Xcursor/Xcursor.h: No such file`，修好之後才是 `X11/extensions/XInput2.h`。一次把上面整組裝完。

> ⚠️ **`apt-get` 在沙箱環境需要 `sudo -n`**（non-interactive）。沒有 sudo 時 `apt-get install` 會靜默失敗。

### 1.2 驗證建置

```bash
make                                    # 應該零錯誤零警告
./build/breachpoint --selftest          # 應該印出一串 PASS，結束碼 0
echo $?                                 # 0
```

**這是「環境好了」的定義。** 不是「編譯過了」，是「自我測試全過」。

### 1.3 macOS / Windows

Makefile 目前把 Linux 的連結庫寫死在 `NATIVE_LIBS_LINUX`。移植時：

```makefile
# macOS
NATIVE_LIBS := -framework Cocoa -framework IOKit -framework CoreFoundation -framework OpenGL

# Windows (MinGW-w64)
NATIVE_LIBS := -lgdi32 -lwinmm -lopengl32
```

並把 `NATIVE_CFLAGS` 的 `-DPLATFORM_DESKTOP` 保留。程式碼本身**沒有** Linux 特定呼叫（存檔路徑用 `getenv("HOME")`，Windows 上需要改成 `SHGetKnownFolderPath` 或用 raylib 的 `GetApplicationDirectory`）。

---

## 2. 日常開發迴圈

### 2.1 Makefile 目標

| 指令 | 產出 | 說明 |
|---|---|---|
| `make` | `build/breachpoint` | `-O2 -g`，含偵錯符號，**日常用這個** |
| `make release` | `build/breachpoint-release` | `-O3 -DNDEBUG` |
| `make run` | — | 建置並執行 |
| `make clean` | — | `rm -rf build/` |
| `make raylib` | `~/tools/raylib/src/libraylib.a` | 原生 raylib |
| `make web` | `build/web/index.html` | WASM（⚠️ 見 §13，目前缺 `web/shell.html`） |
| `make serve` | — | web 建置 + `python3 -m http.server 8080`（⚠️ 同上） |
| `make test` | `build/test_systems` | 無 GL 系統測試（⚠️ 缺 `tests/test_systems.c`） |
| `make cfg` | `engine/bp_config_data.h` | 設定嵌入（⚠️ 缺 `tools/gen_cfg.py`） |
| `make help` | — | 列出所有目標 |

> ⚠️ **`release` 與 `all` 共用同一個物件檔目錄**，而且 `release:` 用 target-specific 變數改 `NATIVE_CFLAGS`。make **不會**因為旗標改變而重建物件檔，所以在 `make` 之後跑 `make release` 會連結 `-O2 -g` 的物件。要真的得到 release 版，先 `make clean`。
>
> 正確修法：把 `WEB_OBJ`／`NATIVE_OBJ` 的模式複製一份給 release（`build/obj-release/`）。

### 2.2 增量建置

`make -j4` 只會重編改動的檔案。22 個 TU 全量重建約 4 秒，增量通常 <1 秒。

**快速語法檢查**（不連結，最快）：

```bash
gcc -std=c11 -fsyntax-only -Iengine -Igame -I. -I~/tools/raylib/src \
    -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33 game/bp_player.c
```

> ⚠️ **這個指令漏了 `-I$(RAYLIB_DIR)/src` 很久**，所以 ad-hoc 語法檢查全過、`make` 卻失敗於 `fatal error: raylib.h: No such file or directory`。要做語法檢查就直接 `make`，它只多花幾秒。

### 2.3 改資料不用重編

武器與敵人的所有數值都在 `assets/cfg/*.cfg`。**改完存檔直接重跑遊戲**，不用 `make`。

```bash
vim assets/cfg/weapons.cfg     # 把 shotgun 的 pellets 改成 12
./build/breachpoint            # 立即生效
```

啟動日誌會確認：

```
[INFO ] weapons: 5/5 definitions patched from config
[INFO ] enemies: 4/4 archetypes patched from config
```

> ⚠️ **設定檔放錯檔案是完全靜默的。** 敵人區塊曾經被寫進 `weapons.cfg`，而載入器讀的是 `enemies.cfg`。失敗路徑只印警告，所以這一類 bug **不產生任何可見錯誤**——只是數值沒有生效。看到 `4/4 archetypes patched` 而不是 `0/4` 才算對。

---

## 3. 無頭執行與 CI

### 3.1 Xvfb

沒有實體顯示器時：

```bash
Xvfb :99 -screen 0 1280x720x24 &
export DISPLAY=:99
export LIBGL_ALWAYS_SOFTWARE=1     # 強制 llvmpipe 軟體光柵化
./build/breachpoint --selftest
```

實測環境：llvmpipe、Mesa 25.0.7、GL 4.5 core、1280×720。

**效能參考**（llvmpipe，1280×720，HIGH 品質）：

| 階段 | 耗時 |
|---|---|
| 完整開機（16 階段） | 0.22–0.27 s |
| 素材生成 | 330–424 ms（其中建築貼圖 ~407 ms） |
| 世界烘焙（FOUNDRY） | <1 ms（12 批次 / 516 三角形） |
| 自我測試每帧 | 6–11 ms |
| 自我測試 fps | 60（鎖定 `SELFTEST_DT`）／~168（未鎖） |

這是**軟體光柵化**的數字。真實 GPU 上素材上傳會快一個數量級。

### 3.2 CI 腳本範例

```yaml
- name: Build raylib
  run: |
    git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git ~/tools/raylib
    make raylib

- name: Build
  run: make -j4                      # 任何警告都應該被當成失敗

- name: Self-test
  run: |
    Xvfb :99 -screen 0 1280x720x24 &
    sleep 2
    DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 ./build/breachpoint --selftest
  # 結束碼 0 = PASS，1 = 有斷言失敗，2 = 開機失敗
```

### 3.3 音訊裝置不存在是正常的

無頭環境會印：

```
WARNING: AUDIO: Failed to initialize playback device
[WARN ] audio: device failed to open -- running silent
```

**這不是錯誤**，自我測試也不會因此失敗。音訊降級為 no-op 是設計行為（見 ARCHITECTURE §11）。

---

## 4. 除錯手冊

### 4.1 日誌系統

```c
BP_LOGD("debug: %d", x);    // DEBUG — 預設會印，但可過濾
BP_LOGI("info: %s", s);     // INFO
BP_LOGW("warn: %d", n);     // WARN
BP_LOGE("error: %s", why);  // ERROR — 會被 bp_log_error_count() 計數
```

輸出同時進 stdout 與一個 **256 × 192 的環形緩衝**（`bp_log_lines(&count)` 可讀出）。環形緩衝存在的理由是：web 建置沒有 console，除錯覆疊要把日誌畫在螢幕上。

> ⚠️ **`-Wdouble-promotion` 會對每一個傳給日誌巨集的 float 報錯。** 必須在呼叫點寫明確的 `(double)`：
> ```c
> BP_LOGI("took %.1f ms", (double)ms);     // ✅
> BP_LOGI("took %.1f ms", ms);             // ❌ 警告
> ```

> ⚠️ **`-Wformat=2` 拒絕把純字串當 format 傳。**
> ```c
> BP_LOGI("%s", label);   // ✅
> BP_LOGI(label);         // ❌
> ```

### 4.2 遊戲內除錯

| 按鍵 | 功能 |
|---|---|
| `F3` 或 `` ` `` | 除錯覆疊（`g_engine.perf.show_overlay`） |
| `H` | 開關 HUD |
| `G` | 切換 HUD 精簡模式 |

除錯繪圖原語（`engine/bp_gfx.h`）：

```c
void bp_gfx_draw_aabb(Vector3 min, Vector3 max, Color c);
void bp_gfx_draw_ray (Vector3 from, Vector3 to, Color c);
void bp_gfx_draw_grid(Vector3 center, float size, float step, Color c);
void bp_world_draw_debug(void);      // 所有 solid、trigger、spawn point
void bp_enemy_draw_debug(void);      // 感知錐、目標、狀態
void bp_player_draw_debug(void);     // 膠囊、速度向量
```

> ⚠️ **HUD 不回報 draw call 數與三角形數**，因為 raylib 5.5 的 `rlRenderBatch.drawCounter` 是內部的，rlgl 沒有公開存取器。`g_engine.perf.draw_calls` 與 `.tris` 欄位存在但**永遠是 0**。不要「修好」它們，除非你打算自己包一個計數層。這一點在 `bp_game.c` 與 `bp_hud.c` 都有註解。

### 4.3 gdb

```bash
gdb -q -batch -ex run -ex bt --args ./build/breachpoint --selftest --frames 60
```

這個指令在開發中抓到了唯一一個 segfault（見 §14 的 `BP_MAX` 陷阱）。`-batch` + `-ex bt` 讓它在崩潰時直接印回溯然後結束，適合腳本化。

### 4.4 「開機卡住，沒有任何錯誤」的診斷流程

這個症狀在本專案發生過**三次**，每次原因不同。診斷順序：

1. **看最後一行日誌。** 分階段開機就是為了這個。最後一行指出卡在哪個階段。

2. **如果最後一行是 `input: N actions bound`（= `BOOT_INPUT` 結束）** → 卡在 `BOOT_ASSETS`。
   先確認 `bp_assets.c` 的 `stage_begin/stage_end` 計時有沒有印出來。若完全沒有，問題在**上一帧的 `EndDrawing()` 沒有回傳** → 見步驟 3。

3. **檢查事件等待。** 這是本專案最常見的「卡住」原因：
   ```bash
   grep -rn "EnableEventWaiting\|DisableEventWaiting" --include=*.c .
   ```
   **正確答案：只有 `engine/bp_log.c` 的 `bp_apply_event_wait()` 一個地方。** 如果任何其他檔案呼叫它，那就是 bug。見 ARCHITECTURE §3 的完整說明。

4. **確認 `boot_tick()` 的冪等守衛還在**：
   ```bash
   grep -n "if (g_game.boot_done) return;" game/bp_game.c
   ```

5. **如果卡在 `BOOT_SAVE` 之後** → `bp_save_init()` 在 apply 設定時可能又啟用了事件等待。確認它走的是 `bp_apply_event_wait()` 而不是自己呼叫 `EnableEventWaiting`。

### 4.5 效能

`g_engine.perf`：

```c
float frame_ms;    // 平滑後的每帧牆鐘時間
float sim_ms;      // 固定步長 update 花的時間
float draw_ms;     // 發 draw call 花的時間
int   fps;
int   sim_steps;   // 上一帧跑了幾個模擬 tick
```

**`sim_steps` 是最重要的效能指標。** 正常值：

| 顯示幀率 | 期望 `sim_steps` |
|---|---|
| 60 Hz | 2 |
| 120 Hz | 1 |
| 144 Hz | 0 或 1 交替 |
| 30 Hz | 4 |

**如果 `sim_steps` 持續等於 8（上限）**，表示機器跟不上，螺旋死亡守衛正在丟棄模擬時間。遊戲會「感覺變慢」而不是「卡頓」——這是刻意的取捨。

---

## 5. 程式碼慣例

### 5.1 命名

| 東西 | 慣例 | 範例 |
|---|---|---|
| 型別 | `bp_` + 蛇形 | `bp_solid`、`bp_weapon_def` |
| 函式 | `bp_<模組>_<動詞>` | `bp_world_load`、`bp_enemy_spawn` |
| 全域單例 | `g_<名字>` | `g_world`、`g_player`、`g_engine` |
| 檔案靜態 | `s_<名字>` | `s_boot_stage`、`s_virtual[]` |
| 常數巨集 | 全大寫 | `MAX_ENEMIES`、`SIM_DT` |
| 列舉值 | 型別前綴全大寫 | `TEX_WALL_BRICK`、`ACT_FIRE`、`SCENE_PLAYING` |

`bp_<模組>_` 前綴不是裝飾：22 個 TU 全部連結進同一個二進位，沒有命名空間。前綴是唯一防止 `load()` 撞 `load()` 的機制。

### 5.2 檔案佈局

每個 `.c` 都是這個形狀：

```c
/****** 檔案標頭註解：這個檔案做什麼、為什麼獨立成一個檔案 ******/
#include "自己的.h"          // 永遠第一個，驗證 header 自足
#include "其他模組.h"
#include <系統.h>

/* === 常數 === */
/* === 檔案靜態狀態 === */
/* === 前置宣告 === */        // ⚠️ 見下方陷阱
/* === 內部 helper === */
/* === 公開 API === */
```

`.h` 的形狀：

```c
#ifndef BP_XXX_H
#define BP_XXX_H
#include "需要的引擎 header"   // ⚠️ 顯式，不依賴呼叫者
/* 列舉、結構、extern 全域、函式宣告 */
#endif
```

> ⚠️ **前置宣告／全域定義放在第一次使用之後**會產生 implicit-declaration 錯誤。這發生過**兩次**：`bp_game.c` 的 helper 函式，以及 `bp_assets.c` 的 `g_assets_stage`。
>
> 修 `bp_assets.c` 那次時，因為那個全域只被兩個 static 函式用，正確解法是**改成 `static char s_stage_name[48]` 並放在函式之前**，而不是加一個公開的 extern。

### 5.3 註解：寫「為什麼」，不寫「是什麼」

這個專案的註解密度很高，但**每一條都在解釋一個不看就會犯錯的決定**。範例：

```c
/* 壞註解：說明程式碼在做什麼 */
// increment i
i++;

/* 好註解：說明為什麼不能寫成看起來更簡單的形式 */
/* Deliberately not BP_MAX(...): that macro evaluates its arguments more than
 * once and ++i would run twice. See the warning at the macro definition. */
i++;
```

**規則**：如果一段程式碼看起來可以用更明顯的方式寫，但沒有——註解必須說明為什麼。否則下一個維護者會「修好」它。

### 5.4 錯誤處理

- **開機期的失敗分兩類**：
  - **可降級**（音訊裝置、選擇性設定檔、存檔損壞）→ `BP_LOGW` + 用預設值繼續
  - **不可降級**（GL context、素材生成失敗）→ `BP_LOGE` + 顯示錯誤畫面
- **帧迴圈內不准失敗。** 池子滿了 → 回 NULL / 跳過，並印警告。絕不 abort。
- **`bp_log_error_count()`** 讓測試可以斷言「這次執行沒有 ERROR」。警告**不**算失敗——缺一個選擇性設定檔是警告，不該讓建置失敗。

### 5.5 ⚠️ `BP_MIN` / `BP_MAX` / `BP_CLAMP` 會多次求值

```c
#define BP_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define BP_MAX(a, b)        ((a) > (b) ? (a) : (b))
#define BP_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
```

**這些是純三元運算式，參數會被求值最多三次。**

```c
selftestFrameCount = BP_MAX(1, atoi(argv[++i]));   // ❌ SEGFAULT
```

`++i` 跑了**兩次**，`argv[i]` 走到 `argv[argc]`（NULL），`atoi(NULL)` 崩潰。**而且沒有任何診斷訊息**，因為崩潰點在 libc 裡。

```c
i++;                                                // ✅
int v = atoi(argv[i]);
selftestFrameCount = v < 1 ? 1800 : v;
```

**規則：絕不把有副作用的運算式傳給這三個巨集。** 先賦值給區域變數。

這條規則靠 code review 執行，不靠編譯器——C11 沒有便攜的方法把它們寫成 statement expression 而不在 `-Wshadow` 下產生警告（gcc 的 `({...})` 在 clang/emcc 上行為不一致）。

檢查现有程式碼有沒有踩到：

```bash
grep -rn "BP_\(MIN\|MAX\|CLAMP\)([^)]*\(++\|--\|+=\|-=\|\*=\|/=\)" --include=*.c --include=*.h --include=*.inc .
```

### 5.6 浮點

- 全專案用 `float`，**不用 `double`**。理由：`-Wdouble-promotion` 強制這個決定是顯式的，而且 GPU 傳給著色器的都是 float。
- 需要 `double` 的地方：`snprintf` 的 `%f`（variadic 會自動提升，但要寫 `(double)` 讓 `-Wdouble-promotion` 滿意）、`GetTime()`（raylib 回傳 double）。
- **用 `bp_lerp` 而不是 raylib 的 `Lerp`**——後者是 double-based，會觸發 `-Wdouble-promotion`。
- 角度插值用 `bp_lerp_angle`，處理 ±180° 的繞回。

### 5.7 結構初始化

```c
bp_engine g_engine = { 0 };      // ✅ 明確歸零
```

`-Wno-missing-field-initializers` 讓這個写法不產生警告。**但歸零不等於初始化**——`time_scale = 0` 會凍結模擬，因為每個消費者都乘它。所以 `bp_log_init()` 明確設：

```c
g_engine.time_scale = 1.0f;      // 零值結構會凍結模擬
g_engine.render_scale = 1;       // 零值會產生 0×0 的渲染目標
bp_rng_seed(&g_engine.rng, 0x5EED1234u);   // 零值 seed 會被換成 0x9E3779B9
```

**規則：任何「1.0 才是正確預設值」的欄位，都必须在 init 時明確設定。**

---

## 6. How-to：新增一把武器

武器是**資料驅動**的。加一把槍大部分情況下不用改 C 程式碼。

### 6.1 純數值調整（不用重編）

編輯 `assets/cfg/weapons.cfg`：

```
weapon smg
    damage = 12          # 原本 10
    fire_rate = 14.0     # 原本 12.5
end
```

重跑遊戲。啟動日誌確認 `weapons: 5/5 definitions patched from config`。

### 6.2 新增第 6 把武器

**步驟 1** — `engine/bp_common.h`：確認 `MAX_WEAPON_DEFS`（目前 12）夠用。

**步驟 2** — `game/bp_weapons.h`：加列舉值

```c
typedef enum {
    WPN_ID_PISTOL = 0, WPN_ID_SHOTGUN, WPN_ID_SMG, WPN_ID_RAIL, WPN_ID_ROCKET,
    WPN_ID_PLASMA,                     // ← 新增
    WPN_ID_COUNT
} bp_weapon_id;
```

**步驟 3** — `game/bp_weapons.c`：在 `s_defaults[]` 加一筆**內建預設值**。

這是必需的，不是選擇性的：設定檔的每個鍵都是可選的，缺鍵時降級到 `s_defaults[]`。沒有預設值 → 欄位是 0 → 傷害 0 的槍。

**步驟 4** — `assets/cfg/weapons.cfg`：加一個 section

```
weapon plasma
    display_name = ARC-4 PLASMA
    slot = 5
    category = projectile
    damage = 34
    ...
end
```

> ⚠️ **section id 必須符合載入器的 dispatch。** 載入器的形狀是：
> ```c
> else if (!strcmp(id, "pistol"))  def = &s_defs[WPN_ID_PISTOL];
> else if (!strcmp(id, "plasma"))  def = &s_defs[WPN_ID_PLASMA];   // ← 要加這行
> ```
> 忘了加 → 設定檔被讀了但**沒有任何效果**，而且不印錯誤。

**步驟 5** — `engine/bp_assets.h`：加視模型部件

```c
MDL_GUN_PLASMA_BODY, MDL_GUN_PLASMA_EMITTER,
```

**步驟 6** — `engine/bp_assets.c`：在模型生成處建它們。

**步驟 7** — `game/bp_weapons.c` 的 `draw_viewmodel()`：加一個 `case WPN_ID_PLASMA:` 畫部件。

**步驟 8** — `engine/bp_audio.h`：加 `SFX_PLASMA_FIRE`，在 `bp_audio.c` 合成它（見 §12）。

**步驟 9** — `assets/cfg/weapons.cfg` 的 `sfx_fire = <SFX id>`。

**步驟 10** — `engine/bp_input.c`：若要給它一個專屬按鍵，需要 `ACT_WEAPON_6`（見 §10）。**或者**只用 `ACT_WEAPON_NEXT` 循環——大部分情況這樣就夠了。

**步驟 11** — 跑 `--selftest`，確認 `weapons fired: N shots` 有增加。

### 6.3 武器鍵的完整清單

```
display_name  slot  category(hitscan|projectile)  damage  pellets
spread_hip  spread_ads  spread_move  fire_rate  auto  mag_size  reserve_max
reload_time  swap_time  recoil_pitch  recoil_yaw  recoil_kick  shake
sfx_fire  sfx_reload  tracer_width  tracer_life  range  knockback  penetration
ammo_per_pickup  headshot_mult  color(#RRGGBB)
# projectile 限定：
proj_speed  proj_radius  proj_gravity
blast_radius  blast_damage  blast_self  blast_knockback
```

**語意註解**：

- `spread_*` 是**錐半角（度）**。實際散布 = `spread_hip × (1 + moveFactor×spread_move + airFactor×spread_air) × (1 − ads×0.65)`
- `pellets > 1` 時，**每顆彈丸各取樣一次錐**，但每次扣扳機只畫**一條**曳光彈
- `penetration` = 額外穿過幾個身體（rail = 4 → 总共可打 5 個）
- `headshot_mult` 在射線命中頭部盒時套用
- `blast_self` = 射手自己被自己的火箭炸到的**傷害比例**；`blast_knockback` 則是**完整套用**
  - **這個不對稱正是火箭跳能用的原因。** 把它們設成一樣，火箭跳就會自殺。

---

## 7. How-to：新增一種敵人

### 7.1 新增第 5 種原型

**步驟 1** — `game/bp_enemies.h`：

```c
typedef enum {
    ENEMY_GRUNT = 0, ENEMY_HOUND, ENEMY_BRUTE, ENEMY_TURRET,
    ENEMY_FLYER,                       // ← 新增
    ENEMY_TYPE_COUNT
} bp_enemy_type;
```

確認 `MAX_ENEMY_DEFS`（16）夠用。

**步驟 2** — `game/bp_enemies.c` 的 `s_archetypes[]` 加內建預設值。

**步驟 3** — `bp_enemy_type_from_cfg(const char *id)` 加 `"flyer"` 的映射。

**步驟 4** — `assets/cfg/enemies.cfg`：

```
enemy flyer
    display_name = WASP
    health = 45
    speed_run = 9.0
    ...
end
```

**步驟 5** — 若要新的身體部件：`bp_assets.h` 加 `MDL_FLYER_*`，`bp_assets.c` 建它們，`bp_enemy_draw()` 加繪製分支。

**步驟 6** — `game/bp_waves.c` 的生成表：決定它從第幾波開始出現、權重多少。

**步驟 7** — 跑 `--selftest`，確認 `enemies spawned: N` 有增加。

### 7.2 敵人鍵的完整清單

```
display_name  health  armor  speed_walk  speed_run
radius  height  scale  sight_range  hear_range
attack_range  attack_damage  attack_cooldown  attack_sfx
pain_chance  score
drop_health  drop_ammo  drop_armor        # 機率 0..1
body(torso|hound|brute|turret)
color_body  color_trim  eye_color         # #RRGGBB
strafe  aggression
projectile(bool)
# projectile 限定：
proj_speed  proj_radius  proj_damage
# 特殊：
static(bool)          # 砲塔：不移動
knockback_scale       # brute = 0.22（78% 抗性）
```

### 7.3 AI 參數的語意

| 鍵 | 意義 | 調參建議 |
|---|---|---|
| `sight_range` | 視覺距離（公尺） | 還要通過 140° 視錐 **和** 視線檢查 |
| `hear_range` | 聽覺距離 | 0 = 聾（砲塔是 0） |
| `pain_chance` | 受擊硬直機率 | **只有** `damage > health × 0.06` 時才擲。brute = 0.12 所以幾乎不硬直 |
| `aggression` | 推進 vs 保持距離的傾向 | 1.0 = 全力推進 |
| `strafe` | 側移量 | hound = 0.75（繞圈），brute = 0.15（直衝） |
| `knockback_scale` | 擊退倍率 | brute = 0.22 |
| `armor` | 額外護甲 | **吸收 55% 傷害**，先扣護甲 |

**「最後已知位置」記憶是 3.5 秒。** 玩家在視線外躲 3.5 秒後，敵人會放棄並回到巡邏。這是刻意的：讓「甩掉敵人」是一個真實的戰術，而不是只能殺光。

**`bp_enemy_alert_nearby(pos, radius)`** 在敵人首次發現玩家時呼叫，會喚醒半徑內的所有同伴。**這是「一群敵人一起反應」的機制**，不要繞過它。

---

## 8. How-to：新增一張地圖

> ⚠️ **這是本專案最容易做錯的事。** 手寫的幾何資料沒有任何驗證，錯誤完全靜默。讀完這一節再動手。

### 8.1 結構

`game/bp_levels.inc`（430 行）被 `bp_world.c` include。每張地圖是一個函式：

```c
static void build_level_foundry(void)
{
    /* 1. 環境 */
    SET_SKY(color_top, color_bottom, yaw);
    SET_FOG(color, density);
    SET_AMBIENT(color);

    /* 2. 地板與外牆 */
    ADD_SOLID(-34, -1, -34, 68, 1, 68, MAT_CONCRETE);   // 地板
    ADD_SOLID(-34,  0, -35, 68, 4,  1, MAT_BRICK);      // 北牆
    ...

    /* 3. 幾何 */
    ADD_SOLID(x, y, z, w, h, d, MAT_xxx);

    /* 4. 觸發器 */
    ADD_TRIGGER(TRIG_PLAYER_START, x, y, z, w, h, d);
    ADD_TRIGGER(TRIG_EXIT,         x, y, z, w, h, d);
    ADD_TRIGGER(TRIG_SECRET,       x, y, z, w, h, d);
    ADD_TRIGGER(TRIG_HEALTH,       x, y, z, w, h, d);

    /* 5. 移動件 */
    ADD_MOVER(...);

    /* 6. 敵人生成點 */
    ADD_SPAWN(x, y, z);
}
```

### 8.2 ⚠️ `ADD_SOLID` 吃的是**最小角**

```c
ADD_SOLID(x, y, z, w, h, d, mat);
//        ↑ min corner      ↑ size
// box.min = (x, y, z)
// box.max = (x+w, y+h, z+d)
```

**不是中心點。** 把一個 2×2×2 的箱子放在原點是 `ADD_SOLID(-1, 0, -1, 2, 2, 2, MAT_CRATE)`。

### 8.3 ⚠️ 高度可達性預算

| 高度差 | 玩家能不能上去 |
|---|---|
| ≤ 0.55 m | ✅ 直接走上去（step-up） |
| 0.55 – 1.10 m | ⚠️ **需要跳** |
| 1.10 – 1.50 m | ⚠️ **需要衝刺跳** |
| > 1.50 m | ❌ **必須有樓梯／斜坡／升降機** |

玩家跳躍高度是 1.32 m（`JUMP_VELOCITY 8.6`，`GRAVITY −28`）。

**每條路徑都必須檢查。** 一個 0.72 m 的落差會讓某個平台**永久不可達**，而且：

- 不會有任何錯誤訊息
- 不會讓建置失敗
- 自我測試**也不會抓到**（玩家永遠走不到那裡，所以不會掉出世界）
- 玩家只會覺得「這張地圖設計得不好」

> 這個 bug 實際發生過。教訓是：**關卡幾何需要一個可達性驗證器**，見 ROADMAP。

### 8.4 驗證清單

新增或修改地圖後，逐項檢查：

- [ ] 啟動日誌 `world: 'NAME' -- N solids, M triggers, K movers, S enemy spawns` 的數字對
- [ ] `world: baked N static batches` — N 應該 ≈ 用到的材質數
- [ ] **只有 1 個 `TRIG_PLAYER_START`** — 0 個會讓玩家生成在原點，2 個會讓 `bp_world_player_start()` 回傳第一個
- [ ] **只有 1 個 `TRIG_EXIT`** — 且它**不會被消耗**
- [ ] 從出生點走到 EXIT 的**每一段路**都符合 §8.3 的預算
- [ ] 生成點數量 ≥ 波次需要（`SPAWN_ATTEMPTS = 9` 次嘗試，不夠會降級到隨機位置）
- [ ] `--selftest` 的 `player stayed inside the level` 與 `inside the level bounds` 都 PASS
- [ ] **手動開除錯覆疊（`F3`）跑一次**，看 `bp_world_draw_debug()` 畫出的所有盒

### 8.5 現有三張地圖的設計參考

```
FOUNDRY   68×68 m，x,z ∈ −34..34
          地板 slab y ∈ −1..0，牆高 1 m
          對稱，中央可行走金字塔 0 → 0.55 → 1.10 → 1.65
          6 個油桶、13 個生成點、1 個 blast-door mover
          天空：琥珀

SLAG PIT  三個高度帶：坑底 0.0 / 平台 2.2 / 棧道 6.9
          坑底有危險格柵（hazard，9 dps）
          兩座 10 階 × 0.69 m 的樓梯塔（**需要跳**，刻意）
          9 個油桶、14 個生成點、1 個 freight-lift mover
          天空：洋紅

CONDUIT   緊密環形走廊，無 >22 m 的視線
          0.6 m 的板條箱墊高到 7.1 m 的核心陽台
          15 個生成點、1 個 shutter mover
          天空：綠
```

**每張地圖都有的東西**：1 PLAYER_START、1 EXIT、1 SECRET、定時重生的 HEALTH/ARMOR/AMMO/WEAPON 觸發器。

### 8.6 註冊新地圖

`game/bp_world.c`：

```c
const bp_level_def g_levels[] = {
    { "foundry", "FOUNDRY",  "Symmetrical arena. Learn the movement.", 6, 150.0f, 0 },
    { "slagpit", "SLAG PIT", "Three height bands. Own the catwalk.",   8, 210.0f, 1 },
    { "conduit", "CONDUIT",  "Tight corridors. Shotguns win.",         9, 240.0f, 2 },
    { "refinery","REFINERY", "Your subtitle here.",                   10, 270.0f, 3 },  // ← 新增
};
```

欄位：`id`（內部）、`display_name`、`subtitle`、`waves`、`par_time`（秒）、`difficulty`（建議難度索引）。

`g_level_count` 用 `BP_ARRAY_COUNT` 自動算，**不用手動改**。確認 `MAX_LEVELS`（8）夠用。

然後在 `bp_world_load()` 的 dispatch 加 `case 3: build_level_refinery(); break;`。

---

## 9. How-to：新增一種材質

**步驟 1** — `game/bp_world.h` 的材質列舉加一項（`MAT_COUNT` 之前）。

**步驟 2** — `game/bp_world.c` 的 `s_materials[]` 加一列：

```c
{ "名称", TEX_XXX, SFX_HIT_XXX, SFX_FOOTSTEP,
  uv_scale, friction, gloss,
  penetrable, emissive, solid_for_movement,
  hazard_dps, tint },
```

**每個欄位都必須填**，因為 `-Wno-missing-field-initializers` 不會警告漏填，漏填的欄位是 0：

- `friction = 0` → 完全無摩擦，玩家永遠停不下來
- `uv_scale = 0` → **除以零**
- `solid_for_movement = false` → 玩家直接穿過去

**步驟 3** — 若要新貼圖：`bp_assets.h` 加 `TEX_*`，`bp_assets.c` 生成它。確認 `TEX_COUNT` 的計數正確（自我測試會檢查 `30/30 textures`）。

**步驟 4** — 新材質會**自動**得到一個靜態批次（烘焙是依材質分組的），所以 draw call 數會 +1。

---

## 10. How-to：新增一個輸入動作

**步驟 1** — `engine/bp_input.h` 的 `bp_action` 加一項（`ACT_COUNT` 之前）。

> ⚠️ **不要插在中間。** `g_save.settings.binds[ACT_COUNT][2]` 用 action index 當陣列索引存進存檔。插入會讓舊存檔的所有綁定**位移**。永遠加在 `ACT_COUNT` 之前。

**步驟 2** — `engine/bp_input.c` 的 `set_bind()` 區塊加預設綁定：

```c
set_bind(ACT_PRONE, "Prone", KEY_Z, 0, 0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, -1, -1, 0);
//                 ↑ 這個字串會出現在 CONTROLS 畫面與除錯日誌
```

簽名：`set_bind(action, name, key0, key1, padAxis_unused, padBtn0, padBtn1, padAxis, padAxisSign)`

**步驟 3** — 存檔相容性：舊存檔沒有 `bind_<新action>_<slot>` 鍵。載入器的規則是「**stored code 全為 0 的動作保留預設綁定**」，所以新動作會自動拿到步驟 2 的預設值。**不需要版本遷移。**

**步驟 4** — 啟動日誌的 `input: N actions bound` 的 N 會 +1。自我測試用它驗證綁定表完整。

> ⚠️ **滑鼠按鍵必須用 `BP_BIND_FROM_MOUSE()` 包裝**，見 ARCHITECTURE §10。直接寫 `MOUSE_BUTTON_LEFT` 會讓它等於 0 = 「未綁定」。

---

## 11. How-to：新增一個選單畫面

**步驟 1** — `game/bp_menu.h` 的 `bp_menu_screen` 加一項。

**步驟 2** — `game/bp_menu.c`：

```c
static void build_myscreen(void)
{
    list_clear();
    g_menu.list.y = 120.0f;        // 虛擬畫布座標（640×360）
    g_menu.list.column_rows = 0;   // 0 = 單欄；>15 列時設成 ~13 讓它流進第二欄

    bp_menu_row *r;
    r = list_push(MROW_LABEL, "MY SCREEN", 0);
    r = list_push(MROW_TOGGLE, "Some Option", 0);
    if (r) { r->bval = &g_save.settings.some_flag; }

    r = list_push(MROW_SLIDER, "Some Value", 0);
    if (r) { r->fval = &s_mirror; r->vmin = 0.0f; r->vmax = 1.0f; r->vstep = 0.05f; }

    r = list_push(MROW_ACTION, "Do Thing", A_MY_ACTION);
    if (r) snprintf(r->hint, sizeof(r->hint), "Explains what it does");

    list_push(MROW_SPACER, "", 0);
    r = list_push(MROW_ACTION, "BACK", A_BACK);
}
```

> ⚠️ **`list_push` 可能回傳 NULL**（列數超過 `MENU_MAX_ROWS`）。每一次存取 `r->` 之前都要檢查 `if (r)`。

**步驟 3** — `build_screen()` 的 switch 加 `case MENU_SCREEN_MYSCREEN: build_myscreen(); break;`

**步驟 4** — 動作 id：在 `bp_menu.c` 的 `enum { A_PLAY = 1, ... }` 加 `A_MY_ACTION`，**並在 `bp_game.c` 的 `enum { GA_PLAY = 1, ... }` 加同名同值的 `GA_MY_ACTION`**。

> ⚠️ **這兩個列舉靠約定同步，編譯器不會幫你檢查。** 值不一致 → 選單回傳的 id 被 dispatch 到錯誤的分支，症狀是「按 A 結果做了 B」。這是已知技術債，見 ROADMAP。

**步驟 5** — `bp_game.c` 的 `bp_game_apply_menu_action()` 加 `case GA_MY_ACTION:`

**步驟 6** — 在某個現有畫面加一個進入它的 `MROW_ACTION`。

### 選單的互動規則（已由框架處理，不用重寫）

- 上下鍵移動選擇，`bp_pressed()` 邊緣觸發
- 滑動器／循環／開關列：左右鍵調整
- 點擊**值的右半邊**：滑動器／循環／開關 +1；中半邊 −1
- 滑鼠滾輪也能調整
- 超過 `column_rows` 的列流進第二欄（**不做捲動**，理由見 ARCHITECTURE §13）
- `bp_menu_update()` **每渲染帧呼叫一次**，不是每模擬步

---

## 12. How-to：新增一個音效

**步驟 1** — `engine/bp_audio.h` 的 `bp_sfx_id` 加一項（`SFX_COUNT` 之前）。

> ⚠️ **設定檔用數值引用音效**：`sfx_fire = 6`、`attack_sfx = 19`。插在中間會讓所有现存的 `.cfg` 引用**指向錯誤的音效**，而且不會有任何錯誤。永遠加在 `SFX_COUNT` 之前，或者改 `.cfg` 的所有數值。

**步驟 2** — `engine/bp_audio.c` 的合成函式加分支。典型形狀：

```c
case SFX_MY_SOUND:
    /* 振盪器 + 包絡 + 濾波。全部在開機時算進 Wave，執行期只是播放。 */
    for (int i = 0; i < frames; i++)
    {
        float t = (float)i / (float)rate;
        float env = expf(-t * 12.0f);
        float osc = sinf(2.0f * 440.0f * (1.0f - t * 0.5f) * t * TAU);
        sample[i] = osc * env * 0.6f;
    }
    break;
```

**步驟 3** — `bp_audio_sfx_name()` 加名字（除錯覆疊與自我測試用）。

**步驟 4** — 播放：

```c
bp_audio_play(SFX_MY_SOUND, 0.8f);        // 0.8 = 音量縮放
```

> ⚠️ **`LoadSoundFromWave()` 會複製資料**，所以呼叫端必须 `UnloadWave()`。
> ⚠️ **`LoadSoundAlias()` 共用資料**，用於同一音效的多個聲部。
> ⚠️ **絕對不要** `LoadMusicStreamFromMemory("wav", NULL, 0)` — 那是 UB。音樂只能用 `LoadAudioStream` + 回呼。
> ⚠️ **型別雙關濾波器**：`(bp_onepole *)v` 會撞到 `v->phase`。用 `&v->lp`。
> ⚠️ **音樂回呼在音訊執行緒上**：只能碰檔案靜態的合成器狀態，不能碰 GL、不能碰 `g_*` 遊戲狀態、不能 allocate。
> ⚠️ **`music_voice.freq` 必須在觸發時鎖存**，不能在回呼中重讀 pattern。

---

## 13. Web / WebAssembly 建置

### 13.1 目前狀態：⚠️ **不完整**

Makefile 的 `web` 目標已經寫好，但缺三個檔案：

| 缺什麼 | 用途 | 狀態 |
|---|---|---|
| `web/shell.html` | emscripten 的外殼頁面（`--shell-file`） | **未撰寫** → `make web` 會失敗 |
| `tools/gen_cfg.py` | 把 `assets/cfg/*.cfg` 嵌進 `engine/bp_config_data.h` | **未撰寫** → `make cfg` 會失敗 |
| `tests/test_systems.c` | 無 GL 的系統層單元測試 | **未撰寫** → `make test` 會失敗 |
| `build/libraylib_web.a` | WASM 版 raylib | 由 `make raylib-web` 產生 |

### 13.2 完整流程（檔案補齊後）

```bash
# 1. emsdk
git clone https://github.com/emscripten-core/emsdk.git ~/tools/emsdk
cd ~/tools/emsdk && ./emsdk install latest && ./emsdk activate latest

# 2. WASM 版 raylib（約 3–5 分鐘）
cd breachpoint && make raylib-web
#    → build/libraylib_web.a

# 3. 遊戲
make web
#    → build/web/{index.html, breachpoint.js, breachpoint.wasm, breachpoint.data}

# 4. 服務
make serve        # python3 -m http.server 8080 --bind 0.0.0.0
```

> ⚠️ **emsdk 修復**：如果 `emcc` 找不到，試 `chmod -R +x ~/tools/emsdk`，然後 `rm -rf upstream node downloads .emscripten` 再重跑 `./emsdk install latest && ./emsdk activate latest`。完整安裝約 1.7 GB。

### 13.3 Web 連結旗標（已在 Makefile）

```
-s USE_GLFW=3                  raylib 在 web 上透過 HTML5 後端驅動 canvas
-s FULL_ES2=1                  OpenGL ES 2.0 — 最廣的瀏覽器覆蓋
-s INITIAL_MEMORY=64MB         池子是固定大小，但音訊／格式轉換需要餘量
-s ALLOW_MEMORY_GROWTH=1
-s MAX_WEBGL_VERSION=2 -s MIN_WEBGL_VERSION=1
-s ASYNCIFY=0 -s EXIT_RUNTIME=0
--shell-file web/shell.html
--preload-file assets@/assets  把 assets/ 烤進 .data 檔
-L$(BUILD) -l:libraylib_web.a
```

編譯旗標：`-DPLATFORM_WEB -DGRAPHICS_API_OPENGL_ES2`（**注意與原生的 `GRAPHICS_API_OPENGL_33` 不同**）。

### 13.4 Web 特定的程式碼差異

| 主題 | 桌面 | Web |
|---|---|---|
| 帧迴圈 | `while (!quit) run_frame();` | `emscripten_set_main_loop(web_main_loop, 0, 1)` |
| 存檔位置 | `~/.local/share/BreachPoint/` | `/bpdata/`（IDBFS → IndexedDB） |
| 存檔持久化 | 直接寫檔 | 每次寫入後 `FS.syncfs(false, cb)` |
| 滑鼠 | `DisableCursor()` + delta | 同上，**不用 pointer lock**（跨來源 iframe 會拒絕） |
| 鍵盤焦點 | 自動 | **需要點一下 canvas** — 啟動時明確印出提示 |
| 素材 | 程序化生成 | 同上（`--preload-file` 只帶 `assets/cfg/*.cfg`） |

> ⚠️ **web 上不能有 `while` 迴圈**當主迴圈——那會凍結整個分頁。`emscripten_set_main_loop` 的回呼簽名是 `void (void)`，所以每帧狀態必須住在模組裡而不是區域變數（`main.c` 有 `web_main_loop` 的註解說明這點）。

### 13.5 `web/shell.html` 需要什麼

emscripten 的 `--shell-file` 期望一個含這些佔位符的 HTML：

```html
{{{ SCRIPT }}}            <!-- 必要 -->
<canvas id="canvas" ...>  <!-- 必要 -->
```

這個專案的外殼還應該：

- 全螢幕、無邊框、黑色背景（letterbox 由 `bp_ui` 處理）
- 一個「點擊以開始」的覆蓋層，因為 **canvas 需要先被點擊才能取得鍵盤焦點**
- 載入進度條，讀 `Module.setStatus`
- 用 `Module.onRuntimeInitialized` 隱藏進度
- **不要**用外部 CDN 的字型或 CSS——`web/shell.html` 會被內嵌進產出的 HTML

---

## 14. 已知陷阱總表

按「多容易踩到」排序。**每一條都實際發生過。**

### 🔴 會讓建置失敗或崩潰

| # | 陷阱 | 症狀 | 修法 |
|---|---|---|---|
| 1 | `BP_MAX(1, atoi(argv[++i]))` | **segfault，無診斷** | 巨集多次求值。先賦值給區域變數（§5.5） |
| 2 | `g_engine` 只有 extern 宣告 | 連結期 `undefined reference to g_engine` | 定義在 `engine/bp_log.c`（最底層 TU） |
| 3 | 全域／前置宣告放在第一次使用之後 | `implicit declaration` / `undeclared` | 移到使用點之前。若只有 static 函式用它，就改成 `static`（§5.2） |
| 4 | 缺 `-I$(RAYLIB_DIR)/src` | `fatal error: raylib.h: No such file` | ad-hoc `gcc` 檢查會過但 `make` 失敗。**直接用 `make`** |
| 5 | 點陣字型一列寫 8 個 byte | 硬編譯錯誤 | 列是 `[7]` |
| 6 | 識別字中間有空格（`bool no collide_;`） | 編譯成兩個 token | 仔細看錯誤行的實際 token |
| 7 | 結構成員名字打錯／憑空發明 | `has no member named 'best_streak'` | **每個成員都要 grep 確認存在**。`g_engine.frame_ms` 不存在，它在 `g_engine.perf.frame_ms` |

### 🟠 會導致執行期掛死或行為錯誤

| # | 陷阱 | 症狀 | 修法 |
|---|---|---|---|
| 8 | 開機期間啟用事件等待 | **開機卡住，無錯誤訊息** | 只有 `run_frame()` 在 `bp_game_frame()` 之後、`EndDrawing()` 之前能翻旗標（ARCHITECTURE §3） |
| 9 | `boot_tick()` 尾段沒有冪等守衛 | 每帧重進 TITLE、重印日誌、重啟事件等待 | `if (g_game.boot_done) return;` |
| 10 | 直接寫 `g_input.act[].held` | 下一帧被 `begin_frame()` 抹掉 | 用 `bp_input_set_virtual()`（ARCHITECTURE §10） |
| 11 | 自我測試用 `GetFrameTime()` | 慢機器上模擬時間不足，一堆假 FAIL | 用固定 `SELFTEST_DT = 1/60`（ARCHITECTURE §15） |
| 12 | 選單 hover 沒有遊標守衛 | 按 ACCEPT 開了錯誤的畫面 | `mouseInside` 檢查（ARCHITECTURE §13） |
| 13 | `MOUSE_BUTTON_LEFT == 0` | 「未綁定」與「滑鼠左鍵」無法區分 | `BP_BIND_MOUSE_BASE 4096` 偏置 |
| 14 | 範圍測試混用滑鼠與鍵盤 code | 方向鍵被當成滑鼠按鈕 | 同上。**這曾經是已上線的 bug** |
| 15 | 在 `InitWindow` 前呼叫 `bp_save_init()` | segfault，崩潰點離原因很遠 | 拆成 `bp_save_boot()` / `bp_save_init()` |
| 16 | 關卡高度差 > 0.55 m 沒有樓梯 | **平台永久不可達，無錯誤訊息** | 遵守 §8.3 預算表；手動跑除錯覆疊 |
| 17 | 設定資料寫錯檔案 | 數值不生效，**只印警告** | 敵人在 `enemies.cfg`，武器在 `weapons.cfg`。看日誌的 `N/N patched` |
| 18 | 新增列舉值插在中間 | 舊存檔／舊 `.cfg` 的數值引用全部位移 | 永遠加在 `*_COUNT` 之前 |

### 🟡 raylib 5.5 API 陷阱

| # | 陷阱 | 修法 |
|---|---|---|
| 19 | `GetKeyName()` / `GetCharName()` / `GetFrameStats()` **不存在** | 自己維護 code→label 表；自己測 perf |
| 20 | `LoadSoundFromSample()` 不存在 | `LoadSoundFromWave(Wave)` + 呼叫端 `UnloadWave` |
| 21 | `LoadFontFromImage()` 行為不符需求 | `GenImageFontAtlas` → `LoadTextureFromImage` |
| 22 | `DrawModelEx` 無法表達關節鏈 | `rlSetMatrixModelview(m)` + `DrawMesh` 逐個畫 |
| 23 | `rlMatrixMode`/`rlMultMatrixf` 在 `BeginMode3D` 內被覆蓋 | 天穹改成反轉法線的頂點色球體 + `rlDisableBackfaceCulling` |
| 24 | 沒有內建霧 | `fog_color`/`fog_density` 目前是純資料 |
| 25 | `rlRenderBatch.drawCounter` 無公開存取器 | **不要回報 draw call 數** |
| 26 | `img_to_texture` 中 `UnloadImage` 的順序 | 必須**先**把 `im->px` 設為 NULL **再** `UnloadImage(img)` |
| 27 | 結構 typedef 與函式名撞名（`} bp_model;` vs `Model bp_model(...)`） | 存取器改名 `bp_model_get()` |
| 28 | `MouseButton` 與 `KeyboardKey` 編碼空間重疊 | 見 #13／#14 |
| 29 | Gamepad 列舉的 dpad 沒有 `_1` 後綴 | `GAMEPAD_BUTTON_LEFT_FACE_UP` 而非 `_1` |
| 30 | `Matrix` 的平移在 `m12/m13/m14` | 不是 `m41/m42/m43` |

完整「不存在 vs 實際可用」對照表在 ARCHITECTURE §8。

### 🔵 編譯器警告陷阱

| # | 陷阱 | 修法 |
|---|---|---|
| 31 | `-Wdouble-promotion`：float 傳給 variadic | 呼叫點寫 `(double)` |
| 32 | `-Wdouble-promotion`：raylib 的 `Lerp` 是 double | 用自己的 float `bp_lerp` |
| 33 | `-Wformat=2`：純字串當 format | 走明確的 `"%s"` |
| 34 | `-Wshadow`：巢狀迴圈同字母（`int n` vs `Vector3 n`） | 內層改名（`fn`） |
| 35 | `-Wformat-truncation`：`snprintf("%s/x", dir)` 且 `dir` 與目的地同大 | 來源緩衝縮短，或把 `%d` 參數夾到可證明範圍 |

### ⚪ 工具與流程

| # | 陷阱 | 修法 |
|---|---|---|
| 36 | `python .replace()` 對不到錨點會**靜默 no-op** | 用 `assert old in s`。錨點要對**定義**的完整簽名，不是較短的前綴 |
| 37 | python heredoc 的三重引號字串內含 `\\"` | SyntaxError。**用 `edit_file` 工具**改 Makefile 旗標 |
| 38 | 用 regex 審計載入器程式碼 | **會產生自信但錯誤的報告。** dispatch 的真實形狀是 `else if (!strcmp(k, "key"))` |
| 39 | `make release` 在 `make` 之後 | 不會重建物件檔。**先 `make clean`** |
| 40 | 沙箱環境工具鏈被清空 | raylib / emsdk / apt 套件可能整個消失。用 §1.1 重建 |
| 41 | 沒有 clang、沒有 cmake | 只能用 `gcc -fsyntax-only` 與 Makefile 驗證 |

---

## 15. 模組 API 索引

> 完整簽名在各自的 `.h`。這裡是「我想做 X，該去哪裡」的索引。

### engine/

| 模組 | 行數 | 責任 | 關鍵 API |
|---|---|---|---|
| `bp_common.h` | 256 | 容量常數、模擬常數、RNG、`g_engine`、日誌巨集、`bp_damp/lerp/lerp_angle`、`BP_MIN/MAX/CLAMP` | — |
| `bp_log.c` | 124 | 環形日誌、`g_engine` 定義、事件等待閘門 | `bp_log_init` `bp_log_lines` `bp_log_error_count` `bp_apply_event_wait` |
| `bp_math.{h,c}` | 238 | AABB、射線、膠囊-盒解算、角度 | `bp_ray_aabb` `bp_body_overlaps_box` `bp_resolve_body_box` `bp_forward_from_angles` |
| `bp_entity.{h,c}` | 162 | 世代式 handle 池 | `bp_pool_alloc/free/get/valid/compact` |
| `bp_config.{h,c}` | 337 | 行區間解析器 + 型別化 getter | `bp_config_load` `bp_config_find_section` `bp_cfg_int/float/bool/str/color/vec3` |
| `bp_assets.{h,c}` | 1387 | 程序化貼圖／模型／字型 | `bp_assets_init` `bp_tex` `bp_model_get` `bp_pal` `bp_draw_model` |
| `bp_audio.{h,c}` | 1049 | 44 種合成音效 + 動態音樂 | `bp_audio_play` `bp_audio_set_intensity` `bp_audio_set_volumes` |
| `bp_gfx.{h,c}` | 371 | 離屏目標、後製、品質、除錯繪圖 | `bp_gfx_begin_scene/end_scene` `bp_gfx_set_quality` `bp_gfx_draw_aabb/ray/grid` |
| `bp_camera.{h,c}` | 273 | yaw/pitch 相機、後座力、視模型矩陣 | `bp_camera_simulate` `bp_camera_render(alpha)` `bp_camera_look` `bp_camera_vm_matrix` |
| `bp_input.{h,c}` | 508 | 27 動作、重綁、**虛擬輸入** | `bp_down/pressed/released/axis` `bp_input_set_virtual` `bp_input_rebind` `bp_input_key_name` |

### game/

| 模組 | 行數 | 責任 | 關鍵 API |
|---|---|---|---|
| `bp_world.{h,c}` | 1322 | 17 材質、AABB 世界、移動件、觸發器、批次烘焙、天穹 | `bp_world_load` `bp_world_sweep` `bp_world_resolve_body` `bp_world_player_start` `bp_world_random_spawn` |
| `bp_levels.inc` | 430 | 三張地圖的幾何資料 | — |
| `bp_hitdetect.{h,c}` | 396 | 射線／球掃掠、傷害、爆炸、油桶 | `bp_trace_ray` `bp_trace_sphere` `bp_apply_damage` `bp_apply_explosion` |
| `bp_weapons.{h,c}` | 1049 | 5 武器、hitscan/projectile、散布、視模型 | `bp_weapons_try_fire` `bp_weapons_update` `bp_weapons_draw_viewmodel` `bp_weapons_accuracy` |
| `bp_enemies.{h,c}` | 1299 | 4 原型、感知、追跡、攻擊、死亡 | `bp_enemy_spawn` `bp_enemy_update` `bp_enemy_take_damage` `bp_enemy_alert_nearby` |
| `bp_player.{h,c}` | 1198 | 移動物理、傷害、掉落物、計分 | `bp_player_update` `bp_player_damage` `bp_pickup_spawn` `bp_player_register_kill` |
| `bp_waves.{h,c}` | 825 | 波次導演、預算、自適應生成、橡皮筋 | `bp_waves_start_level` `bp_waves_update` `bp_waves_notify_kill` `bp_waves_level_complete` |
| `bp_fx.{h,c}` | 751 | 粒子／貼花／曳光彈池 + 發射器 + 震動 | `bp_fx_impact_*` `bp_fx_explosion` `bp_fx_decal` `bp_fx_tracer` `bp_fx_shake` |
| `bp_ui.{h,c}` | 624 | 640×360 虛擬畫布 + 排版原語 | `bp_ui_text/text_align/text_shadow` `bp_ui_panel/panel_notch` `bp_ui_bar` `bp_ui_crosshair` |
| `bp_hud.{h,c}` | 739 | HUD、橫幅、提示、受擊指示 | `bp_hud_draw` `bp_hud_banner` `bp_hud_toast` `bp_hud_notify_damage` |
| `bp_menu.{h,c}` | 1135 | 列堆疊選單框架（8 畫面） | `bp_menu_open` `bp_menu_update` `bp_menu_draw` `bp_menu_set_results` |
| `bp_save.{h,c}` | 782 | 文字存檔、原子寫入、版本遷移、IDBFS | `bp_save_boot` `bp_save_init` `bp_save_submit_run` `bp_save_level_unlocked` |
| `bp_game.{h,c}` | 1123 | **場景機、開機順序、固定步長** | `bp_game_frame` `bp_game_start_level` `bp_game_apply_menu_action` `bp_game_submit_run` |

### 頂層

| 檔案 | 行數 | 責任 |
|---|---|---|
| `main.c` | 447 | 平台入口、視窗、帧迴圈、`--selftest` |
| `Makefile` | 209 | native / release / web / test / cfg / clean |

**總計：17,474 行，22 個編譯單元。**
