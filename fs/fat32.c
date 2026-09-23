#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kos/block.h>
#include <kos/block_cache.h>
#include <kos/heap.h>
#include <kos/log.h>
#include <kos/memory.h>
#include <kos/spinlock.h>
#include <kos/vfs.h>
#include <kos/fat32.h>

#define FAT32_MAX_VOLUMES 8
#define FAT32_MAX_PATH_LEN 256
#define FAT32_MAX_CACHED_FILES 128

struct fat32_cached_file {
    char path[FAT32_MAX_PATH_LEN];
    uint8_t *data;
    uint64_t size;
    struct vfs_file vfs_f;
    bool valid;
};

struct fat32_volume {
    struct block_device *dev;
    struct fat32_bpb bpb;
    uint64_t fat_start_lba;
    uint64_t cluster_start_lba;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t total_clusters;
    uint32_t root_cluster;

    kos_spinlock_t lock;
    struct vfs_backend backend;

    /* Cached file list for VFS backend compliance */
    struct fat32_cached_file cached_files[FAT32_MAX_CACHED_FILES];
    uint64_t cached_file_count;

    bool mounted;
};

static struct fat32_volume g_fat_volumes[FAT32_MAX_VOLUMES];
static uint32_t g_fat_volume_count = 0;

/* Character conversion helpers */
static inline char to_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

static int strcasecmp_kos(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        if (to_upper(*s1) != to_upper(*s2)) {
            return (int)((uint8_t)to_upper(*s1) - (uint8_t)to_upper(*s2));
        }
        s1++;
        s2++;
    }
    return (int)((uint8_t)to_upper(*s1) - (uint8_t)to_upper(*s2));
}

static uint64_t cluster_to_lba(struct fat32_volume *vol, uint32_t cluster) {
    return vol->cluster_start_lba + (uint64_t)(cluster - 2) * vol->sectors_per_cluster;
}

static uint32_t fat32_read_fat_entry(struct fat32_volume *vol, uint32_t cluster) {
    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint64_t fat_sec = vol->fat_start_lba + (fat_offset / vol->bpb.bytes_per_sector);
    uint32_t entry_offset = (uint32_t)(fat_offset % vol->bpb.bytes_per_sector);

    uint8_t sec_buf[512];
    if (!block_cache_read(vol->dev, fat_sec, 1, sec_buf)) {
        return FAT32_CLUSTER_BAD;
    }

    uint32_t val = *(uint32_t *)(sec_buf + entry_offset);
    return val & 0x0FFFFFFF;
}

static bool fat32_write_fat_entry(struct fat32_volume *vol, uint32_t cluster, uint32_t next_cluster) {
    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint32_t entry_offset = (uint32_t)(fat_offset % vol->bpb.bytes_per_sector);

    /* Write to all FAT tables */
    for (uint32_t f = 0; f < vol->bpb.num_fats; f++) {
        uint64_t fat_sec = vol->fat_start_lba + (uint64_t)f * vol->bpb.fat_size_32 + (fat_offset / vol->bpb.bytes_per_sector);
        uint8_t sec_buf[512];
        if (!block_cache_read(vol->dev, fat_sec, 1, sec_buf)) {
            return false;
        }

        uint32_t *entry_ptr = (uint32_t *)(sec_buf + entry_offset);
        *entry_ptr = (*entry_ptr & 0xF0000000) | (next_cluster & 0x0FFFFFFF);

        if (!block_cache_write(vol->dev, fat_sec, 1, sec_buf)) {
            return false;
        }
    }
    return true;
}

static uint32_t fat32_allocate_cluster(struct fat32_volume *vol, uint32_t prev_cluster) {
    for (uint32_t clus = 2; clus < vol->total_clusters + 2; clus++) {
        uint32_t val = fat32_read_fat_entry(vol, clus);
        if (val == FAT32_CLUSTER_FREE) {
            if (!fat32_write_fat_entry(vol, clus, FAT32_CLUSTER_END)) {
                return 0;
            }
            if (prev_cluster >= 2) {
                fat32_write_fat_entry(vol, prev_cluster, clus);
            }

            /* Zero out newly allocated cluster */
            uint8_t zero_sec[512];
            kmemset(zero_sec, 0, 512);
            uint64_t lba = cluster_to_lba(vol, clus);
            for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
                block_cache_write(vol->dev, lba + s, 1, zero_sec);
            }

            return clus;
        }
    }
    log_error("[fat32] Volume is full - no free clusters available");
    return 0;
}

static void fat32_free_cluster_chain(struct fat32_volume *vol, uint32_t start_cluster) {
    uint32_t curr = start_cluster;
    while (curr >= 2 && curr < FAT32_CLUSTER_END_MIN) {
        uint32_t next = fat32_read_fat_entry(vol, curr);
        fat32_write_fat_entry(vol, curr, FAT32_CLUSTER_FREE);
        curr = next;
    }
}

/* 8.3 Name formatting */
static void fat32_format_83_name(const uint8_t raw[11], char out[13]) {
    int out_idx = 0;
    for (int i = 0; i < 8; i++) {
        if (raw[i] != ' ') {
            out[out_idx++] = (char)raw[i];
        }
    }
    if (raw[8] != ' ') {
        out[out_idx++] = '.';
        for (int i = 8; i < 11; i++) {
            if (raw[i] != ' ') {
                out[out_idx++] = (char)raw[i];
            }
        }
    }
    out[out_idx] = '\0';
}

static void fat32_generate_83_name(const char *name, uint8_t raw_out[11]) {
    kmemset(raw_out, ' ', 11);
    const char *dot = NULL;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '.') dot = p;
    }

    int name_len = dot ? (int)(dot - name) : (int)kstrlen(name);
    if (name_len > 8) name_len = 8;
    for (int i = 0; i < name_len; i++) {
        raw_out[i] = (uint8_t)to_upper(name[i]);
    }

    if (dot != NULL) {
        int ext_len = (int)kstrlen(dot + 1);
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++) {
            raw_out[8 + i] = (uint8_t)to_upper(dot[1 + i]);
        }
    }
}

/* Locate a directory entry by path */
struct fat32_lookup_result {
    uint32_t dir_cluster;
    uint32_t dir_sector_offset;
    uint32_t entry_index_in_sector;
    struct fat32_dir_entry entry;
    bool found;
};

static bool fat32_lookup_entry_in_dir(
    struct fat32_volume *vol,
    uint32_t dir_cluster,
    const char *target_name,
    struct fat32_lookup_result *result
) {
    uint32_t cur_clus = dir_cluster;
    uint8_t sec_buf[512];
    char lfn_buf[256];
    kmemset(lfn_buf, 0, sizeof(lfn_buf));
    bool has_lfn = false;

    while (cur_clus >= 2 && cur_clus < FAT32_CLUSTER_END_MIN) {
        uint64_t clus_lba = cluster_to_lba(vol, cur_clus);

        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            if (!block_cache_read(vol->dev, clus_lba + s, 1, sec_buf)) {
                return false;
            }

            struct fat32_dir_entry *entries = (struct fat32_dir_entry *)sec_buf;
            for (uint32_t e = 0; e < 512 / sizeof(struct fat32_dir_entry); e++) {
                struct fat32_dir_entry *de = &entries[e];

                if (de->name[0] == 0x00) {
                    /* End of directory */
                    return false;
                }
                if (de->name[0] == 0xE5) {
                    /* Deleted entry */
                    has_lfn = false;
                    continue;
                }

                if (de->attr == FAT_ATTR_LFN) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)de;
                    uint8_t seq = lfn->sequence & 0x1F;
                    if (seq > 0 && seq <= 20) {
                        int char_base = (seq - 1) * 13;
                        for (int i = 0; i < 5; i++) {
                            uint16_t c = lfn->name0_4[i];
                            if (c != 0 && c != 0xFFFF && char_base + i < 255) {
                                lfn_buf[char_base + i] = (char)c;
                            }
                        }
                        for (int i = 0; i < 6; i++) {
                            uint16_t c = lfn->name5_10[i];
                            if (c != 0 && c != 0xFFFF && char_base + 5 + i < 255) {
                                lfn_buf[char_base + 5 + i] = (char)c;
                            }
                        }
                        for (int i = 0; i < 2; i++) {
                            uint16_t c = lfn->name11_12[i];
                            if (c != 0 && c != 0xFFFF && char_base + 11 + i < 255) {
                                lfn_buf[char_base + 11 + i] = (char)c;
                            }
                        }
                        has_lfn = true;
                    }
                    continue;
                }

                if (de->attr & FAT_ATTR_VOLUME_ID) {
                    has_lfn = false;
                    continue;
                }

                char sfn_name[13];
                fat32_format_83_name(de->name, sfn_name);

                bool match = false;
                if (has_lfn && strcasecmp_kos(lfn_buf, target_name) == 0) {
                    match = true;
                } else if (strcasecmp_kos(sfn_name, target_name) == 0) {
                    match = true;
                }

                if (match) {
                    if (result != NULL) {
                        result->dir_cluster = cur_clus;
                        result->dir_sector_offset = s;
                        result->entry_index_in_sector = e;
                        result->entry = *de;
                        result->found = true;
                    }
                    return true;
                }

                has_lfn = false;
                kmemset(lfn_buf, 0, sizeof(lfn_buf));
            }
        }

        cur_clus = fat32_read_fat_entry(vol, cur_clus);
    }
    return false;
}

static bool fat32_lookup_path(
    struct fat32_volume *vol,
    const char *path,
    struct fat32_lookup_result *result
) {
    if (path == NULL) {
        return false;
    }

    while (*path == '/' || *path == '\\') path++;

    if (*path == '\0') {
        /* Root directory query */
        if (result != NULL) {
            result->dir_cluster = vol->root_cluster;
            result->entry.first_cluster_high = (uint16_t)(vol->root_cluster >> 16);
            result->entry.first_cluster_low = (uint16_t)(vol->root_cluster & 0xFFFF);
            result->entry.attr = FAT_ATTR_DIRECTORY;
            result->entry.file_size = 0;
            result->found = true;
        }
        return true;
    }

    uint32_t cur_clus = vol->root_cluster;
    char component[FAT32_MAX_PATH_LEN];

    while (*path != '\0') {
        int comp_len = 0;
        while (*path != '\0' && *path != '/' && *path != '\\') {
            if (comp_len + 1 < FAT32_MAX_PATH_LEN) {
                component[comp_len++] = *path;
            }
            path++;
        }
        component[comp_len] = '\0';
        while (*path == '/' || *path == '\\') path++;

        struct fat32_lookup_result sub_res;
        kmemset(&sub_res, 0, sizeof(sub_res));

        if (!fat32_lookup_entry_in_dir(vol, cur_clus, component, &sub_res)) {
            return false;
        }

        if (*path == '\0') {
            if (result != NULL) {
                *result = sub_res;
            }
            return true;
        }

        if (!(sub_res.entry.attr & FAT_ATTR_DIRECTORY)) {
            return false; /* Path element is not a directory */
        }

        cur_clus = ((uint32_t)sub_res.entry.first_cluster_high << 16) | sub_res.entry.first_cluster_low;
        if (cur_clus == 0) cur_clus = vol->root_cluster;
    }

    return false;
}

bool fat32_probe(struct block_device *dev) {
    if (dev == NULL) return false;

    uint8_t boot_sec[512];
    if (!block_read(dev, 0, 1, boot_sec)) {
        return false;
    }

    struct fat32_bpb *bpb = (struct fat32_bpb *)boot_sec;
    if (boot_sec[510] != 0x55 || boot_sec[511] != 0xAA) {
        return false;
    }

    if (bpb->bytes_per_sector != 512 || bpb->sectors_per_cluster == 0 || bpb->num_fats == 0) {
        return false;
    }

    if (bpb->fat_size_16 != 0 || bpb->fat_size_32 == 0) {
        return false; /* Not FAT32 */
    }

    return true;
}

static bool fat32_format_geometry(uint64_t total_sectors, uint8_t *sectors_per_cluster,
                                  uint32_t *fat_sectors, uint32_t *total_clusters) {
    if (total_sectors <= 32 || total_sectors > UINT32_MAX
        || sectors_per_cluster == NULL || fat_sectors == NULL || total_clusters == NULL) {
        return false;
    }

    for (uint32_t spc = 1; spc <= 128; spc <<= 1) {
        uint32_t fat = 1;
        uint64_t clusters = 0;
        for (uint32_t iteration = 0; iteration < 8; ++iteration) {
            uint64_t data_sectors = total_sectors - 32ULL - 2ULL * fat;
            clusters = data_sectors / spc;
            if (clusters == 0 || clusters > 0x0FFFFFF5ULL) break;
            uint64_t required_fat = ((clusters + 2ULL) * 4ULL + 511ULL) / 512ULL;
            if (required_fat == fat) break;
            if (required_fat > UINT32_MAX) {
                clusters = 0;
                break;
            }
            fat = (uint32_t)required_fat;
        }
        if (clusters == 0 || clusters > 0x0FFFFFF5ULL) continue;
        uint64_t required_fat = ((clusters + 2ULL) * 4ULL + 511ULL) / 512ULL;
        uint64_t data_start = 32ULL + 2ULL * required_fat;
        if (required_fat == 0 || required_fat > UINT32_MAX
            || data_start >= total_sectors
            || data_start + clusters * spc > total_sectors) {
            continue;
        }
        *sectors_per_cluster = (uint8_t)spc;
        *fat_sectors = (uint32_t)required_fat;
        *total_clusters = (uint32_t)clusters;
        return true;
    }
    return false;
}

bool fat32_format(struct block_device *dev, const char *label) {
    if (dev == NULL || dev->state != BLOCK_DEVICE_ONLINE
        || dev->block_size != 512 || dev->total_blocks <= 32
        || dev->total_blocks > UINT32_MAX) {
        return false;
    }

    uint8_t sectors_per_cluster = 0;
    uint32_t fat_sectors = 0;
    uint32_t total_clusters = 0;
    if (!fat32_format_geometry(dev->total_blocks, &sectors_per_cluster,
                               &fat_sectors, &total_clusters)) {
        return false;
    }

    (void)block_cache_flush_device(dev);
    block_cache_invalidate_device(dev);

    uint8_t boot_sector[512];
    kmemset(boot_sector, 0, sizeof(boot_sector));
    struct fat32_bpb *bpb = (struct fat32_bpb *)boot_sector;
    bpb->jmp_boot[0] = 0xEB;
    bpb->jmp_boot[1] = 0x58;
    bpb->jmp_boot[2] = 0x90;
    kmemcpy(bpb->oem_name, "VINO OS ", 8);
    bpb->bytes_per_sector = 512;
    bpb->sectors_per_cluster = sectors_per_cluster;
    bpb->reserved_sector_count = 32;
    bpb->num_fats = 2;
    bpb->media = 0xF8;
    bpb->sectors_per_track = 63;
    bpb->num_heads = 255;
    bpb->total_sectors_32 = (uint32_t)dev->total_blocks;
    bpb->fat_size_32 = fat_sectors;
    bpb->root_cluster = 2;
    bpb->fs_info_sector = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number = 0x80;
    bpb->boot_signature = 0x29;
    bpb->volume_id = (uint32_t)dev->total_blocks ^ 0x56494E4FU;
    kmemset(bpb->volume_label, ' ', sizeof(bpb->volume_label));
    if (label != NULL) {
        for (uint32_t index = 0; index < sizeof(bpb->volume_label) && label[index] != '\0'; ++index) {
            bpb->volume_label[index] = label[index];
        }
    }
    kmemcpy(bpb->fs_type, "FAT32   ", 8);
    boot_sector[510] = 0x55;
    boot_sector[511] = 0xAA;
    if (!block_write(dev, 0, 1, boot_sector)) return false;

    uint8_t fsinfo_sector[512];
    kmemset(fsinfo_sector, 0, sizeof(fsinfo_sector));
    struct fat32_fsinfo *fsinfo = (struct fat32_fsinfo *)fsinfo_sector;
    fsinfo->lead_sig = 0x41615252;
    fsinfo->struct_sig = 0x61417272;
    fsinfo->free_count = total_clusters > 0 ? total_clusters - 1 : 0xFFFFFFFFU;
    fsinfo->next_free = 3;
    fsinfo->trail_sig = 0xAA550000;
    if (!block_write(dev, 1, 1, fsinfo_sector)
        || !block_write(dev, 6, 1, boot_sector)
        || !block_write(dev, 7, 1, fsinfo_sector)) {
        return false;
    }

    uint8_t zero_sector[512];
    kmemset(zero_sector, 0, sizeof(zero_sector));
    uint8_t fat_sector[512];
    kmemset(fat_sector, 0, sizeof(fat_sector));
    uint32_t *fat_entries = (uint32_t *)fat_sector;
    fat_entries[0] = 0x0FFFFFF8;
    fat_entries[1] = FAT32_CLUSTER_END;
    fat_entries[2] = FAT32_CLUSTER_END;

    uint64_t fat_start = 32;
    for (uint32_t fat_index = 0; fat_index < 2; ++fat_index) {
        uint64_t fat_lba = fat_start + (uint64_t)fat_index * fat_sectors;
        for (uint32_t sector = 0; sector < fat_sectors; ++sector) {
            const uint8_t *data = sector == 0 ? fat_sector : zero_sector;
            if (!block_write(dev, fat_lba + sector, 1, data)) return false;
        }
    }

    uint64_t cluster_start = fat_start + 2ULL * fat_sectors;
    for (uint32_t sector = 0; sector < sectors_per_cluster; ++sector) {
        if (!block_write(dev, cluster_start + sector, 1, zero_sector)) return false;
    }
    return block_flush(dev);
}

static void fat32_refresh_cached_files_locked(struct fat32_volume *vol);

struct fat32_volume *fat32_mount(struct block_device *dev) {
    if (!fat32_probe(dev)) {
        log_errorf("[fat32] Device %s is not a valid FAT32 filesystem", dev ? dev->name : "null");
        return NULL;
    }

    if (g_fat_volume_count >= FAT32_MAX_VOLUMES) {
        return NULL;
    }

    uint8_t boot_sec[512];
    if (!block_read(dev, 0, 1, boot_sec)) {
        return NULL;
    }

    struct fat32_volume *vol = &g_fat_volumes[g_fat_volume_count];
    kmemset(vol, 0, sizeof(struct fat32_volume));

    vol->dev = dev;
    kmemcpy(&vol->bpb, boot_sec, sizeof(struct fat32_bpb));
    spinlock_init(&vol->lock);

    vol->fat_start_lba = vol->bpb.reserved_sector_count;
    vol->cluster_start_lba = vol->fat_start_lba + (uint64_t)vol->bpb.num_fats * vol->bpb.fat_size_32;
    vol->sectors_per_cluster = vol->bpb.sectors_per_cluster;
    vol->cluster_size = vol->sectors_per_cluster * vol->bpb.bytes_per_sector;
    vol->root_cluster = vol->bpb.root_cluster;

    uint32_t total_sec = vol->bpb.total_sectors_32;
    uint32_t data_sec = total_sec - (uint32_t)vol->cluster_start_lba;
    vol->total_clusters = data_sec / vol->sectors_per_cluster;
    vol->mounted = true;

    log_infof("[fat32] Mounted FAT32 on %s (%u clusters, %u KiB/cluster, root %u)",
              dev->name, (unsigned int)vol->total_clusters,
              (unsigned int)(vol->cluster_size / 1024), (unsigned int)vol->root_cluster);

    /* Initial scan to populate cached files */
    fat32_refresh_cached_files_locked(vol);

    g_fat_volume_count++;
    return vol;
}

bool fat32_unmount(struct fat32_volume *vol) {
    if (vol == NULL || !vol->mounted) return false;

    uint64_t flags = spinlock_lock_irqsave(&vol->lock);
    block_cache_flush_device(vol->dev);

    for (uint64_t i = 0; i < vol->cached_file_count; i++) {
        if (vol->cached_files[i].data != NULL) {
            kfree(vol->cached_files[i].data);
            vol->cached_files[i].data = NULL;
        }
    }
    vol->cached_file_count = 0;
    vol->mounted = false;
    spinlock_unlock_irqrestore(&vol->lock, flags);

    return true;
}

bool fat32_read_file(struct fat32_volume *vol, const char *path, const uint8_t **data_out, uint64_t *size_out) {
    if (vol == NULL || !vol->mounted || path == NULL || data_out == NULL || size_out == NULL) {
        return false;
    }

    uint64_t flags = spinlock_lock_irqsave(&vol->lock);

    struct fat32_lookup_result res;
    if (!fat32_lookup_path(vol, path, &res) || (res.entry.attr & FAT_ATTR_DIRECTORY)) {
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return false;
    }

    uint64_t file_size = res.entry.file_size;
    uint32_t start_clus = ((uint32_t)res.entry.first_cluster_high << 16) | res.entry.first_cluster_low;

    if (file_size == 0 || start_clus < 2) {
        *data_out = (const uint8_t *)"";
        *size_out = 0;
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return true;
    }

    uint8_t *buf = (uint8_t *)kmalloc(file_size + 1);
    if (buf == NULL) {
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return false;
    }

    uint32_t cur_clus = start_clus;
    uint64_t bytes_read = 0;
    uint8_t *clus_buf = (uint8_t *)kmalloc(vol->cluster_size);
    if (clus_buf == NULL) {
        kfree(buf);
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return false;
    }

    while (cur_clus >= 2 && cur_clus < FAT32_CLUSTER_END_MIN && bytes_read < file_size) {
        uint64_t lba = cluster_to_lba(vol, cur_clus);
        if (!block_cache_read(vol->dev, lba, vol->sectors_per_cluster, clus_buf)) {
            kfree(clus_buf);
            kfree(buf);
            spinlock_unlock_irqrestore(&vol->lock, flags);
            return false;
        }

        uint64_t to_copy = file_size - bytes_read;
        if (to_copy > vol->cluster_size) to_copy = vol->cluster_size;

        kmemcpy(buf + bytes_read, clus_buf, to_copy);
        bytes_read += to_copy;

        cur_clus = fat32_read_fat_entry(vol, cur_clus);
    }

    kfree(clus_buf);
    buf[file_size] = '\0';

    *data_out = buf;
    *size_out = file_size;

    spinlock_unlock_irqrestore(&vol->lock, flags);
    return true;
}

static bool fat32_add_dir_entry(
    struct fat32_volume *vol,
    uint32_t dir_cluster,
    const char *name,
    uint8_t attr,
    uint32_t first_clus,
    uint32_t file_size
) {
    uint32_t cur_clus = dir_cluster;
    uint8_t sec_buf[512];
    uint8_t sfn[11];
    fat32_generate_83_name(name, sfn);

    while (cur_clus >= 2 && cur_clus < FAT32_CLUSTER_END_MIN) {
        uint64_t clus_lba = cluster_to_lba(vol, cur_clus);

        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            if (!block_cache_read(vol->dev, clus_lba + s, 1, sec_buf)) {
                return false;
            }

            struct fat32_dir_entry *entries = (struct fat32_dir_entry *)sec_buf;
            for (uint32_t e = 0; e < 512 / sizeof(struct fat32_dir_entry); e++) {
                struct fat32_dir_entry *de = &entries[e];

                if (de->name[0] == 0x00 || de->name[0] == 0xE5) {
                    /* Found free slot */
                    kmemset(de, 0, sizeof(struct fat32_dir_entry));
                    kmemcpy(de->name, sfn, 11);
                    de->attr = attr;
                    de->first_cluster_high = (uint16_t)(first_clus >> 16);
                    de->first_cluster_low = (uint16_t)(first_clus & 0xFFFF);
                    de->file_size = file_size;

                    return block_cache_write(vol->dev, clus_lba + s, 1, sec_buf);
                }
            }
        }

        uint32_t next = fat32_read_fat_entry(vol, cur_clus);
        if (next >= FAT32_CLUSTER_END_MIN) {
            /* Extend directory cluster */
            uint32_t new_clus = fat32_allocate_cluster(vol, cur_clus);
            if (new_clus == 0) return false;
            cur_clus = new_clus;
        } else {
            cur_clus = next;
        }
    }

    return false;
}

bool fat32_write_file(struct fat32_volume *vol, const char *path, const uint8_t *data, uint64_t size) {
    if (vol == NULL || !vol->mounted || path == NULL) return false;

    uint64_t flags = spinlock_lock_irqsave(&vol->lock);

    struct fat32_lookup_result res;
    if (fat32_lookup_path(vol, path, &res)) {
        /* Overwrite existing file */
        if (res.entry.attr & FAT_ATTR_DIRECTORY) {
            spinlock_unlock_irqrestore(&vol->lock, flags);
            return false;
        }

        uint32_t old_clus = ((uint32_t)res.entry.first_cluster_high << 16) | res.entry.first_cluster_low;
        if (old_clus >= 2) {
            fat32_free_cluster_chain(vol, old_clus);
        }

        uint32_t new_start_clus = 0;
        if (size > 0) {
            uint32_t needed_clusters = (uint32_t)((size + vol->cluster_size - 1) / vol->cluster_size);
            uint32_t prev = 0;

            for (uint32_t i = 0; i < needed_clusters; i++) {
                uint32_t c = fat32_allocate_cluster(vol, prev);
                if (c == 0) {
                    if (new_start_clus != 0) fat32_free_cluster_chain(vol, new_start_clus);
                    spinlock_unlock_irqrestore(&vol->lock, flags);
                    return false;
                }
                if (new_start_clus == 0) new_start_clus = c;

                uint64_t offset = (uint64_t)i * vol->cluster_size;
                uint64_t chunk = size - offset;
                if (chunk > vol->cluster_size) chunk = vol->cluster_size;

                uint8_t *clus_buf = (uint8_t *)kmalloc(vol->cluster_size);
                if (clus_buf != NULL) {
                    kmemset(clus_buf, 0, vol->cluster_size);
                    if (data != NULL) kmemcpy(clus_buf, data + offset, chunk);
                    block_cache_write(vol->dev, cluster_to_lba(vol, c), vol->sectors_per_cluster, clus_buf);
                    kfree(clus_buf);
                }
                prev = c;
            }
        }

        /* Update directory entry */
        uint64_t dir_lba = cluster_to_lba(vol, res.dir_cluster) + res.dir_sector_offset;
        uint8_t sec_buf[512];
        if (block_cache_read(vol->dev, dir_lba, 1, sec_buf)) {
            struct fat32_dir_entry *entries = (struct fat32_dir_entry *)sec_buf;
            struct fat32_dir_entry *de = &entries[res.entry_index_in_sector];
            de->first_cluster_high = (uint16_t)(new_start_clus >> 16);
            de->first_cluster_low = (uint16_t)(new_start_clus & 0xFFFF);
            de->file_size = (uint32_t)size;
            block_cache_write(vol->dev, dir_lba, 1, sec_buf);
        }

        block_cache_flush_device(vol->dev);
        fat32_refresh_cached_files_locked(vol);
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return true;
    }

    /* Create new file: extract parent dir and filename */
    const char *last_slash = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') last_slash = p;
    }

    uint32_t parent_cluster = vol->root_cluster;
    const char *filename = path;

    if (last_slash != NULL) {
        char parent_path[FAT32_MAX_PATH_LEN];
        int plen = (int)(last_slash - path);
        if (plen >= FAT32_MAX_PATH_LEN) plen = FAT32_MAX_PATH_LEN - 1;
        kmemcpy(parent_path, path, plen);
        parent_path[plen] = '\0';
        filename = last_slash + 1;

        struct fat32_lookup_result parent_res;
        if (!fat32_lookup_path(vol, parent_path, &parent_res) || !(parent_res.entry.attr & FAT_ATTR_DIRECTORY)) {
            spinlock_unlock_irqrestore(&vol->lock, flags);
            return false;
        }
        parent_cluster = ((uint32_t)parent_res.entry.first_cluster_high << 16) | parent_res.entry.first_cluster_low;
        if (parent_cluster == 0) parent_cluster = vol->root_cluster;
    }

    uint32_t new_start_clus = 0;
    if (size > 0) {
        uint32_t needed = (uint32_t)((size + vol->cluster_size - 1) / vol->cluster_size);
        uint32_t prev = 0;

        for (uint32_t i = 0; i < needed; i++) {
            uint32_t c = fat32_allocate_cluster(vol, prev);
            if (c == 0) {
                if (new_start_clus != 0) fat32_free_cluster_chain(vol, new_start_clus);
                spinlock_unlock_irqrestore(&vol->lock, flags);
                return false;
            }
            if (new_start_clus == 0) new_start_clus = c;

            uint64_t offset = (uint64_t)i * vol->cluster_size;
            uint64_t chunk = size - offset;
            if (chunk > vol->cluster_size) chunk = vol->cluster_size;

            uint8_t *clus_buf = (uint8_t *)kmalloc(vol->cluster_size);
            if (clus_buf != NULL) {
                kmemset(clus_buf, 0, vol->cluster_size);
                if (data != NULL) kmemcpy(clus_buf, data + offset, chunk);
                block_cache_write(vol->dev, cluster_to_lba(vol, c), vol->sectors_per_cluster, clus_buf);
                kfree(clus_buf);
            }
            prev = c;
        }
    }

    if (!fat32_add_dir_entry(vol, parent_cluster, filename, FAT_ATTR_ARCHIVE, new_start_clus, (uint32_t)size)) {
        if (new_start_clus != 0) fat32_free_cluster_chain(vol, new_start_clus);
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return false;
    }

    block_cache_flush_device(vol->dev);
    fat32_refresh_cached_files_locked(vol);
    spinlock_unlock_irqrestore(&vol->lock, flags);
    return true;
}

bool fat32_unlink_file(struct fat32_volume *vol, const char *path) {
    if (vol == NULL || !vol->mounted || path == NULL) return false;

    uint64_t flags = spinlock_lock_irqsave(&vol->lock);

    struct fat32_lookup_result res;
    if (!fat32_lookup_path(vol, path, &res) || (res.entry.attr & FAT_ATTR_DIRECTORY)) {
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return false;
    }

    uint32_t clus = ((uint32_t)res.entry.first_cluster_high << 16) | res.entry.first_cluster_low;
    if (clus >= 2) {
        fat32_free_cluster_chain(vol, clus);
    }

    /* Mark entry deleted (0xE5) */
    uint64_t dir_lba = cluster_to_lba(vol, res.dir_cluster) + res.dir_sector_offset;
    uint8_t sec_buf[512];
    if (block_cache_read(vol->dev, dir_lba, 1, sec_buf)) {
        struct fat32_dir_entry *entries = (struct fat32_dir_entry *)sec_buf;
        entries[res.entry_index_in_sector].name[0] = 0xE5;
        block_cache_write(vol->dev, dir_lba, 1, sec_buf);
    }

    block_cache_flush_device(vol->dev);
    fat32_refresh_cached_files_locked(vol);
    spinlock_unlock_irqrestore(&vol->lock, flags);
    return true;
}

bool fat32_mkdir(struct fat32_volume *vol, const char *path) {
    if (vol == NULL || !vol->mounted || path == NULL) return false;

    uint64_t flags = spinlock_lock_irqsave(&vol->lock);

    struct fat32_lookup_result res;
    if (fat32_lookup_path(vol, path, &res)) {
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return false; /* Already exists */
    }

    const char *last_slash = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') last_slash = p;
    }

    uint32_t parent_cluster = vol->root_cluster;
    const char *dirname = path;

    if (last_slash != NULL) {
        char parent_path[FAT32_MAX_PATH_LEN];
        int plen = (int)(last_slash - path);
        if (plen >= FAT32_MAX_PATH_LEN) plen = FAT32_MAX_PATH_LEN - 1;
        kmemcpy(parent_path, path, plen);
        parent_path[plen] = '\0';
        dirname = last_slash + 1;

        struct fat32_lookup_result parent_res;
        if (!fat32_lookup_path(vol, parent_path, &parent_res) || !(parent_res.entry.attr & FAT_ATTR_DIRECTORY)) {
            spinlock_unlock_irqrestore(&vol->lock, flags);
            return false;
        }
        parent_cluster = ((uint32_t)parent_res.entry.first_cluster_high << 16) | parent_res.entry.first_cluster_low;
        if (parent_cluster == 0) parent_cluster = vol->root_cluster;
    }

    uint32_t new_clus = fat32_allocate_cluster(vol, 0);
    if (new_clus == 0) {
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return false;
    }

    /* Initialize '.' and '..' entries */
    uint8_t sec_buf[512];
    kmemset(sec_buf, 0, 512);
    struct fat32_dir_entry *dot = (struct fat32_dir_entry *)&sec_buf[0];
    kmemset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = FAT_ATTR_DIRECTORY;
    dot->first_cluster_high = (uint16_t)(new_clus >> 16);
    dot->first_cluster_low = (uint16_t)(new_clus & 0xFFFF);

    struct fat32_dir_entry *dotdot = (struct fat32_dir_entry *)&sec_buf[sizeof(struct fat32_dir_entry)];
    kmemset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT_ATTR_DIRECTORY;
    dotdot->first_cluster_high = (uint16_t)(parent_cluster >> 16);
    dotdot->first_cluster_low = (uint16_t)(parent_cluster & 0xFFFF);

    block_cache_write(vol->dev, cluster_to_lba(vol, new_clus), 1, sec_buf);

    if (!fat32_add_dir_entry(vol, parent_cluster, dirname, FAT_ATTR_DIRECTORY, new_clus, 0)) {
        fat32_free_cluster_chain(vol, new_clus);
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return false;
    }

    block_cache_flush_device(vol->dev);
    spinlock_unlock_irqrestore(&vol->lock, flags);
    return true;
}

bool fat32_rmdir(struct fat32_volume *vol, const char *path) {
    if (vol == NULL || !vol->mounted || path == NULL) return false;

    uint64_t flags = spinlock_lock_irqsave(&vol->lock);

    struct fat32_lookup_result res;
    if (!fat32_lookup_path(vol, path, &res) || !(res.entry.attr & FAT_ATTR_DIRECTORY)) {
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return false;
    }

    uint32_t clus = ((uint32_t)res.entry.first_cluster_high << 16) | res.entry.first_cluster_low;
    if (clus >= 2) {
        fat32_free_cluster_chain(vol, clus);
    }

    /* Mark directory entry deleted */
    uint64_t dir_lba = cluster_to_lba(vol, res.dir_cluster) + res.dir_sector_offset;
    uint8_t sec_buf[512];
    if (block_cache_read(vol->dev, dir_lba, 1, sec_buf)) {
        struct fat32_dir_entry *entries = (struct fat32_dir_entry *)sec_buf;
        entries[res.entry_index_in_sector].name[0] = 0xE5;
        block_cache_write(vol->dev, dir_lba, 1, sec_buf);
    }

    block_cache_flush_device(vol->dev);
    spinlock_unlock_irqrestore(&vol->lock, flags);
    return true;
}

bool fat32_rename(struct fat32_volume *vol, const char *old_path, const char *new_path) {
    if (vol == NULL || !vol->mounted || old_path == NULL || new_path == NULL) return false;

    const uint8_t *data = NULL;
    uint64_t size = 0;
    if (!fat32_read_file(vol, old_path, &data, &size)) {
        return false;
    }

    if (!fat32_write_file(vol, new_path, data, size)) {
        if (data != NULL && size > 0) kfree((void *)data);
        return false;
    }

    fat32_unlink_file(vol, old_path);
    if (data != NULL && size > 0) {
        kfree((void *)data);
    }
    return true;
}

bool fat32_readdir(struct fat32_volume *vol, const char *dir_path, fat32_readdir_cb callback, void *user_arg) {
    if (vol == NULL || !vol->mounted || callback == NULL) return false;

    uint64_t flags = spinlock_lock_irqsave(&vol->lock);

    struct fat32_lookup_result res;
    if (!fat32_lookup_path(vol, dir_path, &res) || !(res.entry.attr & FAT_ATTR_DIRECTORY)) {
        spinlock_unlock_irqrestore(&vol->lock, flags);
        return false;
    }

    uint32_t cur_clus = ((uint32_t)res.entry.first_cluster_high << 16) | res.entry.first_cluster_low;
    if (cur_clus == 0) cur_clus = vol->root_cluster;

    uint8_t sec_buf[512];
    char lfn_buf[256];
    kmemset(lfn_buf, 0, sizeof(lfn_buf));
    bool has_lfn = false;

    while (cur_clus >= 2 && cur_clus < FAT32_CLUSTER_END_MIN) {
        uint64_t clus_lba = cluster_to_lba(vol, cur_clus);

        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            if (!block_cache_read(vol->dev, clus_lba + s, 1, sec_buf)) {
                spinlock_unlock_irqrestore(&vol->lock, flags);
                return false;
            }

            struct fat32_dir_entry *entries = (struct fat32_dir_entry *)sec_buf;
            for (uint32_t e = 0; e < 512 / sizeof(struct fat32_dir_entry); e++) {
                struct fat32_dir_entry *de = &entries[e];

                if (de->name[0] == 0x00) {
                    spinlock_unlock_irqrestore(&vol->lock, flags);
                    return true;
                }
                if (de->name[0] == 0xE5) {
                    has_lfn = false;
                    continue;
                }

                if (de->attr == FAT_ATTR_LFN) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)de;
                    uint8_t seq = lfn->sequence & 0x1F;
                    if (seq > 0 && seq <= 20) {
                        int char_base = (seq - 1) * 13;
                        for (int i = 0; i < 5; i++) {
                            uint16_t c = lfn->name0_4[i];
                            if (c != 0 && c != 0xFFFF && char_base + i < 255) {
                                lfn_buf[char_base + i] = (char)c;
                            }
                        }
                        for (int i = 0; i < 6; i++) {
                            uint16_t c = lfn->name5_10[i];
                            if (c != 0 && c != 0xFFFF && char_base + 5 + i < 255) {
                                lfn_buf[char_base + 5 + i] = (char)c;
                            }
                        }
                        for (int i = 0; i < 2; i++) {
                            uint16_t c = lfn->name11_12[i];
                            if (c != 0 && c != 0xFFFF && char_base + 11 + i < 255) {
                                lfn_buf[char_base + 11 + i] = (char)c;
                            }
                        }
                        has_lfn = true;
                    }
                    continue;
                }

                if (de->attr & FAT_ATTR_VOLUME_ID) {
                    has_lfn = false;
                    continue;
                }

                char sfn_name[13];
                fat32_format_83_name(de->name, sfn_name);

                const char *display_name = has_lfn ? lfn_buf : sfn_name;
                callback(display_name, de->file_size, de->attr, user_arg);

                has_lfn = false;
                kmemset(lfn_buf, 0, sizeof(lfn_buf));
            }
        }

        cur_clus = fat32_read_fat_entry(vol, cur_clus);
    }

    spinlock_unlock_irqrestore(&vol->lock, flags);
    return true;
}

/* Recursive directory scanner for VFS cached file list */
static void fat32_scan_files_recursive(struct fat32_volume *vol, uint32_t dir_clus, const char *prefix) {
    if (vol->cached_file_count >= FAT32_MAX_CACHED_FILES) return;

    uint32_t cur_clus = dir_clus;
    uint8_t sec_buf[512];
    char lfn_buf[256];
    kmemset(lfn_buf, 0, sizeof(lfn_buf));
    bool has_lfn = false;

    while (cur_clus >= 2 && cur_clus < FAT32_CLUSTER_END_MIN) {
        uint64_t clus_lba = cluster_to_lba(vol, cur_clus);

        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            if (!block_cache_read(vol->dev, clus_lba + s, 1, sec_buf)) {
                return;
            }

            struct fat32_dir_entry *entries = (struct fat32_dir_entry *)sec_buf;
            for (uint32_t e = 0; e < 512 / sizeof(struct fat32_dir_entry); e++) {
                struct fat32_dir_entry *de = &entries[e];

                if (de->name[0] == 0x00) return;
                if (de->name[0] == 0xE5) {
                    has_lfn = false;
                    continue;
                }
                if (de->attr == FAT_ATTR_LFN) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)de;
                    uint8_t seq = lfn->sequence & 0x1F;
                    if (seq > 0 && seq <= 20) {
                        int char_base = (seq - 1) * 13;
                        for (int i = 0; i < 5; i++) {
                            uint16_t c = lfn->name0_4[i];
                            if (c != 0 && c != 0xFFFF && char_base + i < 255) lfn_buf[char_base + i] = (char)c;
                        }
                        for (int i = 0; i < 6; i++) {
                            uint16_t c = lfn->name5_10[i];
                            if (c != 0 && c != 0xFFFF && char_base + 5 + i < 255) lfn_buf[char_base + 5 + i] = (char)c;
                        }
                        for (int i = 0; i < 2; i++) {
                            uint16_t c = lfn->name11_12[i];
                            if (c != 0 && c != 0xFFFF && char_base + 11 + i < 255) lfn_buf[char_base + 11 + i] = (char)c;
                        }
                        has_lfn = true;
                    }
                    continue;
                }
                if (de->attr & FAT_ATTR_VOLUME_ID) {
                    has_lfn = false;
                    continue;
                }

                char sfn_name[13];
                fat32_format_83_name(de->name, sfn_name);
                const char *entry_name = has_lfn ? lfn_buf : sfn_name;

                if (kstrcmp(entry_name, ".") == 0 || kstrcmp(entry_name, "..") == 0) {
                    has_lfn = false;
                    kmemset(lfn_buf, 0, sizeof(lfn_buf));
                    continue;
                }

                char full_rel_path[FAT32_MAX_PATH_LEN];
                int pos = 0;
                if (prefix != NULL && *prefix != '\0') {
                    for (int i = 0; prefix[i] != '\0' && pos + 1 < FAT32_MAX_PATH_LEN; i++) {
                        full_rel_path[pos++] = prefix[i];
                    }
                    if (pos + 1 < FAT32_MAX_PATH_LEN) full_rel_path[pos++] = '/';
                }
                for (int i = 0; entry_name[i] != '\0' && pos + 1 < FAT32_MAX_PATH_LEN; i++) {
                    full_rel_path[pos++] = entry_name[i];
                }
                full_rel_path[pos] = '\0';

                uint32_t target_clus = ((uint32_t)de->first_cluster_high << 16) | de->first_cluster_low;

                if (de->attr & FAT_ATTR_DIRECTORY) {
                    if (target_clus >= 2) {
                        fat32_scan_files_recursive(vol, target_clus, full_rel_path);
                    }
                } else {
                    if (vol->cached_file_count < FAT32_MAX_CACHED_FILES) {
                        struct fat32_cached_file *cf = &vol->cached_files[vol->cached_file_count++];
                        kstrncpy(cf->path, full_rel_path, FAT32_MAX_PATH_LEN - 1);
                        cf->size = de->file_size;
                        cf->data = NULL;
                        cf->vfs_f.path = cf->path;
                        cf->vfs_f.data = NULL;
                        cf->vfs_f.size = cf->size;
                        cf->vfs_f.type = VFS_NODE_FILE;
                        cf->vfs_f.attributes = de->attr;
                        cf->valid = true;
                    }
                }

                has_lfn = false;
                kmemset(lfn_buf, 0, sizeof(lfn_buf));
            }
        }
        cur_clus = fat32_read_fat_entry(vol, cur_clus);
    }
}

static void fat32_refresh_cached_files_locked(struct fat32_volume *vol) {
    for (uint64_t i = 0; i < vol->cached_file_count; i++) {
        if (vol->cached_files[i].data != NULL) {
            kfree(vol->cached_files[i].data);
            vol->cached_files[i].data = NULL;
        }
    }
    vol->cached_file_count = 0;
    fat32_scan_files_recursive(vol, vol->root_cluster, "");
}

/* VFS backend adapter methods. Each mounted FAT32 volume carries its own context. */
static bool fat32_vfs_read_file(void *context, const char *path, const uint8_t **data, uint64_t *size) {
    return fat32_read_file((struct fat32_volume *)context, path, data, size);
}

static bool fat32_vfs_write_file(void *context, const char *path, const uint8_t *data, uint64_t size) {
    return fat32_write_file((struct fat32_volume *)context, path, data, size);
}

static uint64_t fat32_vfs_file_count(void *context) {
    struct fat32_volume *vol = (struct fat32_volume *)context;
    return vol == NULL || !vol->mounted ? 0 : vol->cached_file_count;
}

static const struct vfs_file *fat32_vfs_file_at(void *context, uint64_t index) {
    struct fat32_volume *vol = (struct fat32_volume *)context;
    if (vol == NULL || !vol->mounted || index >= vol->cached_file_count) return NULL;
    struct fat32_cached_file *cf = &vol->cached_files[index];
    if (cf->data == NULL && cf->size > 0) {
        const uint8_t *d = NULL;
        uint64_t s = 0;
        if (fat32_read_file(vol, cf->path, &d, &s)) {
            cf->data = (uint8_t *)d;
            cf->vfs_f.data = d;
        }
    }
    return &cf->vfs_f;
}

static bool fat32_vfs_unlink_file(void *context, const char *path) {
    struct fat32_volume *vol = (struct fat32_volume *)context;
    return fat32_unlink_file(vol, path) || fat32_rmdir(vol, path);
}

static bool fat32_vfs_mkdir(void *context, const char *path) {
    return fat32_mkdir((struct fat32_volume *)context, path);
}

struct fat32_readdir_bridge {
    vfs_readdir_callback callback;
    void *user_arg;
};

static void fat32_vfs_readdir_callback(const char *name, uint64_t size, uint8_t attr, void *user_arg) {
    struct fat32_readdir_bridge *bridge = (struct fat32_readdir_bridge *)user_arg;
    struct vfs_dir_entry entry = {0};
    kstrncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.size = size;
    entry.type = (attr & FAT_ATTR_DIRECTORY) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    entry.attributes = attr;
    bridge->callback(&entry, bridge->user_arg);
}

static bool fat32_vfs_readdir(void *context, const char *path, vfs_readdir_callback callback, void *user_arg) {
    if (callback == NULL) return false;
    struct fat32_readdir_bridge bridge = { .callback = callback, .user_arg = user_arg };
    return fat32_readdir((struct fat32_volume *)context, path, fat32_vfs_readdir_callback, &bridge);
}

static bool fat32_vfs_rename(void *context, const char *old_path, const char *new_path) {
    return fat32_rename((struct fat32_volume *)context, old_path, new_path);
}

static bool fat32_vfs_stat(void *context, const char *path, struct vfs_metadata *metadata) {
    struct fat32_volume *vol = (struct fat32_volume *)context;
    if (vol == NULL || !vol->mounted || path == NULL || metadata == NULL) return false;
    uint64_t flags = spinlock_lock_irqsave(&vol->lock);
    struct fat32_lookup_result result;
    bool found = fat32_lookup_path(vol, path, &result);
    if (found) {
        metadata->type = (result.entry.attr & FAT_ATTR_DIRECTORY) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
        metadata->size = result.entry.file_size;
        metadata->attributes = result.entry.attr;
    }
    spinlock_unlock_irqrestore(&vol->lock, flags);
    return found;
}

const struct vfs_backend *fat32_get_vfs_backend(struct fat32_volume *vol) {
    if (vol == NULL) return NULL;
    vol->backend.context = vol;
    vol->backend.read_file = fat32_vfs_read_file;
    vol->backend.write_file = fat32_vfs_write_file;
    vol->backend.file_count = fat32_vfs_file_count;
    vol->backend.file_at = fat32_vfs_file_at;
    vol->backend.unlink_file = fat32_vfs_unlink_file;
    vol->backend.mkdir = fat32_vfs_mkdir;
    vol->backend.readdir = fat32_vfs_readdir;
    vol->backend.rename = fat32_vfs_rename;
    vol->backend.stat = fat32_vfs_stat;
    return &vol->backend;
}
