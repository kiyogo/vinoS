#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/kos/syscall_numbers.h>
#include <uapi/kos/system_control.h>
#include <vino/system.h>

static long vino_system_syscall2(uint64_t number, uint64_t arg0, uint64_t arg1) {
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = arg0;
    register uint64_t second __asm__("rsi") = arg1;
    __asm__ volatile("syscall"
        : "+a"(result)
        : "D"(first), "S"(second)
        : "rcx", "r11", "memory");
    return (long)result;
}

long vino_system_info_get(struct kos_system_info *info) {
    if (info == NULL) return -1;
    info->abi_version = KOS_SYSTEM_INFO_ABI_VERSION;
    return vino_system_syscall2(SYS_system_info, (uint64_t)info, 0);
}

long vino_gui_control(struct kos_gui_control_request *request) {
    if (request == NULL) return -1;
    request->abi_version = KOS_GUI_CONTROL_ABI_VERSION;
    return vino_system_syscall2(SYS_gui_control, (uint64_t)request, 0);
}

long vino_system_control(uint32_t action, bool confirmed) {
    return vino_system_syscall2(SYS_system_control, action,
        confirmed ? KOS_SYSTEM_CONTROL_CONFIRM_TOKEN : 0);
}
