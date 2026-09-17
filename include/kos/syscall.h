#ifndef KOS_SYSCALL_H
#define KOS_SYSCALL_H

#include <stdint.h>

/* Stable user/kernel ABI for the first Ring 3 programs. Arguments use the
 * System V register convention and return values are placed in RAX. */
enum kos_syscall_number {
    KOS_SYSCALL_GETTIME = 1,
    KOS_SYSCALL_SYSINFO = 2,
    KOS_SYSCALL_WRITE = 3,
    KOS_SYSCALL_EXIT = 4,
};

uint64_t kos_syscall_dispatch(uint64_t number, uint64_t arg0, uint64_t arg1,
    uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
const char *kos_syscall_name(uint64_t number);

#endif
