#include <stdint.h>

#include <uapi/kos/syscall_numbers.h>
#include <vino/installer.h>

long vino_installer_action(const struct kos_installer_action_request *request,
                           struct kos_installer_state *state) {
    if (request == 0 || state == 0) return -1;
    register uint64_t result __asm__("rax") = SYS_installer;
    register const struct kos_installer_action_request *action __asm__("rdi") = request;
    register struct kos_installer_state *output __asm__("rsi") = state;
    __asm__ volatile("syscall"
        : "+a"(result)
        : "D"(action), "S"(output)
        : "rcx", "r11", "memory");
    return (long)result;
}
