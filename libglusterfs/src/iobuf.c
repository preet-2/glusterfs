/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include "glusterfs/iobuf.h"
#include "glusterfs/statedump.h"
#include <stdio.h>
#include "glusterfs/libglusterfs-messages.h"

/*
  TODO: implement destroy margins and prefetching of arenas
*/

#define IOBUF_ARENA_MAX_INDEX                                                  \
    (sizeof(gf_iobuf_init_config) / (sizeof(struct iobuf_init_config)))

/* Make sure this array is sorted based on pagesize */
static const struct iobuf_init_config gf_iobuf_init_config[] = {
    /* { pagesize, num_pages }, */
    {128, 1024},     {512, 512},       {2 * 1024, 512}, {8 * 1024, 128},
    {32 * 1024, 64}, {128 * 1024, 32}, {256 * 1024, 8}, {1 * 1024 * 1024, 2},
};

static int
gf_iobuf_get_arena_index(const size_t page_size)
{
    int i;

    for (i = 0; i < IOBUF_ARENA_MAX_INDEX; i++) {
        if (page_size <= gf_iobuf_init_config[i].pagesize)
            return i;
    }

    return -1;
}

static size_t
gf_iobuf_get_pagesize(const size_t page_size, int *index)
{
    int i;
    size_t size = 0;

    for (i = 0; i < IOBUF_ARENA_MAX_INDEX; i++) {
        size = gf_iobuf_init_config[i].pagesize;
        if (page_size <= size) {
            if (index != NULL)
                *index = i;
            return size;
        }
    }

    return -1;
}

static void
__iobuf_arena_init_iobufs(struct iobuf_arena *iobuf_arena)
{
    const int iobuf_cnt = iobuf_arena->page_count;
    struct iobuf *iobuf = NULL;
    int offset = 0;
    int i = 0;

    iobuf_arena->iobufs = GF_CALLOC(sizeof(*iobuf), iobuf_cnt,
                                    gf_common_mt_iobuf);
    if (!iobuf_arena->iobufs)
        return;

    iobuf = iobuf_arena->iobufs;
    for (i = 0; i < iobuf_cnt; i++) {
        INIT_LIST_HEAD(&iobuf->list);
        LOCK_INIT(&iobuf->lock);

        iobuf->iobuf_arena = iobuf_arena;

        iobuf->ptr = iobuf_arena->mem_base + offset;

        list_add(&iobuf->list, &iobuf_arena->passive_list);
        iobuf_arena->passive_cnt++;

        offset += iobuf_arena->page_size;
        iobuf++;
    }

    return;
}

static void
__iobuf_arena_destroy_iobufs(struct iobuf_arena *iobuf_arena)
{
    int iobuf_cnt = 0;
    struct iobuf *iobuf = NULL;
    int i = 0;

    if (!iobuf_arena->iobufs) {
        gf_msg_callingfn(THIS->name, GF_LOG_ERROR, 0, LG_MSG_IOBUFS_NOT_FOUND,
                         "iobufs not found");
        return;
    }

    iobuf_cnt = iobuf_arena->page_count;
    iobuf = iobuf_arena->iobufs;
    for (i = 0; i < iobuf_cnt; i++) {
        GF_ASSERT(GF_ATOMIC_GET(iobuf->ref) == 0);

        LOCK_DESTROY(&iobuf->lock);
        list_del_init(&iobuf->list);
        iobuf++;
    }

    GF_FREE(iobuf_arena->iobufs);

    return;
}

static void
__iobuf_arena_destroy(struct iobuf_pool *iobuf_pool,
                      struct iobuf_arena *iobuf_arena)
{
    GF_VALIDATE_OR_GOTO("iobuf", iobuf_arena, out);

    __iobuf_arena_destroy_iobufs(iobuf_arena);

    if (iobuf_arena->mem_base && iobuf_arena->mem_base != MAP_FAILED)
        munmap(iobuf_arena->mem_base, iobuf_arena->arena_size);

    GF_FREE(iobuf_arena);
out:
    return;
}

static struct iobuf_arena *
__iobuf_arena_alloc(struct iobuf_pool *iobuf_pool, size_t page_size,
                    int32_t num_iobufs)
{
    struct iobuf_arena *iobuf_arena = NULL;
    size_t rounded_size = 0;
    int index = 0; /* unused */

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_pool, out);

    iobuf_arena = GF_CALLOC(sizeof(*iobuf_arena), 1, gf_common_mt_iobuf_arena);
    if (!iobuf_arena)
        goto err;

    INIT_LIST_HEAD(&iobuf_arena->list);
    INIT_LIST_HEAD(&iobuf_arena->all_list);
    INIT_LIST_HEAD(&iobuf_arena->passive_list);
    INIT_LIST_HEAD(&iobuf_arena->active_list);
    iobuf_arena->iobuf_pool = iobuf_pool;

    rounded_size = gf_iobuf_get_pagesize(page_size, &index);

    iobuf_arena->page_size = rounded_size;
    iobuf_arena->page_count = num_iobufs;

    iobuf_arena->arena_size = rounded_size * num_iobufs;

    iobuf_arena->mem_base = mmap(NULL, iobuf_arena->arena_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (iobuf_arena->mem_base == MAP_FAILED) {
        gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_MAPPING_FAILED, NULL);
        goto err;
    }

    list_add_tail(&iobuf_arena->all_list, &iobuf_pool->all_arenas);

    __iobuf_arena_init_iobufs(iobuf_arena);
    if (!iobuf_arena->iobufs) {
        gf_smsg(THIS->name, GF_LOG_ERROR, 0, LG_MSG_INIT_IOBUF_FAILED, NULL);
        goto err;
    }

    iobuf_pool->arena_cnt++;

    return iobuf_arena;

err:
    __iobuf_arena_destroy(iobuf_pool, iobuf_arena);

out:
    return NULL;
}

static struct iobuf_arena *
__iobuf_arena_unprune(struct iobuf_pool *iobuf_pool, const size_t page_size,
                      const int index)
{
    struct iobuf_arena *iobuf_arena = NULL;
    struct iobuf_arena *tmp = NULL;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_pool, out);

    list_for_each_entry(tmp, &iobuf_pool->purge[index], list)
    {
        list_del_init(&tmp->list);
        iobuf_arena = tmp;
        break;
    }
out:
    return iobuf_arena;
}

static struct iobuf_arena *
__iobuf_pool_add_arena(struct iobuf_pool *iobuf_pool, const size_t page_size,
                       const int32_t num_pages, const int index)
{
    struct iobuf_arena *iobuf_arena = NULL;

    iobuf_arena = __iobuf_arena_unprune(iobuf_pool, page_size, index);

    if (!iobuf_arena) {
        iobuf_arena = __iobuf_arena_alloc(iobuf_pool, page_size, num_pages);
        if (!iobuf_arena) {
            gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_ARENA_NOT_FOUND,
                    NULL);
            return NULL;
        }
    }
    list_add(&iobuf_arena->list, &iobuf_pool->arenas[index]);

    return iobuf_arena;
}

/* This function destroys all the iobufs and the iobuf_pool */
void
iobuf_pool_destroy(struct iobuf_pool *iobuf_pool)
{
    struct iobuf_arena *iobuf_arena = NULL;
    struct iobuf_arena *tmp = NULL;
    int i = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_pool, out);

    pthread_mutex_lock(&iobuf_pool->mutex);
    {
        for (i = 0; i < IOBUF_ARENA_MAX_INDEX; i++) {
            list_for_each_entry_safe(iobuf_arena, tmp, &iobuf_pool->arenas[i],
                                     list)
            {
                list_del_init(&iobuf_arena->list);
                iobuf_pool->arena_cnt--;

                __iobuf_arena_destroy(iobuf_pool, iobuf_arena);
            }
            list_for_each_entry_safe(iobuf_arena, tmp, &iobuf_pool->purge[i],
                                     list)
            {
                list_del_init(&iobuf_arena->list);
                iobuf_pool->arena_cnt--;
                __iobuf_arena_destroy(iobuf_pool, iobuf_arena);
            }
            /* If there are no iobuf leaks, there should be no
             * arenas in the filled list. If at all there are any
             * arenas in the filled list, the below function will
             * assert.
             */
            list_for_each_entry_safe(iobuf_arena, tmp, &iobuf_pool->filled[i],
                                     list)
            {
                list_del_init(&iobuf_arena->list);
                iobuf_pool->arena_cnt--;
                __iobuf_arena_destroy(iobuf_pool, iobuf_arena);
            }
            /* If there are no iobuf leaks, there shoould be
             * no standard allocated arenas, iobuf_put will free
             * such arenas.
             * TODO: Free the stdalloc arenas forcefully if present?
             */
        }
    }
    pthread_mutex_unlock(&iobuf_pool->mutex);

    pthread_mutex_destroy(&iobuf_pool->mutex);

    GF_FREE(iobuf_pool);

out:
    return;
}

static void
iobuf_create_stdalloc_arena(struct iobuf_pool *iobuf_pool)
{
    struct iobuf_arena *iobuf_arena = NULL;

    /* No locking required here as its called only once during init */
    iobuf_arena = GF_CALLOC(sizeof(*iobuf_arena), 1, gf_common_mt_iobuf_arena);
    if (!iobuf_arena)
        goto err;

    INIT_LIST_HEAD(&iobuf_arena->list);
    INIT_LIST_HEAD(&iobuf_arena->passive_list);
    INIT_LIST_HEAD(&iobuf_arena->active_list);

    iobuf_arena->iobuf_pool = iobuf_pool;

    iobuf_arena->page_size = 0x7fffffff;

    list_add_tail(&iobuf_arena->list,
                  &iobuf_pool->arenas[IOBUF_ARENA_MAX_INDEX]);

err:
    return;
}

struct iobuf_pool *
iobuf_pool_new(void)
{
    struct iobuf_pool *iobuf_pool = NULL;
    int i = 0;
    size_t page_size = 0;
    size_t arena_size = 0;
    int32_t num_pages = 0;

    iobuf_pool = GF_CALLOC(sizeof(*iobuf_pool), 1, gf_common_mt_iobuf_pool);
    if (!iobuf_pool)
        goto out;
    INIT_LIST_HEAD(&iobuf_pool->all_arenas);
    pthread_mutex_init(&iobuf_pool->mutex, NULL);
    for (i = 0; i <= IOBUF_ARENA_MAX_INDEX; i++) {
        INIT_LIST_HEAD(&iobuf_pool->arenas[i]);
        INIT_LIST_HEAD(&iobuf_pool->filled[i]);
        INIT_LIST_HEAD(&iobuf_pool->purge[i]);
    }

    iobuf_pool->default_page_size = 128 * GF_UNIT_KB;

    /* No locking required here
     * as no one else can use this pool yet
     */
    for (i = 0; i < IOBUF_ARENA_MAX_INDEX; i++) {
        page_size = gf_iobuf_init_config[i].pagesize;
        num_pages = gf_iobuf_init_config[i].num_pages;

        if (__iobuf_pool_add_arena(iobuf_pool, page_size, num_pages, i) != NULL)
            arena_size += page_size * num_pages;
    }

    /* Need an arena to handle all the bigger iobuf requests */
    iobuf_create_stdalloc_arena(iobuf_pool);

    iobuf_pool->arena_size = arena_size;
out:

    return iobuf_pool;
}

static void
__iobuf_arena_prune(struct iobuf_pool *iobuf_pool,
                    struct iobuf_arena *iobuf_arena, const int index)
{
    /* code flow comes here only if the arena is in purge list and we can
     * free the arena only if we have at least one arena in 'arenas' list
     * (ie, at least few iobufs free in arena), that way, there won't
     * be spurious mmap/unmap of buffers
     */
    if (list_empty(&iobuf_pool->arenas[index]))
        goto out;

    /* All cases matched, destroy */
    list_del_init(&iobuf_arena->list);
    list_del_init(&iobuf_arena->all_list);
    iobuf_pool->arena_cnt--;

    __iobuf_arena_destroy(iobuf_pool, iobuf_arena);

out:
    return;
}

void
iobuf_pool_prune(struct iobuf_pool *iobuf_pool)
{
    struct iobuf_arena *iobuf_arena = NULL;
    struct iobuf_arena *tmp = NULL;
    int i = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_pool, out);

    pthread_mutex_lock(&iobuf_pool->mutex);
    {
        for (i = 0; i < IOBUF_ARENA_MAX_INDEX; i++) {
            if (list_empty(&iobuf_pool->arenas[i])) {
                continue;
            }

            list_for_each_entry_safe(iobuf_arena, tmp, &iobuf_pool->purge[i],
                                     list)
            {
                __iobuf_arena_prune(iobuf_pool, iobuf_arena, i);
            }
        }
    }
    pthread_mutex_unlock(&iobuf_pool->mutex);

out:
    return;
}

/* Always called under the iobuf_pool mutex lock */
static struct iobuf_arena *
__iobuf_select_arena(struct iobuf_pool *iobuf_pool, const size_t page_size,
                     const int index)
{
    struct iobuf_arena *iobuf_arena = NULL;
    struct iobuf_arena *trav = NULL;

    /* look for unused iobuf from the head-most arena */
    list_for_each_entry(trav, &iobuf_pool->arenas[index], list)
    {
        if (trav->passive_cnt) {
            iobuf_arena = trav;
            break;
        }
    }

    if (!iobuf_arena) {
        /* all arenas were full, find the right count to add */
        iobuf_arena = __iobuf_pool_add_arena(
            iobuf_pool, page_size, gf_iobuf_init_config[index].num_pages,
            index);
    }

    return iobuf_arena;
}

/* Always called under the iobuf_pool mutex lock */
static struct iobuf *
__iobuf_get(struct iobuf_pool *iobuf_pool, const size_t page_size,
            const int index)
{
    struct iobuf *iobuf = NULL;
    struct iobuf_arena *iobuf_arena = NULL;

    /* most eligible arena for picking an iobuf */
    iobuf_arena = __iobuf_select_arena(iobuf_pool, page_size, index);
    if (!iobuf_arena)
        return NULL;

    iobuf = list_first_entry(&iobuf_arena->passive_list, struct iobuf, list);

    list_del(&iobuf->list);
    iobuf_arena->passive_cnt--;

    list_add(&iobuf->list, &iobuf_arena->active_list);
    iobuf_arena->active_cnt++;

    /* no resetting requied for this element */
    iobuf_arena->alloc_cnt++;

    if (iobuf_arena->max_active < iobuf_arena->active_cnt)
        iobuf_arena->max_active = iobuf_arena->active_cnt;

    if (iobuf_arena->passive_cnt == 0) {
        list_del(&iobuf_arena->list);
        list_add(&iobuf_arena->list, &iobuf_pool->filled[index]);
    }

    iobuf->page_size = page_size;
    return iobuf;
}

static struct iobuf *
iobuf_get_from_stdalloc(struct iobuf_pool *iobuf_pool, const size_t page_size)
{
    struct iobuf *iobuf = NULL;
    struct iobuf_arena *iobuf_arena = NULL;
    struct iobuf_arena *trav = NULL;
    int ret = -1;

    /* The first arena in the 'MAX-INDEX' will always be used for misc */
    list_for_each_entry(trav, &iobuf_pool->arenas[IOBUF_ARENA_MAX_INDEX], list)
    {
        iobuf_arena = trav;
        break;
    }

    iobuf = GF_CALLOC(1, sizeof(*iobuf), gf_common_mt_iobuf);
    if (!iobuf)
        goto out;

    /* 4096 is the alignment */
    iobuf->free_ptr = GF_CALLOC(1, ((page_size + GF_IOBUF_ALIGN_SIZE) - 1),
                                gf_common_mt_char);
    if (!iobuf->free_ptr)
        goto out;

    iobuf->ptr = GF_ALIGN_BUF(iobuf->free_ptr, GF_IOBUF_ALIGN_SIZE);
    iobuf->iobuf_arena = iobuf_arena;
    iobuf->page_size = page_size;
    LOCK_INIT(&iobuf->lock);

    /* Hold a ref because you are allocating and using it */
    GF_ATOMIC_INIT(iobuf->ref, 1);

    ret = 0;
out:
    if (ret && iobuf) {
        GF_FREE(iobuf->free_ptr);
        GF_FREE(iobuf);
        iobuf = NULL;
    }

    return iobuf;
}

static struct iobuf *
iobuf_get_from_small(const size_t page_size)
{
    struct iobuf *iobuf = NULL;
    int ret = -1;

    iobuf = GF_MALLOC(sizeof(*iobuf), gf_common_mt_iobuf);
    if (!iobuf)
        goto out;

    iobuf->free_ptr = GF_MALLOC(page_size, gf_common_mt_iobuf_pool);
    if (!iobuf->free_ptr)
        goto out;

    iobuf->ptr = iobuf->free_ptr;
    iobuf->page_size = page_size;
    INIT_LIST_HEAD(&iobuf->list);
    iobuf->iobuf_arena = NULL;
    LOCK_INIT(&iobuf->lock);
    /* Hold a ref because you are allocating and using it */
    GF_ATOMIC_INIT(iobuf->ref, 1);

    ret = 0;
out:
    if (ret && iobuf) {
        GF_FREE(iobuf->free_ptr);
        GF_FREE(iobuf);
        iobuf = NULL;
    }

    return iobuf;
}

struct iobuf *
iobuf_get2(struct iobuf_pool *iobuf_pool, size_t page_size)
{
    struct iobuf *iobuf = NULL;
    size_t rounded_size = 0;
    int index = 0;

    if (page_size == 0) {
        page_size = iobuf_pool->default_page_size;
    }

    /* During smallfile testing we have observed the performance
       is improved significantly while use standard allocation if
       page size is less than equal to 128KB, the data is available
       on the link https://github.com/gluster/glusterfs/issues/2771
    */
    if (page_size <= USE_IOBUF_POOL_IF_SIZE_GREATER_THAN) {
        iobuf = iobuf_get_from_small(page_size);
        if (!iobuf)
            gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_IOBUF_NOT_FOUND,
                    NULL);
        return iobuf;
    }

    rounded_size = gf_iobuf_get_pagesize(page_size, &index);
    if (rounded_size == -1) {
        /* make sure to provide the requested buffer with standard
           memory allocations */
        iobuf = iobuf_get_from_stdalloc(iobuf_pool, page_size);

        gf_msg_debug("iobuf", 0,
                     "request for iobuf of size %zu "
                     "is serviced using standard calloc() (%p) as it "
                     "exceeds the maximum available buffer size",
                     page_size, iobuf);

        iobuf_pool->request_misses++;
        return iobuf;
    } else if (index == -1) {
        gf_smsg("iobuf", GF_LOG_ERROR, 0, LG_MSG_PAGE_SIZE_EXCEEDED,
                "page_size=%zu", page_size, NULL);
        return NULL;
    }

    pthread_mutex_lock(&iobuf_pool->mutex);
    {
        iobuf = __iobuf_get(iobuf_pool, rounded_size, index);
        if (!iobuf) {
            pthread_mutex_unlock(&iobuf_pool->mutex);
            gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_IOBUF_NOT_FOUND,
                    NULL);
            goto post_unlock;
        }
        iobuf_ref(iobuf);
    }
    pthread_mutex_unlock(&iobuf_pool->mutex);
post_unlock:
    return iobuf;
}

struct iobuf *
iobuf_get_page_aligned(struct iobuf_pool *iobuf_pool, size_t page_size,
                       size_t align_size)
{
    size_t req_size = 0;
    struct iobuf *iobuf = NULL;

    req_size = page_size;

    if (req_size == 0) {
        req_size = iobuf_pool->default_page_size;
    }

    req_size = req_size + align_size;
    iobuf = iobuf_get2(iobuf_pool, req_size);
    if (!iobuf)
        return NULL;
    /* If std allocation was used, then free_ptr will be non-NULL. In this
     * case, we do not want to modify the original free_ptr.
     * On the other hand, if the buf was gotten through the available
     * arenas, then we use iobuf->free_ptr to store the original
     * pointer to the offset into the mmap'd block of memory and in turn
     * reuse iobuf->ptr to hold the page-aligned address. And finally, in
     * iobuf_put(), we copy iobuf->free_ptr into iobuf->ptr - back to where
     * it was originally when __iobuf_get() returned this iobuf.
     */
    if (!iobuf->free_ptr)
        iobuf->free_ptr = iobuf->ptr;
    iobuf->ptr = GF_ALIGN_BUF(iobuf->ptr, align_size);

    return iobuf;
}

struct iobuf *
iobuf_get(struct iobuf_pool *iobuf_pool)
{
    struct iobuf *iobuf = NULL;
    size_t page_size = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_pool, out);

    page_size = iobuf_pool->default_page_size;
    iobuf = iobuf_get2(iobuf_pool, page_size);

out:
    return iobuf;
}

static void
__iobuf_put(struct iobuf *iobuf, struct iobuf_arena *iobuf_arena)
{
    struct iobuf_pool *iobuf_pool = NULL;
    int index = 0;

    iobuf_pool = iobuf_arena->iobuf_pool;

    index = gf_iobuf_get_arena_index(iobuf_arena->page_size);
    if (index == -1) {
        gf_msg_debug("iobuf", 0,
                     "freeing the iobuf (%p) "
                     "allocated with standard calloc()",
                     iobuf);

        /* free up properly without bothering about lists and all */
        LOCK_DESTROY(&iobuf->lock);
        GF_FREE(iobuf->free_ptr);
        GF_FREE(iobuf);
        return;
    }

    if (iobuf_arena->passive_cnt == 0) {
        list_del(&iobuf_arena->list);
        list_add_tail(&iobuf_arena->list, &iobuf_pool->arenas[index]);
    }

    list_del_init(&iobuf->list);
    iobuf_arena->active_cnt--;

    if (iobuf->free_ptr) {
        iobuf->ptr = iobuf->free_ptr;
        iobuf->free_ptr = NULL;
    }

    list_add(&iobuf->list, &iobuf_arena->passive_list);
    iobuf_arena->passive_cnt++;

    if (iobuf_arena->active_cnt == 0) {
        list_del(&iobuf_arena->list);
        list_add_tail(&iobuf_arena->list, &iobuf_pool->purge[index]);
        GF_VALIDATE_OR_GOTO("iobuf", iobuf_pool, out);
        __iobuf_arena_prune(iobuf_pool, iobuf_arena, index);
    }
out:
    return;
}

void
iobuf_put(struct iobuf *iobuf)
{
    struct iobuf_arena *iobuf_arena = NULL;
    struct iobuf_pool *iobuf_pool = NULL;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);

    iobuf_arena = iobuf->iobuf_arena;
    if (!iobuf_arena) {
        LOCK_DESTROY(&iobuf->lock);
        GF_FREE(iobuf->free_ptr);
        GF_FREE(iobuf);
        return;
    }

    iobuf_pool = iobuf_arena->iobuf_pool;
    if (!iobuf_pool) {
        gf_smsg(THIS->name, GF_LOG_WARNING, 0, LG_MSG_POOL_NOT_FOUND, "iobuf",
                NULL);
        return;
    }

    pthread_mutex_lock(&iobuf_pool->mutex);
    {
        __iobuf_put(iobuf, iobuf_arena);
    }
    pthread_mutex_unlock(&iobuf_pool->mutex);

out:
    return;
}

void
iobuf_unref(struct iobuf *iobuf)
{
    int ref = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);

    ref = GF_ATOMIC_DEC(iobuf->ref);

    if (!ref)
        iobuf_put(iobuf);

out:
    return;
}

struct iobuf *
iobuf_ref(struct iobuf *iobuf)
{
    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);
    GF_ATOMIC_INC(iobuf->ref);

out:
    return iobuf;
}

struct iobref *
iobref_new()
{
    struct iobref *iobref = NULL;

    iobref = GF_MALLOC(sizeof(*iobref), gf_common_mt_iobref);
    if (!iobref)
        return NULL;

    iobref->iobrefs = GF_CALLOC(sizeof(*iobref->iobrefs), 16,
                                gf_common_mt_iobrefs);
    if (!iobref->iobrefs) {
        GF_FREE(iobref);
        return NULL;
    }

    iobref->allocated = 16;
    iobref->used = 0;

    LOCK_INIT(&iobref->lock);

    GF_ATOMIC_INIT(iobref->ref, 1);
    return iobref;
}

struct iobref *
iobref_ref(struct iobref *iobref)
{
    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);
    GF_ATOMIC_INC(iobref->ref);

out:
    return iobref;
}

void
iobref_destroy(struct iobref *iobref)
{
    int i = 0;
    struct iobuf *iobuf = NULL;

    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);

    for (i = 0; i < iobref->allocated; i++) {
        iobuf = iobref->iobrefs[i];

        iobref->iobrefs[i] = NULL;
        if (iobuf)
            iobuf_unref(iobuf);
    }

    LOCK_DESTROY(&iobref->lock);

    GF_FREE(iobref->iobrefs);
    GF_FREE(iobref);

out:
    return;
}

void
iobref_unref(struct iobref *iobref)
{
    int ref = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);
    ref = GF_ATOMIC_DEC(iobref->ref);

    if (!ref)
        iobref_destroy(iobref);

out:
    return;
}

void
iobref_clear(struct iobref *iobref)
{
    int i = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);

    for (; i < iobref->allocated; i++) {
        if (iobref->iobrefs[i] != NULL) {
            iobuf_unref(iobref->iobrefs[i]);
        } else {
            /** iobuf's are attached serially */
            break;
        }
    }

    iobref_unref(iobref);

out:
    return;
}

static void
__iobref_grow(struct iobref *iobref)
{
    void *newptr = NULL;
    int i = 0;

    newptr = GF_REALLOC(iobref->iobrefs,
                        iobref->allocated * 2 * (sizeof(*iobref->iobrefs)));
    if (newptr) {
        iobref->iobrefs = newptr;
        iobref->allocated *= 2;

        for (i = iobref->used; i < iobref->allocated; i++)
            iobref->iobrefs[i] = NULL;
    }
}

int
__iobref_add(struct iobref *iobref, struct iobuf *iobuf)
{
    int i = 0;
    int ret = -ENOMEM;

    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);
    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);

    if (iobref->used == iobref->allocated) {
        __iobref_grow(iobref);

        if (iobref->used == iobref->allocated) {
            ret = -ENOMEM;
            goto out;
        }
    }

    for (i = 0; i < iobref->allocated; i++) {
        if (iobref->iobrefs[i] == NULL) {
            iobref->iobrefs[i] = iobuf_ref(iobuf);
            iobref->used++;
            ret = 0;
            break;
        }
    }

out:
    return ret;
}

int
iobref_add(struct iobref *iobref, struct iobuf *iobuf)
{
    int ret = -EINVAL;

    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);
    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);

    LOCK(&iobref->lock);
    {
        ret = __iobref_add(iobref, iobuf);
    }
    UNLOCK(&iobref->lock);

out:
    return ret;
}

int
iobref_merge(struct iobref *to, struct iobref *from)
{
    int i = 0;
    int ret = 0;
    struct iobuf *iobuf = NULL;

    GF_VALIDATE_OR_GOTO("iobuf", to, out);
    GF_VALIDATE_OR_GOTO("iobuf", from, out);

    LOCK(&from->lock);
    {
        for (i = 0; i < from->allocated; i++) {
            iobuf = from->iobrefs[i];

            if (!iobuf)
                break;

            ret = iobref_add(to, iobuf);

            if (ret < 0)
                break;
        }
    }
    UNLOCK(&from->lock);

out:
    return ret;
}

size_t
iobuf_size(struct iobuf *iobuf)
{
    size_t size = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);
    size = iobuf_pagesize(iobuf);

out:
    return size;
}

size_t
iobref_size(struct iobref *iobref)
{
    size_t size = 0;
    int i = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobref, out);

    LOCK(&iobref->lock);
    {
        for (i = 0; i < iobref->allocated; i++) {
            if (iobref->iobrefs[i])
                size += iobuf_size(iobref->iobrefs[i]);
        }
    }
    UNLOCK(&iobref->lock);

out:
    return size;
}

void
iobuf_info_dump(struct iobuf *iobuf, const char *key_prefix)
{
    char key[GF_DUMP_MAX_BUF_LEN];
    struct iobuf my_iobuf;
    int ret = 0;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf, out);

    ret = TRY_LOCK(&iobuf->lock);
    if (ret) {
        return;
    }
    memcpy(&my_iobuf, iobuf, sizeof(my_iobuf));
    UNLOCK(&iobuf->lock);

    gf_proc_dump_build_key(key, key_prefix, "ref");
    gf_proc_dump_write(key, "%" GF_PRI_ATOMIC, GF_ATOMIC_GET(my_iobuf.ref));
    gf_proc_dump_build_key(key, key_prefix, "ptr");
    gf_proc_dump_write(key, "%p", my_iobuf.ptr);

out:
    return;
}

void
iobuf_arena_info_dump(struct iobuf_arena *iobuf_arena, const char *key_prefix)
{
    char key[GF_DUMP_MAX_BUF_LEN];
    int i = 1;
    struct iobuf *trav;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_arena, out);

    gf_proc_dump_build_key(key, key_prefix, "mem_base");
    gf_proc_dump_write(key, "%p", iobuf_arena->mem_base);
    gf_proc_dump_build_key(key, key_prefix, "active_cnt");
    gf_proc_dump_write(key, "%d", iobuf_arena->active_cnt);
    gf_proc_dump_build_key(key, key_prefix, "passive_cnt");
    gf_proc_dump_write(key, "%d", iobuf_arena->passive_cnt);
    gf_proc_dump_build_key(key, key_prefix, "alloc_cnt");
    gf_proc_dump_write(key, "%" PRIu64, iobuf_arena->alloc_cnt);
    gf_proc_dump_build_key(key, key_prefix, "max_active");
    gf_proc_dump_write(key, "%d", iobuf_arena->max_active);
    gf_proc_dump_build_key(key, key_prefix, "page_size");
    gf_proc_dump_write(key, "%" GF_PRI_SIZET, iobuf_arena->page_size);
    list_for_each_entry(trav, &iobuf_arena->active_list, list)
    {
        gf_proc_dump_build_key(key, key_prefix, "active_iobuf.%d", i++);
        gf_proc_dump_add_section("%s", key);
        iobuf_info_dump(trav, key);
    }

out:
    return;
}

void
iobuf_stats_dump(struct iobuf_pool *iobuf_pool)
{
    char msg[1024];
    struct iobuf_arena *trav = NULL;
    int i = 1;
    int j = 0;
    int ret = -1;

    GF_VALIDATE_OR_GOTO("iobuf", iobuf_pool, out);

    ret = pthread_mutex_trylock(&iobuf_pool->mutex);

    if (ret) {
        return;
    }
    gf_proc_dump_add_section("iobuf.global");
    gf_proc_dump_write("iobuf_pool", "%p", iobuf_pool);
    gf_proc_dump_write("iobuf_pool.default_page_size", "%" GF_PRI_SIZET,
                       iobuf_pool->default_page_size);
    gf_proc_dump_write("iobuf_pool.arena_size", "%" GF_PRI_SIZET,
                       iobuf_pool->arena_size);
    gf_proc_dump_write("iobuf_pool.arena_cnt", "%d", iobuf_pool->arena_cnt);
    gf_proc_dump_write("iobuf_pool.request_misses", "%" PRId64,
                       iobuf_pool->request_misses);

    for (j = 0; j < IOBUF_ARENA_MAX_INDEX; j++) {
        list_for_each_entry(trav, &iobuf_pool->arenas[j], list)
        {
            snprintf(msg, sizeof(msg), "arena.%d", i);
            gf_proc_dump_add_section("%s", msg);
            iobuf_arena_info_dump(trav, msg);
            i++;
        }
        list_for_each_entry(trav, &iobuf_pool->purge[j], list)
        {
            snprintf(msg, sizeof(msg), "purge.%d", i);
            gf_proc_dump_add_section("%s", msg);
            iobuf_arena_info_dump(trav, msg);
            i++;
        }
        list_for_each_entry(trav, &iobuf_pool->filled[j], list)
        {
            snprintf(msg, sizeof(msg), "filled.%d", i);
            gf_proc_dump_add_section("%s", msg);
            iobuf_arena_info_dump(trav, msg);
            i++;
        }
    }

    pthread_mutex_unlock(&iobuf_pool->mutex);

out:
    return;
}

void
iobuf_to_iovec(struct iobuf *iob, struct iovec *iov)
{
    GF_VALIDATE_OR_GOTO("iobuf", iob, out);
    GF_VALIDATE_OR_GOTO("iobuf", iov, out);

    iov->iov_base = iobuf_ptr(iob);
    iov->iov_len = iobuf_pagesize(iob);

out:
    return;
}

int
iobuf_copy(struct iobuf_pool *iobuf_pool, const struct iovec *iovec_src,
           int iovcnt, struct iobref **iobref, struct iobuf **iobuf,
           struct iovec *iov_dst)
{
    size_t size = -1;
    int ret = 0;

    size = iov_length(iovec_src, iovcnt);

    *iobuf = iobuf_get2(iobuf_pool, size);
    if (!(*iobuf)) {
        ret = -1;
        errno = ENOMEM;
        goto out;
    }

    *iobref = iobref_new();
    if (!(*iobref)) {
        iobuf_unref(*iobuf);
        errno = ENOMEM;
        ret = -1;
        goto out;
    }

    ret = iobref_add(*iobref, *iobuf);
    if (ret) {
        iobuf_unref(*iobuf);
        iobref_unref(*iobref);
        errno = ENOMEM;
        ret = -1;
        goto out;
    }

    iov_unload(iobuf_ptr(*iobuf), iovec_src, iovcnt);

    iov_dst->iov_base = iobuf_ptr(*iobuf);
    iov_dst->iov_len = size;

out:
    return ret;
}
