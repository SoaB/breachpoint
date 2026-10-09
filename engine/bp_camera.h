/******************************************************************************************
 * bp_camera.h -- 第一人稱攝影機：視角、FOV 踢動、翻滾（roll）、晃動與視圖模型（viewmodel）
 *               機架。
 *
 * 刻意與玩家控制器分開。控制器擁有的是「身體在哪裡」；攝影機擁有的是「身體被怎麼看」。正因為
 * 有這層切割，日後才能加入觀戰攝影機、死亡攝影機與擊殺回放，而不必去動移動（movement）程式碼。
 *
 * 手感目標（這些數字就是整體遊戲手感）：
 *   - 視角與滑鼠 1:1 對應，不做平滑、不做加速度。有任何偏離，遊戲就會像在跟玩家角力。
 *   - FOV 隨速度展開。以 14 m/s 衝刺大約增加 9 度，這正是「不需要速度計也覺得很快」的來源。
 *   - 落地時有向下的俯角下沉與翻滾擺動；中彈時則朝受傷方向翻滾。兩者都小於 2 度、短於 100
 *     毫秒 —— 剛好能被感覺到，又不足以影響瞄準。
 ******************************************************************************************/
#ifndef BP_CAMERA_H
#define BP_CAMERA_H

#include "bp_common.h"
#include "bp_math.h"

typedef struct {
    Camera3D cam;

    float   yaw;            /* 度 */
    float   pitch;          /* 度，已做夾限 */
    float   pitch_limit;

    /* 手感調整參數  */
    float   fov_base;
    float   fov_current;
    float   fov_speed_kick;
    float   roll;           /* 度，繞視線（view axis）套用 */
    float   dip;            /* 蹲下／落地／後座力造成的垂直位移 */
    float   recoil_pitch;   /* 累加值，會隨時間回復 */
    float   recoil_yaw;

    /* 平滑狀態  */
    float   shake_roll;
    Vector3 shake_pos;

    /* 視圖模型機架：一個偏移的攝影機座標空間，供 bp_weapons 繪製槍枝使用   */
    Vector3 vm_position;
    Vector3 vm_rotation;    /* 度 */
    float   vm_bob;
    float   vm_sway;
    float   vm_recoil;

    /* 供繪製使用的插值輸出（模擬以 120 Hz 執行，繪製則依顯示器刷新率）                  */
    Vector3 render_position;
    float   render_yaw;
    float   render_pitch;
    float   alpha;
} bp_camera;

extern bp_camera g_camera;

void    bp_camera_init(void);
void    bp_camera_reset(Vector3 position);

/* 喂入原始的滑鼠／觸控視角位移。`sensitivity` 的單位是「每像素相當量」的度數。 */
void    bp_camera_look(float dx, float dy, float sensitivity);
void    bp_camera_set_angles(float yaw, float pitch);

/* 由固定步進模擬呼叫。 */
void    bp_camera_simulate(float dt, Vector3 bodyPosition, float speed01, bool onGround,
                           bool crouched, float damageRoll);

/* 每個繪製畫面呼叫一次，用來產出經過插值的 Camera3D。 */
Camera3D bp_camera_render(float alpha);

Vector3 bp_camera_forward(void);
Vector3 bp_camera_right(void);
Vector3 bp_camera_eye(void);

/* 戰鬥回饋 */
void    bp_camera_add_recoil(float pitchAmount, float yawAmount);
void    bp_camera_add_dip(float amount);
void    bp_camera_add_roll(float amount);
void    bp_camera_set_fov_base(float fov);

/* 視圖模型輔助函式 */
void    bp_camera_vm_update(float dt, float speed01, float recoil01, bool moving);
Matrix  bp_camera_vm_matrix(void);

#endif /* BP_CAMERA_H */
