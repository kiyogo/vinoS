#include <stdbool.h>
#include <stdint.h>

#include <kos/block_cache.h>
#include <kos/crypto.h>
#include <kos/fat32.h>
#include <kos/gui_manager.h>
#include <kos/heap.h>
#include <kos/initramfs.h>
#include <kos/installer.h>
#include <kos/installer_engine.h>
#include <kos/log.h>
#include <kos/memory.h>
#include <kos/vfs.h>
#include <kos/volume.h>

enum {
    INSTALLER_MAX_OS_PAYLOADS = 40,
    INSTALLER_MAX_VERIFY_ITEMS = INSTALLER_MAX_OS_PAYLOADS + 4,
    INSTALLER_MAX_CREATED_DIRECTORIES = 64,
};

struct installer_os_payload {
    char path[VFS_MAX_PATH];
    const uint8_t *data;
    uint64_t size;
    uint8_t checksum[32];
};

struct installer_verify_item {
    char path[VFS_MAX_PATH];
    uint64_t size;
    uint8_t checksum[32];
};

static uint64_t next_transaction_id = 1;
static const uint8_t efi_system_partition_guid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
};

static void copy_text(char *destination, uint64_t capacity, const char *source) {
    if (destination == NULL || capacity == 0) return;
    destination[0] = '\0';
    if (source == NULL) return;
    kstrncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
}

static bool make_absolute_path(char *destination, uint64_t capacity,
                               char volume, const char *relative_path) {
    if (destination == NULL || capacity == 0 || volume < 'A' || volume > 'Z'
        || relative_path == NULL || relative_path[0] == '\0') {
        return false;
    }
    int length = snprintf(destination, capacity, "%c:/%s", volume, relative_path);
    return length > 0 && (uint64_t)length < capacity;
}

static bool make_volume_root(char *destination, uint64_t capacity, char volume,
                             const char *suffix) {
    if (destination == NULL || capacity == 0 || volume < 'A' || volume > 'Z'
        || suffix == NULL) {
        return false;
    }
    int length = suffix[0] == '\0'
        ? snprintf(destination, capacity, "%c:", volume)
        : snprintf(destination, capacity, "%c:/%s", volume, suffix);
    return length > 0 && (uint64_t)length < capacity;
}

static bool path_is_safe_relative(const char *path) {
    if (path == NULL || path[0] == '\0' || path[0] == '/' || path[0] == '\\') {
        return false;
    }
    const char *segment = path;
    for (const char *cursor = path; ; ++cursor) {
        if (*cursor == ':' || *cursor == '\\') return false;
        if (*cursor == '/' || *cursor == '\0') {
            uint64_t segment_size = (uint64_t)(cursor - segment);
            if (segment_size == 0
                || (segment_size == 1 && segment[0] == '.')
                || (segment_size == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            if (*cursor == '\0') break;
            segment = cursor + 1;
        }
    }
    return true;
}

static bool target_partition_geometry_is_valid(const struct installer_flow *flow) {
    if (flow == NULL || flow->selected_disk == NULL || flow->selected_partition == NULL) {
        return false;
    }
    const struct block_device *disk = flow->selected_disk;
    const struct block_device *partition = flow->selected_partition;
    if (disk->type == BLOCK_TYPE_UNKNOWN || disk->type == BLOCK_TYPE_PARTITION
        || disk->state != BLOCK_DEVICE_ONLINE
        || disk->block_size != BLOCK_DEFAULT_SECTOR_SIZE || disk->total_blocks == 0
        || disk->total_blocks > UINT64_MAX / disk->block_size
        || disk->capacity_bytes != disk->block_size * disk->total_blocks
        || partition->parent != disk || partition->type != BLOCK_TYPE_PARTITION
        || partition->state != BLOCK_DEVICE_ONLINE
        || partition->block_size != disk->block_size || partition->total_blocks == 0
        || partition->total_blocks > UINT64_MAX / partition->block_size
        || partition->capacity_bytes != partition->block_size * partition->total_blocks
        || partition->start_lba >= disk->total_blocks
        || partition->total_blocks > disk->total_blocks - partition->start_lba) {
        return false;
    }
    return true;
}

static bool mounted_volume_for_partition(struct block_device *partition, char *letter) {
    if (partition == NULL || letter == NULL) return false;
    for (uint32_t index = 0; index < volume_manager_get_count(); ++index) {
        const struct volume_entry *volume = volume_manager_get_at(index);
        if (volume != NULL && volume->is_mounted && volume->bdev == partition
            && kstrcmp(volume->fs_type, "FAT32") == 0) {
            *letter = volume->letter;
            return true;
        }
    }
    return false;
}

static bool mount_install_target(struct installer_flow *flow, char *letter) {
    if (letter == NULL || !target_partition_geometry_is_valid(flow)
        || !fat32_probe(flow->selected_partition)) {
        return false;
    }
    if (!mounted_volume_for_partition(flow->selected_partition, letter)) {
        if (!volume_manager_mount_device(flow->selected_partition, 0, "VINO_INSTALL")) {
            return false;
        }
        if (!mounted_volume_for_partition(flow->selected_partition, letter)) return false;
    }
    if (!make_volume_root(flow->state.transaction.target_mount_path,
                          sizeof(flow->state.transaction.target_mount_path), *letter, "")) {
        return false;
    }
    return true;
}

static bool transaction_target_volume(const struct installer_flow *flow, char *letter) {
    if (letter == NULL || !target_partition_geometry_is_valid(flow)
        || flow->state.transaction.target_mount_path[0] < 'A'
        || flow->state.transaction.target_mount_path[0] > 'Z'
        || flow->state.transaction.target_mount_path[1] != ':') {
        return false;
    }
    *letter = flow->state.transaction.target_mount_path[0];
    for (uint32_t index = 0; index < volume_manager_get_count(); ++index) {
        const struct volume_entry *volume = volume_manager_get_at(index);
        if (volume != NULL && volume->is_mounted && volume->letter == *letter
            && volume->bdev == flow->selected_partition
            && kstrcmp(volume->fs_type, "FAT32") == 0) {
            return true;
        }
    }
    return false;
}

static bool target_is_efi_system_partition(const struct block_device *partition) {
    if (partition == NULL || partition->type != BLOCK_TYPE_PARTITION) return false;
    return (partition->has_type_guid
            && kmemcmp(partition->partition_type_guid, efi_system_partition_guid,
                       sizeof(efi_system_partition_guid)) == 0)
        || partition->partition_type_mbr == MBR_TYPE_EFI_SYSTEM;
}

static bool ensure_parent_directories(char volume, const char *relative_file,
                                      char (*created_directories)[VFS_MAX_PATH],
                                      uint32_t *directory_count) {
    if (!path_is_safe_relative(relative_file) || created_directories == NULL
        || directory_count == NULL) {
        return false;
    }
    char directory[VFS_MAX_PATH];
    uint64_t length = kstrlen(relative_file);
    if (length >= sizeof(directory)) return false;
    kstrncpy(directory, relative_file, sizeof(directory) - 1);
    directory[sizeof(directory) - 1] = '\0';

    for (uint64_t index = 0; index < length; ++index) {
        if (directory[index] != '/') continue;
        directory[index] = '\0';
        char absolute[VFS_MAX_PATH];
        struct vfs_metadata metadata;
        if (!make_absolute_path(absolute, sizeof(absolute), volume, directory)) return false;
        if (vfs_stat(absolute, &metadata)) {
            if (metadata.type != VFS_NODE_DIRECTORY) return false;
        }
        else {
            if (*directory_count >= INSTALLER_MAX_CREATED_DIRECTORIES
                || !vfs_mkdir(absolute)) {
                return false;
            }
            copy_text(created_directories[*directory_count], VFS_MAX_PATH, directory);
            ++*directory_count;
        }
        directory[index] = '/';
    }
    return true;
}

static bool verify_file(char volume, const struct installer_verify_item *item) {
    char absolute[VFS_MAX_PATH];
    if (item == NULL || item->size > UINT32_MAX
        || !make_absolute_path(absolute, sizeof(absolute), volume, item->path)) {
        return false;
    }
    const uint8_t *data = NULL;
    uint64_t size = 0;
    if (!vfs_read_file(absolute, &data, &size) || data == NULL || size != item->size) {
        if (data != NULL && size != 0) kfree((void *)data);
        return false;
    }
    uint8_t checksum[32];
    kos_sha256(data, size, checksum);
    bool matches = kmemcmp(checksum, item->checksum, sizeof(checksum)) == 0;
    if (size != 0) kfree((void *)data);
    return matches;
}

static bool write_verified(char volume, const char *relative_path,
                           const uint8_t *data, uint64_t size,
                           struct installer_verify_item *items,
                           uint32_t *item_count,
                           char (*created_directories)[VFS_MAX_PATH],
                           uint32_t *directory_count) {
    if (relative_path == NULL || data == NULL || size > UINT32_MAX
        || items == NULL || item_count == NULL
        || *item_count >= INSTALLER_MAX_VERIFY_ITEMS
        || !path_is_safe_relative(relative_path)
        || !ensure_parent_directories(volume, relative_path,
                                      created_directories, directory_count)) {
        return false;
    }
    char absolute[VFS_MAX_PATH];
    struct vfs_metadata metadata;
    if (!make_absolute_path(absolute, sizeof(absolute), volume, relative_path)
        || vfs_stat(absolute, &metadata)) {
        return false;
    }
    struct installer_verify_item *item = &items[*item_count];
    *item = (struct installer_verify_item){ .size = size };
    copy_text(item->path, sizeof(item->path), relative_path);
    kos_sha256(data, size, item->checksum);
    ++*item_count;

    if (!vfs_write_file(absolute, data, size)) {
        return false;
    }
    return verify_file(volume, item);
}

static void rollback_created(char volume,
                             struct installer_verify_item *items,
                             uint32_t item_count,
                             char (*created_directories)[VFS_MAX_PATH],
                             uint32_t directory_count,
                             struct block_device *partition) {
    char absolute[VFS_MAX_PATH];
    while (item_count > 0) {
        --item_count;
        if (make_absolute_path(absolute, sizeof(absolute), volume, items[item_count].path)) {
            (void)vfs_unlink_file(absolute);
        }
    }
    while (directory_count > 0) {
        --directory_count;
        if (make_absolute_path(absolute, sizeof(absolute), volume,
                               created_directories[directory_count])) {
            (void)vfs_unlink_file(absolute);
        }
    }
    if (partition != NULL) {
        (void)block_cache_flush_device(partition);
        block_cache_invalidate_device(partition);
    }
}

static bool add_payload(struct installer_os_payload *payloads, uint32_t *count,
                        const char *path, const uint8_t *data, uint64_t size) {
    if (payloads == NULL || count == NULL || *count >= INSTALLER_MAX_OS_PAYLOADS
        || !path_is_safe_relative(path) || data == NULL || size > UINT32_MAX) {
        return false;
    }
    for (uint32_t index = 0; index < *count; ++index) {
        if (kstrcmp(payloads[index].path, path) == 0) return false;
    }
    struct installer_os_payload *payload = &payloads[*count];
    *payload = (struct installer_os_payload){ .data = data, .size = size };
    copy_text(payload->path, sizeof(payload->path), path);
    kos_sha256(data, size, payload->checksum);
    ++*count;
    return true;
}

static bool build_os_payloads(struct installer_os_payload *payloads,
                              uint32_t *payload_count) {
    const uint8_t *kernel = NULL;
    uint64_t kernel_size = 0;
    const uint8_t *archive = NULL;
    uint64_t archive_size = 0;
    if (!vfs_read_file("C:/KOS.ELF", &kernel, &kernel_size)
        || kernel == NULL || kernel_size == 0
        || !initramfs_get_archive(&archive, &archive_size)
        || !add_payload(payloads, payload_count, "BOOT/KOS.ELF", kernel, kernel_size)
        || !add_payload(payloads, payload_count, "BOOT/INITRAMFS.CPIO", archive,
                        archive_size)) {
        return false;
    }

    for (uint64_t index = 0; index < vfs_file_count_on_volume('C'); ++index) {
        const struct vfs_file *source = vfs_file_at_on_volume('C', index);
        if (source == NULL || source->type == VFS_NODE_DIRECTORY || source->path == NULL) {
            return false;
        }
        if (kstrcmp(source->path, "KOS.ELF") == 0
            || kstrcmp(source->path, "EFI/BOOT/BOOTX64.EFI") == 0) {
            continue;
        }
        if (!path_is_safe_relative(source->path)) return false;

        const uint8_t *data = NULL;
        uint64_t size = 0;
        char source_path[VFS_MAX_PATH];
        char destination_path[VFS_MAX_PATH];
        int source_path_size = snprintf(source_path, sizeof(source_path), "C:/%s", source->path);
        int destination_size = snprintf(destination_path, sizeof(destination_path),
                                        "SYSTEM/%s", source->path);
        if (source_path_size <= 0 || (uint64_t)source_path_size >= sizeof(source_path)
            || destination_size <= 0 || (uint64_t)destination_size >= sizeof(destination_path)
            || !vfs_read_file(source_path, &data, &size)
            || !add_payload(payloads, payload_count, destination_path, data, size)) {
            return false;
        }
    }
    return *payload_count >= 3;
}

static void checksum_to_hex(const uint8_t checksum[32], char output[65]) {
    static const char hexadecimal[] = "0123456789abcdef";
    for (uint32_t index = 0; index < 32; ++index) {
        output[index * 2] = hexadecimal[checksum[index] >> 4];
        output[index * 2 + 1] = hexadecimal[checksum[index] & 0x0F];
    }
    output[64] = '\0';
}

static bool build_checksum_manifest(const struct installer_os_payload *payloads,
                                    uint32_t payload_count,
                                    char *manifest, uint64_t capacity,
                                    uint64_t *manifest_size) {
    if (payloads == NULL || manifest == NULL || manifest_size == NULL) return false;
    uint64_t used = 0;
    for (uint32_t index = 0; index < payload_count; ++index) {
        char hexadecimal[65];
        checksum_to_hex(payloads[index].checksum, hexadecimal);
        int written = snprintf(manifest + used, capacity - used,
                               "%s  /VINO/%s\n", hexadecimal,
                               payloads[index].path);
        if (written <= 0 || (uint64_t)written >= capacity - used) return false;
        used += (uint64_t)written;
    }
    *manifest_size = used;
    return true;
}

int install_base_os(struct installer_flow *flow, void *context) {
    (void)context;
    if (flow == NULL || flow->profile == NULL || flow->selected_disk == NULL
        || flow->selected_partition == NULL
        || !installer_profile_is_valid(flow->profile)
        || !target_is_efi_system_partition(flow->selected_partition)
        || kstrcmp(flow->profile->os_edition, VINO_OS_EDITION_DEFAULT) != 0) {
        log_error("[installer] Select a FAT32 EFI System partition for the Vino OS target.");
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }

    char volume = 0;
    if (!mount_install_target(flow, &volume)) {
        log_error("[installer] Base OS target must be a mounted, valid FAT32 partition.");
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    char install_root[VFS_MAX_PATH];
    if (!make_absolute_path(install_root, sizeof(install_root), volume, "VINO")) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    struct vfs_metadata existing;
    if (vfs_stat(install_root, &existing)) {
        log_error("[installer] VINO directory already exists; refusing to overwrite it.");
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }

    struct installer_os_payload *payloads =
        kcalloc(INSTALLER_MAX_OS_PAYLOADS, sizeof(*payloads));
    struct installer_verify_item *verify_items =
        kcalloc(INSTALLER_MAX_VERIFY_ITEMS, sizeof(*verify_items));
    char (*created_directories)[VFS_MAX_PATH] =
        kcalloc(INSTALLER_MAX_CREATED_DIRECTORIES, VFS_MAX_PATH);
    uint64_t checksum_capacity =
        INSTALLER_MAX_OS_PAYLOADS * (VFS_MAX_PATH + 72u) + 1u;
    char *checksum_manifest = kcalloc(checksum_capacity, 1);
    if (payloads == NULL || verify_items == NULL || created_directories == NULL
        || checksum_manifest == NULL) {
        if (payloads != NULL) kfree(payloads);
        if (verify_items != NULL) kfree(verify_items);
        if (created_directories != NULL) kfree(created_directories);
        if (checksum_manifest != NULL) kfree(checksum_manifest);
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }

    uint32_t payload_count = 0;
    uint32_t verify_count = 0;
    uint32_t directory_count = 0;
    bool ok = build_os_payloads(payloads, &payload_count);
    for (uint32_t index = 0; ok && index < payload_count; ++index) {
        ok = write_verified(volume, payloads[index].path, payloads[index].data,
                            payloads[index].size, verify_items, &verify_count,
                            created_directories, &directory_count);
    }

    uint64_t checksum_size = 0;
    if (ok) {
        ok = build_checksum_manifest(payloads, payload_count, checksum_manifest,
                                     checksum_capacity, &checksum_size)
            && write_verified(volume, "SYSTEM/SHA256SUMS.TXT",
                (const uint8_t *)checksum_manifest, checksum_size,
                verify_items, &verify_count, created_directories, &directory_count);
    }

    struct block_device *partition = flow->selected_partition;
    if (ok) ok = block_cache_flush_device(partition);
    if (ok) {
        block_cache_invalidate_device(partition);
        for (uint32_t index = 0; ok && index < verify_count; ++index) {
            ok = verify_file(volume, &verify_items[index]);
        }
    }

    char metadata[512];
    uint64_t transaction_id = next_transaction_id++;
    if (transaction_id == 0) transaction_id = next_transaction_id++;
    if (ok) {
        int metadata_size = snprintf(metadata, sizeof(metadata),
            "format=VINO_OS_INSTALL_V1\n"
            "os_name=Vino OS\n"
            "os_edition=%s\n"
            "installer_profile=%s\n"
            "transaction_id=%llu\n"
            "kernel=/VINO/BOOT/KOS.ELF\n"
            "initramfs=/VINO/BOOT/INITRAMFS.CPIO\n"
            "checksums=/VINO/SYSTEM/SHA256SUMS.TXT\n"
            "install_state=committed\n",
            VINO_OS_EDITION_DEFAULT, flow->profile->id,
            (unsigned long long)transaction_id);
        ok = metadata_size > 0 && (uint64_t)metadata_size < sizeof(metadata)
            && write_verified(volume, "SYSTEM/OS.META", (const uint8_t *)metadata,
                (uint64_t)metadata_size, verify_items, &verify_count,
                created_directories, &directory_count)
            && block_cache_flush_device(partition);
    }
    if (ok) {
        block_cache_invalidate_device(partition);
        ok = verify_file(volume, &verify_items[verify_count - 1]);
    }

    if (!ok) {
        log_error("[installer] Base OS copy, checksum verification, or metadata commit failed.");
        rollback_created(volume, verify_items, verify_count, created_directories,
                         directory_count, partition);
    }
    else {
        flow->state.transaction.transaction_id = transaction_id;
        flow->state.transaction.base_os_applied = true;
        log_infof("[installer] Base Vino OS committed on %s as edition=%s (%u payload files).",
                  flow->selected_partition->name, VINO_OS_EDITION_DEFAULT, payload_count);
    }

    kfree(checksum_manifest);
    kfree(created_directories);
    kfree(verify_items);
    kfree(payloads);
    return ok ? 0 : INSTALLER_FLOW_ERR_INVALID_STATE;
}

static void set_pending_gui(struct installer_flow *flow, const struct gui_manifest *gui,
                            enum pending_gui_install_state state, const char *message,
                            int last_error) {
    pending_gui_install_init(&flow->state.pending_gui);
    struct pending_gui_install *pending = &flow->state.pending_gui;
    pending->state = state;
    copy_text(pending->gui_id, sizeof(pending->gui_id), gui->id);
    copy_text(pending->repository, sizeof(pending->repository), gui->download_url);
    copy_text(pending->version, sizeof(pending->version), gui->version);
    copy_text(pending->checksum_sha256, sizeof(pending->checksum_sha256),
              gui->checksum_sha256);
    copy_text(pending->signature, sizeof(pending->signature), gui->signature);
    copy_text(pending->error, sizeof(pending->error), message);
    pending->last_error = last_error;
}

struct gui_package_reader {
    const uint8_t *data;
    uint64_t size;
};

static bool gui_package_read(void *context, uint64_t offset, void *buffer, size_t size) {
    struct gui_package_reader *reader = context;
    if (reader == NULL || buffer == NULL || offset > reader->size
        || (uint64_t)size > reader->size - offset) {
        return false;
    }
    kmemcpy(buffer, reader->data + offset, size);
    return true;
}

static uint64_t gui_package_size(void *context) {
    struct gui_package_reader *reader = context;
    return reader == NULL ? 0 : reader->size;
}

int install_gui(struct installer_flow *flow, const struct gui_manifest *gui,
                void *context) {
    (void)context;
    char volume = 0;
    if (flow == NULL || gui == NULL || !gui_manifest_is_valid(gui)
        || !flow->state.transaction.base_os_applied
        || !transaction_target_volume(flow, &volume)) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    if (gui->source == GUI_PACKAGE_SOURCE_REMOTE) {
        set_pending_gui(flow, gui, PENDING_GUI_WAITING_NETWORK,
                        "Remote GUI transport is not available in this installer.",
                        INSTALLER_FLOW_ERR_NOT_IMPLEMENTED);
        log_errorf("[installer] GUI '%s' is pending; remote transport is unavailable.", gui->id);
        return INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
    }

    const uint8_t *package_data = NULL;
    uint64_t package_size = 0;
    if (!vfs_read_file(gui->package_path, &package_data, &package_size)
        || package_data == NULL || package_size == 0) {
        set_pending_gui(flow, gui, PENDING_GUI_FAILED,
                        "The selected GUI package is missing from the installation media.",
                        INSTALLER_FLOW_ERR_NOT_IMPLEMENTED);
        log_errorf("[installer] GUI package is missing: %s", gui->package_path);
        return INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
    }

    struct gui_package_reader memory_reader = {
        .data = package_data,
        .size = package_size,
    };
    struct kpkg_reader reader = {
        .context = &memory_reader,
        .read = gui_package_read,
        .get_size = gui_package_size,
    };
    char target_root[VFS_MAX_PATH];
    char target_data_root[VFS_MAX_PATH];
    if (!make_volume_root(target_root, sizeof(target_root), volume,
                          "VINO/SYSTEM/Apps")
        || !make_volume_root(target_data_root, sizeof(target_data_root), volume,
                             "VINO/SYSTEM/Data")) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    struct installer_options options = {
        .target_root = target_root,
        .target_data_root = target_data_root,
    };
    int result = installer_install(reader, &options);
    if (result != INSTALLER_OK) {
        set_pending_gui(flow, gui, PENDING_GUI_FAILED,
                        installer_status_string(result), result);
        log_errorf("[installer] GUI package install failed for %s: %s",
                   gui->id, installer_status_string(result));
        return result;
    }

    char selected_gui_path[VFS_MAX_PATH];
    char selection[160];
    int selection_size = snprintf(selection, sizeof(selection),
                                  "gui_id=%s\nversion=%s\n",
                                  gui->id, gui->version);
    if (selection_size <= 0 || (uint64_t)selection_size >= sizeof(selection)
        || !make_absolute_path(selected_gui_path, sizeof(selected_gui_path), volume,
                               "VINO/SYSTEM/GUI/SELECTED.TXT")
        || vfs_stat(selected_gui_path, &(struct vfs_metadata){0})
        || !vfs_write_file(selected_gui_path, (const uint8_t *)selection,
                           (uint64_t)selection_size)
        || !block_cache_flush_device(flow->selected_partition)) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    block_cache_invalidate_device(flow->selected_partition);
    struct installer_verify_item selection_item = { .size = (uint64_t)selection_size };
    copy_text(selection_item.path, sizeof(selection_item.path),
              "VINO/SYSTEM/GUI/SELECTED.TXT");
    kos_sha256((const uint8_t *)selection, (uint64_t)selection_size,
               selection_item.checksum);
    if (!verify_file(volume, &selection_item)) return INSTALLER_FLOW_ERR_INVALID_STATE;
    pending_gui_install_init(&flow->state.pending_gui);
    log_infof("[installer] GUI '%s' installed under Vino OS edition=default.", gui->id);
    return 0;
}

int install_bootloader(struct installer_flow *flow, void *context) {
    (void)context;
    char volume = 0;
    if (flow == NULL || !flow->state.transaction.base_os_applied
        || !target_is_efi_system_partition(flow->selected_partition)
        || !transaction_target_volume(flow, &volume)) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }

    const uint8_t *efi_binary = NULL;
    uint64_t efi_size = 0;
    if (!vfs_read_file("C:/EFI/BOOT/BOOTX64.EFI", &efi_binary, &efi_size)
        || efi_binary == NULL || efi_size == 0 || efi_size > UINT32_MAX) {
        log_error("[installer] Limine UEFI boot application is missing from installer media.");
        return INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
    }

    static const uint8_t limine_configuration[] =
        "timeout: 3\n"
        "quiet: yes\n"
        "serial: yes\n"
        "\n"
        "/Vino OS\n"
        "protocol: limine\n"
        "path: boot():/VINO/BOOT/KOS.ELF\n"
        "kaslr: no\n"
        "paging_mode: 4level\n"
        "resolution: 1024x768x32\n"
        "module_path: boot():/VINO/BOOT/INITRAMFS.CPIO\n"
        "module_string: kos-initramfs\n";

    char (*created_directories)[VFS_MAX_PATH] =
        kcalloc(INSTALLER_MAX_CREATED_DIRECTORIES, VFS_MAX_PATH);
    struct installer_verify_item *verify_items =
        kcalloc(3, sizeof(*verify_items));
    if (created_directories == NULL || verify_items == NULL) {
        if (created_directories != NULL) kfree(created_directories);
        if (verify_items != NULL) kfree(verify_items);
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }

    const char *efi_path = "EFI/BOOT/BOOTX64.EFI";
    const char *config_path = "EFI/BOOT/limine.conf";
    char absolute[VFS_MAX_PATH];
    struct vfs_metadata metadata;
    bool ok = make_absolute_path(absolute, sizeof(absolute), volume, efi_path)
        && !vfs_stat(absolute, &metadata)
        && make_absolute_path(absolute, sizeof(absolute), volume, config_path)
        && !vfs_stat(absolute, &metadata);
    uint32_t directory_count = 0;
    uint32_t verify_count = 0;
    if (ok) {
        ok = write_verified(volume, efi_path, efi_binary, efi_size,
                            verify_items, &verify_count, created_directories,
                            &directory_count)
            && write_verified(volume, config_path, limine_configuration,
                sizeof(limine_configuration) - 1, verify_items, &verify_count,
                created_directories, &directory_count);
    }

    char checksum_record[256];
    if (ok) {
        char efi_hex[65];
        char config_hex[65];
        uint8_t config_checksum[32];
        kos_sha256(efi_binary, efi_size, verify_items[0].checksum);
        checksum_to_hex(verify_items[0].checksum, efi_hex);
        kos_sha256(limine_configuration, sizeof(limine_configuration) - 1,
                   config_checksum);
        checksum_to_hex(config_checksum, config_hex);
        int record_size = snprintf(checksum_record, sizeof(checksum_record),
            "%s  /EFI/BOOT/BOOTX64.EFI\n%s  /EFI/BOOT/limine.conf\n",
            efi_hex, config_hex);
        ok = record_size > 0 && (uint64_t)record_size < sizeof(checksum_record)
            && write_verified(volume, "VINO/SYSTEM/BOOT.SHA256",
                (const uint8_t *)checksum_record, (uint64_t)record_size,
                verify_items, &verify_count, created_directories, &directory_count);
    }
    if (ok) ok = block_cache_flush_device(flow->selected_partition);
    if (ok) {
        block_cache_invalidate_device(flow->selected_partition);
        for (uint32_t index = 0; ok && index < verify_count; ++index) {
            ok = verify_file(volume, &verify_items[index]);
        }
    }
    if (!ok) {
        rollback_created(volume, verify_items, verify_count, created_directories,
                         directory_count, flow->selected_partition);
        log_error("[installer] Limine bootloader copy or checksum verification failed.");
    }
    else {
        log_info("[installer] Limine UEFI bootloader committed; Vino OS is bootable in CLI mode.");
    }
    kfree(verify_items);
    kfree(created_directories);
    return ok ? 0 : INSTALLER_FLOW_ERR_INVALID_STATE;
}
