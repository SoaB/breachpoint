# BREACH POINT — 路線圖與已知問題

> 誠實的現況。這份文件的目的是讓接手的人**立刻知道什麼能用、什麼不能用、什麼看起來能用其實有坑**。
> 最後更新：自我測試基礎設施完成、原生建置零警告通過之後。

---

## 1. 現況總覽

| 指標 | 值 |
|---|---|
| 程式碼量 | **13,630 行 `.c`**（22 個編譯單元）+ 2,765 行 `.h` + 543 行 `.inc` = **16,938 行原始碼** |
| 建置狀態 | ✅ `make` 零錯誤零警告 → `build/breachpoint`（3.0 MB） |
| 警告旗標 | `-Wall -Wextra -Wshadow -Wdouble-promotion -Wformat=2` |
| 開機實測 | ✅ 0.22–0.27 s（16 階段，llvmpipe 軟體光柵化） |
| 素材實測 | ✅ 30/30 貼圖、35/35 模型、字型 OK，330–424 ms |
| 世界實測 | ✅ FOUNDRY：50 solids / 28 triggers / 1 mover / 13 spawns / 12 批次 / 516 三角形 |
| 自我測試載具 | ✅ 18 項斷言，确定性 60 Hz，~12 s 牆鐘跑 30 s 模擬 |
| **自我測試結果** | ❌ **6 項 FAIL**（見 §2） |
| Web 建置 | ❌ 缺 `web/shell.html` |
| 無 GL 單元測試 | ❌ 缺 `tests/test_systems.c` |
| 設定嵌入 | ❌ 缺 `tools/gen_cfg.py` |

**一句話總結**：引擎與遊戲系統**全部存在、全部編譯、開機與載入實測通過**；但**尚未驗證過一場完整的戰鬥**——自我測試目前卡在標題畫面，所以武器／敵人／波次的**執行期**行為還沒被任何自動化檢查覆蓋。

---

## 2. 🔴 最高優先：自我測試的 6 項 FAIL

### 現況

```
[INFO ] selftest: 1800 frames at a fixed 60.0 Hz = 30.0 s of simulated play
[INFO ] selftest: frame  120  sim   2.0s  scene 1  wave 0   0 alive  0 particles  9.5 ms/frame
[INFO ] selftest: frame 1800  sim  30.0s  scene 1  wave 0   0 alive  0 particles  9.7 ms/frame
                                                            ^^^^^^^^
                                            scene 1 = SCENE_TITLE，30 秒都沒離開
```

```
[ERROR] selftest: FAIL  enemies spawned: 0 (0 alive at end)
[ERROR] selftest: FAIL  combat produced kills: 0
[ERROR] selftest: FAIL  weapons fired: 0 shots, 0 hits (0%)
[ERROR] selftest: FAIL  wave director advanced past wave 1 to 0 (IDLE)
[ERROR] selftest: FAIL  player simulated 0.0s alive
[ERROR] selftest: FAIL  no error-level log lines raised by the run (5 new)   ← 前 5 項自己的輸出
```

**11 項 PASS**：boot、assets、font、level load、geometry bake、scoring、particle pool、sim time、fps、fixed-step loop、player-in-bounds（×2）、save reachable。

### 診斷到哪裡了

已排除：

| 假設 | 排除方式 |
|---|---|
| 開機沒完成 | ✅ `boot complete in 0.22s` 有印，`boot_done = true` |
| 固定步長壞了 | ✅ `[ACC]` 插樁顯示 `scene_time` 以正確速率推進（30 帧 = 0.2s） |
| 虛擬輸入沒生效 | ✅ 插樁顯示 `held = 1`，`apply_virtual` 有跑 |
| 選單沒開 | ✅ `menuOpen = 1`，`screen = 1`（TITLE），`count = 6` 列 |
| ACCEPT 邊緣沒觸發 | ✅ 插樁顯示 `pressed = 1`，且 `sel` 非 NULL |

**已修掉的一個真 bug**：選單的滑鼠 hover 沒有「遊標在視窗內」守衛，所以第一帧就把 `selected` 設成 2（OPTIONS）而不是 0（NEW RUN）——ACCEPT 因此開了 OPTIONS。已加 `mouseInside` 檢查。

**但修完之後症狀不變**，所以還有第二個原因。最後一次插樁顯示：

```
[MDBG] accept held=1 pressed=1 sel=<non-null> count=6 selected=2 screen=1 init=1
```

`selected` **仍然是 2**。這表示 `mouseInside` 守衛沒有生效，或者還有另一條路徑在改 `selected`。

### 下一步（按順序）

1. **確認 `mouseInside` 守衛真的在編譯產物裡**，並印出 `GetMousePosition()` / `GetScreenWidth()` 的實際值。Xvfb 下游標可能**在**視窗內（0,0 是視窗的合法座標），那守衛就不會擋——需要改成「游標真的移動过」而不是「游標在視窗內」。

   建議的正確守衛：
   ```c
   /* 只在游標真的移動过时才让 hover 改变选择。一个从未移动的游标停在 (0,0)
      不是一个"选择"意图。 */
   static Vector2 s_lastMouse;
   Vector2 msp = GetMousePosition();
   bool mouseMoved = !Vector2Equals(msp, s_lastMouse);
   s_lastMouse = msp;
   ```

2. **或者繞過選單**：自我測試的目的是覆蓋戰鬥，不是覆蓋選單導覽（那該由 `tests/` 的單元測試負責）。直接呼叫 `bp_game_start_level(0)`，並把「選單 ACCEPT 能啟動遊戲」拆成一個**獨立的、更小的**檢查。

   這是務實的做法：目前 6 項 FAIL 全部源自「沒進到 PLAYING」，一個呼叫就能解鎖全部覆蓋率。

3. **兩個都做**：先做 (2) 讓自我測試開始覆蓋戰鬥（這是真正的價值），再做 (1) 修好選單的遊標 bug（這是真實的玩家-facing bug）。

### 為什麼這件事重要

6 項 FAIL 不是「測試寫錯了」。它們是**「整個戰鬥層從未在執行期被驗證过」**的訊號：

- `bp_weapons.c`（851 行）的開火路徑**沒有跑过一次**
- `bp_enemies.c`（1117 行）的生成／AI／死亡**沒有跑过一次**
- `bp_waves.c`（698 行）的狀態機只跑到 `WAVE_INTRO`
- `bp_player.c`（1049 行）的 `time_alive` 是 0，表示**玩家 update 沒有在 PLAYING 場景跑过**

這些是專案最大的四個檔案，合計 3,715 行。**編譯通過 ≠ 執行正確。**

---

## 3. 🟠 缺失的檔案（Makefile 已引用）

### 3.1 `web/shell.html` — 阻擋 `make web`

emscripten 的 `--shell-file` 目標。需要：

```html
{{{ SCRIPT }}}            <!-- emscripten 必要佔位符 -->
<canvas id="canvas">      <!-- 必要 -->
```

專案特定需求：

- [ ] 全螢幕、無邊框、黑色背景（letterbox 由 `bp_ui` 處理，外殼不要自己加）
- [ ] **「點擊以開始」覆蓋層** — canvas 需要先被點擊才能取得鍵盤焦點。啟動日誌已經在印這句話，但畫面上也要有
- [ ] 載入進度條，讀 `Module.setStatus`
- [ ] `Module.onRuntimeInitialized` 隱藏進度
- [ ] **不引用任何外部 CDN**（字型、CSS、JS）— 產出的 HTML 要能離線開啟
- [ ] `Module.onAbort` 顯示錯誤訊息而不是空白頁
- [ ] WebGL 不可用時的 fallback 訊息

### 3.2 `tools/gen_cfg.py` — 阻擋 `make cfg`

把 `assets/cfg/*.cfg` 嵌進 `engine/bp_config_data.h`，讓 web 建置不需要 `--preload-file`。

```python
# 預期輸出形狀
static const char bp_cfg_weapons[] =
    "weapon pistol\n"
    "    display_name = M9 SIDEARM\n"
    ...;
static const int bp_cfg_weapons_len = sizeof(bp_cfg_weapons) - 1;
```

然後 `bp_weapons_load_config()` 在 `PLATFORM_WEB` 下走 `bp_config_load_mem(bp_cfg_weapons, bp_cfg_weapons_len)`。

**`bp_config_load_mem()` 已經存在**，所以這纯粹是生成器的問題。

### 3.3 `tests/test_systems.c` — 阻擋 `make test`

無 GL 的系統層單元測試。Makefile 的 `test` 目標已經把正確的四個 TU 列出來：

```makefile
TEST_SRC := tests/test_systems.c engine/bp_log.c engine/bp_math.c \
            engine/bp_entity.c engine/bp_config.c
```

注意它**不含** raylib 的連結（只有 `-lm`），所以這四個 TU 必须能在沒有 GL context 的情況下編譯。`-DBP_HEADLESS_TEST` 已經傳進去了。

應該覆蓋：

- [ ] `bp_math`：`bp_ray_aabb` 的命中／未命中／起點在盒內、`bp_resolve_body_box` 的逐軸推出方向、`bp_lerp_angle` 的 ±180° 繞回
- [ ] `bp_entity`：世代遞增（free 後的 handle 必須失效）、free list 重用、`bp_pool_valid` 對越界 index
- [ ] `bp_config`：缺失鍵的 fallback、未知鍵被忽略、`#` 與 `;` 註解、大小寫不敏感、`bp_cfg_color` 的 `#RRGGBB`、`bp_cfg_vec3` 的 `"x y z"`、損壞檔案不崩潰
- [ ] `bp_log`：環形緩衝繞回（寫 300 行後 `bp_log_lines` 回傳 256）、`bp_log_error_count` 只數 ERROR

> ⚠️ `engine/bp_math.h` include 了 `bp_common.h`，而 `bp_common.h` include 了 `raylib.h`。所以 `make test` **仍然需要 `-I$(RAYLIB_DIR)/src`**（已加），但**不需要連結 raylib**，因為這四個 TU 不呼叫任何 raylib 函式。這一點要實測驗證——`bp_log.c` 的 `bp_apply_event_wait()` 呼叫了 `IsWindowReady()`／`EnableEventWaiting()`／`SetTargetFPS()`，**那會造成連結錯誤**。
>
> **修法**：把 `bp_apply_event_wait()` 移到 `bp_gfx.c`，或用 `#ifndef BP_HEADLESS_TEST` 包起來。這是寫 `tests/test_systems.c` 時會立刻撞到的第一個問題。

### 3.4 `engine/bp_config_data.h`

由 `tools/gen_cfg.py` 產生，不該手寫，不該進版控（或進版控但在 CI 驗證它與 `.cfg` 同步）。

### 3.5 其他不存在的目錄內容

`web/`、`docs/`（本文件之前）、`tests/`、`tools/` 都是空的佔位目錄。`docs/` 現在有四份文件。

---

## 4. 🟡 已知技術債

### 4.1 選單動作 id 靠「約定」同步

`game/bp_menu.c`：
```c
enum { A_PLAY = 1, A_LEVELS, A_OPTIONS, A_CONTROLS, A_CREDITS, A_QUIT,
       A_BACK, A_RESET_BINDS, A_RESET_SETTINGS, A_APPLY,
       A_LEVEL_BASE = 100, A_BIND_BASE = 200 };
```

`game/bp_game.c`：
```c
enum { GA_PLAY = 1, GA_LEVELS, GA_OPTIONS, GA_CONTROLS, GA_CREDITS, GA_QUIT,
       GA_BACK, GA_RESET_BINDS, GA_RESET_SETTINGS, GA_APPLY,
       GA_LEVEL_BASE = 100, GA_BIND_BASE = 200 };
```

**編譯器不會檢查這兩個列舉一致。** 值不一致 → 選單回傳的 id 被 dispatch 到錯誤分支，症狀是「按 A 結果做了 B」，而且**只在執行期、只在那一個按鈕上**出現。

**修法**：抽到 `game/bp_menu_ids.h`，兩邊都 include。這是 20 分鐘的工作，應該在加第二個選單畫面之前做。

### 4.2 `make release` 不會重建物件檔

```makefile
release: NATIVE_CFLAGS := $(subst -O2 -g,-O3 -DNDEBUG,$(NATIVE_CFLAGS))
release: $(BUILD)/$(GAME)-release
```

target-specific 變數改了旗標，但 `$(NATIVE_OBJS)` 的時間戳記沒變 → make 認為物件檔是新的 → **連結的是 `-O2 -g` 的物件**。

**症狀**：`build/breachpoint-release` 存在、能跑、但沒有 `-O3 -DNDEBUG`。完全靜默。

**修法**：加一個 `build/obj-release/` 目錄，複製 `$(NATIVE_OBJ)/%.o` 的規則。或者在文件裡寫明「先 `make clean`」（DEVELOPMENT §2.1 已經寫了，但那是在補漏洞而不是修漏洞）。

### 4.3 draw call / 三角形數無法取得

`g_engine.perf.draw_calls` 與 `.tris` **永遠是 0**。raylib 5.5 的 `rlRenderBatch.drawCounter` 是內部的，rlgl 沒有公開存取器。

HUD 與自我測試都**不回報**它們（有註解說明）。但欄位存在，所以有人會試圖填它們。

**選項**：
1. 保持現狀，欄位留著當文件（目前的選擇）
2. 刪掉欄位，避免誤導
3. 自己包一個計數層：在 `bp_world_draw` 等函式裡手動累加。這會得到「我們發了多少個 Draw* 呼叫」，但不是「GL 收到多少個 draw call」——兩者不同（raylib 會批次化）。

### 4.4 沒有霧

`bp_world` 的 `fog_color` 與 `fog_density` 是**純資料欄位**，没有任何程式碼讀它們來影響渲染。raylib 5.5 沒有內建霧。

目前天穹渐变 + `CULL_DISTANCE 170` 的距離剔除承擔了大部分「遠處淡出」的視覺工作，所以**不明顯缺**。但三個關卡都設了 `fog_color`，那是未兌現的承諾。

**修法**：一個自訂的 `Shader`，在 `bp_gfx` 的後製 pass 之前套用到世界幾何。需要 GLSL 100（web/ES2）與 GLSL 330（桌面）兩個方言——`bp_gfx.c` 已經有 `BP_GLSL_HEADER` 的雙方言路徑，但**那條路徑未經驗證**（見 §5）。

### 4.5 `MAX_MAP_CELLS` 與 `MAX_SAVE_SLOTS` 未使用

- `MAX_MAP_CELLS (128×128)` — 碰撞網格欄位已預留，但世界是扁平 AABB 陣列線性掃描。**在 50 個 solid 的規模下這是正確的**（任何空間分割都是負最佳化）。關卡長到 500+ solids 時才需要填。
- `MAX_SAVE_SLOTS (4)` — 只有 1 個 profile。多存檔槽需要 UI（選單畫面）+ 遷移邏輯。

### 4.6 `bp_pool_compact()` 未被呼叫

它存在、編譯通過、但帧迴圈不用它（因為它會使所有 handle 失效）。**应该在關卡切換時呼叫**（`bp_world_unload` 或 `bp_enemies_clear` 之後），把 free list 收回來。目前不呼叫的後果是：長時間遊玩後池子的 `count` 只增不減，雖然 `alive_count` 是對的。

### 4.7 存檔路徑在 Windows 上不對

`bp_save.c` 用 `getenv("HOME")`。Windows 上 `HOME` 通常不存在（是 `USERPROFILE`）。

**修法**：`#ifdef _WIN32` 走 `SHGetKnownFolderPath(FOLDERID_RoamingAppData)`，或用 raylib 的 `GetApplicationDirectory()`（但那是**執行檔**目錄，不是使用者資料目錄，寫入可能需要管理員權限）。

---

## 5. ⚪ 未驗證的程式碼路徑

這些**編譯通過但從未在執行期觀察过**。不要「修好」它們，先**測量**。

| 路徑 | 位置 | 為什麼未驗證 | 風險 |
|---|---|---|---|
| 後製著色器的雙方言路徑 | `bp_gfx.c` 的 `BP_GLSL_HEADER` | 只在桌面 GLSL 330 下跑过；ES2/GLSL 100 分支沒編譯过（沒有 web 建置） | 中 — web 建置會立刻暴露 |
| `img_to_texture` 的 `UnloadImage` 順序 | `bp_assets.c` | 必須先把 `im->px` 設 NULL 再 `UnloadImage`。目前**看起來**是對的，但沒有用 ASan 跑过 | 高 — double-free |
| `bp_draw_model` 算了又 `BP_UNUSED` 的 `Matrix t/r/s` | `bp_assets.c` | 疑似死程式碼，或疑似漏了套用 | 低 — 但表示意圖不明 |
| 火箭跳 | `bp_weapons.c` | `blast_self = 0.45` / `blast_knockback = 12.0` 的不對稱**應該**讓火箭跳可用，但從未實測 | 中 — 這是設計核心 |
| 油桶連鎖爆炸 | `bp_hitdetect.c` 的 `bp_barrel_damage` | 編譯通過，從未觸發过 | 中 |
| 移動件載人 | `bp_world.c` 的 mover | 玩家站在升降機上時的相對運動未測 | 高 — 常見 bug 來源 |
| SECRET 觸發器 | 三張地圖各有 1 個 | `+500` 分與 `g_player.secret_reached` 的傳遞未測 | 低 |
| `SCENE_LEVEL_COMPLETE` → `bp_game_next_level()` | `bp_game.c` | 需要先通關一整個關卡 | 高 — 從未被執行过 |
| `SCENE_GAME_COMPLETE` | `bp_game.c` | 需要通關全部三張 | 低（路徑短） |
| 存檔版本遷移 | `bp_save.c` | 需要一個 version < 3 的檔案 | 中 |
| Web IDBFS 同步 | `bp_save.c` | 沒有 web 建置 | 高 — 重新整理就丟存檔 |
| 手把輸入 | `bp_input.c` | 沙箱沒有手把 | 中 |
| 視窗 resize | `bp_game.c` 的 `IsWindowResized()` | Xvfb 下不會 resize | 中 — `bp_gfx_resize` + `bp_ui_resize` 的互動未測 |
| `--frames` 以外的 CLI 參數 | `main.c` | `--help` 沒跑过 | 低 |

---

## 6. 建議的下一步（按投資報酬排序）

### 🔴 P0 — 讓自我測試覆蓋戰鬥

這是**唯一**應該現在做的事。目前 3,715 行核心遊戲邏輯（weapons / enemies / waves / player）的執行期行為**零覆蓋**。

1. 自我測試改成直接呼叫 `bp_game_start_level(0)`（繞過選單），把選單導覽拆成獨立檢查
2. 確認 `enemies spawned > 0`、`shots fired > 0`、`kills > 0`、`wave_index >= 2`、`time_alive > 1.0`
3. 修掉暴露出來的所有真 bug
4. **然後**才修選單的遊標 hover bug

預期：這一步會暴露 3–10 個真 bug。這是它的價值。

### 🟠 P1 — 補齊三個缺失檔案

1. `tests/test_systems.c`（先把 `bp_apply_event_wait` 移出 `bp_log.c`，否則連結失敗）
2. `web/shell.html`
3. `tools/gen_cfg.py`

順序很重要：`test_systems.c` 最先，因為它不需要工具鏈修復（emsdk 目前是半殘的），而且它會立刻驗證 `bp_math`／`bp_entity`／`bp_config` 的正確性——那三個是所有系統的基礎。

### 🟡 P2 — web 建置 + 瀏覽器預覽

使用者明確要求「可在瀏覽器遊玩的 web 版」。需要：

1. 修 emsdk（`chmod -R +x`，`rm -rf upstream node downloads .emscripten`，重跑 `./emsdk install latest && ./emsdk activate latest`，約 1.7 GB）
2. `make raylib-web`（3–5 分鐘）
3. 寫 `web/shell.html`
4. `make web` → `make serve`
5. 在瀏覽器實測（**這裡會暴露 §5 的 ES2 著色器路徑問題**）

### 🟢 P3 — 清技術債

1. `bp_menu_ids.h`（§4.1）— 20 分鐘，消除一整類靜默 bug
2. `make release` 的物件檔目錄（§4.2）
3. `bp_pool_compact()` 在關卡切換時呼叫（§4.6）

### 🔵 P4 — 內容

1. 第 4–5 張地圖（**先寫可達性驗證器**，見下）
2. 第 6 把武器、第 5 種敵人
3. 霧效著色器（§4.4）
4. 多存檔槽（§4.5）

### 工具：關卡可達性驗證器

**這是 P4 的前置條件。** 手寫關卡幾何的可達性 bug 完全靜默（DEVELOPMENT §8.3），而且自我測試抓不到（玩家永遠走不到那個平台，所以不會掉出世界）。

最小可行版本：從 `PLAYER_START` 做 BFS，節點是「玩家可以站立的位置」（網格化到 0.5 m），邊是「走／跳／衝刺跳可達」。然後斷言：

- `EXIT` 可達
- `SECRET` 可達
- 所有 `SPAWN` 點可達（否則敵人會生成在玩家永遠打不到的地方）
- 所有 HEALTH/ARMOR/AMMO 觸發器可達

這可以在 `--selftest` 裡跑，成本是 <100 ms。**它會抓到的 bug 數量，可能比其他所有測試加起來還多。**

---

## 7. 環境注意事項

### 沙箱的工具鏈會消失

本專案開發期間，**整個工具鏈被清空过至少兩次**：`~/tools/raylib` 的 `src/` 消失、`~/tools/emsdk` 掉回 65 MB（完整安裝是 1.7 GB）、apt 套件還在但 build 產物消失。

**每次開始工作前先驗證**：

```bash
ls ~/tools/raylib/src/libraylib.a        # 應該 ~2.7 MB
ls ~/tools/emsdk/upstream/emscripten/emcc
cd breachpoint && make -j4 && ./build/breachpoint --selftest
```

重建 raylib 只要 15 秒（clone + `make raylib`）。重建 emsdk 要幾分鐘 + 1.7 GB。

### 執行需要 Xvfb

沙箱沒有實體顯示器：

```bash
Xvfb :99 -screen 0 1280x720x24 &
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1
```

會印兩行 `WARNING: GLFW: Failed to find selected monitor` 與一行 `WARNING: AUDIO: Failed to initialize playback device`。**這兩個都是正常的**，音訊會降級為靜音。

### 沒有 clang、沒有 cmake

只能用 `gcc` + `make` + `gcc -fsyntax-only` 驗證。沒有 sanitizer（ASan/UBSan）可用，所以 §5 的 double-free 風險無法用工具排除，只能靠 code review。

---

## 8. 已完成的工作（供參考，不用重做）

### 引擎層（9 個模組）

- ✅ `bp_common.h` — 容量、模擬常數、RNG（xorshift32）、`g_engine`、日誌巨集、`bp_damp/lerp/lerp_angle`
- ✅ `bp_log.c` — 256×192 環形緩衝、`g_engine` 定義、`bp_apply_event_wait()` 事件等待閘門、錯誤計數器
- ✅ `bp_math.{h,c}` — AABB、`bp_ray_aabb`、膠囊-盒解算、`bp_forward_from_angles`（yaw 0 = +Z）
- ✅ `bp_entity.{h,c}` — 世代式 handle 池
- ✅ `bp_config.{h,c}` — 行區間解析器、6 種型別化 getter、`load_mem`
- ✅ `bp_assets.{h,c}` + `bp_assets_font.inc` — 30 貼圖 / 35 模型 / 4 字型，分組計時
- ✅ `bp_audio.{h,c}` — 43 種合成音效（索引 0–42）、3 聲部 alias bank、180 BPM 串流音樂、動態強度、裝置失敗降級
- ✅ `bp_gfx.{h,c}` — 離屏目標、後製、4 級品質、除錯繪圖原語
- ✅ `bp_camera.{h,c}` — yaw/pitch、後座力、FOV 呼吸、視模型 ZXY 矩陣
- ✅ `bp_input.{h,c}` — 27 動作、重綁、焦點遺失釋放、`BP_BIND_MOUSE_BASE` 偏置、**虛擬輸入**

### 遊戲層（13 個模組）

- ✅ `bp_world.{h,c}` + `bp_levels.inc` — 17 材質、3 關卡、AABB 碰撞、移動件、觸發器、天穹、靜態批次烘焙
- ✅ `bp_hitdetect.{h,c}` — 射線／球掃掠、傷害、爆炸、油桶
- ✅ `bp_weapons.{h,c}` + `weapons.cfg` — 5 武器、hitscan/projectile、均勻錐散布、視模型
- ✅ `bp_enemies.{h,c}` + `enemies.cfg` — 4 原型、140° 視錐 + LOS、3.5s 記憶、同伴警報、硬直、死亡動畫
- ✅ `bp_player.{h,c}` — 移動物理（空氣加速、土狼時間、跳躍緩衝、可變高度）、掉落傷害、掉落物、計分、連殺
- ✅ `bp_waves.{h,c}` — 6 狀態導演、預算、自適應生成、±18% 橡皮筋、完美波次、120s 軟鎖防護
- ✅ `bp_fx.{h,c}` — 粒子／貼花／曳光彈池、11 種發射器、畫面震動
- ✅ `bp_ui.{h,c}` — 640×360 虛擬畫布、整數倍縮放、點陣字排版原語
- ✅ `bp_hud.{h,c}` — HUD、橫幅、提示、受擊方向指示
- ✅ `bp_menu.{h,c}` — 列堆疊框架、8 畫面、欄位流、按鍵捕捉、滑鼠互動
- ✅ `bp_save.{h,c}` — 文字存檔、原子寫入、版本遷移、boot/init 拆分、IDBFS
- ✅ `bp_game.{h,c}` — 9 場景機、16 階段開機、固定步長 + 插值、吸引模式相機、結果提交
- ✅ `main.c` — 平台入口、視窗旗標、監視器夾限、帧迴圈、`--selftest`

### 基礎設施

- ✅ `Makefile` — native / release / web / test / cfg / clean / raylib / raylib-web
- ✅ 零警告建置（`-Wall -Wextra -Wshadow -Wdouble-promotion -Wformat=2`）
- ✅ 自我測試載具（18 項斷言、确定性 60 Hz、逐批計時、結束碼）
- ✅ `docs/` — ARCHITECTURE / DEVELOPMENT / GAMEPLAY_DATA / ROADMAP

### 這個過程中修掉的真 bug（不是測試 bug）

| # | bug | 症狀 | 修法 |
|---|---|---|---|
| 1 | `g_engine` 只有 extern 宣告 | 連結失敗 | 定義在 `bp_log.c` |
| 2 | `BP_MAX(1, atoi(argv[++i]))` | **segfault，無診斷** | 先賦值給區域變數 + 在巨集定義處加警告註解 |
| 3 | 開機期間啟用事件等待 | **開機卡死，無錯誤訊息** | 集中到 `bp_apply_event_wait()`，只在 `run_frame()` 翻旗標 |
| 4 | `boot_tick()` 尾段非冪等 | 每帧重進 TITLE + 重啟事件等待 | `if (boot_done) return;` |
| 5 | 選單 hover 無遊標守衛 | ACCEPT 開了錯誤的畫面 | `mouseInside` 檢查（**尚未確認有效**，見 §2） |
| 6 | 自我測試直接寫 `g_input.act[]` | 合成輸入被 `begin_frame()` 抹掉 | 新增 `bp_input_set_virtual()` API |
| 7 | 自我測試用 `GetFrameTime()` | 慢機器上模擬時間不足 → 6 個假 FAIL | 固定 `SELFTEST_DT` |
| 8 | `bp_save_init()` 在 `InitWindow` 前呼叫 | segfault | 拆成 `bp_save_boot()` / `bp_save_init()` |
| 9 | Makefile 缺 `-I$(RAYLIB_DIR)/src` | `raylib.h: No such file` | 加到 `NATIVE_CFLAGS` 與 `test` 目標 |
| 10 | `-Wformat-truncation` ×3 | 警告 | 縮短來源緩衝 / 夾限 `%d` 參數 |

**#2、#3、#4 是完全靜默的**——沒有錯誤訊息，只是程式不做該做的事。這三條是這個專案最有價值的教訓，都已經寫進 ARCHITECTURE 與 DEVELOPMENT 的陷阱表。
