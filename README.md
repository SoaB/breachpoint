# BREACH POINT

**復古快節奏競技場射擊遊戲（Boomer Shooter）· C11 + raylib 5.5 · 零外部素材**

> *A high-velocity arena shooter* — Nullstate Interactive · v0.9.0

---

## 這是什麼

BREACH POINT 是一款 DOOM／Quake 血統的第一人称競技場射擊遊戲：**跑得快、跳得遠、槍很痛、敵人很多**。

但這個 repo 的重點不只是「一款遊戲」，而是 **一套可以直接長成商業產品的 C 語言遊戲工程架構**：

- **17,474 行 C11**，22 個編譯單元，`-Wall -Wextra -Wshadow -Wdouble-promotion -Wformat=2` 全開、**零警告**通過。
- **所有素材皆程序化產生**：30 張貼圖、35 個 3D 模型、4 套點陣字型、44 種音效、背景音樂 — 全部在啟動時由程式碼算出。**repo 裡沒有任何 `.png`／`.wav`／`.obj` 二進位檔。**
- **固定時間步長模擬（120 Hz）+ 渲染插值**，30 fps 和 300 fps 的手感完全一致。
- **啟動後零動態配置**：所有池子（敵人 128、粒子 4096、彈體 256…）在開機時一次分配。
- **資料驅動**：武器與敵人全部由 `assets/cfg/*.cfg` 調參，改數字不用重編。
- **單檔 Makefile**：原生 Linux / Windows / macOS 與 WebAssembly 兩條產線。

---

## 快速開始

### 需求

| 項目 | 版本 | 說明 |
|---|---|---|
| GCC / Clang | 支援 C11 | 只用 C11，沒有任何 C++ |
| GNU Make | 3.81+ | |
| raylib | **5.5**（原始碼） | 由 Makefile 建置靜態庫 |
| X11 + OpenGL 開發檔 | — | Linux 原生建置需要（見下） |
| emsdk | 3.x / 6.x | 只在 `make web` 時需要 |

### 1. 取得 raylib

```bash
git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git ~/tools/raylib
cd breachpoint && make raylib        # 建出 ~/tools/raylib/src/libraylib.a
```

raylib 放別處也可以，用變數覆寫：

```bash
make RAYLIB_DIR=/path/to/raylib
```

### 2. Linux 系統套件

```bash
sudo apt-get install -y build-essential git cmake \
    libasound2-dev libx11-dev libxrandr-dev libxi-dev \
    libgl1-mesa-dev libglu1-mesa-dev libxcursor-dev \
    libxinerama-dev libwayland-dev libxkbcommon-dev xvfb
```

<details>
<summary>macOS / Windows</summary>

**macOS** — `brew install raylib` 或自行編譯；連結改用 `-framework Cocoa -framework IOKit -framework CoreFoundation -framework OpenGL`。

**Windows (MinGW-w64)** — raylib 官方 release 附 `libraylib.a`；連結改用 `-lgdi32 -lwinmm -lopengl32`，並把 Makefile 的 `NATIVE_LIBS_LINUX` 換掉。

</details>

### 3. 建置與執行

```bash
make            # → build/breachpoint      （-O2 -g，含偵錯符號）
make run        # 建置並執行

make release    # → build/breachpoint-release（-O3 -DNDEBUG）
make clean
```

無圖形環境（CI、SSH）下用虛擬顯示：

```bash
Xvfb :99 -screen 0 1280x720x24 &
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 ./build/breachpoint
```

### 4. 自我測試（**跑這個來確認建置是好的**）

```bash
./build/breachpoint --selftest              # 30 秒模擬遊玩，輸出逐項 PASS/FAIL
./build/breachpoint --selftest --frames 600 # 自訂長度
./build/breachpoint --help
```

`--selftest` 會用**合成輸入**驅動完整的真實帧迴圈（含 GL context、渲染器、場景機），然後斷言 17 項條件：開機完成、素材生成、關卡載入、敵人生成、武器擊發、波次推進、玩家未掉出世界、執行期間沒有 ERROR 級日誌……**結束碼 0 = 全過，1 = 有檢查失敗，2 = 開機失敗。**

---

## 操作

| 動作 | 預設鍵位 | 手把 |
|---|---|---|
| 移動 | `W` `A` `S` `D` / 方向鍵 | 左類比 |
| 跳躍 | `Space` | A |
| 蹲下 | `Ctrl` / `C` | B |
| 冲刺 | `Shift` | LB |
| 開火 | 滑鼠左鍵 | RT |
| 瞄準 | 滑鼠右鍵 | LT |
| 換彈 | `R` | X |
| 武器 1–5 | `1`–`5` | — |
| 上/下一把 | `E` / `Q`（`E` 亦可滑鼠側鍵） | RB / LB |
| 互動 | `F` | Y |
| 暫停 | `Esc` / `P` | Start |
| 計分板 | `Tab` | — |
| 偵錯覆疊 | `F3` / `` ` `` | — |

**全部可在 OPTIONS → CONTROLS 重新綁定**，綁定存於存檔，每個動作支援 2 個綁定槽 + 手把。

HUD 切換：`H` 開關 HUD、`G` 切換精簡模式。

---

## 遊戲內容（v0.9.0）

### 三張競技場

| 關卡 | 波次 | 標準時間 | 設計意圖 |
|---|---|---|---|
| **FOUNDRY** | 6 | 2:30 | 對稱競技場，中央可行走金字塔。學移動。 |
| **SLAG PIT** | 8 | 3:30 | 三個高度帶（坑底／平台 2.2m／棧道 6.9m）+ 危險格柵坑。佔領棧道。 |
| **CONDUIT** | 9 | 4:00 | 緊密環形走廊，無 >22m 視線。霰彈槍為王。 |

每張地圖皆有：1 個玩家出生點、1 個 EXIT 觸發器、1 個 SECRET（+500 分）、定時重生的血量／護甲／彈藥／武器觸發器、可移動的門／升降機、可爆炸油桶。

### 五把武器

| 武器 | 傷害 | 彈匣 | 射速 | 特性 |
|---|---|---|---|---|
| **M9 SIDEARM** | 21 | 14 | 5.2/s | 半自動、精準、爆頭 ×1.6 |
| **SCATTER-12** | 12 × 10 發 | 6 | 1.15/s | 霰彈、高擊退、近距離統治 |
| **VK-7 VIPER** | 10 | 36 | 12.5/s | 全自動、低後座、彈藥多 |
| **LANCE-9 RAILGUN** | 118 | 4 | 0.82/s | 幾乎零散布、**穿透 4 個身體** |
| **ANVIL RPG** | 96 直擊 + 128 爆炸（半徑 5.2m） | 3 | 0.85/s | 自我傷害 45%，但擊退 100% → **火箭跳可用** |

### 四種敵人

| 敵人 | 血量 | 跑速 | 攻擊 | 分數 |
|---|---|---|---|---|
| **SCRAPHOUND**（grunt） | 62 | 6.6 m/s | 近戰 13 | 100 |
| **RIPPER**（hound） | 38 | **11.4 m/s** | 近戰 9、撲擊 | 75 |
| **SIEGEBREAKER**（brute） | 340 + 60 護甲 | 4.3 m/s | 近戰 26、擊退抗性 78% | 500 |
| **SENTRY NODE**（turret） | 120 + 45 護甲 | 靜止 | 彈體 30 m/s / 8 傷 / 0.26s | 250 |

AI：140° 視錐 + 視線檢查、3.5 秒「最後已知位置」記憶、首次發現時呼叫附近同伴、護甲吸收 55% 傷害、受擊硬直（僅當傷害 > 血量 6% 且通過痛覺機率）、死亡後下沉淡出 1.55 秒。

### 四種難度

| 難度 | 敵血 | 敵傷 | 生成壓力 | 分數 | 補給 |
|---|---|---|---|---|---|
| **RECRUIT** | ×0.70 | ×0.55 | ×0.72 | ×0.70 | ×1.00 |
| **VETERAN** | ×1.00 | ×1.00 | ×1.00 | ×1.00 | ×0.60 |
| **NIGHTMARE** | ×1.30 | ×1.45 | ×1.26 | ×1.60 | ×0.35 |
| **BREACH** | ×1.55 | ×1.85 | ×1.45 | ×2.50 | **×0.00** |

BREACH 難度下敵人**不會硬直**。

### 移動手感（這是 boomer shooter 的全部）

- 地面 8.2 m/s、冲刺 11.5 m/s、蹲下 3.4 m/s
- 地面加速度 72 m/s² → **0 到滿速約 0.11 秒**，刻意做成「立即」
- 空中控制 16 m/s²，搭配 1.35× 投影速度上限 → 可空氣加速／圓弧走位
- 跳躍初速 8.6 m/s → 站立跳 1.32 m（**預算**：0.55m 可直接走上、1.10m 需跳、1.50m 需衝刺跳）
- 土狼時間 0.12s、跳躍緩衝 0.14s、可變跳躍高度（放開按鍵即切斷上升）
- 硬速度上限 24 m/s（防止圓弧走位發散到無限）
- 掉落傷害：14 m/s 以下無傷，之後每 m/s 超額 3.2 點，**護甲不吸收**
- 連殺窗口 3.5s，倍率 `1 + min(streak, 8)/4`

---

## 專案結構

```
breachpoint/
├── Makefile                  單一建置系統（native / release / web / test / cfg）
├── main.c                    平台入口 + 視窗 + 帧迴圈 + --selftest 測試載具
│
├── engine/                   可重用的引擎層（不含遊戲規則）
│   ├── bp_common.h           容量上限、模擬常數、RNG、g_engine 單例、日誌巨集
│   ├── bp_log.c              256×192 環形日誌緩衝 + g_engine 定義 + 事件等待閘門
│   ├── bp_math.{h,c}         AABB、射線、膠囊-盒碰撞解算、角度插值
│   ├── bp_entity.{h,c}       世代式 handle 池（懸空指標偵測）
│   ├── bp_config.{h,c}       行區間解析器 + 型別化 getter（含降級預設值）
│   ├── bp_assets.{h,c}       程序化貼圖／模型／點陣字型產生器
│   ├── bp_assets_font.inc    手寫 5×7 點陣字型位元組表
│   ├── bp_audio.{h,c}        44 種程序化音效 + 3 聲部混音 + 180 BPM 動態音樂
│   ├── bp_gfx.{h,c}          離屏渲染目標、後製著色器、品質预设、除錯繪圖
│   ├── bp_camera.{h,c}       yaw/pitch 相機、後座力、視野呼吸、視模型矩陣
│   └── bp_input.{h,c}        27 個動作的重新綁定輸入層 + **虛擬輸入**（測試用）
│
├── game/                     遊戲規則層
│   ├── bp_world.{h,c}        17 種材質、AABB 世界、移動件、觸發器、靜態批次烘焙、天穹
│   ├── bp_levels.inc         三張關卡的手寫幾何資料
│   ├── bp_hitdetect.{h,c}    射線／球體掃掠、傷害套用、爆炸、油桶連鎖
│   ├── bp_weapons.{h,c}      5 把武器、hitscan／projectile、散布、視模型
│   ├── bp_enemies.{h,c}      4 種原型、感知、追跡、攻擊、死亡
│   ├── bp_player.{h,c}       移動物理、傷害、掉落物、計分
│   ├── bp_waves.{h,c}        波次導演：預算、自適應生成、技術橡皮筋、完美波次
│   ├── bp_fx.{h,c}           粒子／貼花／曳光彈池 + 各種發射器 + 畫面震動
│   ├── bp_ui.{h,c}           640×360 虛擬畫布 + 點陣字排版原語
│   ├── bp_hud.{h,c}          HUD、橫幅、提示、受擊方向指示
│   ├── bp_menu.{h,c}         列堆疊選單框架（8 個畫面共用一套導覽）
│   ├── bp_save.{h,c}         文字存檔、原子寫入、版本遷移、web IDBFS
│   └── bp_game.{h,c}         **場景機 + 固定步長迴圈 + 分階段開機**（頂層協調者）
│
├── assets/cfg/
│   ├── weapons.cfg           武器調參（34 個鍵 × 5 把）
│   └── enemies.cfg           敵人調參（28 個鍵 × 4 種）
│
├── docs/                     ← 詳細開發文件
├── web/                      ← WASM 外殼（待完成，見下）
├── tests/                    ← 無 GL 的系統層單元測試（待完成，見下）
└── tools/                    ← 設定檔嵌入產生器（待完成，見下）
```

### 相依方向（單向，不可逆）

```
main.c
  └─ bp_game          場景機、開機順序、固定步長
       ├─ bp_menu  bp_hud  bp_save  bp_waves
       ├─ bp_player  bp_weapons  bp_enemies
       ├─ bp_world  bp_hitdetect  bp_fx
       └─ engine/*   日誌、數學、輸入、相機、圖形、音效、素材、設定、實體池
```

規則：**上層可以呼叫下層，下層絕不 include 上層。** `engine/` 完全不知道「武器」或「敵人」是什麼。

---

## 目前完成度

| 子系統 | 狀態 |
|---|---|
| 原生建置（Linux, GCC） | ✅ 零警告，2.9–3.0 MB 二進位 |
| 分階段開機（16 階段／每帧一階段） | ✅ 實測 0.22s 完成 |
| 程序化素材管線 | ✅ 30/30 貼圖、35/35 模型、字型 OK，實測 ~330–420 ms |
| 世界／關卡／碰撞／烘焙 | ✅ 50 solids、28 triggers、12 靜態批次、516 三角形 |
| 玩家移動、武器、敵人 AI、波次導演 | ✅ 編譯與單元層級就緒 |
| HUD／選單／存檔／音訊 | ✅ |
| `--selftest` 整合測試載具 | ✅ 17 項斷言，确定性 60 Hz 模擬 |
| **WebAssembly 建置** | ⚠️ Makefile 規則已就緒，但 `web/shell.html` 尚未撰寫 → `make web` 會失敗 |
| **`make test`（無 GL 單元測試）** | ⚠️ `tests/test_systems.c` 尚未撰寫 |
| **`make cfg`（設定嵌入）** | ⚠️ `tools/gen_cfg.py` 尚未撰寫 |
| 自訂霧效著色器 | ⚠️ `fog_color`/`fog_density` 目前是純資料，raylib 5.5 無內建霧 |

見 [`docs/ROADMAP.md`](docs/ROADMAP.md) 取得完整的待辦與已知問題清單。

---

## 文件

| 文件 | 內容 |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | 架構設計：分層、相依規則、帧解剖、記憶體策略、各子系統設計決策與理由 |
| [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) | 開發指南：建置、除錯、程式碼慣例、**如何新增一把武器／一種敵人／一張地圖**、踩過的坑 |
| [`docs/GAMEPLAY_DATA.md`](docs/GAMEPLAY_DATA.md) | 完整數據參考：所有常數、武器表、敵人表、難度倍率、存檔格式、設定檔語法 |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | 已知問題、未完成項目、下一步 |

---

## 授權

程式碼：MIT（見 `LICENSE`，若未提供則依 MIT 條款使用）。
raylib：zlib/libpng 授權，© Ramon Santamaria。

本專案所有素材皆由程式碼於執行期產生，不含任何第三方版權素材。
