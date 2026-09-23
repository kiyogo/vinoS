#include <stddef.h>

#ifndef GUI_PACKAGE_NAME
#define GUI_PACKAGE_NAME "Vino GUI"
#endif

static const char gui_banner[] = "Vino GUI package entry: " GUI_PACKAGE_NAME "\n";

__attribute__((noreturn)) void _start(void) {
    register long number __asm__("rax") = 4;
    register long descriptor __asm__("rdi") = 1;
    register const char *buffer __asm__("rsi") = gui_banner;
    register long length __asm__("rdx") = (long)(sizeof(gui_banner) - 1);
    __asm__ volatile("syscall"
        : "+a"(number)
        : "D"(descriptor), "S"(buffer), "d"(length)
        : "rcx", "r11", "memory");

    number = 1;
    descriptor = 0;
    __asm__ volatile("syscall"
        : "+a"(number)
        : "D"(descriptor)
        : "rcx", "r11", "memory");
    __builtin_unreachable();
}
