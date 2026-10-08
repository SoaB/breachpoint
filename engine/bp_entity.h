/******************************************************************************************
 * bp_entity.h -- generational handle pool ("ECS-lite").
 *
 * Why handles instead of pointers or raw indices?
 *   A raw index goes stale the moment an entity is freed and another one reuses the slot;
 *   in a shooter that means a bullet "hits" an enemy that died three frames ago. A pointer
 *   is worse: it dangles into recycled memory.
 *
 *   A handle is { index, generation }. The pool keeps a generation counter per slot and
 *   bumps it on free. bp_pool_get() validates the generation and returns NULL for a stale
 *   handle, so every use-after-free becomes a clean no-op instead of a crash or a ghost hit.
 *
 * The pool is a single contiguous array -> linear iteration is cache friendly, and there is
 * zero allocation during gameplay.
 ******************************************************************************************/
#ifndef BP_ENTITY_H
#define BP_ENTITY_H

#include "bp_common.h"

#define BP_NULL_HANDLE ((bp_handle){ 0, 0 })

typedef struct {
    uint32_t index;      /* slot in the array; 0 is reserved as "null" */
    uint32_t generation; /* bumped every time the slot is recycled     */
} bp_handle;

typedef struct {
    uint32_t  capacity;
    uint32_t  count;              /* live entities                        */
    uint32_t  stride;             /* sizeof(element)                      */
    uint8_t  *data;               /* capacity * stride                    */
    uint32_t *generations;        /* capacity                             */
    uint8_t  *alive;              /* capacity                             */
    uint32_t *free_stack;         /* capacity                             */
    uint32_t  free_top;
    uint32_t *iter_list;          /* dense list of live indices           */
    uint32_t  iter_count;
    const char *name;
} bp_pool;

bool  bp_pool_init(bp_pool *p, const char *name, void *storage, uint32_t *gens, uint8_t *alive,
                   uint32_t *free_stack, uint32_t *iter_list, uint32_t capacity, uint32_t stride);
void  bp_pool_reset(bp_pool *p);
bp_handle bp_pool_alloc(bp_pool *p);
void  bp_pool_free(bp_pool *p, bp_handle h);
bool  bp_pool_valid(const bp_pool *p, bp_handle h);
void *bp_pool_get(bp_pool *p, bp_handle h);          /* NULL if stale */
void *bp_pool_at(bp_pool *p, uint32_t index);        /* NULL if dead  */
bp_handle bp_pool_handle_at(const bp_pool *p, uint32_t index);

/* Rebuild the dense iteration list. Call once per frame before iterating. */
void  bp_pool_compact(bp_pool *p);

static inline bool bp_handle_eq(bp_handle a, bp_handle b)
{
    return a.index == b.index && a.generation == b.generation;
}

#endif /* BP_ENTITY_H */
