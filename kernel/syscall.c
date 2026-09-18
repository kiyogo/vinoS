#include <stdint.h>

#include <kos/syscall.h>
#include <kos/task.h>
#include <kos/timer.h>
#include <kos/process.h>

uint64_t kos_syscall_dispatch(uint64_t number, uint64_t arg0, uint64_t arg1,
        uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    switch (number) {
        case KOS_SYSCALL_GETTIME:
            return timer_uptime_seconds();
        case KOS_SYSCALL_SYSINFO:
            return task_count();
        case KOS_SYSCALL_WRITE:
            /* Pointer validation and a user copy belong to the process VM
             * layer. Refuse this call until that layer is active. */
            (void)arg0;
            return UINT64_MAX;
        case KOS_SYSCALL_EXIT: {
            struct process *proc = process_get_current();
            if (proc != NULL && proc->pid > 1) {
                process_exit(proc, (int)arg0);
            }
            return UINT64_MAX;
        }
        default:
            return UINT64_MAX;
    }
}

const char *kos_syscall_name(uint64_t number) {
    switch (number) {
        case KOS_SYSCALL_GETTIME: return "gettime";
        case KOS_SYSCALL_SYSINFO: return "sysinfo";
        case KOS_SYSCALL_WRITE: return "write";
        case KOS_SYSCALL_EXIT: return "exit";
        default: return "unknown";
    }
}
