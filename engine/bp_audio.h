/******************************************************************************************
 * bp_audio.h -- 100% 由程序合成（procedurally synthesised）的音訊。
 *
 * 為什麼不使用 .ogg / .wav 資產？
 *   1. Web/WASM 版每個位元組都要付出兩次代價：下載體積「與」解碼時間。整套武器音效在啟動
 *      時合成，只佔約 400 KB 記憶體，下載量是 0 KB。
 *   2. 商業發行版本沒有授權風險 —— 這裡每一段取樣都是我們自己的 DSP 碼生成的。
 *   3. 音效設計變成資料：改 `shotgun.body_freq`，槍聲就不同，不必往返 DAW。
 *
 * 架構：
 *   bp_sfx_init()      把每一個一次性音效（one-shot）合成到單聲道 float 暫存緩衝區，
 *                      再交給 raylib 變成 Sound（LoadSoundFromSample）。
 *   bp_audio_play()    配置一個 voice，依聽者相對位置套用隨機音高與立體聲聲像（pan），
 *                      然後啟動它。
 *   bp_audio_update()  推進每個音效的 3D 衰減，並刷新音樂循環。
 ******************************************************************************************/
#ifndef BP_AUDIO_H
#define BP_AUDIO_H

#include "bp_common.h"

typedef enum {
    SFX_NONE = -1,

    /* 武器 */
    SFX_PISTOL_FIRE, SFX_PISTOL_DRY,
    SFX_SHOTGUN_FIRE, SFX_SHOTGUN_PUMP,
    SFX_SMG_FIRE,
    SFX_RAIL_CHARGE, SFX_RAIL_FIRE,
    SFX_ROCKET_FIRE,
    SFX_RELOAD_OUT, SFX_RELOAD_IN, SFX_RELOAD_DONE,
    SFX_WEAPON_SWAP,

    /* 命中／衝擊 */
    SFX_HIT_FLESH, SFX_HIT_ARMOR, SFX_HIT_WALL, SFX_RICOCHET,
    SFX_EXPLOSION, SFX_EXPLOSION_DISTANT,

    /* 敵人 */
    SFX_GRUNT_SIGHT, SFX_GRUNT_PAIN, SFX_GRUNT_DIE,
    SFX_HOUND_SIGHT, SFX_HOUND_ATTACK, SFX_HOUND_DIE,
    SFX_TURRET_FIRE, SFX_TURRET_DIE,
    SFX_BRUTE_ROAR, SFX_BRUTE_DIE,

    /* 玩家 */
    SFX_JUMP, SFX_LAND, SFX_FOOTSTEP, SFX_HURT, SFX_HEAL, SFX_ARMOR, SFX_AMMO_PICKUP,
    SFX_LOW_HEALTH,

    /* 世界／UI */
    SFX_UI_MOVE, SFX_UI_SELECT, SFX_UI_BACK,
    SFX_WAVE_START, SFX_WAVE_CLEAR, SFX_DOOR, SFX_SECRET,

    SFX_COUNT
} bp_sfx_id;

typedef struct {
    Sound  sound;
    bool   valid;
    float  base_volume;
    float  pitch_min;      /* 隨機範圍，逐 voice 套用 */
    float  pitch_max;
    float  falloff;        /* 距離超過多少世界單位就完全聽不見 */
    float  cooldown;       /* 同一個 id 兩次觸發之間的最短秒數 */
    double last_played;
} bp_sfx_def;

typedef struct {
    bool  initialised;
    float master_volume;
    float sfx_volume;
    float music_volume;

    bp_sfx_def sfx[SFX_COUNT];

    /* 動態音樂 */
    Music music_calm;
    Music music_combat;
    Music music_boss;
    bool  music_valid[3];
    int   music_current;      /* -1 = 靜音，0 = 平靜，1 = 戰鬥，2 = 首領 */
    float music_fade;         /* 交叉淡入淡出（crossfade）進度 0..1 */
    float intensity;          /* 平滑後的 0..1，代表「當下戰鬥有多激烈」 */

    int   active_voices;
    int   dropped_voices;     /* 遙測：撞到 voice 上限的次數 */
} bp_audio_sys;

extern bp_audio_sys g_audio;

void bp_audio_init(void);
void bp_audio_shutdown(void);
void bp_audio_update(float dt);

/* 在聽者所在的位置播放（UI 音效、開槍聲）。 */
void bp_audio_play(bp_sfx_id id, float volumeScale);

/* 在世界中播放：音量隨與 `listener` 的距離衰減，並依方位決定聲像（pan）。 */
void bp_audio_play_at(bp_sfx_id id, Vector3 position, Vector3 listenerPos,
                      Vector3 listenerForward, float volumeScale);

/* 隨機變體，用於腳步聲／環境聲，避免重複播放的機械感被聽出來。 */
void bp_audio_play_variant(bp_sfx_id id, Vector3 position, Vector3 listenerPos,
                           Vector3 listenerForward, float volumeScale, float pitchJitter);

void bp_audio_set_intensity(float target01, float dt);
void bp_audio_set_volumes(float master, float sfx, float music);
const char *bp_audio_sfx_name(bp_sfx_id id);

#endif /* BP_AUDIO_H */
