#ifndef KOS_VFS_H
#define KOS_VFS_H

#include <stdbool.h>
#include <stdint.h>

#define VFS_MAX_PATH 256

enum vfs_node_type {
    VFS_NODE_FILE = 1,
    VFS_NODE_DIRECTORY = 2,
};

struct vfs_metadata {
    enum vfs_node_type type;
    uint64_t size;
    uint32_t attributes;
};

struct vfs_file {
    const char *path;
    const uint8_t *data;
    uint64_t size;
    enum vfs_node_type type;
    uint32_t attributes;
};

struct vfs_dir_entry {
    char name[VFS_MAX_PATH];
    uint64_t size;
    enum vfs_node_type type;
    uint32_t attributes;
};

typedef void (*vfs_readdir_callback)(const struct vfs_dir_entry *entry, void *user_arg);
typedef void (*vfs_file_data_release_callback)(void *context, const uint8_t *data,
                                               uint64_t size);

struct vfs_file_data {
    const uint8_t *data;
    uint64_t size;
    uint64_t revision;
    void *release_context;
    vfs_file_data_release_callback release;
};

struct vfs_backend {
    void *context;
    bool (*read_file)(void *context, const char *path, const uint8_t **data, uint64_t *size);
    vfs_file_data_release_callback release_file_data;
    bool (*write_file)(void *context, const char *path, const uint8_t *data, uint64_t size);
    uint64_t (*file_count)(void *context);
    const struct vfs_file *(*file_at)(void *context, uint64_t index);
    bool (*unlink_file)(void *context, const char *path);
    bool (*mkdir)(void *context, const char *path);
    bool (*readdir)(void *context, const char *path, vfs_readdir_callback callback, void *user_arg);
    bool (*rename)(void *context, const char *old_path, const char *new_path);
    bool (*stat)(void *context, const char *path, struct vfs_metadata *metadata);
};

struct vfs_volume_info {
    char letter;
    const char *label;
    uint64_t file_count;
};

/* Volumes use one ASCII letter, for example C: or D:. */
bool vfs_mount_volume(char letter, const char *label, const struct vfs_backend *backend);
bool vfs_unmount_volume(char letter);
bool vfs_mount_root(const struct vfs_backend *backend);
bool vfs_is_initialized(void);
bool vfs_read_file(const char *path, const uint8_t **data, uint64_t *size);
bool vfs_read_file_acquire(const char *path, struct vfs_file_data *file_data);
void vfs_file_data_release(struct vfs_file_data *file_data);
uint64_t vfs_content_revision(void);
bool vfs_write_file(const char *path, const uint8_t *data, uint64_t size);
bool vfs_unlink_file(const char *path);
bool vfs_mkdir(const char *path);
bool vfs_readdir(const char *path, vfs_readdir_callback callback, void *user_arg);
bool vfs_rename(const char *old_path, const char *new_path);
bool vfs_stat(const char *path, struct vfs_metadata *metadata);
uint64_t vfs_file_count(void);
const struct vfs_file *vfs_file_at(uint64_t index);
uint64_t vfs_volume_count(void);
bool vfs_volume_info_at(uint64_t index, struct vfs_volume_info *info);
bool vfs_volume_is_mounted(char letter);
uint64_t vfs_file_count_on_volume(char letter);
const struct vfs_file *vfs_file_at_on_volume(char letter, uint64_t index);

#endif
