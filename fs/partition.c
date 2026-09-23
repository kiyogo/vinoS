#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kos/block.h>
#include <kos/block_cache.h>
#include <kos/heap.h>
#include <kos/log.h>
#include <kos/memory.h>
#include <kos/partition.h>

#define GPT_MIN_HEADER_SIZE 92U
#define GPT_MAX_ENTRIES 128U

/* IEEE 802.3 CRC32 Implementation */
uint32_t partition_crc32(const void *data, size_t length) {
    if (data == NULL || length == 0) {
        return 0;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint32_t)bytes[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static const char hex_digits[] = "0123456789ABCDEF";

void partition_format_guid(const uint8_t guid[16], char buffer[37]) {
    if (guid == NULL || buffer == NULL) return;
    /* Format: {3-2-1-0}-{5-4}-{7-6}-{8-9}-{10-11-12-13-14-15} (mixed-endian GPT standard) */
    size_t pos = 0;
    /* Data1 (uint32_t little-endian: bytes 3,2,1,0) */
    for (int i = 3; i >= 0; i--) {
        buffer[pos++] = hex_digits[(guid[i] >> 4) & 0xF];
        buffer[pos++] = hex_digits[guid[i] & 0xF];
    }
    buffer[pos++] = '-';
    /* Data2 (uint16_t little-endian: bytes 5,4) */
    for (int i = 5; i >= 4; i--) {
        buffer[pos++] = hex_digits[(guid[i] >> 4) & 0xF];
        buffer[pos++] = hex_digits[guid[i] & 0xF];
    }
    buffer[pos++] = '-';
    /* Data3 (uint16_t little-endian: bytes 7,6) */
    for (int i = 7; i >= 6; i--) {
        buffer[pos++] = hex_digits[(guid[i] >> 4) & 0xF];
        buffer[pos++] = hex_digits[guid[i] & 0xF];
    }
    buffer[pos++] = '-';
    /* Data4 (bytes 8,9) */
    for (int i = 8; i <= 9; i++) {
        buffer[pos++] = hex_digits[(guid[i] >> 4) & 0xF];
        buffer[pos++] = hex_digits[guid[i] & 0xF];
    }
    buffer[pos++] = '-';
    /* Data5 (bytes 10..15) */
    for (int i = 10; i <= 15; i++) {
        buffer[pos++] = hex_digits[(guid[i] >> 4) & 0xF];
        buffer[pos++] = hex_digits[guid[i] & 0xF];
    }
    buffer[pos] = '\0';
}

static bool guid_is_zero(const uint8_t guid[16]) {
    for (int i = 0; i < 16; i++) {
        if (guid[i] != 0) return false;
    }
    return true;
}

/* Parse GPT Partition Table */
static uint32_t partition_scan_gpt(struct block_device *dev) {
    if (dev == NULL || dev->block_size != BLOCK_DEFAULT_SECTOR_SIZE
        || dev->total_blocks < 34) {
        return 0;
    }

    uint8_t sector_buf[512];
    if (!block_read(dev, 1, 1, sector_buf)) {
        log_errorf("[partition] Failed to read GPT header (LBA 1) from %s", dev->name);
        return 0;
    }

    struct gpt_header *hdr = (struct gpt_header *)sector_buf;

    /* Check "EFI PART" signature */
    if (hdr->signature[0] != 'E' || hdr->signature[1] != 'F' ||
        hdr->signature[2] != 'I' || hdr->signature[3] != ' ' ||
        hdr->signature[4] != 'P' || hdr->signature[5] != 'A' ||
        hdr->signature[6] != 'R' || hdr->signature[7] != 'T') {
        return 0;
    }

    if (hdr->header_size < 92 || hdr->header_size > 512) {
        log_errorf("[partition] Invalid GPT header size: %u", hdr->header_size);
        return 0;
    }

    /* Verify Header CRC32 */
    uint32_t orig_crc = hdr->header_crc32;
    hdr->header_crc32 = 0;
    uint32_t calc_crc = partition_crc32(hdr, hdr->header_size);
    hdr->header_crc32 = orig_crc;

    if (calc_crc != orig_crc) {
        log_errorf("[partition] GPT Header CRC32 mismatch on %s (expected 0x%08X, calculated 0x%08X)",
                   dev->name, orig_crc, calc_crc);
        return 0;
    }

    char disk_guid_str[37];
    partition_format_guid(hdr->disk_guid, disk_guid_str);
    log_infof("[partition] Found valid GPT on %s (Disk GUID: %s, entries=%u)",
              dev->name, disk_guid_str, hdr->num_partition_entries);

    uint32_t num_entries = hdr->num_partition_entries;
    uint32_t entry_size = hdr->size_partition_entry;
    if (num_entries == 0 || num_entries > GPT_MAX_ENTRIES
        || entry_size != sizeof(struct gpt_partition_entry)) {
        log_errorf("[partition] Unsupported GPT entry geometry on %s (entries=%u size=%u)",
                   dev->name, num_entries, entry_size);
        return 0;
    }

    uint64_t entries_lba = hdr->partition_entries_lba;
    uint64_t entries_bytes64 = (uint64_t)num_entries * entry_size;
    uint64_t entries_sectors64 = (entries_bytes64 + dev->block_size - 1) / dev->block_size;
    if (entries_bytes64 == 0 || entries_bytes64 > UINT32_MAX
        || entries_sectors64 == 0 || entries_sectors64 > UINT32_MAX
        || entries_lba >= dev->total_blocks
        || entries_sectors64 > dev->total_blocks - entries_lba) {
        log_errorf("[partition] GPT entry array is outside %s", dev->name);
        return 0;
    }
    uint32_t entries_bytes = (uint32_t)entries_bytes64;
    uint32_t entries_sectors = (uint32_t)entries_sectors64;

    uint8_t *entries_buf = (uint8_t *)kmalloc(entries_sectors * dev->block_size);
    if (entries_buf == NULL) {
        log_error("[partition] Out of memory allocating GPT entries buffer!");
        return 0;
    }

    if (!block_read(dev, entries_lba, entries_sectors, entries_buf)) {
        log_errorf("[partition] Failed to read GPT partition entries from %s", dev->name);
        kfree(entries_buf);
        return 0;
    }

    /* Verify Partition Array CRC32 */
    uint32_t array_crc = partition_crc32(entries_buf, entries_bytes);
    if (hdr->partition_array_crc32 != 0 && array_crc != hdr->partition_array_crc32) {
        log_errorf("[partition] GPT Partition Array CRC32 mismatch on %s (0x%08X != 0x%08X)",
                   dev->name, array_crc, hdr->partition_array_crc32);
        kfree(entries_buf);
        return 0;
    }

    uint32_t valid_parts = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        struct gpt_partition_entry *e = (struct gpt_partition_entry *)(entries_buf + (i * entry_size));
        if (guid_is_zero(e->type_guid)) {
            continue;
        }
        if (e->starting_lba > e->ending_lba
            || e->starting_lba < hdr->first_usable_lba
            || e->ending_lba > hdr->last_usable_lba
            || e->ending_lba >= dev->total_blocks) {
            log_warnf("[partition] Skipping invalid GPT partition %u bounds (%llu..%llu)",
                      i + 1, (unsigned long long)e->starting_lba, (unsigned long long)e->ending_lba);
            continue;
        }

        uint64_t count = (e->ending_lba - e->starting_lba) + 1;
        bool overlaps = false;
        for (uint32_t previous = 0; previous < i; ++previous) {
            const struct gpt_partition_entry *other =
                (const struct gpt_partition_entry *)(entries_buf
                    + (uint64_t)previous * entry_size);
            if (!guid_is_zero(other->type_guid)
                && other->starting_lba <= other->ending_lba
                && other->starting_lba >= hdr->first_usable_lba
                && other->ending_lba <= hdr->last_usable_lba
                && other->starting_lba <= e->ending_lba
                && e->starting_lba <= other->ending_lba) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            log_warnf("[partition] Skipping overlapping GPT partition %u on %s",
                      i + 1, dev->name);
            continue;
        }
        valid_parts++;
        struct block_device *partition = block_partition_register(
            dev, valid_parts, e->starting_lba, count, 0xEE, e->unique_partition_guid);
        if (partition != NULL) {
            partition->bootable = (e->attributes & (1ULL << 2)) != 0;
            kmemcpy(partition->partition_type_guid, e->type_guid,
                    sizeof(partition->partition_type_guid));
            partition->has_type_guid = true;
        }
    }

    kfree(entries_buf);
    return valid_parts;
}

/* Parse MBR Partition Table */
static uint32_t partition_scan_mbr(struct block_device *dev) {
    uint8_t sector_buf[512];
    if (!block_read(dev, 0, 1, sector_buf)) {
        log_errorf("[partition] Failed to read MBR sector 0 from %s", dev->name);
        return 0;
    }

    struct mbr_sector *mbr = (struct mbr_sector *)sector_buf;
    if (mbr->signature != MBR_BOOT_SIGNATURE) {
        log_debugf("[partition] No valid MBR signature on %s (got 0x%04X, expect 0x%04X)",
                   dev->name, mbr->signature, MBR_BOOT_SIGNATURE);
        return 0;
    }

    /* Check if this is a GPT protective MBR (Type 0xEE on partition 0) */
    if (mbr->partitions[0].partition_type == MBR_GPT_PROTECTIVE_TYPE) {
        uint32_t gpt_count = partition_scan_gpt(dev);
        if (gpt_count > 0) {
            return gpt_count;
        }
        /* Never expose the protective MBR entry as an installable child,
         * including when the GPT metadata is empty or corrupt. */
        return 0;
    }

    uint32_t valid_parts = 0;
    for (uint32_t i = 0; i < MBR_PARTITION_COUNT; i++) {
        struct mbr_partition_entry *p = &mbr->partitions[i];
        if (p->partition_type == MBR_TYPE_EMPTY || p->sector_count == 0) {
            continue;
        }
        if ((uint64_t)p->start_lba + p->sector_count > dev->total_blocks) {
            log_warnf("[partition] Skipping out-of-bounds MBR partition %u on %s (LBA %u + %u > %llu)",
                      i + 1, dev->name, p->start_lba, p->sector_count, (unsigned long long)dev->total_blocks);
            continue;
        }

        bool overlaps = false;
        for (uint32_t previous = 0; previous < i; ++previous) {
            const struct mbr_partition_entry *other = &mbr->partitions[previous];
            if (other->partition_type != MBR_TYPE_EMPTY && other->sector_count != 0
                && (uint64_t)p->start_lba < (uint64_t)other->start_lba + other->sector_count
                && (uint64_t)other->start_lba < (uint64_t)p->start_lba + p->sector_count) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            log_warnf("[partition] Skipping overlapping MBR partition %u on %s",
                      i + 1, dev->name);
            continue;
        }
        valid_parts++;
        struct block_device *partition = block_partition_register(
            dev, valid_parts, p->start_lba, p->sector_count, p->partition_type, NULL);
        if (partition != NULL) partition->bootable = p->boot_indicator == 0x80;
    }

    if (valid_parts > 0) {
        log_infof("[partition] Found MBR with %u valid partitions on %s", valid_parts, dev->name);
    }
    return valid_parts;
}

uint32_t partition_scan(struct block_device *dev) {
    if (dev == NULL || dev->state != BLOCK_DEVICE_ONLINE
        || dev->type == BLOCK_TYPE_PARTITION
        || dev->block_size != BLOCK_DEFAULT_SECTOR_SIZE) {
        return 0;
    }
    log_infof("[partition] Scanning partition table on %s (%llu blocks)...",
              dev->name, (unsigned long long)dev->total_blocks);
    return partition_scan_mbr(dev);
}

/* ------------------------------------------------------------------------- */
/* Installer table inspection and mutation                                   */
/* ------------------------------------------------------------------------- */

static const uint8_t gpt_basic_data_guid[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7,
};

static uint32_t g_partition_guid_seed = 1;

static bool partition_mutable_disk(const struct block_device *dev) {
    return dev != NULL && dev->state == BLOCK_DEVICE_ONLINE
        && dev->parent == NULL && dev->type != BLOCK_TYPE_PARTITION
        && dev->block_size == BLOCK_DEFAULT_SECTOR_SIZE
        && dev->total_blocks > 0;
}

static bool partition_range_valid(const struct block_device *dev,
                                  uint64_t start_lba, uint64_t block_count) {
    return partition_mutable_disk(dev) && start_lba > 0 && block_count > 0
        && start_lba <= UINT64_MAX - block_count
        && start_lba + block_count <= dev->total_blocks;
}

static bool partition_ranges_overlap(uint64_t left_start, uint64_t left_count,
                                     uint64_t right_start, uint64_t right_count) {
    if (left_count == 0 || right_count == 0) return false;
    if (left_start > UINT64_MAX - left_count
        || right_start > UINT64_MAX - right_count) {
        /* Treat an overflowing range as overlapping so callers fail closed. */
        return true;
    }
    return left_start < right_start + right_count
        && right_start < left_start + left_count;
}

static bool partition_is_gpt_signature(const uint8_t *sector) {
    return sector != NULL && sector[0] == 'E' && sector[1] == 'F'
        && sector[2] == 'I' && sector[3] == ' '
        && sector[4] == 'P' && sector[5] == 'A'
        && sector[6] == 'R' && sector[7] == 'T';
}

static bool partition_gpt_read(const struct block_device *dev,
                               struct gpt_header *header,
                               uint8_t **entries_out,
                               uint32_t *entries_bytes_out,
                               uint32_t *entries_sectors_out) {
    if (!partition_mutable_disk(dev) || header == NULL || entries_out == NULL
        || entries_bytes_out == NULL || entries_sectors_out == NULL
        || dev->total_blocks < 34) {
        return false;
    }

    uint8_t sector[BLOCK_DEFAULT_SECTOR_SIZE];
    if (!block_read((struct block_device *)dev, 1, 1, sector)
        || !partition_is_gpt_signature(sector)) {
        return false;
    }

    struct gpt_header *raw = (struct gpt_header *)sector;
    if (raw->header_size < GPT_MIN_HEADER_SIZE
        || raw->header_size > BLOCK_DEFAULT_SECTOR_SIZE
        || raw->current_lba != 1
        || raw->backup_lba <= raw->current_lba
        || raw->backup_lba >= dev->total_blocks
        || raw->first_usable_lba > raw->last_usable_lba
        || raw->last_usable_lba >= raw->backup_lba
        || raw->num_partition_entries == 0
        || raw->num_partition_entries > GPT_MAX_ENTRIES
        || raw->size_partition_entry != sizeof(struct gpt_partition_entry)) {
        return false;
    }

    uint32_t original_crc = raw->header_crc32;
    raw->header_crc32 = 0;
    uint32_t calculated_crc = partition_crc32(sector, raw->header_size);
    raw->header_crc32 = original_crc;
    if (calculated_crc != original_crc) {
        return false;
    }

    uint64_t entries_bytes64 = (uint64_t)raw->num_partition_entries
        * raw->size_partition_entry;
    uint64_t entries_sectors64 = (entries_bytes64 + dev->block_size - 1)
        / dev->block_size;
    if (entries_bytes64 == 0 || entries_bytes64 > UINT32_MAX
        || entries_sectors64 == 0 || entries_sectors64 > UINT32_MAX
        || raw->partition_entries_lba >= dev->total_blocks
        || entries_sectors64 > dev->total_blocks - raw->partition_entries_lba) {
        return false;
    }

    uint32_t entries_bytes = (uint32_t)entries_bytes64;
    uint32_t entries_sectors = (uint32_t)entries_sectors64;
    uint8_t *entries = (uint8_t *)kmalloc((uint64_t)entries_sectors * dev->block_size);
    if (entries == NULL || !block_read((struct block_device *)dev,
                                        raw->partition_entries_lba,
                                        entries_sectors, entries)) {
        if (entries != NULL) kfree(entries);
        return false;
    }
    if (raw->partition_array_crc32 != 0
        && partition_crc32(entries, entries_bytes) != raw->partition_array_crc32) {
        kfree(entries);
        return false;
    }

    *header = *raw;
    *entries_out = entries;
    *entries_bytes_out = entries_bytes;
    *entries_sectors_out = entries_sectors;
    return true;
}

static bool partition_gpt_entries_valid(const struct block_device *dev,
                                        const struct gpt_header *header,
                                        const uint8_t *entries,
                                        uint32_t *count_out) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < header->num_partition_entries; ++index) {
        const struct gpt_partition_entry *entry =
            (const struct gpt_partition_entry *)(entries
                + (uint64_t)index * header->size_partition_entry);
        if (guid_is_zero(entry->type_guid)) continue;
        if (entry->starting_lba > entry->ending_lba
            || entry->starting_lba < header->first_usable_lba
            || entry->ending_lba > header->last_usable_lba
            || entry->ending_lba >= dev->total_blocks) {
            return false;
        }
        uint64_t block_count = entry->ending_lba - entry->starting_lba + 1;
        for (uint32_t other_index = 0; other_index < index; ++other_index) {
            const struct gpt_partition_entry *other =
                (const struct gpt_partition_entry *)(entries
                    + (uint64_t)other_index * header->size_partition_entry);
            if (!guid_is_zero(other->type_guid)
                && partition_ranges_overlap(entry->starting_lba, block_count,
                    other->starting_lba,
                    other->ending_lba - other->starting_lba + 1)) {
                return false;
            }
        }
        ++count;
    }
    if (count_out != NULL) *count_out = count;
    return true;
}

bool partition_table_inspect(const struct block_device *dev,
                             struct partition_table_info *info) {
    if (info == NULL || !partition_mutable_disk(dev)) return false;
    *info = (struct partition_table_info){0};

    uint8_t sector[BLOCK_DEFAULT_SECTOR_SIZE];
    if (!block_read((struct block_device *)dev, 0, 1, sector)) return false;
    const struct mbr_sector *mbr = (const struct mbr_sector *)sector;
    if (mbr->signature != MBR_BOOT_SIGNATURE) {
        info->type = PARTITION_TABLE_NONE;
        info->first_usable_lba = 1;
        info->last_usable_lba = dev->total_blocks - 1;
        return true;
    }

    if (mbr->partitions[0].partition_type == MBR_GPT_PROTECTIVE_TYPE) {
        struct gpt_header header;
        uint8_t *entries = NULL;
        uint32_t entries_bytes = 0;
        uint32_t entries_sectors = 0;
        if (!partition_gpt_read(dev, &header, &entries, &entries_bytes, &entries_sectors)) {
            return false;
        }
        uint32_t count = 0;
        bool valid = partition_gpt_entries_valid(dev, &header, entries, &count);
        if (valid) {
            info->type = PARTITION_TABLE_GPT;
            info->partition_count = count;
            info->first_usable_lba = header.first_usable_lba;
            info->last_usable_lba = header.last_usable_lba;
            info->partition_entries_lba = header.partition_entries_lba;
            info->backup_partition_entries_lba = header.backup_lba - entries_sectors;
            info->partition_entry_count = header.num_partition_entries;
            info->partition_entry_size = header.size_partition_entry;
        }
        (void)entries_bytes;
        kfree(entries);
        return valid;
    }

    uint32_t count = 0;
    for (uint32_t index = 0; index < MBR_PARTITION_COUNT; ++index) {
        const struct mbr_partition_entry *entry = &mbr->partitions[index];
        if (entry->partition_type == MBR_TYPE_EMPTY || entry->sector_count == 0) continue;
        if ((uint64_t)entry->start_lba + entry->sector_count > dev->total_blocks) {
            return false;
        }
        for (uint32_t other_index = 0; other_index < index; ++other_index) {
            const struct mbr_partition_entry *other = &mbr->partitions[other_index];
            if (other->partition_type != MBR_TYPE_EMPTY && other->sector_count != 0
                && partition_ranges_overlap(entry->start_lba, entry->sector_count,
                    other->start_lba, other->sector_count)) {
                return false;
            }
        }
        ++count;
    }
    info->type = PARTITION_TABLE_MBR;
    info->partition_count = count;
    info->first_usable_lba = 1;
    info->last_usable_lba = dev->total_blocks - 1;
    return true;
}

static bool partition_remove_children(struct block_device *dev) {
    for (;;) {
        struct block_device *child = NULL;
        for (uint32_t index = 0; index < block_device_count(); ++index) {
            struct block_device *candidate = block_device_get_at(index);
            if (candidate != NULL && candidate->parent == dev) {
                child = candidate;
                break;
            }
        }
        if (child == NULL) return true;
        if (!block_device_unregister(child)) return false;
    }
}

static bool partition_prepare_write(struct block_device *dev) {
    if (!partition_remove_children(dev)) return false;
    if (!block_cache_flush_device(dev)) return false;
    block_cache_invalidate_device(dev);
    return true;
}

static bool partition_request_is_free(const struct block_device *dev,
                                      uint64_t start_lba, uint64_t block_count) {
    for (uint32_t index = 0; index < block_device_count(); ++index) {
        const struct block_device *child = block_device_get_at(index);
        if (child == NULL || child->type != BLOCK_TYPE_PARTITION
            || child->parent != dev || child->total_blocks == 0) {
            continue;
        }
        if (partition_ranges_overlap(start_lba, block_count,
                                     child->start_lba, child->total_blocks)) {
            return false;
        }
    }
    return true;
}

bool partition_rescan(struct block_device *dev) {
    if (!partition_mutable_disk(dev) || !partition_remove_children(dev)) return false;
    return partition_scan(dev) != 0;
}

static void partition_make_guid(uint8_t guid[16], uint64_t start_lba,
                                uint64_t block_count) {
    uint32_t seed = g_partition_guid_seed++;
    kmemset(guid, 0, 16);
    kmemcpy(guid, &seed, sizeof(seed));
    kmemcpy(guid + 4, &start_lba, sizeof(start_lba));
    kmemcpy(guid + 12, &block_count, sizeof(uint32_t));
    guid[6] = (uint8_t)((guid[6] & 0x0F) | 0x40);
    guid[8] = (uint8_t)((guid[8] & 0x3F) | 0x80);
}

static bool partition_mbr_create(struct block_device *dev,
                                 const struct partition_create_request *request) {
    uint8_t sector[BLOCK_DEFAULT_SECTOR_SIZE];
    if (!block_read(dev, 0, 1, sector)) return false;
    struct mbr_sector *mbr = (struct mbr_sector *)sector;
    if (mbr->signature != MBR_BOOT_SIGNATURE) {
        kmemset(sector, 0, sizeof(sector));
        mbr = (struct mbr_sector *)sector;
        mbr->signature = MBR_BOOT_SIGNATURE;
    }
    if (mbr->partitions[0].partition_type == MBR_GPT_PROTECTIVE_TYPE
        || request->start_lba > UINT32_MAX || request->block_count > UINT32_MAX) {
        return false;
    }

    int free_slot = -1;
    for (uint32_t index = 0; index < MBR_PARTITION_COUNT; ++index) {
        struct mbr_partition_entry *entry = &mbr->partitions[index];
        if (entry->partition_type == MBR_TYPE_EMPTY || entry->sector_count == 0) {
            if (free_slot < 0) free_slot = (int)index;
            continue;
        }
        if (partition_ranges_overlap(request->start_lba, request->block_count,
                                     entry->start_lba, entry->sector_count)) {
            return false;
        }
    }
    if (free_slot < 0) return false;

    struct mbr_partition_entry *entry = &mbr->partitions[free_slot];
    kmemset(entry, 0, sizeof(*entry));
    entry->boot_indicator = request->bootable ? 0x80 : 0;
    entry->partition_type = request->mbr_type == MBR_TYPE_EMPTY
        ? MBR_TYPE_FAT32_LBA : request->mbr_type;
    entry->start_lba = (uint32_t)request->start_lba;
    entry->sector_count = (uint32_t)request->block_count;
    return block_write(dev, 0, 1, sector) && block_flush(dev);
}

static bool partition_mbr_delete(struct block_device *dev, uint64_t start_lba,
                                 uint64_t block_count) {
    uint8_t sector[BLOCK_DEFAULT_SECTOR_SIZE];
    if (!block_read(dev, 0, 1, sector)) return false;
    struct mbr_sector *mbr = (struct mbr_sector *)sector;
    for (uint32_t index = 0; index < MBR_PARTITION_COUNT; ++index) {
        struct mbr_partition_entry *entry = &mbr->partitions[index];
        if (entry->partition_type != MBR_TYPE_EMPTY
            && entry->start_lba == start_lba && entry->sector_count == block_count) {
            kmemset(entry, 0, sizeof(*entry));
            return block_write(dev, 0, 1, sector) && block_flush(dev);
        }
    }
    return false;
}

static void partition_gpt_fill_header_sector(uint8_t sector[BLOCK_DEFAULT_SECTOR_SIZE],
                                             struct gpt_header *header) {
    kmemset(sector, 0, BLOCK_DEFAULT_SECTOR_SIZE);
    header->header_crc32 = 0;
    kmemcpy(sector, header, sizeof(*header));
    header->header_crc32 = partition_crc32(sector, header->header_size);
    kmemcpy(sector, header, sizeof(*header));
}

static bool partition_gpt_write(struct block_device *dev, struct gpt_header *header,
                                uint8_t *entries, uint32_t entries_bytes,
                                uint32_t entries_sectors) {
    uint64_t backup_entries_lba = header->backup_lba - entries_sectors;
    if (header->backup_lba <= entries_sectors
        || backup_entries_lba <= header->last_usable_lba
        || header->partition_entries_lba >= dev->total_blocks
        || entries_sectors > dev->total_blocks - header->partition_entries_lba
        || entries_sectors > dev->total_blocks - backup_entries_lba) {
        return false;
    }

    header->partition_array_crc32 = partition_crc32(entries, entries_bytes);
    uint8_t header_sector[BLOCK_DEFAULT_SECTOR_SIZE];
    struct gpt_header primary = *header;
    primary.current_lba = 1;
    primary.partition_entries_lba = header->partition_entries_lba;
    struct gpt_header backup = *header;
    backup.current_lba = header->backup_lba;
    backup.partition_entries_lba = backup_entries_lba;

    if (!block_write(dev, primary.partition_entries_lba, entries_sectors, entries)
        || !block_write(dev, backup.partition_entries_lba, entries_sectors, entries)) {
        return false;
    }
    partition_gpt_fill_header_sector(header_sector, &backup);
    if (!block_write(dev, backup.current_lba, 1, header_sector)) return false;
    partition_gpt_fill_header_sector(header_sector, &primary);
    if (!block_write(dev, primary.current_lba, 1, header_sector)) return false;
    return block_flush(dev);
}

static bool partition_gpt_create(struct block_device *dev,
                                 const struct partition_create_request *request,
                                 const struct partition_table_info *existing) {
    struct gpt_header header;
    uint8_t *entries = NULL;
    uint32_t entries_bytes = 0;
    uint32_t entries_sectors = 0;
    bool new_table = existing->type == PARTITION_TABLE_NONE;

    if (new_table) {
        if (dev->total_blocks < 68) return false;
        header = (struct gpt_header){0};
        kmemcpy(header.signature, "EFI PART", 8);
        header.revision = 0x00010000;
        header.header_size = GPT_MIN_HEADER_SIZE;
        header.current_lba = 1;
        header.backup_lba = dev->total_blocks - 1;
        header.first_usable_lba = 34;
        header.last_usable_lba = dev->total_blocks - 34;
        partition_make_guid(header.disk_guid, dev->total_blocks, dev->block_size);
        header.partition_entries_lba = 2;
        header.num_partition_entries = GPT_MAX_ENTRIES;
        header.size_partition_entry = sizeof(struct gpt_partition_entry);
        entries_bytes = GPT_MAX_ENTRIES * sizeof(struct gpt_partition_entry);
        entries_sectors = (entries_bytes + BLOCK_DEFAULT_SECTOR_SIZE - 1)
            / BLOCK_DEFAULT_SECTOR_SIZE;
        entries = (uint8_t *)kmalloc((uint64_t)entries_sectors * dev->block_size);
        if (entries == NULL) return false;
        kmemset(entries, 0, (uint64_t)entries_sectors * dev->block_size);

        uint8_t protective_sector[BLOCK_DEFAULT_SECTOR_SIZE];
        kmemset(protective_sector, 0, sizeof(protective_sector));
        struct mbr_sector *protective = (struct mbr_sector *)protective_sector;
        protective->signature = MBR_BOOT_SIGNATURE;
        protective->partitions[0].partition_type = MBR_GPT_PROTECTIVE_TYPE;
        protective->partitions[0].start_lba = 1;
        protective->partitions[0].sector_count = dev->total_blocks - 1 > UINT32_MAX
            ? UINT32_MAX : (uint32_t)(dev->total_blocks - 1);
        if (!block_write(dev, 0, 1, protective_sector)) {
            kfree(entries);
            return false;
        }
    }
    else {
        if (!partition_gpt_read(dev, &header, &entries, &entries_bytes, &entries_sectors)) {
            return false;
        }
    }

    if (request->start_lba < header.first_usable_lba
        || request->start_lba + request->block_count - 1 > header.last_usable_lba) {
        kfree(entries);
        return false;
    }

    int free_slot = -1;
    for (uint32_t index = 0; index < header.num_partition_entries; ++index) {
        struct gpt_partition_entry *entry =
            (struct gpt_partition_entry *)(entries
                + (uint64_t)index * header.size_partition_entry);
        if (guid_is_zero(entry->type_guid)) {
            if (free_slot < 0) free_slot = (int)index;
            continue;
        }
        uint64_t existing_count = entry->ending_lba - entry->starting_lba + 1;
        if (partition_ranges_overlap(request->start_lba, request->block_count,
                                     entry->starting_lba, existing_count)) {
            kfree(entries);
            return false;
        }
    }
    if (free_slot < 0) {
        kfree(entries);
        return false;
    }

    struct gpt_partition_entry *entry =
        (struct gpt_partition_entry *)(entries
            + (uint64_t)free_slot * header.size_partition_entry);
    kmemset(entry, 0, sizeof(*entry));
    kmemcpy(entry->type_guid,
            guid_is_zero(request->type_guid) ? gpt_basic_data_guid : request->type_guid,
            16);
    if (guid_is_zero(request->unique_guid)) {
        partition_make_guid(entry->unique_partition_guid,
                            request->start_lba, request->block_count);
    }
    else {
        kmemcpy(entry->unique_partition_guid, request->unique_guid, 16);
    }
    entry->starting_lba = request->start_lba;
    entry->ending_lba = request->start_lba + request->block_count - 1;
    entry->attributes = request->bootable ? (1ULL << 2) : 0;
    entry->name[0] = 'V';
    entry->name[1] = 'I';
    entry->name[2] = 'N';
    entry->name[3] = 'O';
    bool result = partition_gpt_write(dev, &header, entries, entries_bytes, entries_sectors);
    kfree(entries);
    return result;
}

static bool partition_gpt_delete(struct block_device *dev, uint64_t start_lba,
                                 uint64_t block_count, const uint8_t guid[16],
                                 bool has_guid) {
    struct gpt_header header;
    uint8_t *entries = NULL;
    uint32_t entries_bytes = 0;
    uint32_t entries_sectors = 0;
    if (!partition_gpt_read(dev, &header, &entries, &entries_bytes, &entries_sectors)) {
        return false;
    }
    bool found = false;
    for (uint32_t index = 0; index < header.num_partition_entries; ++index) {
        struct gpt_partition_entry *entry =
            (struct gpt_partition_entry *)(entries
                + (uint64_t)index * header.size_partition_entry);
        if (!guid_is_zero(entry->type_guid)
            && entry->starting_lba == start_lba
            && entry->ending_lba == start_lba + block_count - 1
            && (!has_guid || kmemcmp(entry->unique_partition_guid, guid, 16) == 0)) {
            kmemset(entry, 0, sizeof(*entry));
            found = true;
            break;
        }
    }
    bool result = found
        && partition_gpt_write(dev, &header, entries, entries_bytes, entries_sectors);
    kfree(entries);
    return result;
}

bool partition_table_create(const struct partition_create_request *request,
                            struct block_device *dev, bool confirmed) {
    if (!confirmed || request == NULL || !partition_range_valid(dev,
            request->start_lba, request->block_count)
        || (request->table_type != PARTITION_TABLE_MBR
            && request->table_type != PARTITION_TABLE_GPT)) {
        return false;
    }

    struct partition_table_info info;
    if (!partition_table_inspect(dev, &info)
        || (info.type != PARTITION_TABLE_NONE && info.type != request->table_type)) {
        return false;
    }
    if (request->table_type == PARTITION_TABLE_MBR
        && (request->start_lba > UINT32_MAX || request->block_count > UINT32_MAX)) {
        return false;
    }
    if (info.type != PARTITION_TABLE_NONE
        && (request->start_lba < info.first_usable_lba
            || request->start_lba + request->block_count - 1 > info.last_usable_lba)) {
        return false;
    }
    if (!partition_request_is_free(dev, request->start_lba, request->block_count)) {
        return false;
    }
    if (!partition_prepare_write(dev)) return false;

    bool result = request->table_type == PARTITION_TABLE_MBR
        ? partition_mbr_create(dev, request)
        : partition_gpt_create(dev, request, &info);
    (void)partition_rescan(dev);
    return result;
}

bool partition_table_delete(struct block_device *partition, bool confirmed) {
    if (!confirmed || partition == NULL || partition->type != BLOCK_TYPE_PARTITION
        || partition->parent == NULL || partition->state != BLOCK_DEVICE_ONLINE
        || partition->total_blocks == 0
        || partition->start_lba > UINT64_MAX - partition->total_blocks) {
        return false;
    }
    struct block_device *dev = partition->parent;
    struct partition_table_info info;
    if (!partition_table_inspect(dev, &info) || info.type == PARTITION_TABLE_NONE) return false;

    uint64_t start_lba = partition->start_lba;
    uint64_t block_count = partition->total_blocks;
    uint8_t guid[16];
    kmemcpy(guid, partition->partition_guid, sizeof(guid));
    bool has_guid = partition->has_guid;
    if (!partition_prepare_write(dev)) return false;

    bool result = info.type == PARTITION_TABLE_MBR
        ? partition_mbr_delete(dev, start_lba, block_count)
        : partition_gpt_delete(dev, start_lba, block_count, guid, has_guid);
    (void)partition_rescan(dev);
    return result;
}
