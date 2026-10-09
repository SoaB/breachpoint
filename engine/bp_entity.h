/******************************************************************************************
 * bp_entity.h -- 世代句柄（generational handle）池，或稱「簡化版 ECS（ECS-lite）」。
 *
 * 為什麼要用句柄（handle），而不是指標或裸索引？
 *   裸索引會在實體被釋放、且另一個實體重用那個槽位的瞬間就過期；在射擊遊戲裡，這代表一顆
 *   子彈「擊中」了三幀前就已經死掉的敵人。指標更糟：它會懸空（dangle）指向已被重用的記憶體。
 *
 *   句柄是 { index, generation }。池為每個槽位保存一個世代計數器，並在釋放時遞增它。
 *   bp_pool_get() 會驗證世代值，對過期的句柄回傳 NULL，於是每一次釋放後再使用（use-after-free）
 *   都變成一個乾淨的無效操作，而不是當機或幽靈命中（ghost hit）。
 *
 * 池是單一連續陣列 -> 線性迭代快取友善，而且遊玩過程中有零配置（zero allocation）。
 ******************************************************************************************/
#ifndef BP_ENTITY_H
#define BP_ENTITY_H

#include "bp_common.h"

#define BP_NULL_HANDLE ((bp_handle){ 0, 0 })

typedef struct {
    uint32_t index;      /* 陣列中的槽位；0 保留給「null」          */
    uint32_t generation; /* 每當槽位被重複使用時遞增                */
} bp_handle;

typedef struct {
    uint32_t  capacity;
    uint32_t  count;              /* 存活的實體數                        */
    uint32_t  stride;             /* 單一元素的大小                       */
    uint8_t  *data;               /* 容量 × stride                       */
    uint32_t *generations;        /* 容量                               */
    uint8_t  *alive;              /* 容量                               */
    uint32_t *free_stack;         /* 容量                               */
    uint32_t  free_top;
    uint32_t *iter_list;          /* 存活索引的稠密清單                   */
    uint32_t  iter_count;
    const char *name;
} bp_pool;

bool  bp_pool_init(bp_pool *p, const char *name, void *storage, uint32_t *gens, uint8_t *alive,
                   uint32_t *free_stack, uint32_t *iter_list, uint32_t capacity, uint32_t stride);
void  bp_pool_reset(bp_pool *p);
bp_handle bp_pool_alloc(bp_pool *p);
void  bp_pool_free(bp_pool *p, bp_handle h);
bool  bp_pool_valid(const bp_pool *p, bp_handle h);
void *bp_pool_get(bp_pool *p, bp_handle h);          /* 過期則 NULL */
void *bp_pool_at(bp_pool *p, uint32_t index);        /* 已死則 NULL  */
bp_handle bp_pool_handle_at(const bp_pool *p, uint32_t index);

/* 重建稠密的迭代清單。請在每幀開始迭代之前呼叫一次。 */
void  bp_pool_compact(bp_pool *p);

static inline bool bp_handle_eq(bp_handle a, bp_handle b)
{
    return a.index == b.index && a.generation == b.generation;
}

#endif /* BP_ENTITY_H */
