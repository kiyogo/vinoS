#include <stdbool.h>
#include <stdint.h>

#include <kos/ramfs.h>
#include <kos/memory.h>
#include <kos/sync.h>

enum {
    RAMFS_DYNAMIC_FILE_MAX = 32,
    RAMFS_DYNAMIC_PATH_MAX = 128,
    RAMFS_DYNAMIC_FILE_SIZE_MAX = 8192,
};

static const uint8_t readme[] =
    "KOS Data volume\n"
    "This is an independent in-memory D: volume.\n"
    "It proves the VFS volume namespace before a disk/FAT32 backend is added.\n";

static const struct vfs_file built_in_files[] = {
    { .path = "README.TXT", .data = readme, .size = sizeof(readme) - 1, .type = VFS_NODE_FILE },
};

struct ramfs_dynamic_file {
    bool used;
    char path[RAMFS_DYNAMIC_PATH_MAX];
    uint8_t data[RAMFS_DYNAMIC_FILE_SIZE_MAX];
    struct vfs_file view;
    bool is_directory;
};

static struct ramfs_dynamic_file dynamic_files[RAMFS_DYNAMIC_FILE_MAX];
static bool initialized;
static struct kos_spinlock ramfs_lock = KOS_SPINLOCK_INITIALIZER;

static bool strings_equal(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == *right;
}

static uint64_t path_length(const char *path) {
    uint64_t length = 0;
    while (path[length] != '\0') length++;
    return length;
}

static bool ramfs_parent_exists_locked(const char *path) {
    const char *slash = 0;
    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/') slash = cursor;
    }
    if (slash == 0) return true;
    uint64_t parent_length = (uint64_t)(slash - path);
    if (parent_length == 0) return true;
    for (uint64_t i = 0; i < RAMFS_DYNAMIC_FILE_MAX; i++) {
        if (!dynamic_files[i].used || !dynamic_files[i].is_directory) continue;
        uint64_t candidate_length = path_length(dynamic_files[i].path);
        if (candidate_length == parent_length &&
            kstrncmp(dynamic_files[i].path, path, parent_length) == 0) return true;
    }
    return false;
}

static bool ramfs_is_direct_child(const char *path, const char *directory, char name[RAMFS_DYNAMIC_PATH_MAX]) {
    uint64_t directory_length = path_length(directory);
    uint64_t path_size = path_length(path);
    uint64_t prefix_length = directory_length == 0 ? 0 : directory_length + 1;
    if (path_size <= prefix_length || (prefix_length > 0 && kstrncmp(path, directory, directory_length) != 0) ||
        (prefix_length > 0 && path[directory_length] != '/')) return false;
    const char *entry_name = path + prefix_length;
    uint64_t entry_length = path_size - prefix_length;
    for (uint64_t i = 0; i < entry_length; i++) {
        if (entry_name[i] == '/') return false;
        name[i] = entry_name[i];
    }
    name[entry_length] = '\0';
    return entry_length > 0;
}

static bool ramfs_read_file(void *context, const char *path, const uint8_t **data, uint64_t *size) {
    (void)context;
    uint64_t flags = spinlock_lock_irqsave(&ramfs_lock);
    if (!initialized || path == 0 || data == 0 || size == 0) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    for (uint64_t index = 0; index < sizeof(built_in_files) / sizeof(built_in_files[0]); ++index) {
        if (strings_equal(path, built_in_files[index].path)) {
            *data = built_in_files[index].data;
            *size = built_in_files[index].size;
            spinlock_unlock_irqrestore(&ramfs_lock, flags);
            return true;
        }
    }
    for (uint64_t index = 0; index < RAMFS_DYNAMIC_FILE_MAX; ++index) {
        if (dynamic_files[index].used && !dynamic_files[index].is_directory && strings_equal(path, dynamic_files[index].path)) {
            *data = dynamic_files[index].data;
            *size = dynamic_files[index].view.size;
            spinlock_unlock_irqrestore(&ramfs_lock, flags);
            return true;
        }
    }
    spinlock_unlock_irqrestore(&ramfs_lock, flags);
    return false;
}

static uint64_t ramfs_file_count(void *context) {
    (void)context;
    uint64_t flags = spinlock_lock_irqsave(&ramfs_lock);
    if (!initialized) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return 0;
    }
    uint64_t count = sizeof(built_in_files) / sizeof(built_in_files[0]);
    for (uint64_t index = 0; index < RAMFS_DYNAMIC_FILE_MAX; ++index) {
        if (dynamic_files[index].used && !dynamic_files[index].is_directory) {
            ++count;
        }
    }
    spinlock_unlock_irqrestore(&ramfs_lock, flags);
    return count;
}

static const struct vfs_file *ramfs_file_at(void *context, uint64_t index) {
    (void)context;
    uint64_t flags = spinlock_lock_irqsave(&ramfs_lock);
    if (!initialized) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return 0;
    }
    const uint64_t built_in_count = sizeof(built_in_files) / sizeof(built_in_files[0]);
    if (index < built_in_count) {
        const struct vfs_file *file = &built_in_files[index];
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return file;
    }
    index -= built_in_count;
    for (uint64_t slot = 0; slot < RAMFS_DYNAMIC_FILE_MAX; ++slot) {
        if (dynamic_files[slot].used && !dynamic_files[slot].is_directory) {
            if (index == 0) {
                const struct vfs_file *file = &dynamic_files[slot].view;
                spinlock_unlock_irqrestore(&ramfs_lock, flags);
                return file;
            }
            --index;
        }
    }
    spinlock_unlock_irqrestore(&ramfs_lock, flags);
    return 0;
}

bool ramfs_write_file(const char *path, const uint8_t *data, uint64_t size) {
    if (path == 0 || *path == '\0' || (data == 0 && size != 0)
        || size > RAMFS_DYNAMIC_FILE_SIZE_MAX) {
        return false;
    }
    uint64_t path_size = 0;
    while (path[path_size] != '\0') {
        if (path_size + 1 >= RAMFS_DYNAMIC_PATH_MAX) {
            return false;
        }
        ++path_size;
    }

    uint64_t flags = spinlock_lock_irqsave(&ramfs_lock);
    if (!initialized) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }

    if (!ramfs_parent_exists_locked(path)) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }

    struct ramfs_dynamic_file *file = 0;
    for (uint64_t index = 0; index < RAMFS_DYNAMIC_FILE_MAX; ++index) {
        if (dynamic_files[index].used && strings_equal(path, dynamic_files[index].path)) {
            if (dynamic_files[index].is_directory) {
                spinlock_unlock_irqrestore(&ramfs_lock, flags);
                return false;
            }
            file = &dynamic_files[index];
            break;
        }
        if (!dynamic_files[index].used && file == 0) {
            file = &dynamic_files[index];
        }
    }
    if (file == 0) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    for (uint64_t index = 0; index <= path_size; ++index) {
        file->path[index] = path[index];
    }
    for (uint64_t index = 0; index < size; ++index) {
        file->data[index] = data[index];
    }
    file->used = true;
    file->is_directory = false;
    file->view = (struct vfs_file){ .path = file->path, .data = file->data, .size = size,
                                    .type = VFS_NODE_FILE, .attributes = 0x20 };
    spinlock_unlock_irqrestore(&ramfs_lock, flags);
    return true;
}

bool ramfs_unlink_file(const char *path) {
    if (path == 0 || *path == '\0') {
        return false;
    }
    uint64_t flags = spinlock_lock_irqsave(&ramfs_lock);
    if (!initialized) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    for (uint64_t index = 0; index < RAMFS_DYNAMIC_FILE_MAX; ++index) {
        if (dynamic_files[index].used && strings_equal(path, dynamic_files[index].path)) {
            if (dynamic_files[index].is_directory) {
                for (uint64_t child = 0; child < RAMFS_DYNAMIC_FILE_MAX; child++) {
                    if (dynamic_files[child].used && dynamic_files[child].path[0] != '\0' &&
                        kstrncmp(dynamic_files[child].path, path, path_length(path)) == 0 &&
                        dynamic_files[child].path[path_length(path)] == '/') {
                        spinlock_unlock_irqrestore(&ramfs_lock, flags);
                        return false;
                    }
                }
            }
            dynamic_files[index].used = false;
            dynamic_files[index].is_directory = false;
            dynamic_files[index].path[0] = '\0';
            dynamic_files[index].view.size = 0;
            spinlock_unlock_irqrestore(&ramfs_lock, flags);
            return true;
        }
    }
    spinlock_unlock_irqrestore(&ramfs_lock, flags);
    return false;
}

static bool ramfs_vfs_write_file(void *context, const char *path, const uint8_t *data, uint64_t size) {
    (void)context;
    return ramfs_write_file(path, data, size);
}

static bool ramfs_vfs_unlink_file(void *context, const char *path) {
    (void)context;
    return ramfs_unlink_file(path);
}

static bool ramfs_mkdir(void *context, const char *path) {
    (void)context;
    if (path == 0 || *path == '\0' || path_length(path) >= RAMFS_DYNAMIC_PATH_MAX) return false;
    uint64_t flags = spinlock_lock_irqsave(&ramfs_lock);
    if (!initialized) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    if (!ramfs_parent_exists_locked(path)) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    struct ramfs_dynamic_file *slot = 0;
    for (uint64_t i = 0; i < RAMFS_DYNAMIC_FILE_MAX; i++) {
        if (dynamic_files[i].used && strings_equal(path, dynamic_files[i].path)) {
            spinlock_unlock_irqrestore(&ramfs_lock, flags);
            return false;
        }
        if (!dynamic_files[i].used && slot == 0) slot = &dynamic_files[i];
    }
    if (slot == 0) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    kstrncpy(slot->path, path, sizeof(slot->path) - 1);
    slot->used = true;
    slot->is_directory = true;
    slot->view = (struct vfs_file){ .path = slot->path, .type = VFS_NODE_DIRECTORY,
                                    .attributes = 0x10 };
    spinlock_unlock_irqrestore(&ramfs_lock, flags);
    return true;
}

static bool ramfs_readdir(void *context, const char *path, vfs_readdir_callback callback, void *user_arg) {
    (void)context;
    if (path == 0 || callback == 0) return false;
    uint64_t flags = spinlock_lock_irqsave(&ramfs_lock);
    if (!initialized) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    if (*path != '\0') {
        bool directory_found = false;
        for (uint64_t i = 0; i < RAMFS_DYNAMIC_FILE_MAX; i++) {
            if (dynamic_files[i].used && dynamic_files[i].is_directory && strings_equal(path, dynamic_files[i].path)) {
                directory_found = true;
                break;
            }
        }
        if (!directory_found) {
            spinlock_unlock_irqrestore(&ramfs_lock, flags);
            return false;
        }
    }
    for (uint64_t i = 0; i < sizeof(built_in_files) / sizeof(built_in_files[0]); i++) {
        char name[RAMFS_DYNAMIC_PATH_MAX];
        if (*path == '\0' && ramfs_is_direct_child(built_in_files[i].path, path, name)) {
            struct vfs_dir_entry entry = {0};
            kstrncpy(entry.name, name, sizeof(entry.name) - 1);
            entry.size = built_in_files[i].size;
            entry.type = VFS_NODE_FILE;
            callback(&entry, user_arg);
        }
    }
    for (uint64_t i = 0; i < RAMFS_DYNAMIC_FILE_MAX; i++) {
        if (!dynamic_files[i].used) continue;
        char name[RAMFS_DYNAMIC_PATH_MAX];
        if (!ramfs_is_direct_child(dynamic_files[i].path, path, name)) continue;
        struct vfs_dir_entry entry = {0};
        kstrncpy(entry.name, name, sizeof(entry.name) - 1);
        entry.size = dynamic_files[i].is_directory ? 0 : dynamic_files[i].view.size;
        entry.type = dynamic_files[i].is_directory ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
        entry.attributes = dynamic_files[i].view.attributes;
        callback(&entry, user_arg);
    }
    spinlock_unlock_irqrestore(&ramfs_lock, flags);
    return true;
}

static bool ramfs_rename(void *context, const char *old_path, const char *new_path) {
    (void)context;
    if (old_path == 0 || new_path == 0 || path_length(new_path) >= RAMFS_DYNAMIC_PATH_MAX) return false;
    uint64_t flags = spinlock_lock_irqsave(&ramfs_lock);
    if (!initialized) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    struct ramfs_dynamic_file *source = 0;
    if (!ramfs_parent_exists_locked(new_path)) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    for (uint64_t i = 0; i < RAMFS_DYNAMIC_FILE_MAX; i++) {
        if (dynamic_files[i].used && strings_equal(old_path, dynamic_files[i].path)) source = &dynamic_files[i];
        if (dynamic_files[i].used && strings_equal(new_path, dynamic_files[i].path)) source = 0;
    }
    if (source == 0 || source->is_directory) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    char new_name[RAMFS_DYNAMIC_PATH_MAX];
    kstrncpy(new_name, new_path, sizeof(new_name) - 1);
    kstrncpy(source->path, new_name, sizeof(source->path) - 1);
    source->view.path = source->path;
    spinlock_unlock_irqrestore(&ramfs_lock, flags);
    return true;
}

static bool ramfs_stat(void *context, const char *path, struct vfs_metadata *metadata) {
    (void)context;
    if (path == 0 || metadata == 0) return false;
    uint64_t flags = spinlock_lock_irqsave(&ramfs_lock);
    if (!initialized) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    if (*path == '\0') {
        *metadata = (struct vfs_metadata){ .type = VFS_NODE_DIRECTORY };
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return true;
    }
    for (uint64_t i = 0; i < RAMFS_DYNAMIC_FILE_MAX; i++) {
        if (dynamic_files[i].used && strings_equal(path, dynamic_files[i].path)) {
            *metadata = (struct vfs_metadata){
                .type = dynamic_files[i].is_directory ? VFS_NODE_DIRECTORY : VFS_NODE_FILE,
                .size = dynamic_files[i].is_directory ? 0 : dynamic_files[i].view.size,
                .attributes = dynamic_files[i].view.attributes,
            };
            spinlock_unlock_irqrestore(&ramfs_lock, flags);
            return true;
        }
    }
    if (strings_equal(path, "README.TXT")) {
        *metadata = (struct vfs_metadata){ .type = VFS_NODE_FILE, .size = sizeof(readme) - 1 };
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return true;
    }
    spinlock_unlock_irqrestore(&ramfs_lock, flags);
    return false;
}

const struct vfs_backend ramfs_vfs_backend = {
    .context = 0,
    .read_file = ramfs_read_file,
    .write_file = ramfs_vfs_write_file,
    .file_count = ramfs_file_count,
    .file_at = ramfs_file_at,
    .unlink_file = ramfs_vfs_unlink_file,
    .mkdir = ramfs_mkdir,
    .readdir = ramfs_readdir,
    .rename = ramfs_rename,
    .stat = ramfs_stat,
};

bool ramfs_initialize(void) {
    uint64_t flags = spinlock_lock_irqsave(&ramfs_lock);
    if (initialized) {
        spinlock_unlock_irqrestore(&ramfs_lock, flags);
        return false;
    }
    initialized = true;
    spinlock_unlock_irqrestore(&ramfs_lock, flags);
    return true;
}
