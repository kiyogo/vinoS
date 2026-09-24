#ifndef KOS_INSTALLER_STORAGE_H
#define KOS_INSTALLER_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include <kos/installer_contracts.h>
#include <kos/partition.h>

#define INSTALLER_FILESYSTEM_NAME_LENGTH 16
#define INSTALLER_MIN_TARGET_BLOCKS 4096ULL

struct installer_disk_info {
    struct block_device *device;
    char name[BLOCK_NAME_MAX];
    uint64_t block_size;
    uint64_t total_blocks;
    uint64_t capacity_bytes;
    enum partition_table_type table_type;
    uint32_t partition_count;
};

struct installer_partition_info {
    struct block_device *device;
    struct block_device *parent;
    char name[BLOCK_NAME_MAX];
    uint64_t start_lba;
    uint64_t block_count;
    uint64_t capacity_bytes;
    uint32_t partition_index;
    uint8_t partition_type_mbr;
    bool has_guid;
    bool bootable;
    char filesystem[INSTALLER_FILESYSTEM_NAME_LENGTH];
};

uint32_t installer_storage_disk_count(void);
bool installer_storage_disk_at(uint32_t index, struct installer_disk_info *info);
uint32_t installer_storage_partition_count(const struct block_device *disk);
bool installer_storage_partition_at(const struct block_device *disk, uint32_t index,
                                    struct installer_partition_info *info);
bool installer_storage_find_free_extent(struct block_device *disk,
                                        enum partition_table_type table_type,
                                        uint64_t minimum_blocks,
                                        uint64_t *start_lba,
                                        uint64_t *block_count);

/* All mutating APIs require confirmed=true and validate the parent/geometry. */
bool installer_storage_create_partition(struct block_device *disk,
                                        enum partition_table_type table_type,
                                        uint64_t start_lba, uint64_t block_count,
                                        bool bootable, bool confirmed);
bool installer_storage_delete_partition(struct block_device *partition, bool confirmed);
bool installer_storage_format_partition(struct block_device *partition,
                                        enum partition_plan_filesystem filesystem,
                                        bool confirmed);

bool installer_storage_self_test(void);

#endif /* KOS_INSTALLER_STORAGE_H */
