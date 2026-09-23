#include <stdbool.h>
#include <stdint.h>

#include <kos/block_cache.h>
#include <kos/fat32.h>
#include <kos/installer_flow.h>
#include <kos/installer_storage.h>
#include <kos/log.h>
#include <kos/memory.h>
#include <kos/ramdisk.h>
#include <kos/volume.h>

static bool is_disk(const struct block_device *device) {
    return device != NULL && device->state == BLOCK_DEVICE_ONLINE
        && device->parent == NULL && device->type != BLOCK_TYPE_PARTITION;
}

static void copy_name(char *destination, uint64_t capacity, const char *source) {
    if (destination == NULL || capacity == 0) return;
    destination[0] = '\0';
    if (source == NULL) return;
    kstrncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
}

uint32_t installer_storage_disk_count(void) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < block_device_count(); ++index) {
        if (is_disk(block_device_get_at(index))) ++count;
    }
    return count;
}

bool installer_storage_disk_at(uint32_t index, struct installer_disk_info *info) {
    if (info == NULL) return false;
    uint32_t seen = 0;
    for (uint32_t cursor = 0; cursor < block_device_count(); ++cursor) {
        struct block_device *device = block_device_get_at(cursor);
        if (!is_disk(device)) continue;
        if (seen++ != index) continue;

        struct partition_table_info table;
        if (!partition_table_inspect(device, &table)) return false;
        *info = (struct installer_disk_info){
            .device = device,
            .block_size = device->block_size,
            .total_blocks = device->total_blocks,
            .capacity_bytes = device->capacity_bytes,
            .table_type = table.type,
            .partition_count = table.partition_count,
        };
        copy_name(info->name, sizeof(info->name), device->name);
        return true;
    }
    return false;
}

uint32_t installer_storage_partition_count(const struct block_device *disk) {
    if (!is_disk(disk)) return 0;
    uint32_t count = 0;
    for (uint32_t index = 0; index < block_device_count(); ++index) {
        struct block_device *device = block_device_get_at(index);
        if (device != NULL && device->type == BLOCK_TYPE_PARTITION && device->parent == disk) {
            ++count;
        }
    }
    return count;
}

bool installer_storage_partition_at(const struct block_device *disk, uint32_t index,
                                    struct installer_partition_info *info) {
    if (!is_disk(disk) || info == NULL) return false;
    uint32_t seen = 0;
    for (uint32_t cursor = 0; cursor < block_device_count(); ++cursor) {
        struct block_device *device = block_device_get_at(cursor);
        if (device == NULL || device->type != BLOCK_TYPE_PARTITION || device->parent != disk) {
            continue;
        }
        if (seen++ != index) continue;

        *info = (struct installer_partition_info){
            .device = device,
            .parent = device->parent,
            .start_lba = device->start_lba,
            .block_count = device->total_blocks,
            .capacity_bytes = device->capacity_bytes,
            .partition_index = device->partition_index,
            .partition_type_mbr = device->partition_type_mbr,
            .has_guid = device->has_guid,
            .bootable = device->bootable,
        };
        copy_name(info->name, sizeof(info->name), device->name);
        copy_name(info->filesystem, sizeof(info->filesystem),
                  fat32_probe(device) ? "FAT32" : "unknown");
        return true;
    }
    return false;
}

static struct block_device *find_partition_by_start(struct block_device *disk,
                                                    uint64_t start_lba) {
    for (uint32_t index = 0; index < block_device_count(); ++index) {
        struct block_device *device = block_device_get_at(index);
        if (device != NULL && device->type == BLOCK_TYPE_PARTITION
            && device->parent == disk && device->start_lba == start_lba) {
            return device;
        }
    }
    return NULL;
}

bool installer_storage_create_partition(struct block_device *disk,
                                        enum partition_table_type table_type,
                                        uint64_t start_lba, uint64_t block_count,
                                        bool bootable, bool confirmed) {
    if (!confirmed || !is_disk(disk)
        || (table_type != PARTITION_TABLE_MBR && table_type != PARTITION_TABLE_GPT)) {
        return false;
    }
    struct partition_create_request request = {
        .table_type = table_type,
        .start_lba = start_lba,
        .block_count = block_count,
        .mbr_type = bootable ? MBR_TYPE_EFI_SYSTEM : MBR_TYPE_FAT32_LBA,
        .bootable = bootable,
    };
    if (bootable && table_type == PARTITION_TABLE_GPT) {
        static const uint8_t efi_system_guid[16] = {
            0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
            0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
        };
        kmemcpy(request.type_guid, efi_system_guid, sizeof(efi_system_guid));
    }
    return partition_table_create(&request, disk, confirmed)
        && find_partition_by_start(disk, start_lba) != NULL;
}

bool installer_storage_delete_partition(struct block_device *partition, bool confirmed) {
    if (!confirmed || partition == NULL || !is_disk(partition->parent)
        || partition->type != BLOCK_TYPE_PARTITION) {
        return false;
    }
    return partition_table_delete(partition, confirmed);
}

bool installer_storage_format_partition(struct block_device *partition,
                                        enum partition_plan_filesystem filesystem,
                                        bool confirmed) {
    if (!confirmed || partition == NULL || !is_disk(partition->parent)
        || partition->type != BLOCK_TYPE_PARTITION || partition->state != BLOCK_DEVICE_ONLINE
        || filesystem != PARTITION_PLAN_FILESYSTEM_FAT32
        || partition->block_size != BLOCK_DEFAULT_SECTOR_SIZE || partition->total_blocks == 0
        || partition->start_lba > UINT64_MAX - partition->total_blocks
        || partition->start_lba + partition->total_blocks > partition->parent->total_blocks) {
        return false;
    }
    (void)volume_manager_unmount_device(partition);
    block_cache_invalidate_device(partition);
    return fat32_format(partition, "VINO_OS");
}

static int installer_storage_test_install_step(struct installer_flow *flow, void *context) {
    (void)flow;
    if (context == NULL) return -1;
    ++*(uint32_t *)context;
    return 0;
}

static bool installer_storage_check_partition(struct block_device *disk,
                                              enum partition_table_type table_type,
                                              uint64_t start_lba,
                                              uint64_t block_count) {
    if (!installer_storage_create_partition(disk, table_type, start_lba,
                                             block_count, true, true)) {
        return false;
    }
    if (start_lba == UINT64_MAX
        || installer_storage_create_partition(disk, table_type, start_lba + 1,
                                              1, false, true)) {
        return false;
    }
    struct block_device *partition = find_partition_by_start(disk, start_lba);
    if (partition == NULL
        || !installer_storage_format_partition(partition,
            PARTITION_PLAN_FILESYSTEM_FAT32, true)
        || !fat32_probe(partition)) {
        return false;
    }
    struct fat32_volume *volume = fat32_mount(partition);
    if (volume == NULL || !fat32_unmount(volume)) return false;

    struct installer_flow flow;
    uint32_t install_calls = 0;
    struct installer_engine engine = {
        .install_base_os = installer_storage_test_install_step,
        .install_bootloader = installer_storage_test_install_step,
        .context = &install_calls,
    };
    if (!installer_flow_init(&flow)
        || installer_flow_select_profile(&flow, "cli") != INSTALLER_FLOW_OK
        || installer_flow_select_disk(&flow, disk->name) != INSTALLER_FLOW_OK
        || installer_flow_select_partition(&flow, partition->name) != INSTALLER_FLOW_OK) {
        return false;
    }
    uint64_t original_start = partition->start_lba;
    partition->start_lba = UINT64_MAX;
    bool overflow_rejected = !installer_flow_is_valid(&flow);
    partition->start_lba = original_start;
    struct block_device *original_parent = partition->parent;
    partition->parent = NULL;
    bool invalid_parent_rejected = !installer_flow_is_valid(&flow);
    partition->parent = original_parent;
    if (!overflow_rejected || !invalid_parent_rejected
        || installer_flow_execute(&flow, &engine) != INSTALLER_FLOW_ERR_INVALID_STATE
        || install_calls != 0
        || installer_flow_format_selected_partition(&flow, true) != INSTALLER_FLOW_OK
        || installer_flow_execute(&flow, &engine) != INSTALLER_FLOW_OK
        || install_calls != 2) {
        return false;
    }

    if (installer_storage_delete_partition(partition, false)) return false;
    return installer_storage_delete_partition(partition, true)
        && installer_storage_partition_count(disk) == 0;
}

bool installer_storage_self_test(void) {
    bool ok = true;
    struct block_device *gpt_disk = ramdisk_create("installer_gpt_test",
                                                    32768ULL * 512, 512);
    if (gpt_disk == NULL
        || !installer_storage_check_partition(gpt_disk, PARTITION_TABLE_GPT,
                                              2048, 16000)
        || installer_storage_create_partition(gpt_disk, PARTITION_TABLE_GPT,
                                               UINT64_MAX, 1, false, true)) {
        ok = false;
    }
    if (gpt_disk != NULL) (void)ramdisk_destroy(gpt_disk);

    struct block_device *mbr_disk = ramdisk_create("installer_mbr_test",
                                                    16384ULL * 512, 512);
    if (mbr_disk == NULL
        || !installer_storage_check_partition(mbr_disk, PARTITION_TABLE_MBR,
                                              2048, 4096)) {
        ok = false;
    }
    if (mbr_disk != NULL) (void)ramdisk_destroy(mbr_disk);

    struct block_device *parent_disk = ramdisk_create("installer_parent_test",
                                                      4096ULL * 512, 512);
    struct block_device *other_disk = ramdisk_create("installer_other_test",
                                                     4096ULL * 512, 512);
    if (parent_disk == NULL || other_disk == NULL
        || !installer_storage_create_partition(parent_disk, PARTITION_TABLE_MBR,
                                               128, 1024, false, true)) {
        ok = false;
    }
    else {
        struct installer_partition_info partition;
        bool have_partition = installer_storage_partition_at(parent_disk, 0, &partition);
        if (!have_partition || installer_storage_partition_at(other_disk, 0, &partition)
            || installer_storage_delete_partition(partition.device, false)) {
            ok = false;
        }
        if (have_partition && !installer_storage_delete_partition(partition.device, true)) {
            ok = false;
        }
    }
    if (parent_disk != NULL) (void)ramdisk_destroy(parent_disk);
    if (other_disk != NULL) (void)ramdisk_destroy(other_disk);

    if (ok) log_info("[installer-storage] RAM disk partition/format self-test passed.");
    else log_error("[installer-storage] RAM disk partition/format self-test failed.");
    return ok;
}
