/*
 * Vulkan GPU memory allocator (Phase 19: AAA foundation).
 *
 * Large VkDeviceMemory blocks per memory class, best-fit free-list
 * suballocation with coalescing, dedicated allocations by policy.
 * LumaC resources (lc_buffer/lc_image) bind subregions; callers
 * never see blocks, heaps, or memory types.
 *
 * Classes (buffer/image separated: correctness over clever packing,
 * PART T — sharing blocks across those classes risks granularity
 * aliasing faults):
 *   DEVICE_IMAGES   device-local optimal images
 *   DEVICE_BUFFERS  device-local GPU-only buffers
 *   UPLOAD          host-visible CPU->GPU (buffers + staging)
 *   READBACK        host-visible GPU->CPU (buffers + staging)
 *
 * Policy (documented):
 * - Lazy growth; default block 32 MiB device / 8 MiB host; a block
 *   always fits its first request (oversize requests grow the
 *   block, not the default).
 * - Dedicated when the aligned request exceeds half the class
 *   default (very large resources skip suballocation).
 * - Empty blocks: keep one warm per class, release the rest.
 * - Host blocks stay persistently mapped; suballocations address
 *   mapped_base + offset. Non-coherent ranges flush/invalidate
 *   aligned to nonCoherentAtomSize.
 * - One allocator mutex guards pools (PART AL). All other LumaC
 *   use stays single-threaded by contract (documented).
 */

#include <stdlib.h>
#include <string.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

/* Class defaults (see policy above). */
#define LC_VK_MEM_DEVICE_BLOCK (32u * 1024u * 1024u)
#define LC_VK_MEM_HOST_BLOCK (8u * 1024u * 1024u)

typedef struct lc_vk_free_node {
    uint64_t offset;
    uint64_t size;
    struct lc_vk_free_node *next;
} lc_vk_free_node;

/* Live-allocation record (always kept: double-free/overlap
 * detection is loud in every build; the O(n) overlap scan is
 * Debug-only). */
typedef struct lc_vk_live_node {
    uint64_t offset;
    uint64_t size;
    uint64_t id;
    struct lc_vk_live_node *next;
} lc_vk_live_node;

typedef struct lc_vk_mem_block {
    VkDeviceMemory memory;
    uint64_t size;
    uint32_t memory_type;
    lc_vk_mem_class cls;
    void *mapped; /* persistent map for host classes, else NULL */
    int coherent;
    uint64_t used;
    uint64_t live_count;
    lc_vk_free_node *free_list;
    lc_vk_live_node *live_list;
    struct lc_vk_mem_block *next;
    struct lc_vk_mem_block *prev;
} lc_vk_mem_block;

static uint64_t lc_vk_align_up(uint64_t v, uint64_t align) {
    uint64_t mask;

    if (align <= 1) {
        return v;
    }
    mask = align - 1;
    return (v + mask) & ~mask;
}

/* Centralized memory-type search: each preference tried in order
 * (required | pref[i]), then required alone. */
static int lc_vk_mem_find_type(VkPhysicalDevice physical,
                               uint32_t type_bits,
                               VkMemoryPropertyFlags required,
                               const VkMemoryPropertyFlags *prefs,
                               uint32_t pref_count, uint32_t *out_index) {
    VkPhysicalDeviceMemoryProperties props;
    uint32_t p;
    uint32_t i;

    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (p = 0; p < pref_count; p++) {
        VkMemoryPropertyFlags want =
            (VkMemoryPropertyFlags)(required | prefs[p]);

        for (i = 0; i < props.memoryTypeCount; i++) {
            if ((type_bits & (1u << i)) == 0) {
                continue;
            }
            if ((props.memoryTypes[i].propertyFlags & want) == want) {
                *out_index = i;
                return 1;
            }
        }
    }
    for (i = 0; i < props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) == 0) {
            continue;
        }
        if ((props.memoryTypes[i].propertyFlags & required) ==
            required) {
            *out_index = i;
            return 1;
        }
    }
    return 0;
}

int lc_vk_find_memory_type(VkPhysicalDevice physical, uint32_t type_bits,
                           VkMemoryPropertyFlags required,
                           VkMemoryPropertyFlags preferred,
                           uint32_t *out_index) {
    return lc_vk_mem_find_type(physical, type_bits, required,
                               &preferred, (preferred != 0) ? 1u : 0u,
                               out_index);
}

/* Class policy: ordered preferences + default block size. The
 * caller's `required` bits always apply on top. */
static void lc_vk_mem_class_policy(lc_vk_mem_class cls,
                                   VkMemoryPropertyFlags *prefs,
                                   uint32_t *pref_count,
                                   uint64_t *default_block) {
    static VkMemoryPropertyFlags upload_prefs[3];
    static VkMemoryPropertyFlags readback_prefs[4];
    static int init = 0;

    if (!init) {
        upload_prefs[0] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        upload_prefs[1] = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        upload_prefs[2] = 0;
        readback_prefs[0] = VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        readback_prefs[1] = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        readback_prefs[2] = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        readback_prefs[3] = 0;
        init = 1;
    }
    switch (cls) {
    case LC_VK_MEM_DEVICE_IMAGES:
        /* Historical behavior: prefer device-local, accept anything
         * the requirements allow (caller's required bits still
         * apply — here always 0 for images). */
        prefs[0] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        *pref_count = 1;
        *default_block = LC_VK_MEM_DEVICE_BLOCK;
        break;
    case LC_VK_MEM_DEVICE_BUFFERS:
        prefs[0] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        *pref_count = 1;
        *default_block = LC_VK_MEM_DEVICE_BLOCK;
        break;
    case LC_VK_MEM_UPLOAD:
        prefs[0] = upload_prefs[0];
        prefs[1] = upload_prefs[1];
        prefs[2] = upload_prefs[2];
        *pref_count = 3;
        *default_block = LC_VK_MEM_HOST_BLOCK;
        break;
    case LC_VK_MEM_READBACK:
    default:
        prefs[0] = readback_prefs[0];
        prefs[1] = readback_prefs[1];
        prefs[2] = readback_prefs[2];
        prefs[3] = readback_prefs[3];
        *pref_count = 4;
        *default_block = LC_VK_MEM_HOST_BLOCK;
        break;
    }
}

static void lc_vk_mem_lock(lc_device *device) {
#if defined(_WIN32) || defined(_WIN64)
    if (device->mem_mutex_init) {
        EnterCriticalSection(&device->mem_mutex);
    }
#else
    if (device->mem_mutex_init) {
        pthread_mutex_lock(&device->mem_mutex);
    }
#endif
}

static void lc_vk_mem_unlock(lc_device *device) {
#if defined(_WIN32) || defined(_WIN64)
    if (device->mem_mutex_init) {
        LeaveCriticalSection(&device->mem_mutex);
    }
#else
    if (device->mem_mutex_init) {
        pthread_mutex_unlock(&device->mem_mutex);
    }
#endif
}

/* Lazily cache limits needed by the allocator (atom size for
 * flush/invalidate alignment). Idempotent; caller holds no lock
 * (single-threaded init races resolve to identical values). */
static void lc_vk_mem_ensure_limits(lc_device *device) {
    if (device->mem_limits_ready) {
        return;
    }
    {
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceMemoryProperties mem;
        uint32_t i;

        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(device->physical_device, &props);
        device->mem_atom_size = props.limits.nonCoherentAtomSize;
        if (device->mem_atom_size == 0) {
            device->mem_atom_size = 1;
        }
        device->mem_granularity = props.limits.bufferImageGranularity;
        if (device->mem_granularity == 0) {
            device->mem_granularity = 1;
        }
        device->mem_max_image_dim = props.limits.maxImageDimension2D;
        device->mem_max_alloc = 0;
        memset(&mem, 0, sizeof(mem));
        vkGetPhysicalDeviceMemoryProperties(device->physical_device,
                                            &mem);
        for (i = 0; i < mem.memoryHeapCount; i++) {
            if (mem.memoryHeaps[i].size > device->mem_max_alloc) {
                device->mem_max_alloc = mem.memoryHeaps[i].size;
            }
        }
    }
    device->mem_limits_ready = 1;
}

static int lc_vk_mem_type_coherent(lc_device *device, uint32_t index) {
    VkPhysicalDeviceMemoryProperties props;

    vkGetPhysicalDeviceMemoryProperties(device->physical_device,
                                        &props);
    if (index >= props.memoryTypeCount) {
        return 1;
    }
    return (props.memoryTypes[index].propertyFlags &
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

static void lc_vk_mem_free_list_destroy(lc_vk_free_node *head) {
    while (head != NULL) {
        lc_vk_free_node *next = head->next;

        free(head);
        head = next;
    }
}

static void lc_vk_mem_live_list_destroy(lc_vk_live_node *head) {
    while (head != NULL) {
        lc_vk_live_node *next = head->next;

        free(head);
        head = next;
    }
}

static void lc_vk_mem_block_destroy(lc_device *device,
                                    lc_vk_mem_block *block) {
    if (block == NULL) {
        return;
    }
    if (block->mapped != NULL && device->device != VK_NULL_HANDLE) {
        vkUnmapMemory(device->device, block->memory);
        block->mapped = NULL;
    }
    if (block->memory != VK_NULL_HANDLE &&
        device->device != VK_NULL_HANDLE) {
        vkFreeMemory(device->device, block->memory, NULL);
        block->memory = VK_NULL_HANDLE;
    }
    lc_vk_mem_free_list_destroy(block->free_list);
    lc_vk_mem_live_list_destroy(block->live_list);
    free(block);
}

/* Create one block (VkDeviceMemory + persistent map for host
 * classes). Caller links it. */
static lc_result lc_vk_mem_block_create(lc_device *device,
                                        lc_vk_mem_class cls,
                                        uint32_t memory_type,
                                        uint64_t size,
                                        lc_vk_mem_block **out) {
    VkMemoryAllocateInfo alloc_info;
    lc_vk_mem_block *block = NULL;
    lc_vk_free_node *node = NULL;

    block = (lc_vk_mem_block *)calloc(1, sizeof(lc_vk_mem_block));
    if (block == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = (VkDeviceSize)size;
    alloc_info.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device->device, &alloc_info, NULL,
                         &block->memory) != VK_SUCCESS) {
        free(block);
        return LC_ERROR_OUT_OF_MEMORY;
    }
    block->size = size;
    block->memory_type = memory_type;
    block->cls = cls;
    block->coherent = lc_vk_mem_type_coherent(device, memory_type);
    if (cls == LC_VK_MEM_UPLOAD || cls == LC_VK_MEM_READBACK) {
        if (vkMapMemory(device->device, block->memory, 0, size, 0,
                        &block->mapped) != VK_SUCCESS) {
            vkFreeMemory(device->device, block->memory, NULL);
            free(block);
            return LC_ERROR_OUT_OF_MEMORY;
        }
    }
    node = (lc_vk_free_node *)malloc(sizeof(lc_vk_free_node));
    if (node == NULL) {
        if (block->mapped != NULL) {
            vkUnmapMemory(device->device, block->memory);
        }
        vkFreeMemory(device->device, block->memory, NULL);
        free(block);
        return LC_ERROR_OUT_OF_MEMORY;
    }
    node->offset = 0;
    node->size = size;
    node->next = NULL;
    block->free_list = node;
    *out = block;
    return LC_SUCCESS;
}

/* Best-fit allocate inside one block. Returns 1 with *out_offset
 * set (prefix gap kept free); Debug builds also scan live records
 * for overlap (loud abort on allocator corruption). */
static int lc_vk_mem_block_alloc(lc_vk_mem_block *block, uint64_t size,
                                 uint64_t align, uint64_t *out_offset) {
    lc_vk_free_node *prev = NULL;
    lc_vk_free_node *node = NULL;
    lc_vk_free_node *best_prev = NULL;
    lc_vk_free_node *best = NULL;
    uint64_t best_start = 0;
    uint64_t best_waste = 0;
    int have_best = 0;

    for (node = block->free_list; node != NULL; node = node->next) {
        uint64_t start = lc_vk_align_up(node->offset, align);
        uint64_t end = start + size;

        if (end < start || end > node->offset + node->size) {
            prev = node;
            continue; /* overflow or does not fit */
        }
        {
            uint64_t waste =
                (start - node->offset) +
                (node->offset + node->size - end);

            if (!have_best || waste < best_waste) {
                have_best = 1;
                best = node;
                best_prev = prev;
                best_start = start;
                best_waste = waste;
                if (waste == 0) {
                    break;
                }
            }
        }
        prev = node;
    }
    if (!have_best) {
        return 0;
    }
    {
        lc_vk_live_node *rec = (lc_vk_live_node *)malloc(
            sizeof(lc_vk_live_node));
        uint64_t node_end;
        uint64_t alloc_end;
        uint64_t prefix;
        uint64_t suffix;
        if (rec == NULL) {
            return 0; /* host OOM before any mutation: clean failure */
        }
        node_end = best->offset + best->size;
        alloc_end = best_start + size;
        prefix = best_start - best->offset;
        suffix = node_end - alloc_end;

        if (prefix > 0 && suffix > 0) {
            /* Shrink in place to the prefix; new node for suffix. */
            lc_vk_free_node *tail = NULL;

            best->size = prefix;
            tail = (lc_vk_free_node *)malloc(sizeof(lc_vk_free_node));
            if (tail == NULL) {
                best->size = node_end - best->offset; /* restore */
                free(rec);
                return 0;
            }
            tail->offset = alloc_end;
            tail->size = suffix;
            tail->next = best->next;
            best->next = tail;
        } else if (prefix > 0) {
            best->size = prefix;
        } else if (suffix > 0) {
            best->offset = alloc_end;
            best->size = suffix;
        } else {
            /* Exact fit: unlink the node. */
            if (best_prev != NULL) {
                best_prev->next = best->next;
            } else {
                block->free_list = best->next;
            }
            free(best);
        }
        block->used += size;
        block->live_count++;
        rec->offset = best_start;
        rec->size = size;
        rec->id = (uint64_t)(uintptr_t)rec;
        rec->next = block->live_list;
        block->live_list = rec;
    }
    *out_offset = best_start;
#ifndef NDEBUG
    {
        /* Overlap scan: loud abort on allocator corruption. */
        lc_vk_live_node *it;
        int seen_self = 0;

        for (it = block->live_list; it != NULL; it = it->next) {
            if (it->offset == best_start && it->size == size && !seen_self) {
                seen_self = 1; /* the record just committed above */
                continue;
            }
            if (!(best_start + size <= it->offset ||
                  it->offset + it->size <= best_start)) {
                fprintf(stderr,
                        "[lumac] allocator corruption: overlapping "
                        "allocation (block %p)\n",
                        (void *)block);
                abort();
            }
        }
    }
#endif
    return 1;
}

/* Return a range (coalesce neighbors). Unknown range = double free:
 * loud abort (Section 13: no silent corruption). */
static void lc_vk_mem_block_free(lc_device *device,
                                 lc_vk_mem_block *block, uint64_t offset,
                                 uint64_t size) {
    lc_vk_live_node *prev_live = NULL;
    lc_vk_live_node *live = NULL;
    lc_vk_free_node *prev = NULL;
    lc_vk_free_node *node = NULL;
    lc_vk_free_node *fresh = NULL;

    (void)device;
    for (live = block->live_list; live != NULL; live = live->next) {
        if (live->offset == offset && live->size == size) {
            break;
        }
        prev_live = live;
    }
    if (live == NULL) {
        fprintf(stderr,
                "[lumac] allocator corruption: free of unknown range "
                "%llu+%llu (double free?)\n",
                (unsigned long long)offset, (unsigned long long)size);
        abort();
    }
    if (prev_live != NULL) {
        prev_live->next = live->next;
    } else {
        block->live_list = live->next;
    }
    free(live);
    fresh = (lc_vk_free_node *)malloc(sizeof(lc_vk_free_node));
    if (fresh == NULL) {
        fprintf(stderr, "[lumac] allocator out of host memory\n");
        abort();
    }
    fresh->offset = offset;
    fresh->size = size;
    /* Sorted insert with neighbor coalescing. */
    for (node = block->free_list; node != NULL; node = node->next) {
        if (node->offset > offset) {
            break;
        }
        prev = node;
    }
    fresh->next = node;
    if (prev != NULL) {
        prev->next = fresh;
    } else {
        block->free_list = fresh;
    }
    if (prev != NULL && prev->offset + prev->size == fresh->offset) {
        prev->size += fresh->size;
        prev->next = fresh->next;
        free(fresh);
        fresh = prev;
    }
    if (node != NULL && fresh->offset + fresh->size == node->offset) {
        fresh->size += node->size;
        fresh->next = node->next;
        free(node);
    }
    block->used -= size;
    block->live_count--;
}

/* Release empty blocks beyond the one warm reserve per class. */
static void lc_vk_mem_reclaim(lc_device *device, lc_vk_mem_class cls) {
    lc_vk_mem_pool *pool = &device->mem_pools[cls];
    lc_vk_mem_block *it;
    lc_vk_mem_block *next;
    int empty_seen = 0;

    for (it = pool->blocks; it != NULL; it = next) {
        next = it->next;
        if (it->live_count == 0) {
            if (empty_seen) {
                if (it->prev != NULL) {
                    it->prev->next = it->next;
                } else {
                    pool->blocks = it->next;
                }
                if (it->next != NULL) {
                    it->next->prev = it->prev;
                }
                lc_vk_mem_block_destroy(device, it);
            } else {
                empty_seen = 1;
            }
        }
    }
}

/* Core suballocation (lock held by caller): find fitting block of a
 * compatible memory type, else grow. */
static lc_result lc_vk_mem_alloc_locked(
    lc_device *device, lc_vk_mem_class cls, uint32_t type_bits,
    VkMemoryPropertyFlags required, const VkMemoryPropertyFlags *prefs,
    uint32_t pref_count, uint64_t size, uint64_t align,
    lc_vk_mem_binding *out) {
    VkMemoryPropertyFlags policy_prefs[4];
    uint32_t policy_count = 0;
    uint64_t default_block = 0;
    lc_vk_mem_pool *pool;
    lc_vk_mem_block *it;
    uint64_t aligned = 0;

    lc_vk_mem_class_policy(cls, policy_prefs, &policy_count,
                           &default_block);
    /* Caller prefs (from memory_usage) override class prefs when
     * given; class prefs already encode them — `prefs` here is the
     * class list for signature symmetry with future overrides. */
    (void)prefs;
    (void)pref_count;
    if (align < 1) {
        align = 1;
    }
    aligned = lc_vk_align_up(size, align);
    /* Early OOM: no heap can back this (avoids a doomed Vulkan
     * call and its validation noise). */
    if (device->mem_max_alloc > 0 && aligned > device->mem_max_alloc) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    pool = &device->mem_pools[cls];
    /* Dedicated policy: oversize requests bypass suballocation. */
    if (aligned > default_block / 2) {
        uint32_t index = 0;
        VkMemoryAllocateInfo info;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = NULL;

        if (!lc_vk_mem_find_type(device->physical_device, type_bits,
                                 required, policy_prefs, policy_count,
                                 &index)) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = (VkDeviceSize)aligned;
        info.memoryTypeIndex = index;
        if (vkAllocateMemory(device->device, &info, NULL, &memory) !=
            VK_SUCCESS) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        if (cls == LC_VK_MEM_UPLOAD || cls == LC_VK_MEM_READBACK) {
            if (vkMapMemory(device->device, memory, 0,
                            (VkDeviceSize)aligned, 0,
                            &mapped) != VK_SUCCESS) {
                vkFreeMemory(device->device, memory, NULL);
                return LC_ERROR_OUT_OF_MEMORY;
            }
        }
        out->memory = memory;
        out->offset = 0;
        out->size = aligned;
        out->mapped = mapped;
        out->coherent =
            lc_vk_mem_type_coherent(device, index);
        out->dedicated = 1;
        out->block = NULL;
        out->memory_type = index;
        out->memory_class = cls;
        device->mem_dedicated_count++;
        return LC_SUCCESS;
    }
    /* Existing blocks (type-compatible only). */
    for (it = pool->blocks; it != NULL; it = it->next) {
        VkPhysicalDeviceMemoryProperties props;
        uint64_t off = 0;

        vkGetPhysicalDeviceMemoryProperties(device->physical_device,
                                            &props);
        if ((type_bits & (1u << it->memory_type)) == 0) {
            continue;
        }
        if ((props.memoryTypes[it->memory_type].propertyFlags &
             required) != required) {
            continue;
        }
        if (lc_vk_mem_block_alloc(it, aligned, align, &off)) {
            out->memory = it->memory;
            out->offset = off;
            out->size = aligned;
            out->mapped = (it->mapped != NULL)
                              ? (void *)((char *)it->mapped + off)
                              : NULL;
            out->coherent = it->coherent;
            out->dedicated = 0;
            out->block = it;
            out->memory_type = it->memory_type;
            out->memory_class = cls;
            return LC_SUCCESS;
        }
    }
    /* Grow: new block sized to fit (at least the default). */
    {
        uint32_t index = 0;
        uint64_t block_size = default_block;
        lc_vk_mem_block *fresh = NULL;
        uint64_t off = 0;
        lc_result res;

        if (block_size < aligned) {
            block_size = aligned;
        }
        if (!lc_vk_mem_find_type(device->physical_device, type_bits,
                                 required, policy_prefs, policy_count,
                                 &index)) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        res = lc_vk_mem_block_create(device, cls, index, block_size,
                                     &fresh);
        if (res != LC_SUCCESS) {
            return res;
        }
        fresh->next = pool->blocks;
        fresh->prev = NULL;
        if (pool->blocks != NULL) {
            pool->blocks->prev = fresh;
        }
        pool->blocks = fresh;
        if (!lc_vk_mem_block_alloc(fresh, aligned, align, &off)) {
            /* Practically unreachable (fresh block fits by construction),
             * but host-OOM inside block_alloc must not leak the linked
             * fresh block: unlink and destroy before reporting OOM. */
            if (fresh->prev != NULL) {
                fresh->prev->next = fresh->next;
            } else {
                pool->blocks = fresh->next;
            }
            if (fresh->next != NULL) {
                fresh->next->prev = fresh->prev;
            }
            lc_vk_mem_block_destroy(device, fresh);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        out->memory = fresh->memory;
        out->offset = off;
        out->size = aligned;
        out->mapped = (fresh->mapped != NULL)
                          ? (void *)((char *)fresh->mapped + off)
                          : NULL;
        out->coherent = fresh->coherent;
        out->dedicated = 0;
        out->block = fresh;
        out->memory_type = index;
        out->memory_class = cls;
        return LC_SUCCESS;
    }
}

lc_result lc_vk_mem_alloc(lc_device *device, lc_vk_mem_class cls,
                          uint32_t type_bits,
                          VkMemoryPropertyFlags required,
                          uint64_t size, uint64_t align,
                          lc_vk_mem_binding *out) {
    lc_result res;

    if (device == NULL || out == NULL || size == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (cls < 0 || cls >= LC_VK_MEM_CLASS_COUNT) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    memset(out, 0, sizeof(*out));
    lc_vk_mem_lock(device);
    lc_vk_mem_ensure_limits(device);
    res = lc_vk_mem_alloc_locked(device, cls, type_bits, required, NULL,
                                 0, size, align, out);
    lc_vk_mem_unlock(device);
    return res;
}

void lc_vk_mem_free(lc_device *device, lc_vk_mem_binding *binding) {
    if (device == NULL || binding == NULL) {
        return;
    }
    lc_vk_mem_lock(device);
    if (binding->dedicated) {
        if (binding->mapped != NULL &&
            device->device != VK_NULL_HANDLE) {
            vkUnmapMemory(device->device, binding->memory);
        }
        if (binding->memory != VK_NULL_HANDLE &&
            device->device != VK_NULL_HANDLE) {
            vkFreeMemory(device->device, binding->memory, NULL);
        }
        if (device->mem_dedicated_count > 0) {
            device->mem_dedicated_count--;
        }
    } else if (binding->block != NULL) {
        lc_vk_mem_class cls = binding->block->cls;

        lc_vk_mem_block_free(device, binding->block, binding->offset,
                             binding->size);
        lc_vk_mem_reclaim(device, cls);
    }
    memset(binding, 0, sizeof(*binding));
    lc_vk_mem_unlock(device);
}

/* Flush/invalidate a subrange (no-op when coherent or unmapped).
 * Ranges align outward to nonCoherentAtomSize for non-coherent. */
static lc_result lc_vk_mem_sync_range(lc_device *device,
                                      const lc_vk_mem_binding *binding,
                                      uint64_t offset, uint64_t size,
                                      int flush) {
    VkMappedMemoryRange range;
    uint64_t atom;
    uint64_t begin;
    uint64_t end;

    if (device == NULL || binding == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (binding->mapped == NULL || binding->coherent) {
        return LC_SUCCESS;
    }
    if (device->device == VK_NULL_HANDLE ||
        binding->memory == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (offset + size < offset || offset + size > binding->size) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    atom = device->mem_atom_size;
    if (atom <= 1) {
        atom = 1;
    }
    begin = (binding->offset + offset) / atom * atom;
    end = lc_vk_align_up(binding->offset + offset + size, atom);
    memset(&range, 0, sizeof(range));
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = binding->memory;
    range.offset = (VkDeviceSize)begin;
    range.size = (VkDeviceSize)(end - begin);
    if (flush) {
        if (vkFlushMappedMemoryRanges(device->device, 1, &range) !=
            VK_SUCCESS) {
            return LC_ERROR_UNKNOWN;
        }
    } else {
        if (vkInvalidateMappedMemoryRanges(device->device, 1, &range) !=
            VK_SUCCESS) {
            return LC_ERROR_UNKNOWN;
        }
    }
    return LC_SUCCESS;
}

lc_result lc_vk_mem_flush(lc_device *device,
                          const lc_vk_mem_binding *binding, uint64_t offset,
                          uint64_t size) {
    return lc_vk_mem_sync_range(device, binding, offset, size, 1);
}

lc_result lc_vk_mem_invalidate(lc_device *device,
                               const lc_vk_mem_binding *binding,
                               uint64_t offset, uint64_t size) {
    return lc_vk_mem_sync_range(device, binding, offset, size, 0);
}

/* Transient staging buffer from a host pool (PART Z): VkBuffer +
 * suballocated bind + persistent pointer. No dedicated VkDeviceMemory
 * churn per upload. Caller must guarantee GPU completion before
 * release (all staging uses are immediate-submit + waited). */
lc_result lc_vk_stage_acquire(lc_device *device, uint64_t size,
                              int upload, VkBufferUsageFlags usage,
                              VkBuffer *out_buffer,
                              lc_vk_mem_binding *out_binding,
                              void **out_mapped) {
    VkBufferCreateInfo info;
    VkMemoryRequirements reqs;
    VkBuffer buffer = VK_NULL_HANDLE;
    lc_result res;

    if (device == NULL || size == 0 || out_buffer == NULL ||
        out_binding == NULL || out_mapped == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = (VkDeviceSize)size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device->device, &info, NULL, &buffer) !=
        VK_SUCCESS) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    vkGetBufferMemoryRequirements(device->device, buffer, &reqs);
    res = lc_vk_mem_alloc(
        device, upload ? LC_VK_MEM_UPLOAD : LC_VK_MEM_READBACK,
        reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        reqs.size, reqs.alignment, out_binding);
    if (res != LC_SUCCESS) {
        vkDestroyBuffer(device->device, buffer, NULL);
        return res;
    }
    if (vkBindBufferMemory(device->device, buffer, out_binding->memory,
                           (VkDeviceSize)out_binding->offset) !=
        VK_SUCCESS) {
        lc_vk_mem_free(device, out_binding);
        vkDestroyBuffer(device->device, buffer, NULL);
        return LC_ERROR_UNKNOWN;
    }
    *out_buffer = buffer;
    *out_mapped = out_binding->mapped;
    return LC_SUCCESS;
}

void lc_vk_stage_release(lc_device *device, VkBuffer buffer,
                         lc_vk_mem_binding *binding) {
    if (device == NULL) {
        return;
    }
    if (buffer != VK_NULL_HANDLE && device->device != VK_NULL_HANDLE) {
        vkDestroyBuffer(device->device, buffer, NULL);
    }
    lc_vk_mem_free(device, binding);
}

void lc_vk_mem_teardown(lc_device *device) {    lc_vk_mem_class c;

    if (device == NULL) {
        return;
    }
    lc_vk_mem_lock(device);
    for (c = 0; c < LC_VK_MEM_CLASS_COUNT; c++) {
        lc_vk_mem_block *it = device->mem_pools[c].blocks;
        lc_vk_mem_block *next;

        for (; it != NULL; it = next) {
            next = it->next;
#ifndef NDEBUG
            if (it->live_count != 0) {
                fprintf(stderr,
                        "[lumac] memory leak: %llu live allocations "
                        "at device teardown\n",
                        (unsigned long long)it->live_count);
            }
#endif
            it->prev = NULL;
            it->next = NULL;
            lc_vk_mem_block_destroy(device, it);
        }
        device->mem_pools[c].blocks = NULL;
    }
    lc_vk_mem_unlock(device);
#if defined(_WIN32) || defined(_WIN64)
    if (device->mem_mutex_init) {
        DeleteCriticalSection(&device->mem_mutex);
        device->mem_mutex_init = 0;
    }
#else
    if (device->mem_mutex_init) {
        pthread_mutex_destroy(&device->mem_mutex);
        device->mem_mutex_init = 0;
    }
#endif
}

void lc_vk_mem_stats(const lc_device *device, lc_memory_stats *out) {
    lc_vk_mem_class c;

    memset(out, 0, sizeof(*out));
    if (device == NULL) {
        return;
    }
    lc_vk_mem_lock((lc_device *)device);
    for (c = 0; c < LC_VK_MEM_CLASS_COUNT; c++) {
        const lc_vk_mem_block *it;
        uint64_t cls_alloc = 0;
        uint64_t cls_used = 0;
        uint64_t cls_live = 0;
        uint64_t cls_blocks = 0;
        uint64_t cls_largest = 0;

        for (it = device->mem_pools[c].blocks; it != NULL;
             it = it->next) {
            const lc_vk_free_node *fn;

            cls_alloc += it->size;
            cls_used += it->used;
            cls_blocks++;
            for (fn = it->free_list; fn != NULL; fn = fn->next) {
                if (fn->size > cls_largest) {
                    cls_largest = fn->size;
                }
            }
            cls_live += it->live_count;
        }
        if (c == LC_VK_MEM_DEVICE_IMAGES ||
            c == LC_VK_MEM_DEVICE_BUFFERS) {
            out->device_local_allocated += cls_alloc;
            out->device_local_used += cls_used;
        } else {
            out->host_visible_allocated += cls_alloc;
            out->host_visible_used += cls_used;
        }
        out->allocation_count += cls_live;
        out->block_count += cls_blocks;
        if (cls_largest > out->largest_free_range) {
            out->largest_free_range = cls_largest;
        }
    }
    out->device_local_free = out->device_local_allocated -
                             out->device_local_used;
    out->host_visible_free = out->host_visible_allocated -
                             out->host_visible_used;
    out->dedicated_allocation_count = device->mem_dedicated_count;
    lc_vk_mem_unlock((lc_device *)device);
}
