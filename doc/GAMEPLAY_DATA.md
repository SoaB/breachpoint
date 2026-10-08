# BREACH POINT — 遊戲數據參考

> 所有數值的單一真相來源。**改平衡前請先讀這份文件**，很多常數之间有耦合關係。
> 權威來源是程式碼；這份文件是它的鏡像，改程式碼時請同步改這裡。

---

## 目錄

1. [模擬常數](#1-模擬常數)
2. [容量上限](#2-容量上限)
3. [玩家移動](#3-玩家移動)
4. [玩家戰鬥與生存](#4-玩家戰鬥與生存)
5. [武器表](#5-武器表)
6. [敵人表](#6-敵人表)
7. [波次導演](#7-波次導演)
8. [難度倍率](#8-難度倍率)
9. [關卡](#9-關卡)
10. [材質](#10-材質)
11. [計分](#11-計分)
12. [圖形品質](#12-圖形品質)
13. [調色盤](#13-調色盤)
14. [音效清單](#14-音效清單)
15. [素材清單](#15-素材清單)
16. [設定檔語法](#16-設定檔語法)
17. [存檔格式](#17-存檔格式)
18. [預設按鍵綁定](#18-預設按鍵綁定)
19. [場景與計時](#19-場景與計時)

---

## 1. 模擬常數

`engine/bp_common.h`

| 常數 | 值 | 說明 |
|---|---|---|
| `SIM_HZ` | 120.0 | 模擬 tick 率。**不是**渲染率 |
| `SIM_DT` | 1/120 ≈ 8.333 ms | 每步固定時長 |
| `MAX_FRAME_DT` | 0.1 s | 幀時間夾限（背景分頁回來時） |
| `MAX_SIM_STEPS_PER_FRAME` | 8 | 每帧最多補 8 步，超過就丟積欠 |
| `GRAVITY` | −28.0 m/s² | **比真實重力（−9.81）快約 2.9 倍**，刻意做成「脆」 |
| `UNIT_SCALE` | 1.0 | 1 世界單位 = 1 公尺 |

### 螺旋死亡守衛

```c
if (accumulator > SIM_DT * MAX_SIM_STEPS_PER_FRAME) accumulator = 0.0f;
```

一帧最多追 `8 × 8.333 ms = 66.7 ms` 的模擬。追不上就丟掉，而不是繼續負債。

**診斷**：`g_engine.perf.sim_steps` 持續 == 8 表示機器跟不上。

---

## 2. 容量上限

`engine/bp_common.h` — **啟動後零配置**，這些是硬上限。

| 常數 | 值 | 滿了會怎樣 |
|---|---|---|
| `MAX_ENEMIES` | 128 | 生成回傳無效 handle + 警告 |
| `MAX_PROJECTILES` | 256 | 該發子彈不生成 |
| `MAX_HITSCAN_TRACERS` | 32 | 最舊的被覆寫 |
| `MAX_PARTICLES` | 4096 | 最舊的被覆寫 |
| `MAX_DECALS` | 256 | 最舊的被覆寫 |
| `MAX_PICKUPS` | 128 | 掉落物不生成 |
| `MAX_DAMAGE_NUMBERS` | 128 | 最舊的被覆寫 |
| `MAX_SFX_VOICES` | 32 | 優先級低的被搶 |
| `MAX_MAP_CELLS` | 128 × 128 | 碰撞網格（**目前未使用**，預留） |
| `MAX_MAP_SPAWNS` | 64 | 生成點上限 |
| `MAX_MAP_ARENAS` | 24 | 競技場幾何盒上限 |
| `MAX_WEAPON_DEFS` | 12 | 武器定義上限（目前用 5） |
| `MAX_ENEMY_DEFS` | 16 | 敵人原型上限（目前用 4） |
| `MAX_LEVELS` | 8 | 關卡表上限（目前用 3） |
| `MAX_MENU_ITEMS` | 24 | 單一選單的列數上限 |
| `MAX_LOG_LINES` | 256 | 環形日誌（192 字元／行） |
| `MAX_SAVE_SLOTS` | 4 | 存檔槽（**目前未使用**，只有 1 個 profile） |

**自我測試會驗證**：`particle pool live count N of 4096`。

---

## 3. 玩家移動

`game/bp_player.c`

### 速度

| 常數 | 值 | 說明 |
|---|---|---|
| `PLAYER_SPEED_GROUND` | 8.2 m/s | 走路 |
| `PLAYER_SPEED_SPRINT` | 11.5 m/s | 衝刺（+40%） |
| `PLAYER_SPEED_CROUCH` | 3.4 m/s | 蹲下 |
| `PLAYER_SPEED_AIR` | 1.35 | **空中控制加速度上限的倍率，不是速度上限** |
| `PLAYER_SPEED_HARD_CAP` | 24.0 m/s | 防止圓弧走位發散到無限 |

### 加速度與摩擦

| 常數 | 值 | 說明 |
|---|---|---|
| `PLAYER_ACCEL_GROUND` | 72.0 m/s² | **0 → 滿速約 0.11 秒。刻意做成「立即」** |
| `PLAYER_ACCEL_AIR` | 16.0 m/s² | 搭配 1.35× 投影上限 → 可空氣加速 |
| `PLAYER_FRICTION_GROUND` | 13.0 | 移動中 |
| `PLAYER_FRICTION_STOP` | 17.0 | **沒按鍵時更高 → 停得脆** |

摩擦力會被**材質的 `friction` 欄位除**：`grate` 是 0.88（滑）、`crate` 是 1.05（澀）。

### 跳躍

| 常數 | 值 | 說明 |
|---|---|---|
| `PLAYER_JUMP_VELOCITY` | 8.6 m/s | 初速 |
| 推導：站立跳高度 | **1.32 m** | `v²/(2g) = 8.6²/(2×28)` |
| `PLAYER_COYOTE_TIME` | 0.12 s | 離開平台後仍可跳 |
| `PLAYER_JUMP_BUFFER` | 0.14 s | 落地前提前按跳仍生效 |
| 可變高度 | 7.5/s 切斷 | 放開按鍵時把 `vel.y` 以 7.5/s 的速率切掉 |

### ⚠️ 高度可達性預算（關卡設計必读）

| 高度差 | 可達方式 |
|---|---|
| ≤ 0.55 m | 直接走上去 |
| ≤ 1.10 m | 站立跳 |
| ≤ 1.50 m | **僅**衝刺跳 |
| > 1.50 m | 必須有樓梯／斜坡／升降機 |

**手寫的關卡高度差曾經默默破壞可達性**（0.72 m 的落差讓平台永久不可達），而且完全沒有錯誤訊息。

### 蹲下

| 常數 | 值 |
|---|---|
| 碰撞高度 | 1.85 → 1.15 m |
| 眼睛高度 | 1.62 → 0.95 m（`PLAYER_EYE_CROUCH`） |
| 過渡速率 | 12.0/s（0→1 混合） |
| `PLAYER_HEAD_CLEAR` | 0.12 m — **站起來需要頭部空間**，沒空間就一直蹲著 |

### 腳步與環境

| 常數 | 值 |
|---|---|
| `PLAYER_STEP_LEN_RUN` | 2.35 m／步 |
| `PLAYER_STEP_LEN_CROUCH` | 1.45 m／步 |
| `PLAYER_HAZARD_TICK` | 0.5 s（危險區傷害間隔） |

### 掉落傷害

| 常數 | 值 |
|---|---|
| `PLAYER_FALL_SAFE_SPEED` | 14.0 m/s — 以下**無傷** |
| `PLAYER_FALL_DMG_PER_MS` | 3.2 點／每 m/s 超額 |
| `PLAYER_LAND_SHAKE_MAX` | 0.55 |

> ⚠️ **護甲不吸收掉落傷害。** 這是刻意的：boomer shooter 裡「從高處跳下來」必須是有代價的戰術決定，不能被護甲抵消。自我測試的 `fall_speed_peak` 欄位記錄每次 run 的峰值。

### 視野

| 常數 | 值 |
|---|---|
| 基礎 FOV | 存檔 `fov`（70–120，預設 95） |
| `PLAYER_FOV_MAX_BONUS` | +9.0° | 

FOV 隨速度增加（速度感），上限 +9°。

---

## 4. 玩家戰鬥與生存

| 常數 | 值 | 說明 |
|---|---|---|
| 起始血量 | 100 | |
| 起始護甲 | 0 | 上限 100 |
| 護甲吸收 | 55% | 敵人護甲也是這個比例 |
| `PLAYER_INVULN_SPAWN` | 1.1 s | 出生無敵 |
| `PLAYER_DEATH_DURATION` | 1.75 s | dying → dead，死亡運鏡窗口 |
| `PLAYER_LOW_HP_THRESH` | 28.0 | 低血量心跳音門檻 |
| `PLAYER_LOW_HP_INTERVAL` | 1.15 s | |

### 連殺

| 常數 | 值 |
|---|---|
| `PLAYER_STREAK_WINDOW` | 3.5 s |
| `PLAYER_STREAK_MAX` | 8 |
| 倍率公式 | `1 + min(streak, 8) / 4` → **最高 3.0×** |

3.5 秒內沒有擊殺就斷連。

### 掉落物拾取

| 參數 | 值 |
|---|---|
| 水平半徑 | 0.95 m |
| 垂直半徑 | 1.60 m |
| 規則 | **只在有用時拾取**（滿血時走過血包不會消耗它） |

---

## 5. 武器表

`assets/cfg/weapons.cfg`（184 行）。**所有數值都可以在不重編的情況下修改。**

### 總表

| | **M9 SIDEARM** | **SCATTER-12** | **VK-7 VIPER** | **LANCE-9 RAILGUN** | **ANVIL RPG** |
|---|---|---|---|---|---|
| id | `pistol` | `shotgun` | `smg` | `rail` | `rocket` |
| slot | 0 | 1 | 2 | 3 | 4 |
| 類別 | hitscan | hitscan | hitscan | hitscan | **projectile** |
| 傷害 | 21 | 12 × 10 發 | 10 | 118 | 96 直擊 |
| 全中傷害 | 21 | **120** | 10 | 118 | 96 + 128 爆炸 |
| 射速 | 5.2/s | 1.15/s | 12.5/s | 0.82/s | 0.85/s |
| DPS | 109 | 138 | 125 | 97 | 82 + 爆炸 |
| 自動 | ✗ | ✗ | ✅ | ✗ | ✗ |
| 彈匣 | 14 | 6 | 36 | 4 | 3 |
| 後備上限 | 999 | 72 | 288 | 32 | 24 |
| 換彈 | 1.05 s | 2.05 s | 1.55 s | 2.40 s | 2.75 s |
| 切槍 | 0.32 s | 0.46 s | 0.36 s | 0.58 s | 0.62 s |
| 射程 | 180 m | 55 m | 140 m | **400 m** | 220 m |
| 穿透 | 0 | 0 | 0 | **4** | 0 |
| 爆頭倍率 | **1.6** | 1.4 | 1.5 | 1.2 | 1.0 |
| 擊退 | 1.2 | 4.5 | 0.7 | 7.0 | 11.0 |
| 彈藥包 | 14 | 12 | 48 | 6 | 4 |
| 顏色 | `#d8dee9` | `#f0d05c` | `#5ce2c4` | `#7fd8ff` | `#ff8a20` |

### 散布（度，錐半角）

| | hip | ads | move 倍率 |
|---|---|---|---|
| pistol | 0.70 | 0.15 | ×1.9 |
| shotgun | 4.60 | 3.20 | ×1.5 |
| smg | 2.10 | 0.85 | ×1.4 |
| rail | **0.02** | **0.00** | ×0.35 |
| rocket | 0.25 | 0.10 | ×0.40 |

**實際散布公式**：

```
spread = spread_hip
       × (1 + moveFactor × spread_move + airFactor × spread_air)
       × (1 − ads × 0.65)
```

- `moveFactor` = 當前速度 / 頂速（0..1）
- `airFactor` = 離地時 1，著地時 0
- `ads` = 瞄準混合（0..1）

**錐內是均勻分佈**（area-uniform），不是高斯。理由：均勻分佈的「最壞情況」是可預測的，玩家能學會武器的極限；高斯的長尾會讓「明明瞄準了卻完全沒中」變成無法學習的隨機。

`pellets > 1` 時**每顆彈丸各取樣一次錐**，但每次扣扳機只畫**一條**曳光彈（畫 10 條會變成一坨白）。

### 後座力

| | pitch | yaw | kick（視模型位移） | shake（畫面震動） |
|---|---|---|---|---|
| pistol | 1.15 | 0.30 | 0.055 | 0.10 |
| shotgun | 5.20 | 1.10 | 0.200 | 0.42 |
| smg | 0.62 | 0.44 | 0.038 | 0.07 |
| rail | 3.40 | 0.25 | 0.260 | 0.52 |
| rocket | 4.20 | 0.60 | 0.300 | 0.60 |

### 曳光彈

| | width | life |
|---|---|---|
| pistol | 0.020 | 0.055 s |
| shotgun | 0.030 | 0.045 s |
| smg | 0.018 | 0.045 s |
| rail | **0.070** | **0.220 s** |
| rocket | 0.0（用彈體模型） | 0.0 |

rail 的曳光彈刻意又粗又久（0.22 s）——它是這把武器的**身份識別**。

### RPG 爆炸參數

| 鍵 | 值 | 說明 |
|---|---|---|
| `proj_speed` | 34.0 m/s | |
| `proj_radius` | 0.16 m | 碰撞半徑 |
| `proj_gravity` | 0.0 | **無重力下墜** |
| `blast_radius` | 5.2 m | |
| `blast_damage` | 128 | |
| `blast_self` | **0.45** | 自己被炸到的傷害**比例** |
| `blast_knockback` | 12.0 | 擊退**完整套用**，包含對自己 |

> ⚠️ **`blast_self` 與 `blast_knockback` 的不對稱正是火箭跳能用的原因。**
> 自己受 45% 傷害（57.6 點，致命但可存活），但受 100% 擊退。
> **如果把它們設成一致，火箭跳就會變成自殺。** 改這兩個值時請一起想。

### 音效索引

`sfx_fire` / `sfx_reload` 是 `bp_sfx_id` 的**數值**。

| 武器 | sfx_fire | 對應 |
|---|---|---|
| pistol | 0 | `SFX_PISTOL_FIRE` |
| shotgun | 2 | `SFX_SHOTGUN_FIRE` |
| smg | 4 | `SFX_SMG_FIRE` |
| rail | 6 | `SFX_RAIL_FIRE` |
| rocket | 7 | `SFX_ROCKET_FIRE` |

全部武器的 `sfx_reload = 8`（`SFX_RELOAD_OUT`）。

> ⚠️ **在 `bp_sfx_id` 中間插入新值會讓這些數值全部指向錯誤的音效**，而且沒有任何錯誤訊息。見 DEVELOPMENT §12。

### 關卡開始的裝備

**只給手槍 + 2 個彈匣。** 其他武器必須從關卡內的 WEAPON 觸發器或敵人掉落取得。

`bp_weapons_reset_loadout(bool fullArsenal)`：
- `false` — 關卡開始（手槍 + 2 彈匣）
- `true` — 除錯／自我測試（全武器）

---

## 6. 敵人表

`assets/cfg/enemies.cfg`（143 行）。

### 總表

| | **SCRAPHOUND** | **RIPPER** | **SIEGEBREAKER** | **SENTRY NODE** |
|---|---|---|---|---|
| id | `grunt` | `hound` | `brute` | `turret` |
| 血量 | 62 | 38 | **340** | 120 |
| 護甲 | 0 | 0 | 60 | 45 |
| 有效血量 | 62 | 38 | **400**（護甲吸收 55%） | 165 |
| 走路速度 | 3.4 | 5.2 | 2.1 | — |
| 跑步速度 | 6.6 | **11.4** | 4.3 | — |
| 半徑 | 0.46 | 0.40 | 0.92 | 0.52 |
| 高度 | 1.82 | 1.05 | **2.85** | 1.55 |
| 縮放 | 1.00 | 1.00 | **1.85** | 1.00 |
| 視覺距離 | 62 m | 70 m | 75 m | **85 m** |
| 聽覺距離 | 26 m | 34 m | 40 m | **0（聾）** |
| 攻擊距離 | 2.30 m | 1.95 m | 3.40 m | **55 m** |
| 攻擊傷害 | 13 | 9 | 26 | 8（彈體） |
| 攻擊冷卻 | 0.85 s | 0.52 s | 1.35 s | **0.26 s** |
| 痛覺機率 | 0.55 | 0.30 | **0.12** | **0.00** |
| 分數 | 100 | 75 | **500** | 250 |
| 側移 | 0.35 | **0.75** | 0.15 | 0.0 |
| 侵略性 | 0.80 | **1.00** | 0.95 | 1.00 |
| 擊退抗性 | 1.00 | 1.00 | **0.22（抗 78%）** | — |
| 靜止 | ✗ | ✗ | ✗ | ✅ |
| 遠程 | ✗ | ✗ | ✗ | ✅ |
| 身體模型 | `torso` | `hound` | `brute` | `turret` |
| 有頭部盒 | ✅ | **❌** | ✅ | ✅ |

### 顏色

| | body | trim | eye |
|---|---|---|---|
| grunt | `#a85c4c` | `#76403a` | `#ffcf3a`（琥珀） |
| hound | `#5c6846` | `#3a4230` | `#ff4a3a`（紅） |
| brute | `#7a443e` | `#4a2a28` | `#ff2a2a`（紅） |
| turret | `#80868e` | `#3a3c44` | `#ff3a2a`（紅） |

**眼睛顏色是刻意的識別設計**：grunt 的琥珀眼在暗處可見，讓玩家在拐角就能讀出「那邊有一隻」。

### 掉落機率

| | health | ammo | armor |
|---|---|---|---|
| grunt | 0.18 | 0.42 | 0.05 |
| hound | 0.10 | 0.30 | 0.02 |
| brute | **0.55** | **0.85** | **0.35** |
| turret | 0.25 | 0.70 | 0.15 |

brute 是**移動補給站**：高血量、高掉落。這是「優先打大目標」的獎勵機制。

掉落機率會被**難度的 `resupply` 倍率**再乘一次（BREACH = 0.00 → 完全無掉落）。

### 砲塔彈體

| 鍵 | 值 |
|---|---|
| `proj_speed` | 30.0 m/s |
| `proj_radius` | 0.11 m |
| `proj_damage` | 8 |

`static = true` → 不移動、不受擊退、不需要尋路。

### AI 常數（硬編碼在 `bp_enemies.c`）

| 參數 | 值 | 說明 |
|---|---|---|
| 感知視錐 | **140°** | 還要通過視線檢查 |
| 最後已知位置記憶 | **3.5 s** | 之後放棄並回巡邏 |
| 護甲吸收 | 55% | 先扣護甲 |
| 硬直條件 | `dmg > health × 0.06` **且** `rand < pain_chance` | 兩個條件都要 |
| 死亡動畫 | 1.55 s | 下沉 + 淡出 |
| 同伴警報 | `bp_enemy_alert_nearby(pos, radius)` | 首次發現玩家時呼叫 |

**BREACH 難度下敵人不會硬直**（`pain_chance` 被強制為 0）。

---

## 7. 波次導演

`game/bp_waves.c`

### 狀態機

```
WAVE_IDLE → WAVE_INTRO → WAVE_ACTIVE → WAVE_CLEARING → WAVE_INTERMISSION → WAVE_ACTIVE(下一波)
                                              ↓
                                       WAVE_COMPLETE（全部波次結束）
```

### 計時

| 常數 | 值 | 說明 |
|---|---|---|
| `WAVE_INTRO_TIME` | **2.60 s** | 橫幅顯示，**不生成敵人**。這是「波次開始可讀」而不是「環境噪音」的原因 |
| `WAVE_INTERMISSION_MIN` | 3.20 s | |
| `WAVE_INTERMISSION_MAX` | 5.60 s | 隨機區間，避免節奏變成節拍器 |

### 預算（敵人總數）

| 常數 | 值 |
|---|---|
| `WAVE_BASE_BUDGET` | 10（第 1 波，VETERAN） |
| `WAVE_BUDGET_GROWTH` | +4／波 |
| `WAVE_BUDGET_MAX` | **46（硬上限）** |

推導：

| 波次 | 預算 |
|---|---|
| 1 | 10 |
| 2 | 14 |
| 3 | 18 |
| 4 | 22 |
| 5 | 26 |
| 6 | 30 |
| 7 | 34 |
| 8 | 38 |
| 9 | 42 |
| 10 | 46（封頂） |

> `WAVE_BUDGET_MAX = 46` 的原始註解：
> **「hard ceiling: the arena simply cannot hold more than this without it becoming a collision-resolution problem, not a fight」**
>
> 這是把**設計限制寫進程式碼常數**的例子。超過 46 個敵人，128 的池子還夠，但 68×68 m 的競技場裡敵人會開始互相推擠，戰鬥就退化成了碰撞解算問題。

### 同時在場

| 常數 | 值 | 說明 |
|---|---|---|
| `ONSCREEN_MIN` | 7 | |
| `ONSCREEN_MAX` | **20** | **同時也是效能預算** |

自適應生成：

```c
float load = alive_count / max_onscreen;
spawn_timer = spawn_interval × (1.0 + load × 0.8);
```

**當在場敵人已經很多時，生成變慢**——而不是排隊一堆會被在場上限拒絕的生成請求。

### 生成間隔

| 常數 | 值 |
|---|---|
| `SPAWN_INTERVAL_MIN` | 0.30 s |
| `SPAWN_INTERVAL_MAX` | 1.50 s |
| `SPAWN_OPENING_BURST` | **4**（波次開始時同時抵達） |

### 生成點選擇

| 常數 | 值 | 說明 |
|---|---|---|
| `SPAWN_ATTEMPTS` | 9 | 嘗試次數，用完就降級到隨機位置 |
| `SPAWN_MIN_DIST_PLAYER` | 9.0 m | **不能在玩家臉上生成** |
| `SPAWN_HIDE_DIST` | 16.0 m | 超過此距離**跳過**視錐檢查（省效能） |
| `SPAWN_VIEW_COS` | 0.35 | ≈ **69° 視線外**才生成 |
| `SPAWN_TURRET_MIN_DIST` | 14.0 m | 砲塔需要視線空間才公平 |

**生成規則是「玩家看不到、但離玩家不太遠」**。在玩家視野內生成敵人是 boomer shooter 的頭號不公平。

### 技術橡皮筋

| 常數 | 值 | 說明 |
|---|---|---|
| `SKILL_LAMBDA` | 0.9 | 技術評估的指數平滑 |
| `SKILL_CLAMP` | **0.18** | 橡皮筋影響上限 **±18%** |

`update_skill(dt)` 追蹤玩家表現（擊殺速率、受傷量），在 ±18% 內調整生成壓力。

**±18% 是刻意的上限**：橡皮筋太強會讓玩家感覺「系統在演我」，完全沒有技術成長的回饋。18% 是可感知但不會破壞作者意圖的量。

### 完美波次

| 常數 | 值 |
|---|---|
| `PERFECT_WAVE_BONUS` | **750 分** |

一整波**完全沒受傷** → +750。追蹤在 `g_waves.damage_taken_this_wave` 與 `g_waves.perfect_waves`。

### 軟鎖防護

```c
case WAVE_CLEARING:
    if (alive_count <= 0) finish_wave();
    else if (state_time > 120.0f) {
        BP_LOGW("waves: %d enemies unreachable after 120s -- forcing wave clear", ...);
        bp_enemies_clear();
        finish_wave();
    }
```

**120 秒強制清除。** 如果最後一個敵人不可達（被移動件埋住、掉出競技場），波次必須還是會結束，否則玩家被軟鎖在出口之外且無任何回復手段。

---

## 8. 難度倍率

`game/bp_waves.c` 的 `g_difficulties[]`

| 難度 | 說明文字 | 敵血 | 敵傷 | 生成壓力 | 分數 | 補給 |
|---|---|---|---|---|---|---|
| **RECRUIT** | "Learning the movement. Generous resupply." | ×0.70 | ×0.55 | ×0.72 | ×0.70 | **×1.00** |
| **VETERAN** | "The intended experience." | ×1.00 | ×1.00 | ×1.00 | ×1.00 | ×0.60 |
| **NIGHTMARE** | "Faster pressure, heavier hits, less help." | ×1.30 | ×1.45 | ×1.26 | ×1.60 | ×0.35 |
| **BREACH** | "No resupply. Enemies do not stagger." | ×1.55 | ×1.85 | ×1.45 | **×2.50** | **×0.00** |

### 各倍率的確切作用點

| 欄位 | 作用 |
|---|---|
| `health` | 乘進敵人 `health` 與 `armor` |
| `damage` | 乘進所有敵人攻擊傷害（近戰與彈體） |
| `pressure` | 乘進 `WAVE_BASE_BUDGET` 與生成間隔（越高越密） |
| `score` | 乘進所有分數（擊殺、完美波次、秘密） |
| `resupply` | 乘進**敵人掉落機率**與**關卡補給觸發器的重生速率**。`0.00` = 完全無補給 |

### ⚠️ BREACH 難度的額外規則

**敵人不會硬直**（`pain_chance` 強制為 0）。

這不是倍率能表達的，是 `bp_enemies.c` 裡一條獨立的判斷。改難度表時不要漏了它。

### VETERAN 是基準

所有 `.cfg` 裡的數值都是 **VETERAN 下的絕對值**。其他難度是倍率。所以調平衡時永遠在 VETERAN 下測。

---

## 9. 關卡

`game/bp_world.c` 的 `g_levels[]` + `game/bp_levels.inc` 的幾何資料

| id | 名稱 | 副標題 | 波次 | 標準時間 | 建議難度 |
|---|---|---|---|---|---|
| `foundry` | **FOUNDRY** | Symmetrical arena. Learn the movement. | 6 | 2:30 (150s) | RECRUIT (0) |
| `slagpit` | **SLAG PIT** | Three height bands. Own the catwalk. | 8 | 3:30 (210s) | VETERAN (1) |
| `conduit` | **CONDUIT** | Tight corridors. Shotguns win. | 9 | 4:00 (240s) | NIGHTMARE (2) |

### 共同規格

| 項目 | 值 |
|---|---|
| 競技場尺寸 | **68 × 68 m**（x, z ∈ −34..34） |
| 地板 slab | y ∈ −1..0 |
| 外牆高度 | 1 m |
| 每張地圖 | 1 × `PLAYER_START`、1 × `EXIT`（**永不消耗**）、1 × `SECRET`（+500 分） |
| 補給觸發器 | HEALTH / ARMOR / AMMO / WEAPON，**定時重生** |

### FOUNDRY

| 項目 | 值 |
|---|---|
| 佈局 | 對稱 |
| 中心 | 可行走金字塔：0 → 0.55 → 1.10 → 1.65 m |
| 油桶 | 6 |
| 敵人生成點 | 13 |
| 移動件 | 1 × blast door |
| 天空 | 琥珀 |
| 實測烘焙 | 50 solids / 28 triggers / 1 mover / **12 靜態批次 / 516 三角形** |

金字塔的每一階都 ≤ 0.55 m，所以**可以一路走上去，不用跳**。這是教學關卡的核心：讓玩家在無壓力下學會「高度是可用的」。

### SLAG PIT

| 項目 | 值 |
|---|---|
| 高度帶 | 坑底 **0.0** / 平台 **2.2** / 棧道 **6.9** m |
| 坑底 | 危險格柵（`hazard` 材質，**9 dps**，每 0.5 s tick） |
| 樓梯 | 2 座塔，各 **10 階 × 0.69 m** |
| 油桶 | 9 |
| 敵人生成點 | 14 |
| 移動件 | 1 × freight lift |
| 天空 | 洋紅 |

> ⚠️ **0.69 m > 0.55 m，所以那些樓梯需要跳。** 這是刻意的：棧道應該是**要努力才能拿到**的位置，而不是免費的高地。

### CONDUIT

| 項目 | 值 |
|---|---|
| 佈局 | 緊密環形走廊 |
| 最長視線 | **< 22 m** |
| 高度 | 0.6 m 板條箱墊高 → **7.1 m 核心陽台** |
| 敵人生成點 | **15** |
| 移動件 | 1 × shutter |
| 天空 | 綠 |

「無 >22 m 視線」直接決定了武器平衡：railgun（射程 400 m）在這裡幾乎無用，shotgun（55 m 但實際有效 <15 m）統治。副標題 "Shotguns win." 是這個幾何約束的**結果**，不是裝飾。

---

## 10. 材質

`game/bp_world.c` 的 `s_materials[]` — **17 種**

```c
{ id, texture, impact_sfx, step_sfx, uv_scale, friction, gloss,
  penetrable, emissive, solid_for_movement, hazard_dps, tint }
```

| # | id | 貼圖 | 撞擊音 | uv_scale | friction | gloss | 可穿透 | 自發光 | 實體 | hazard dps |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `concrete` | WALL_CONCRETE | HIT_WALL | 2.60 | 1.00 | 0.55 | ✗ | ✗ | ✅ | 0 |
| 1 | `brick` | WALL_BRICK | HIT_WALL | 2.20 | 1.00 | 0.45 | ✗ | ✗ | ✅ | 0 |
| 2 | `metal` | WALL_METAL | **HIT_ARMOR** | 2.40 | 0.92 | 0.85 | ✗ | ✗ | ✅ | 0 |
| 3 | `panel` | WALL_PANEL | **HIT_ARMOR** | 3.00 | 0.95 | 0.80 | ✗ | ✗ | ✅ | 0 |
| 4 | `rust` | WALL_RUSTY | HIT_WALL | 2.40 | 1.00 | 0.40 | ✗ | ✗ | ✅ | 0 |
| 5 | `grate` | FLOOR_GRATE | **HIT_ARMOR** | 1.80 | **0.88** | 0.90 | ✅ | ✗ | **✗** | 0 |
| 6 | `tile` | FLOOR_TILE | HIT_WALL | 3.20 | 1.00 | 0.50 | ✗ | ✗ | ✅ | 0 |
| 7 | `hazard` | FLOOR_HAZARD | HIT_WALL | 2.00 | 1.00 | 0.50 | ✗ | ✗ | ✅ | **9.0** |
| 8 | `crate` | CRATE | HIT_WALL | 1.00 | **1.05** | 0.30 | ✗ | ✗ | ✅ | 0 |
| 9 | `barrel` | BARREL | **HIT_ARMOR** | 1.00 | 1.00 | 0.70 | ✗ | ✗ | **✗** | 0 |
| 10 | `trim` | TRIM | **HIT_ARMOR** | 1.60 | 1.00 | 0.85 | ✗ | ✗ | ✅ | 0 |
| 11 | `pillar` | PILLAR | HIT_WALL | 2.00 | 1.00 | 0.60 | ✗ | ✗ | ✅ | 0 |
| 12 | `light` | LIGHT_PANEL | **HIT_ARMOR** | 1.50 | 1.00 | 0.90 | ✗ | **✅** | ✗ | 0 |
| 13 | `screen` | SCREEN | **HIT_ARMOR** | 1.50 | 1.00 | 0.90 | ✗ | **✅** | ✗ | 0 |
| 14 | `vent` | VENT | **HIT_ARMOR** | 1.40 | 1.00 | 0.85 | ✗ | ✗ | ✅ | 0 |
| 15 | `pipe` | PIPE | **HIT_ARMOR** | 1.20 | 1.00 | 0.85 | ✗ | ✗ | ✅ | 0 |
| 16 | `field` | RING | **HIT_ARMOR** | 2.00 | 1.00 | 1.00 | ✅ | **✅** | ✗ | 0 |

`field` 的 tint 是 `{120, 220, 255}`（淡藍），其餘是白 `{255,255,255,255}`。

### 一個材質同時驅動五件事

1. **貼圖**（`texture`）
2. **撞擊音效**（`impact_sfx`）— 打金屬是 `HIT_ARMOR`，打混凝土是 `HIT_WALL`
3. **腳步音效**（`step_sfx`）
4. **摩擦力**（`friction`）— 玩家的 `FRICTION_GROUND` 會被它除
5. **UV 密度**（`uv_scale`）— 每面的 UV 以公尺計再除以它，所以 2m 與 8m 的牆貼圖密度一致

**這是刻意的統一**：加一種材質就同時得到正確的視覺**與**正確的手感與正確的聽覺。

> ⚠️ 新增材質時**每個欄位都要填**。`-Wno-missing-field-initializers` 不會警告漏填，而漏填的欄位是 0：
> - `friction = 0` → 除以零或完全無摩擦
> - `uv_scale = 0` → **除以零**
> - `solid_for_movement = false` → 玩家直接穿過去

### 假 Lambert 面光

```c
static const float s_face_light[6] = { 1.00, 0.30, 0.78, 0.62, 0.86, 0.55 };
                              /*       +Y     -Y     +X     -X     +Z     -Z   */
```

烘焙時直接乘進頂點色。沒有光源、沒有法線貼圖、沒有 shadow map——但幾何體立刻有體積感。

**頂面 1.00、底面 0.30** 是這組數值的關鍵：它模擬「光從上面來」，讓玩家能一眼讀出哪個面可以踩。

---

## 11. 計分

| 來源 | 分數 |
|---|---|
| 擊殺 SCRAPHOUND | 100 |
| 擊殺 RIPPER | 75 |
| 擊殺 SIEGEBREAKER | 500 |
| 擊殺 SENTRY NODE | 250 |
| 發現 SECRET | **500** |
| 完美波次（無傷） | **750** |

**全部乘以**：
1. 連殺倍率 `1 + min(streak, 8)/4`（最高 **3.0×**）
2. 難度的 `score` 倍率（BREACH = **2.50×**）

理論單擊最高分：SIEGEBREAKER (500) × 3.0 × 2.5 = **3750**。

### 存檔只記最佳

`bp_save_submit_run()` **只在改進時**更新 `best_score` / `best_kills` / `best_time` / `best_accuracy` / `best_wave`。一次爛 run 不會降低最佳成績。

`attempts` 與 `total_deaths` 則**每次都加**。

---

## 12. 圖形品質

`engine/bp_gfx.h`

| 品質 | 列舉值 | `resolution_scale` | 後製 | MSAA | 各向異性 |
|---|---|---|---|---|---|
| LOW | 0 | **0.50×** | 無 | 無 | 無 |
| MEDIUM | 1 | **0.75×** | 顆粒 + 暗角 | 無 | 無 |
| HIGH | 2（**預設**） | **1.00×** | 完整 | **4×** | 4× |
| ULTRA | 3 | **1.25×**（超取樣） | 完整 | 4× | 4× |

### 渲染管線

```
視窗尺寸 × resolution_scale → 離屏 RenderTexture2D
  → 3D 場景（天穹、世界、敵人、掉落物、FX、彈體）
  → 後製著色器 resolve 到 backbuffer
  → 視模型（**在 resolve 之後**，不受後製影響）
  → HUD / 選單
```

**視模型在後製之後**是刻意的：讓槍不受暗角與顆粒影響，保持銳利。這是「槍是你身体的延伸」的視覺語言。

### 剔除

| 常數 | 值 |
|---|---|
| `CULL_DISTANCE` | 170 m |

所有實體繪圖先算到 viewer 的距離，超過就跳過。搭配 `bp_world` 的 AABB 距離早退，這是「沒有空間分割也能跑 128 個敵人」的原因。

### ⚠️ MSAA 必須在 `InitWindow` 之前

```c
if (quality >= BP_QUALITY_HIGH) SetConfigFlags(FLAG_MSAA_4X_HINT);
```

raylib 在建立 GL context 時讀取旗標，**之後忽略**。所以存檔必須在建視窗之前讀好（`bp_save_boot()`）。

### ⚠️ 沒有 draw call 統計

`g_engine.perf.draw_calls` 與 `.tris` **永遠是 0**。raylib 5.5 的 `rlRenderBatch.drawCounter` 是內部的，rlgl 沒有公開存取器。**不要回報它們**，也不要試圖「修好」。

---

## 13. 調色盤

`engine/bp_assets.c` → `g_assets`

| 名稱 | RGB | 用途 |
|---|---|---|
| `pal_accent` | **(255, 138, 32)** 琥珀 | 主強調色、Logo、關鍵 UI |
| `pal_accent2` | **(92, 226, 196)** 青 | 次強調色、SMG 顏色 |
| `pal_danger` | **(232, 52, 60)** 紅 | 傷害、敵人、低血量 |
| `pal_health` | **(86, 214, 110)** 綠 | 血量條、血包 |
| `pal_armor` | **(96, 168, 255)** 藍 | 護甲條、護甲包 |
| `pal_ammo` | **(240, 208, 92)** 黃 | 彈藥、shotgun 顏色 |
| `pal_bg` | **(12, 13, 17)** | 背景 |
| `pal_panel` | **(24, 26, 33, α=220)** | 面板（半透明） |
| `pal_text` | **(236, 238, 242)** | 主要文字 |
| `pal_text_dim` | **(138, 144, 156)** | 次要文字、提示 |

**調色盤集中在 `g_assets`，HUD／粒子／關卡燈光／選單全部引用它。改一次，全遊戲的色調一起變。**

`bp_pal(index)` 提供索引式存取，用於程序化光照漸层。

---

## 14. 音效清單

`engine/bp_audio.h` — **44 種**（`SFX_COUNT`）

### 索引表（設定檔用數值引用）

| id | 名稱 | id | 名稱 |
|---|---|---|---|
| 0 | `SFX_PISTOL_FIRE` | 22 | `SFX_HOUND_ATTACK` |
| 1 | `SFX_PISTOL_DRY` | 23 | `SFX_HOUND_DIE` |
| 2 | `SFX_SHOTGUN_FIRE` | 24 | `SFX_TURRET_FIRE` |
| 3 | `SFX_SHOTGUN_PUMP` | 25 | `SFX_TURRET_DIE` |
| 4 | `SFX_SMG_FIRE` | 26 | `SFX_BRUTE_ROAR` |
| 5 | `SFX_RAIL_CHARGE` | 27 | `SFX_BRUTE_DIE` |
| 6 | `SFX_RAIL_FIRE` | 28 | `SFX_JUMP` |
| 7 | `SFX_ROCKET_FIRE` | 29 | `SFX_LAND` |
| 8 | `SFX_RELOAD_OUT` | 30 | `SFX_FOOTSTEP` |
| 9 | `SFX_RELOAD_IN` | 31 | `SFX_HURT` |
| 10 | `SFX_RELOAD_DONE` | 32 | `SFX_HEAL` |
| 11 | `SFX_WEAPON_SWAP` | 33 | `SFX_ARMOR` |
| 12 | `SFX_HIT_FLESH` | 34 | `SFX_AMMO_PICKUP` |
| 13 | `SFX_HIT_ARMOR` | 35 | `SFX_LOW_HEALTH` |
| 14 | `SFX_HIT_WALL` | 36 | `SFX_UI_MOVE` |
| 15 | `SFX_RICOCHET` | 37 | `SFX_UI_SELECT` |
| 16 | `SFX_EXPLOSION` | 38 | `SFX_UI_BACK` |
| 17 | `SFX_EXPLOSION_DISTANT` | 39 | `SFX_WAVE_START` |
| 18 | `SFX_GRUNT_SIGHT` | 40 | `SFX_WAVE_CLEAR` |
| 19 | `SFX_GRUNT_PAIN` | 41 | `SFX_DOOR` |
| 20 | `SFX_GRUNT_DIE` | 42 | `SFX_SECRET` |
| 21 | `SFX_HOUND_SIGHT` | 43 | — |

> ⚠️ 這個表的數值**必須與 `enemies.cfg` / `weapons.cfg` 的 `sfx_*` / `attack_sfx` 一致**。
> 現存引用：`attack_sfx = 19`（grunt pain）、`22`（hound attack）、`24`（turret fire）、`27`（brute die）。
>
> **在列舉中間插入新值會靜默地讓所有引用指向錯誤的音效。** 永遠加在 `SFX_COUNT` 之前。

### 音樂

| 參數 | 值 |
|---|---|
| BPM | **180** |
| 實作 | `LoadAudioStream` + `SetAudioStreamCallback`（**不是**檔案） |
| 聲部 | 鼓 / bass / lead，依強度漸進加入 |
| 格式 | float32 interleaved |

### 動態強度

| 場景 | `bp_audio_set_intensity(target, dt)` |
|---|---|
| `SCENE_TITLE` | 0.20, 1.0 |
| `SCENE_LEVEL_INTRO` | 0.30, 0.8 |
| `SCENE_PLAYING` | 依在場敵人數量上升 |
| `SCENE_PAUSED` | 0.14, 0.4 |
| `SCENE_DEAD` | 0.10, 1.2 |
| `SCENE_RESULTS` 等 | 0.16, 1.0 |

### 聲部管理

| 參數 | 值 |
|---|---|
| `MAX_SFX_VOICES` | 32 |
| alias bank | **3 聲部** — 同一音效短時間重複觸發時共用 voice 並重置包絡 |

alias bank 是「機槍聲不會變成白噪聲」的關鍵。

---

## 15. 素材清單

全部**程序化產生**，零外部二進位檔。自我測試驗證 `30/30 textures, 35/35 models, fonts ok`。

### 貼圖（30）

| 類別 | 尺寸 | 數量 | id |
|---|---|---|---|
| 建築 | **256²** | 17 | `TEX_WALL_BRICK/METAL/CONCRETE/PANEL/RUSTY`、`TEX_FLOOR_GRATE/TILE/CONCRETE/HAZARD`、`TEX_CEILING`、`TEX_TRIM`、`TEX_CRATE`、`TEX_BARREL`、`TEX_PILLAR` |
| 發光／細節 | 256² | 8 | `TEX_LIGHT_PANEL`、`TEX_SCREEN`、`TEX_VENT`、`TEX_PIPE`、`TEX_BLOOD_SPLAT`、`TEX_SCORCH`、`TEX_BULLET_HOLE`、`TEX_LOGO` |
| Billboard / FX | **128²** | 8 | `TEX_PARTICLE_SOFT/HARD`、`TEX_SPARK`、`TEX_SMOKE`、`TEX_RING`、`TEX_CROSSHAIR`、`TEX_GRADIENT`、`TEX_NOISE` |

全部含 **mipmap**，HIGH 以上開 **各向異性 4×**，過濾設為對應品質。

> ⚠️ **實測：17 張 256² 含 mipmap + 各向異性的貼圖在 llvmpipe（軟體光柵化）下上傳要 407 ms**，是整個素材生成 424 ms 的 96%。真實 GPU 上快一個數量級。這就是 `bp_assets.c` 裡有 `stage_begin/stage_end` 分組計時的原因。

### 模型（35）

| 類別 | id |
|---|---|
| 敵人部件（13） | `MDL_ENEMY_TORSO/HEAD/ARM/LEG`、`MDL_HOUND_BODY/HEAD/LEG`、`MDL_BRUTE_TORSO/ARM/LEG/HEAD`、`MDL_TURRET_BASE/BARREL` |
| 視模型部件（14） | `MDL_GUN_PISTOL_BODY/SLIDE`、`MDL_GUN_HAND`、`MDL_GUN_SHOTGUN_BODY/PUMP/BARREL`、`MDL_GUN_SMG_BODY/MAG/STOCK`、`MDL_GUN_RAIL_BODY/COIL/CORE`、`MDL_GUN_ROCKET_TUBE/SIGHT` |
| 世界道具（8） | `MDL_PICKUP_HEALTH/ARMOR/AMMO/WEAPON`、`MDL_PROJECTILE_ROCKET/PLASMA`、`MDL_BARREL`、`MDL_CRATE` |

**敵人與視模型是「部件」而不是整體**，用 per-part 矩陣繪製，所以肢體可以動畫。見 ARCHITECTURE §8 的矩陣鏈說明。

每個 `bp_model` 帶 render metadata：`origin`（socket 位置）、`half_extent`（廉價陰影／邊界）、`tint`、`valid`。

### 字型（4）

| 名稱 | 尺寸 | 用途 |
|---|---|---|
| `font_small` | 8 px | HUD 標籤、除錯 |
| `font_main` | 16 px | 內文、選單 |
| `font_big` | 32 px | 標題 |
| `font_huge` | 64 px | 大標題、擊殺計數 |

**手寫 5×7 點陣**（`bp_assets_font.inc`，113 行），每字元 `[7]` 個 byte，低 5 位元是一列。

建構路徑：`GlyphInfo[]` → `GenImageFontAtlas()` → `LoadTextureFromImage()`。`glyphPadding = 2`（防滲色），過濾 = **POINT**（保持像素銳利）。

> ⚠️ 一列寫 8 個 byte 是**硬編譯錯誤**。列數是 7。

### 生成手法

```c
static uint32_t hash2i(int x, int y, uint32_t seed);   // 整數雜湊，無浮點
static float    hashf2(int x, int y, uint32_t seed);   // [0,1)
static float    fbm(float x, float y, int octaves, float lacunarity, float gain);
static void     surface_pass(...);                      // 材質特化
static Texture2D make_tex(...);                         // Image → Texture2D
static void     build_font(...);
static Model    model_simple(...);
```

**全部用整數座標雜湊，不用 Perlin 的梯度表。** 理由：可重現（同 seed 永遠同結果，跨平台一致）、無查表、無分支。對 256² 建築貼圖，品質差異肉眼不可見。

> ⚠️ `img_to_texture` 必須**先**把 `im->px` 設為 NULL **再** `UnloadImage(img)`，否則 double-free。

---

## 16. 設定檔語法

`engine/bp_config.c`（267 行）— **存檔與武器／敵人設定檔共用同一套語法**。

### 語法

```
# 註解（# 或 ; 開頭）
                                    ← 空行忽略

section_type section_id             ← 區塊標頭（無括號）
    key = value                     ← 一個鍵值一行
    another_key = another value
end                                 ← 區塊結束
```

- **鍵大小寫不敏感**
- **值保留原始大小寫**
- 未知鍵**被忽略**
- 缺失鍵**降級到內建預設值**
- 檔案不存在**只印警告，不讓開機失敗**

### 型別化 getter

```c
int     bp_cfg_int   (const bp_config *cfg, int fallback);
float   bp_cfg_float (const bp_config *cfg, float fallback);
bool    bp_cfg_bool  (const bp_config *cfg, bool fallback);
const char *bp_cfg_str(const bp_config *cfg, const char *fallback);
Color   bp_cfg_color (const bp_config *cfg, Color fallback);      // #RRGGBB
Vector3 bp_cfg_vec3  (const bp_config *cfg, Vector3 fallback);    // "x y z"
```

**每個 getter 都吃 fallback。** 這是「部分損壞的檔案會優雅降級而不是炸掉」的機制。

### 遍歷 API

```c
bool bp_config_load(bp_config *cfg, const char *path);
bool bp_config_load_mem(bp_config *cfg, const char *data, int length);  // 嵌入用
void bp_config_rewind(bp_config *cfg);
bool bp_config_next_section(bp_config *cfg);
bool bp_config_find_section(bp_config *cfg, const char *type, const char *id);
const char *bp_config_section_type(const bp_config *cfg);
const char *bp_config_section_id(const bp_config *cfg);
bool bp_config_next_key(bp_config *cfg);
```

### 實作方式：預先切分行區間

解析器**不做**游標 push-back。它在載入時把整個檔案切成「行區間」陣列，然後 getter 在區間上掃描。

> ⚠️ **第一版用游標 push-back + 函式區域 `static char saved_id`**，非常脆弱（重新進入會靜默地讀到上一次的 id）。**已棄用，不要恢復。**

### 現有設定檔

| 檔案 | 行數 | 區塊 |
|---|---|---|
| `assets/cfg/weapons.cfg` | 184 | `weapon pistol/shotgun/smg/rail/rocket`（34 鍵 × 5） |
| `assets/cfg/enemies.cfg` | 143 | `enemy grunt/hound/brute/turret`（28 鍵 × 4） |

> ⚠️ **放錯檔案是完全靜默的。** 敵人區塊曾被寫進 `weapons.cfg`，而載入器讀 `enemies.cfg`。失敗路徑只印警告 → **沒有可見錯誤，只是數值沒生效**。
> 驗證方式：看啟動日誌的 `weapons: 5/5 definitions patched` 與 `enemies: 4/4 archetypes patched`。

> ⚠️ **用 regex 審計載入器程式碼會產生自信但錯誤的報告。** dispatch 的真實形狀是 `else if (!strcmp(k, "key"))`，不是 `case "key":`。

---

## 17. 存檔格式

`game/bp_save.c`（634 行）。`SAVE_VERSION = 3`。

### 檔案位置

| 平台 | 路徑 |
|---|---|
| 桌面 | `$XDG_DATA_HOME/BreachPoint/` 或 `~/.local/share/BreachPoint/` |
| Web | `/bpdata/`（emscripten IDBFS → IndexedDB） |

兩個檔案：`settings.cfg` 與 `progress.cfg`。

實測桌面寫入：`settings.cfg` = **4611 bytes / 163 行**。

### `settings.cfg`

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
    crt_filter         = false
    fullscreen         = false
    window_w           = 1280
    window_h           = 720

    vol_master         = 1.0000
    vol_sfx            = 1.0000
    vol_music          = 0.7000

    sensitivity        = 0.1300
    invert_y           = false

    hud_visible        = true
    hud_minimal        = false
    show_fps           = false

    difficulty         = 1
    last_level         = 0

    # key bindings: bind_<action>_<slot> = raylib key code
    bind_0_0           = 87
    bind_0_1           = 265
    ...
```

### `progress.cfg`

```
totals all
    total_runs         = 12
    total_kills        = 843
    total_deaths       = 5
    total_play_time    = 1834.2
    highest_streak     = 11
    best_score         = 48200

level 0-0                 ← <level>-<difficulty>
    completed          = true
    secret_found       = false
    best_score         = 41200
    best_kills         = 187
    best_time          = 143.8
    best_accuracy      = 0.4120
    best_wave          = 6
    attempts           = 3
    perfect_waves      = 2
    total_deaths       = 1

level 0-1
    ...
```

`SAVE_MAX_LEVELS = 16`，難度固定 4 → 最多 64 個 `level` 區塊。

### 序列化：一個 buffer，一次寫入

```c
n += snprintf(s_buf + n, SAVE_BUF_SIZE - n, "    %-18s = %d\n", key, val);
```

**理由**：
1. 避免幾百次小 write 的 syscall 開銷
2. 保證「寫出的內容」與「buffer 裡的內容」完全一致
3. 有 `SAVE_BUF_SIZE - 512` 的剩餘空間檢查，滿了印警告而不是溢位

### 原子寫入

```
寫到 <path>.tmp → fflush → remove(path) → rename(tmp, path)
```

`rename` 在同一檔案系統上是原子的，所以舊檔與新檔之間**沒有中間狀態**。寫到一半時斷電不會毀掉存檔。

### 版本遷移規則

| 檔案版本 | 行為 |
|---|---|
| **>** `SAVE_VERSION` | **保留檔案不動**，用預設值執行，印警告。（不覆寫一個比我們新的存檔） |
| **<** `SAVE_VERSION` | 遷移，然後重寫 |
| 損壞／不存在 | 預設值 + 警告。**絕不讓開機失敗** |

```
[WARN ] save: settings.cfg is version 4, this build understands 3 -- using defaults
[WARN ] config: '/home/user/.local/share/BreachPoint/progress.cfg' not found
```

### Web 持久化

```c
void bp_save_sync_platform(void);   // web: FS.syncfs(false, cb); desktop: no-op
```

Web 上每次寫入後**必須** `FS.syncfs`，否則資料留在 MEMFS，重新整理就消失。

### 解鎖規則

| 規則 | 說明 |
|---|---|
| 關卡解鎖 | 關卡 N 在 N−1 於**任何難度**完成後解鎖 |
| 最佳成績 | **只在改進時**更新，不會因爛 run 降低 |
| `attempts` / `total_deaths` | 每次都加 |
| 完成度 | `已完成的 (關卡 × 難度) 組合 / 總組合數` |

### ⚠️ `-Wformat-truncation`

`snprintf("%s/settings.cfg", dir)` 當 `dir` 與目的地一樣大時會觸發警告。修法：`char dir[200]` → `char path[256]`。

---

## 18. 預設按鍵綁定

`engine/bp_input.c` — **27 個動作**

| 動作 | 顯示名稱 | 槽 0 | 槽 1 | 手把按鈕 | 手把軸 |
|---|---|---|---|---|---|
| `ACT_MOVE_FORWARD` | Move Forward | `W` | `↑` | — | LEFT_Y (−1) |
| `ACT_MOVE_BACK` | Move Back | `S` | `↓` | — | LEFT_Y (+1) |
| `ACT_MOVE_LEFT` | Strafe Left | `A` | — | — | LEFT_X (−1) |
| `ACT_MOVE_RIGHT` | Strafe Right | `D` | — | — | LEFT_X (+1) |
| `ACT_JUMP` | Jump | `Space` | — | RIGHT_FACE_DOWN | — |
| `ACT_CROUCH` | Crouch | `L-Ctrl` | `C` | RIGHT_FACE_RIGHT | — |
| `ACT_SPRINT` | Sprint | `L-Shift` | — | LEFT_TRIGGER_1 | LEFT_Y (−1) |
| `ACT_FIRE` | Fire | **滑鼠左** | — | RIGHT_TRIGGER_1 | — |
| `ACT_ALT_FIRE` | Alt Fire | **滑鼠右** | — | LEFT_TRIGGER_1 | — |
| `ACT_RELOAD` | Reload | `R` | — | RIGHT_FACE_LEFT | — |
| `ACT_WEAPON_1..5` | Weapon 1..5 | `1`–`5` | — | — | — |
| `ACT_WEAPON_NEXT` | Next Weapon | `E` | **滑鼠側鍵** | LEFT_FACE_RIGHT | — |
| `ACT_WEAPON_PREV` | Prev Weapon | `Q` | — | LEFT_FACE_LEFT | — |
| `ACT_USE` | Use | `F` | — | RIGHT_FACE_UP | — |
| `ACT_PAUSE` | Pause | `Esc` | `P` | MIDDLE_RIGHT | — |
| `ACT_SCOREBOARD` | Scoreboard | `Tab` | — | — | — |
| `ACT_DEBUG_OVERLAY` | Debug Overlay | `F3` | `` ` `` | — | — |
| `ACT_MENU_UP` | Menu Up | `W` | `↑` | LEFT_FACE_UP | LEFT_Y (−1) |
| `ACT_MENU_DOWN` | Menu Down | `S` | `↓` | LEFT_FACE_DOWN | LEFT_Y (+1) |
| `ACT_MENU_LEFT` | Menu Left | `A` | `←` | LEFT_FACE_LEFT | LEFT_X (−1) |
| `ACT_MENU_RIGHT` | Menu Right | `D` | `→` | LEFT_FACE_RIGHT | LEFT_X (+1) |
| `ACT_MENU_ACCEPT` | Accept | `Enter` | `Space` | RIGHT_FACE_DOWN | — |
| `ACT_MENU_BACK` | Back | `Esc` | `Backspace` | MIDDLE_RIGHT | — |

啟動日誌會逐行印出全部綁定（DEBUG 級）：

```
[INFO ] input: 27 actions bound
```

### ⚠️ 滑鼠按鍵的編碼

```c
#define BP_BIND_MOUSE_BASE   4096
#define BP_BIND_FROM_MOUSE(mb)  (BP_BIND_MOUSE_BASE + (mb))
#define BP_BIND_IS_MOUSE(code)  ((code) >= BP_BIND_MOUSE_BASE)
#define BP_BIND_INDEX(code)     ((code) - BP_BIND_MOUSE_BASE)
```

**因為 `MOUSE_BUTTON_LEFT == 0`，而 0 是我們的「未綁定」哨兵值。** 而且 `MouseButton (0..6)` 與 `KeyboardKey (32..)` 的編碼空間重疊：

```
MOUSE_BUTTON_FORWARD == 5 == KEY_E
MOUSE_BUTTON_BACK    == 6 == KEY_F
```

> ⚠️ **範圍測試 `code >= MOUSE_BUTTON_LEFT && code <= 10` 會靜默匹配方向鍵與 Ctrl 鍵。** 這**曾經是已上線的 bug**。

### 存檔格式

`bind_<action>_<slot> = <raylib key code>`，例如 `bind_0_0 = 87`（`KEY_W`）。

滑鼠綁定存為 `4096 + button`，例如滑鼠左 = `4096`。

**舊存檔缺少新動作的鍵時，該動作自動保留預設綁定**（規則：stored code 全為 0 → 用預設）。所以新增動作**不需要版本遷移**。

---

## 19. 場景與計時

`game/bp_game.c`

### 場景列舉

```c
SCENE_BOOT = 0        // 開機帧
SCENE_TITLE           // 主選單 + 即時漂移相機
SCENE_LEVEL_INTRO     // 區域名稱卡，世界已載入，玩家靜止
SCENE_PLAYING         // 遊戲本體
SCENE_PAUSED          // 模擬凍結，選單活著
SCENE_DEAD            // 死亡運鏡 → 結算
SCENE_RESULTS         // 單次 run 結算
SCENE_LEVEL_COMPLETE  // 抵達出口：統計 → 下一區或選單
SCENE_GAME_COMPLETE   // 全部通關
```

### 計时常數

| 常數 | 值 | 說明 |
|---|---|---|
| `INTRO_CARD_TIME` | **3.0 s** | 區域名稱卡。**開火或 Accept 可跳過**，但跳過不能跳過波次開始 |
| `DEAD_TO_RESULTS` | **2.2 s** | 死亡運鏡：慢速漂移 + 紅暗角，然後自動提交並開 RESULTS |

**2.2 秒的死亡延遲是刻意的**：立刻切到結算畫面會讓死亡失去重量。

### 吸引模式相機

`SCENE_TITLE` / `SCENE_RESULTS` / `SCENE_LEVEL_COMPLETE` / `SCENE_GAME_COMPLETE` 的相機：

| 參數 | 值 |
|---|---|
| yaw 速率 | **6.5°/s** |
| 半徑 | **26 ± 6 m** |
| pitch | 由高度推導 |

**走真的 `bp_camera_simulate()`**，不是自己算一個 `Camera3D`。這樣它自動繼承畫面震動、FOV 呼吸、後座力衰減——選單背後的畫面與實際遊玩用的是同一套相機程式碼。

### 開機階段（16）

```
BOOT_LOG INPUT ASSETS AUDIO GFX FX WORLD WEAPONS ENEMIES PLAYER
WAVES SAVE UI HUD MENU LEVEL
```

**每帧一個階段**，所以：
- 載入進度條是真的（`boot_progress = stage / 16`）
- 視窗不會被標記為「未回應」
- 失敗可以從最後一行日誌定位

實測：**0.22–0.27 秒完成全部 16 個階段**（llvmpipe）。

### `SCENE_PAUSED` 凍結**相機**

暫停時讓相機繼續軌道會讓畫面看起來還在跑。「暫停」的定義就是模擬**與視角**都停止。

### 關卡開始的裝備

**只給手槍 + 2 個彈匣。** 其他武器要從關卡內的 WEAPON 觸發器或敵人掉落取得。

重啟關卡**保留**該 run 的分數與擊殺數。

`bp_game_submit_run()` 是**冪等**的（`g_game.submitted` 旗標）——死亡路徑與通關路徑都可能觸發它，不能送兩次。
