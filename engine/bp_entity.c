/******************************************************************************************
 * bp_entity.c
 ******************************************************************************************/
#include "bp_entity.h"

bool bp_pool_init(bp_pool *p, const char *name, void *storage, uint32_t *gens, uint8_t *alive,
                  uint32_t *free_stack, uint32_t *iter_list, uint32_t capacity, uint32_t stride)
{
    if (!p || !storage || !gens || !alive || !free_stack || !iter_list || capacity == 0) return false;

    p->name        = name;
    p->capacity    = capacity;
    p->stride      = stride;
    p->data        = (uint8_t *)storage;
    p->generations = gens;
    p->alive       = alive;
    p->free_stack  = free_stack;
    p->iter_list   = iter_list;
    bp_pool_reset(p);
    return true;
}

void bp_pool_reset(bp_pool *p)
{
    memset(p->data, 0, (size_t)p->capacity * p->stride);
    memset(p->alive, 0, p->capacity);
    p->count = 0;
    p->iter_count = 0;

    /* Slot 0 is permanently reserved so BP_NULL_HANDLE is never a live entity. */
    p->generations[0] = 0;
    p->alive[0] = 0;

    p->free_top = 0;
    for (uint32_t i = p->capacity - 1; i >= 1; i--)
        p->free_stack[p->free_top++] = i;
}

bp_handle bp_pool_alloc(bp_pool *p)
{
    if (p->free_top == 0)
    {
        BP_LOGW("pool '%s' exhausted (%u slots)", p->name, p->capacity);
        return BP_NULL_HANDLE;
    }

    uint32_t idx = p->free_stack[--p->free_top];
    p->alive[idx] = 1;
    p->count++;
    memset(p->data + (size_t)idx * p->stride, 0, p->stride);

    bp_handle h;
    h.index = idx;
    h.generation = p->generations[idx];
    if (h.generation == 0) h.generation = 1;   /* avoid colliding with NULL_HANDLE */
    p->generations[idx] = h.generation;
    return h;
}

void bp_pool_free(bp_pool *p, bp_handle h)
{
    if (h.index == 0 || h.index >= p->capacity) return;
    if (!p->alive[h.index]) return;
    if (p->generations[h.index] != h.generation) return;

    p->alive[h.index] = 0;
    p->count--;
    p->generations[h.index]++;
    p->free_stack[p->free_top++] = h.index;
}

bool bp_pool_valid(const bp_pool *p, bp_handle h)
{
    if (h.index == 0 || h.index >= p->capacity) return false;
    return p->alive[h.index] && p->generations[h.index] == h.generation;
}

void *bp_pool_get(bp_pool *p, bp_handle h)
{
    if (!bp_pool_valid(p, h)) return NULL;
    return p->data + (size_t)h.index * p->stride;
}

void *bp_pool_at(bp_pool *p, uint32_t index)
{
    if (index == 0 || index >= p->capacity || !p->alive[index]) return NULL;
    return p->data + (size_t)index * p->stride;
}

bp_handle bp_pool_handle_at(const bp_pool *p, uint32_t index)
{
    bp_handle h = { index, p->generations[index] };
    if (index == 0 || index >= p->capacity || !p->alive[index]) return BP_NULL_HANDLE;
    return h;
}

void bp_pool_compact(bp_pool *p)
{
    p->iter_count = 0;
    for (uint32_t i = 1; i < p->capacity; i++)
        if (p->alive[i]) p->iter_list[p->iter_count++] = i;
}
