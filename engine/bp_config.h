/******************************************************************************************
 * bp_config.h -- 迷你的資料驅動（data-driven）設定載入器。
 *
 * 商用開發團隊不會為了改一個傷害數值就重新編譯。本模組讀取純文字表格（鍵值區塊），並填入
 * 一般的 C 結構，讓企劃只需要一支文字編輯器就能重新平衡整套武器，完全不需要建置步驟。
 *
 * 格式（刻意保持簡單、方便手改、也方便做 diff）：
 *
 *      # comment
 *      weapon shotgun
 *          display_name = SCATTER-12
 *          damage       = 11
 *          pellets      = 9
 *      end
 *
 * 解析器完全不配置記憶體：檔案先讀進一個靜態緩衝區，一次性切成行（line span），之後再以
 * 遊標（cursor）走訪。未知的鍵只產生警告、絕不產生錯誤 —— 這樣舊的用戶端才載得動新的資料檔。
 ******************************************************************************************/
#ifndef BP_CONFIG_H
#define BP_CONFIG_H

#include "bp_common.h"

#define BP_CFG_MAX_LINE     256
#define BP_CFG_MAX_FILE     (128 * 1024)
#define BP_CFG_MAX_LINES    2048

typedef struct {
    char  buffer[BP_CFG_MAX_FILE];
    int   length;
    int   line_start[BP_CFG_MAX_LINES];   /* 每一行的位元組位移                  */
    int   line_len[BP_CFG_MAX_LINES];     /* 長度，不含換行字元                  */
    int   line_count;
    int   cursor;                         /* 索引，指向 line_*                   */

    char  section_type[32];               /* 區塊型別，如 "weapon"                 */
    char  section_id[32];                 /* 區塊 id，如 "shotgun"                 */
    char  key[64];
    char  value[BP_CFG_MAX_LINE];
    bool  ok;
} bp_config;

bool        bp_config_load(bp_config *cfg, const char *path);
bool        bp_config_load_mem(bp_config *cfg, const char *data, int length);
void        bp_config_rewind(bp_config *cfg);

/* 前進到下一個 "<type> <id>" 區塊。成功時 section_type/section_id 會被填入，且遊標指向該
 * 區塊的第一個鍵。抵達檔案結尾（EOF）時回傳 false。 */
bool        bp_config_next_section(bp_config *cfg);

/* 依型別＋id 尋找指定的區塊，例如 bp_config_find_section(&c, "weapon", "railgun")。 */
bool        bp_config_find_section(bp_config *cfg, const char *type, const char *id);

const char *bp_config_section_type(const bp_config *cfg);
const char *bp_config_section_id(const bp_config *cfg);

/* 前進到目前區塊中的下一個鍵。碰到 "end" 時回傳 false。 */
bool        bp_config_next_key(bp_config *cfg);

/* 針對 bp_config_next_key 最後回傳的那個鍵所提供的型別化取值函式。 */
int         bp_cfg_int(const bp_config *cfg, int fallback);
float       bp_cfg_float(const bp_config *cfg, float fallback);
bool        bp_cfg_bool(const bp_config *cfg, bool fallback);
const char *bp_cfg_str(const bp_config *cfg, const char *fallback);
Color       bp_cfg_color(const bp_config *cfg, Color fallback);
Vector3     bp_cfg_vec3(const bp_config *cfg, Vector3 fallback);

#endif /* BP_CONFIG_H */
