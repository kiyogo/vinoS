#include <stdbool.h>
#include <stdint.h>

#include <kos/file.h>
#include <kos/handle.h>
#include <kos/heap.h>
#include <kos/memory.h>
#include <uapi/kos/syscall_numbers.h>

static void kos_file_destroy(void *object) {
    if (object != 0) {
        kfree(object);
    }
}

static int vfs_error_for_path(const char *path) {
    struct vfs_metadata metadata;
    return vfs_stat(path, &metadata) ? -EEXIST : -ENOENT;
}

struct kos_file *kos_file_open(const char *path, uint32_t flags, int *error_out) {
    int error = -EINVAL;
    uint32_t access = flags & KOS_O_ACCMODE;
    if (path == 0 || *path == '\0' || access == 3) goto fail;

    struct vfs_metadata metadata;
    bool exists = vfs_stat(path, &metadata);
    if (!exists) {
        if ((flags & KOS_O_CREAT) == 0) {
            error = -ENOENT;
            goto fail;
        }
        if (!vfs_write_file(path, (const uint8_t *)"", 0)) {
            error = vfs_error_for_path(path);
            goto fail;
        }
        if (!vfs_stat(path, &metadata)) {
            error = -EIO;
            goto fail;
        }
    } else if ((flags & KOS_O_CREAT) != 0 && (flags & KOS_O_EXCL) != 0) {
        error = -EEXIST;
        goto fail;
    }

    if ((flags & KOS_O_DIRECTORY) != 0 && metadata.type != VFS_NODE_DIRECTORY) {
        error = -ENOTDIR;
        goto fail;
    }
    if (metadata.type == VFS_NODE_DIRECTORY && access != KOS_O_RDONLY) {
        error = -EISDIR;
        goto fail;
    }
    if ((flags & KOS_O_TRUNC) != 0 && metadata.type == VFS_NODE_FILE && access != KOS_O_RDONLY
        && !vfs_write_file(path, (const uint8_t *)"", 0)) {
        error = -EIO;
        goto fail;
    }

    struct kos_file *file = kmalloc(sizeof(*file));
    if (file == 0) {
        error = -ENOMEM;
        goto fail;
    }
    kos_object_init(&file->header, KOS_OBJ_FILE, kos_file_destroy);
    kstrncpy(file->path, path, sizeof(file->path) - 1);
    file->flags = flags;
    file->can_read = access == KOS_O_RDONLY || access == KOS_O_RDWR;
    file->can_write = access == KOS_O_WRONLY || access == KOS_O_RDWR;
    file->type = metadata.type;
    file->offset = 0;
    return file;

fail:
    if (error_out != 0) *error_out = error;
    return 0;
}

int kos_file_read(struct kos_file *file, void *buffer, uint64_t length, uint64_t *read_out) {
    if (read_out == 0 || file == 0 || (buffer == 0 && length != 0)) return -EFAULT;
    *read_out = 0;
    if (!file->can_read) return -EBADF;
    if (file->type == VFS_NODE_DIRECTORY) return -EISDIR;

    uint64_t flags = spinlock_lock_irqsave(&file->header.lock);
    const uint8_t *data = 0;
    uint64_t size = 0;
    if (!vfs_read_file(file->path, &data, &size)) {
        spinlock_unlock_irqrestore(&file->header.lock, flags);
        return -EIO;
    }
    if (file->offset < size) {
        uint64_t available = size - file->offset;
        uint64_t count = length < available ? length : available;
        kmemcpy(buffer, data + file->offset, count);
        file->offset += count;
        *read_out = count;
    }
    spinlock_unlock_irqrestore(&file->header.lock, flags);
    return 0;
}

int kos_file_write(struct kos_file *file, const void *buffer, uint64_t length, uint64_t *written_out) {
    if (written_out == 0 || file == 0 || (buffer == 0 && length != 0)) return -EFAULT;
    *written_out = 0;
    if (!file->can_write) return -EBADF;
    if (file->type == VFS_NODE_DIRECTORY) return -EISDIR;

    uint64_t flags = spinlock_lock_irqsave(&file->header.lock);
    const uint8_t *old_data = 0;
    uint64_t old_size = 0;
    if (!vfs_read_file(file->path, &old_data, &old_size)) {
        spinlock_unlock_irqrestore(&file->header.lock, flags);
        return -EIO;
    }
    if ((file->flags & KOS_O_APPEND) != 0) file->offset = old_size;
    if (file->offset > UINT64_MAX - length) {
        spinlock_unlock_irqrestore(&file->header.lock, flags);
        return -EOVERFLOW;
    }
    uint64_t end = file->offset + length;
    uint64_t new_size = old_size > end ? old_size : end;
    uint8_t *new_data = new_size == 0 ? 0 : kmalloc(new_size);
    if (new_size != 0 && new_data == 0) {
        spinlock_unlock_irqrestore(&file->header.lock, flags);
        return -ENOMEM;
    }
    if (old_size != 0) kmemcpy(new_data, old_data, old_size);
    if (end > old_size) kmemset(new_data + old_size, 0, end - old_size);
    if (length != 0) kmemcpy(new_data + file->offset, buffer, length);

    bool ok = vfs_write_file(file->path, new_data, new_size);
    if (new_data != 0) kfree(new_data);
    if (!ok) {
        spinlock_unlock_irqrestore(&file->header.lock, flags);
        return -EIO;
    }
    file->offset = end;
    *written_out = length;
    spinlock_unlock_irqrestore(&file->header.lock, flags);
    return 0;
}

int kos_file_seek(struct kos_file *file, int64_t offset, uint32_t whence, uint64_t *position_out) {
    if (file == 0 || position_out == 0) return -EFAULT;
    if (file->type == VFS_NODE_DIRECTORY) return -ESPIPE;
    if (whence != KOS_SEEK_SET && whence != KOS_SEEK_CUR && whence != KOS_SEEK_END) return -EINVAL;

    uint64_t flags = spinlock_lock_irqsave(&file->header.lock);
    uint64_t base = file->offset;
    if (whence == KOS_SEEK_SET) {
        base = 0;
    } else if (whence == KOS_SEEK_END) {
        struct vfs_metadata metadata;
        if (!vfs_stat(file->path, &metadata)) {
            spinlock_unlock_irqrestore(&file->header.lock, flags);
            return -EIO;
        }
        base = metadata.size;
    }

    uint64_t next = 0;
    if (offset >= 0) {
        uint64_t delta = (uint64_t)offset;
        if (base > UINT64_MAX - delta) {
            spinlock_unlock_irqrestore(&file->header.lock, flags);
            return -EOVERFLOW;
        }
        next = base + delta;
    } else {
        uint64_t delta = (uint64_t)(-(offset + 1)) + 1;
        if (delta > base) {
            spinlock_unlock_irqrestore(&file->header.lock, flags);
            return -EINVAL;
        }
        next = base - delta;
    }

    file->offset = next;
    *position_out = next;
    spinlock_unlock_irqrestore(&file->header.lock, flags);
    return 0;
}

int kos_file_stat(const struct kos_file *file, struct vfs_metadata *metadata) {
    if (file == 0 || metadata == 0) return -EFAULT;
    return vfs_stat(file->path, metadata) ? 0 : -EIO;
}

struct getdents_bridge {
    kos_file_dir_callback callback;
    void *user_arg;
    uint64_t start;
    uint64_t index;
    uint64_t next_offset;
    bool stopped;
};

static void kos_file_getdents_bridge(const struct vfs_dir_entry *entry, void *user_arg) {
    struct getdents_bridge *bridge = user_arg;
    uint64_t current = bridge->index++;
    if (current < bridge->start || bridge->stopped) return;
    if (!bridge->callback(entry, bridge->user_arg)) {
        bridge->stopped = true;
        return;
    }
    bridge->next_offset = current + 1;
}

int kos_file_getdents(struct kos_file *file, kos_file_dir_callback callback, void *user_arg) {
    if (file == 0 || callback == 0) return -EFAULT;
    if (file->type != VFS_NODE_DIRECTORY) return -ENOTDIR;
    uint64_t flags = spinlock_lock_irqsave(&file->header.lock);
    struct getdents_bridge bridge = {
        .callback = callback,
        .user_arg = user_arg,
        .start = file->offset,
        .next_offset = file->offset,
    };
    bool ok = vfs_readdir(file->path, kos_file_getdents_bridge, &bridge);
    if (ok) file->offset = bridge.next_offset;
    spinlock_unlock_irqrestore(&file->header.lock, flags);
    return ok ? 0 : -EIO;
}
