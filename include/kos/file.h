#ifndef KOS_FILE_H
#define KOS_FILE_H

#include <stdbool.h>
#include <stdint.h>

#include <kos/object.h>
#include <kos/vfs.h>

struct kos_file {
    struct kos_object_header header;
    char path[VFS_MAX_PATH];
    uint32_t flags;
    bool can_read;
    bool can_write;
    enum vfs_node_type type;
    uint64_t offset;
};

typedef bool (*kos_file_dir_callback)(const struct vfs_dir_entry *entry, void *user_arg);

struct kos_file *kos_file_open(const char *path, uint32_t flags, int *error_out);
int kos_file_read(struct kos_file *file, void *buffer, uint64_t length, uint64_t *read_out);
int kos_file_write(struct kos_file *file, const void *buffer, uint64_t length, uint64_t *written_out);
int kos_file_seek(struct kos_file *file, int64_t offset, uint32_t whence, uint64_t *position_out);
int kos_file_stat(const struct kos_file *file, struct vfs_metadata *metadata);
int kos_file_getdents(struct kos_file *file, kos_file_dir_callback callback, void *user_arg);

#endif /* KOS_FILE_H */
