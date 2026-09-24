#include <stdint.h>

#include <uapi/kos/syscall_numbers.h>
#include <vino/vfs.h>

static long vino_vfs_syscall2(uint64_t number, uint64_t arg0, uint64_t arg1) {
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = arg0;
    register uint64_t second __asm__("rsi") = arg1;
    __asm__ volatile("syscall"
        : "+a"(result)
        : "D"(first), "S"(second)
        : "rcx", "r11", "memory");
    return (long)result;
}

static long vino_vfs_syscall3(uint64_t number, uint64_t arg0, uint64_t arg1,
                              uint64_t arg2) {
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = arg0;
    register uint64_t second __asm__("rsi") = arg1;
    register uint64_t third __asm__("rdx") = arg2;
    __asm__ volatile("syscall"
        : "+a"(result)
        : "D"(first), "S"(second), "d"(third)
        : "rcx", "r11", "memory");
    return (long)result;
}

long vino_fs_open(const char *path, uint32_t flags) {
    if ((flags & KOS_O_ACCMODE) != KOS_O_RDONLY
        || (flags & (KOS_O_CREAT | KOS_O_EXCL | KOS_O_TRUNC | KOS_O_APPEND)) != 0) {
        return -1;
    }
    return vino_vfs_syscall2(SYS_open, (uint64_t)path, flags);
}

long vino_fs_close(long descriptor) {
    return vino_vfs_syscall2(SYS_close, (uint64_t)descriptor, 0);
}

long vino_fs_read(long descriptor, void *buffer, uint64_t length) {
    return vino_vfs_syscall3(SYS_read, (uint64_t)descriptor, (uint64_t)buffer, length);
}

long vino_fs_stat(const char *path, struct kos_stat *stat) {
    return vino_vfs_syscall2(SYS_stat, (uint64_t)path, (uint64_t)stat);
}

long vino_fs_getdents(long descriptor, struct kos_dirent *entries, uint64_t capacity) {
    return vino_vfs_syscall3(SYS_getdents, (uint64_t)descriptor,
                             (uint64_t)entries, capacity);
}
