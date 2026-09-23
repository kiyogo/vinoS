#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kos/block.h>
#include <kos/block_cache.h>
#include <kos/heap.h>
#include <kos/log.h>
#include <kos/memory.h>
#include <kos/pmm.h>
#include <kos/spinlock.h>
#include <kos/vmm.h>
#include <kos/volume.h>

static struct block_device block_devices[BLOCK_MAX_DEVICES];
static struct list_head block_device_list = LIST_HEAD_INIT(block_device_list);
static kos_spinlock_t block_lock = SPINLOCK_INIT;
static uint32_t next_device_id = 1;
static bool block_initialized = false;

static void block_device_reset(struct block_device *dev) {
    kmemset(dev, 0, sizeof(*dev));
    dev->type = BLOCK_TYPE_UNKNOWN;
    dev->state = BLOCK_DEVICE_OFFLINE;
    list_init(&dev->list_node);
    spinlock_init(&dev->lock);
}

/* Helper string comparison */
static bool block_streq(const char *a, const char *b) {
    if (a == NULL || b == NULL) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

/* Helper string copy */
static void block_strcpy(char *dst, const char *src, size_t max_len) {
    if (dst == NULL || src == NULL || max_len == 0) return;
    size_t i = 0;
    while (src[i] != '\0' && i + 1 < max_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Partition Block Device Forwarding Operations */
static bool partition_read_blocks(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer) {
    if (dev == NULL || dev->parent == NULL) {
        return false;
    }
    if (lba + count > dev->total_blocks) {
        log_errorf("[block] Partition %s read beyond bounds (LBA %llu + %u > total %llu)",
                   dev->name, (unsigned long long)lba, count, (unsigned long long)dev->total_blocks);
        return false;
    }
    return block_read(dev->parent, dev->start_lba + lba, count, buffer);
}

static bool partition_write_blocks(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer) {
    if (dev == NULL || dev->parent == NULL) {
        return false;
    }
    if (lba + count > dev->total_blocks) {
        log_errorf("[block] Partition %s write beyond bounds (LBA %llu + %u > total %llu)",
                   dev->name, (unsigned long long)lba, count, (unsigned long long)dev->total_blocks);
        return false;
    }
    return block_write(dev->parent, dev->start_lba + lba, count, buffer);
}

static bool partition_flush(struct block_device *dev) {
    if (dev == NULL || dev->parent == NULL) {
        return false;
    }
    return block_flush(dev->parent);
}

static const struct block_device_ops partition_ops = {
    .read_blocks = partition_read_blocks,
    .write_blocks = partition_write_blocks,
    .flush = partition_flush,
    .destroy = NULL,
};

bool block_subsystem_init(void) {
    if (block_initialized) {
        return true;
    }
    uint64_t flags = spinlock_lock_irqsave(&block_lock);
    list_init(&block_device_list);
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; i++) {
        block_device_reset(&block_devices[i]);
    }
    block_initialized = true;
    spinlock_unlock_irqrestore(&block_lock, flags);
    log_info("[block] Block device subsystem initialized.");
    return true;
}

struct block_device *block_device_register(
    const char *name,
    enum block_device_type type,
    uint64_t block_size,
    uint64_t total_blocks,
    const struct block_device_ops *ops,
    void *private_data
) {
    if (name == NULL || ops == NULL || ops->read_blocks == NULL || block_size == 0 || total_blocks == 0) {
        return NULL;
    }

    if (!block_initialized) {
        block_subsystem_init();
    }

    uint64_t flags = spinlock_lock_irqsave(&block_lock);
    struct block_device *dev = NULL;
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; i++) {
        if (block_devices[i].state == BLOCK_DEVICE_OFFLINE && block_devices[i].id == 0) {
            dev = &block_devices[i];
            break;
        }
    }

    if (dev == NULL) {
        spinlock_unlock_irqrestore(&block_lock, flags);
        log_error("[block] Maximum block device capacity reached!");
        return NULL;
    }

    dev->id = next_device_id++;
    block_strcpy(dev->name, name, sizeof(dev->name));
    dev->type = type;
    dev->state = BLOCK_DEVICE_ONLINE;
    dev->block_size = block_size;
    dev->total_blocks = total_blocks;
    dev->capacity_bytes = block_size * total_blocks;
    dev->ops = ops;
    dev->private_data = private_data;
    spinlock_init(&dev->lock);
    dev->parent = NULL;
    dev->start_lba = 0;
    dev->partition_index = 0;
    dev->partition_type_mbr = 0;
    kmemset(dev->partition_type_guid, 0, sizeof(dev->partition_type_guid));
    dev->has_type_guid = false;
    dev->has_guid = false;
    dev->bootable = false;

    list_add_tail(&dev->list_node, &block_device_list);
    spinlock_unlock_irqrestore(&block_lock, flags);

    log_infof("[block] Registered %s (ID %u, type %d, %llu blocks of %llu bytes, %llu KiB total)",
              dev->name, dev->id, (int)dev->type,
              (unsigned long long)dev->total_blocks, (unsigned long long)dev->block_size,
              (unsigned long long)(dev->capacity_bytes / 1024));

    return dev;
}

struct block_device *block_partition_register(
    struct block_device *parent,
    uint32_t part_index,
    uint64_t start_lba,
    uint64_t block_count,
    uint8_t part_type_mbr,
    const uint8_t *guid
) {
    if (parent == NULL || parent->state != BLOCK_DEVICE_ONLINE || block_count == 0 ||
        start_lba >= parent->total_blocks || block_count > parent->total_blocks - start_lba) {
        log_error("[block] Invalid partition registration parameters!");
        return NULL;
    }

    char part_name[BLOCK_NAME_MAX];
    /* Format name: e.g. parent "blk0" -> "blk0p1" */
    size_t plen = 0;
    while (parent->name[plen] != '\0' && plen < sizeof(part_name) - 8) {
        part_name[plen] = parent->name[plen];
        plen++;
    }
    part_name[plen++] = 'p';
    if (part_index >= 10) {
        part_name[plen++] = (char)('0' + (part_index / 10));
    }
    part_name[plen++] = (char)('0' + (part_index % 10));
    part_name[plen] = '\0';

    uint64_t flags = spinlock_lock_irqsave(&block_lock);
    struct block_device *dev = NULL;
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; i++) {
        if (block_devices[i].state == BLOCK_DEVICE_OFFLINE && block_devices[i].id == 0) {
            dev = &block_devices[i];
            break;
        }
    }

    if (dev == NULL) {
        spinlock_unlock_irqrestore(&block_lock, flags);
        log_error("[block] No slot available for partition!");
        return NULL;
    }

    dev->id = next_device_id++;
    block_strcpy(dev->name, part_name, sizeof(dev->name));
    dev->type = BLOCK_TYPE_PARTITION;
    dev->state = BLOCK_DEVICE_ONLINE;
    dev->block_size = parent->block_size;
    dev->total_blocks = block_count;
    dev->capacity_bytes = parent->block_size * block_count;
    dev->ops = &partition_ops;
    dev->private_data = NULL;
    spinlock_init(&dev->lock);

    dev->parent = parent;
    dev->start_lba = start_lba;
    dev->partition_index = part_index;
    dev->partition_type_mbr = part_type_mbr;
    dev->bootable = false;

    if (guid != NULL) {
        kmemcpy(dev->partition_guid, guid, 16);
        dev->has_guid = true;
    } else {
        kmemset(dev->partition_guid, 0, 16);
        dev->has_guid = false;
    }
    kmemset(dev->partition_type_guid, 0, sizeof(dev->partition_type_guid));
    dev->has_type_guid = false;

    list_add_tail(&dev->list_node, &block_device_list);
    spinlock_unlock_irqrestore(&block_lock, flags);

    log_infof("[block] Registered partition %s (LBA %llu..%llu, %llu KiB)",
              dev->name, (unsigned long long)start_lba,
              (unsigned long long)(start_lba + block_count - 1),
              (unsigned long long)(dev->capacity_bytes / 1024));

    return dev;
}

bool block_device_unregister(struct block_device *dev) {
    if (dev == NULL) {
        return false;
    }

    struct block_device *children[BLOCK_MAX_DEVICES];
    uint32_t child_count = 0;

    uint64_t flags = spinlock_lock_irqsave(&block_lock);
    if (dev->state != BLOCK_DEVICE_ONLINE || dev->id == 0) {
        spinlock_unlock_irqrestore(&block_lock, flags);
        return false;
    }

    /* Collect child partitions before flushing their mounted filesystems. */
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; i++) {
        if (block_devices[i].parent == dev && block_devices[i].state == BLOCK_DEVICE_ONLINE) {
            children[child_count++] = &block_devices[i];
        }
    }

    spinlock_unlock_irqrestore(&block_lock, flags);

    /* Filesystems and dirty cache entries must be flushed while I/O is valid. */
    for (uint32_t i = 0; i < child_count; i++) {
        (void)volume_manager_unmount_device(children[i]);
        block_cache_invalidate_device(children[i]);
    }
    (void)volume_manager_unmount_device(dev);
    block_cache_invalidate_device(dev);

    flags = spinlock_lock_irqsave(&block_lock);
    if (dev->state != BLOCK_DEVICE_ONLINE || dev->id == 0) {
        spinlock_unlock_irqrestore(&block_lock, flags);
        return false;
    }

    /* Remove and fully reset every child before releasing the parent slot. */
    for (size_t i = 0; i < BLOCK_MAX_DEVICES; i++) {
        struct block_device *child = &block_devices[i];
        if (child->parent == dev && child->state == BLOCK_DEVICE_ONLINE) {
            log_infof("[block] Unregistered partition %s with parent %s", child->name, dev->name);
            list_del_init(&child->list_node);
            block_device_reset(child);
        }
    }

    list_del_init(&dev->list_node);
    const struct block_device_ops *ops = dev->ops;
    uint32_t device_id = dev->id;
    dev->state = BLOCK_DEVICE_OFFLINE;
    spinlock_unlock_irqrestore(&block_lock, flags);

    if (ops != NULL && ops->destroy != NULL) {
        ops->destroy(dev);
    }

    flags = spinlock_lock_irqsave(&block_lock);
    block_device_reset(dev);
    spinlock_unlock_irqrestore(&block_lock, flags);

    log_infof("[block] Unregistered device ID %u and %u child partition(s)",
              device_id, child_count);
    return true;
}

struct block_device *block_device_find_by_name(const char *name) {
    if (name == NULL) return NULL;
    uint64_t flags = spinlock_lock_irqsave(&block_lock);
    struct list_head *pos = block_device_list.next;
    while (pos != &block_device_list) {
        struct block_device *dev = list_entry(pos, struct block_device, list_node);
        if (dev->state == BLOCK_DEVICE_ONLINE && block_streq(dev->name, name)) {
            spinlock_unlock_irqrestore(&block_lock, flags);
            return dev;
        }
        pos = pos->next;
    }
    spinlock_unlock_irqrestore(&block_lock, flags);
    return NULL;
}

struct block_device *block_device_find_by_id(uint32_t id) {
    if (id == 0) return NULL;
    uint64_t flags = spinlock_lock_irqsave(&block_lock);
    struct list_head *pos = block_device_list.next;
    while (pos != &block_device_list) {
        struct block_device *dev = list_entry(pos, struct block_device, list_node);
        if (dev->state == BLOCK_DEVICE_ONLINE && dev->id == id) {
            spinlock_unlock_irqrestore(&block_lock, flags);
            return dev;
        }
        pos = pos->next;
    }
    spinlock_unlock_irqrestore(&block_lock, flags);
    return NULL;
}

uint32_t block_device_count(void) {
    uint32_t count = 0;
    uint64_t flags = spinlock_lock_irqsave(&block_lock);
    struct list_head *pos = block_device_list.next;
    while (pos != &block_device_list) {
        count++;
        pos = pos->next;
    }
    spinlock_unlock_irqrestore(&block_lock, flags);
    return count;
}

struct block_device *block_device_get_at(uint32_t index) {
    uint32_t cur = 0;
    uint64_t flags = spinlock_lock_irqsave(&block_lock);
    struct list_head *pos = block_device_list.next;
    while (pos != &block_device_list) {
        if (cur == index) {
            struct block_device *dev = list_entry(pos, struct block_device, list_node);
            spinlock_unlock_irqrestore(&block_lock, flags);
            return dev;
        }
        cur++;
        pos = pos->next;
    }
    spinlock_unlock_irqrestore(&block_lock, flags);
    return NULL;
}

bool block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer) {
    if (dev == NULL || buffer == NULL || count == 0) {
        return false;
    }
    if (dev->state != BLOCK_DEVICE_ONLINE) {
        log_errorf("[block] Cannot read from offline/error device %s", dev->name);
        return false;
    }
    if (lba >= dev->total_blocks || count > dev->total_blocks - lba) {
        log_errorf("[block] Read out of bounds on %s (LBA %llu + %u > total %llu)",
                   dev->name, (unsigned long long)lba, count, (unsigned long long)dev->total_blocks);
        return false;
    }
    if (dev->ops == NULL || dev->ops->read_blocks == NULL) {
        return false;
    }
    return dev->ops->read_blocks(dev, lba, count, buffer);
}

bool block_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer) {
    if (dev == NULL || buffer == NULL || count == 0) {
        return false;
    }
    if (dev->state != BLOCK_DEVICE_ONLINE) {
        log_errorf("[block] Cannot write to offline/error device %s", dev->name);
        return false;
    }
    if (lba >= dev->total_blocks || count > dev->total_blocks - lba) {
        log_errorf("[block] Write out of bounds on %s (LBA %llu + %u > total %llu)",
                   dev->name, (unsigned long long)lba, count, (unsigned long long)dev->total_blocks);
        return false;
    }
    if (dev->ops == NULL || dev->ops->write_blocks == NULL) {
        return false;
    }
    return dev->ops->write_blocks(dev, lba, count, buffer);
}

bool block_flush(struct block_device *dev) {
    if (dev == NULL) {
        return false;
    }
    if (dev->ops != NULL && dev->ops->flush != NULL) {
        return dev->ops->flush(dev);
    }
    return true;
}

bool block_alloc_dma_pages(uint32_t count, void **out_virt, uint64_t *out_phys) {
    if (count == 0 || out_virt == NULL || out_phys == NULL) {
        return false;
    }
    uint64_t phys = pmm_alloc_pages((uint64_t)count);
    if (phys == 0) {
        return false;
    }
    void *virt = vmm_physical_to_virtual(phys);
    if (virt == NULL) {
        pmm_free_pages(phys, (uint64_t)count);
        return false;
    }
    memset(virt, 0, (size_t)count * 4096);
    *out_virt = virt;
    *out_phys = phys;
    return true;
}

void block_free_dma_pages(void *virt, uint64_t phys, uint32_t count) {
    (void)virt;
    if (phys != 0 && count > 0) {
        pmm_free_pages(phys, (uint64_t)count);
    }
}
