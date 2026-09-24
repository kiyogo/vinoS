#ifndef VINO_USER_VFS_H
#define VINO_USER_VFS_H

#include <stdint.h>

#include <uapi/kos/syscall_numbers.h>

/* Read-only VFS client for UI file browsers and preview panes. */
long vino_fs_open(const char *path, uint32_t flags);
long vino_fs_close(long descriptor);
long vino_fs_read(long descriptor, void *buffer, uint64_t length);
long vino_fs_stat(const char *path, struct kos_stat *stat);
long vino_fs_getdents(long descriptor, struct kos_dirent *entries, uint64_t capacity);

#endif /* VINO_USER_VFS_H */
