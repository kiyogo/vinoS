#include <stdint.h>

#include <uapi/kos/gui.h>
#include <uapi/kos/syscall_numbers.h>
#include <vino/gui.h>

#ifndef GUI_PACKAGE_NAME
#define GUI_PACKAGE_NAME "Vino GUI"
#endif

static const struct kos_gui_color background = { 11, 18, 32, 0 };
static const struct kos_gui_color panel = { 22, 33, 53, 0 };
static const struct kos_gui_color accent = { 94, 184, 255, 0 };
static const struct kos_gui_color foreground = { 239, 245, 255, 0 };
static const struct kos_gui_color muted = { 165, 181, 202, 0 };

static long sleep_ms(uint32_t milliseconds) {
    register uint64_t result __asm__("rax") = SYS_sleep;
    register uint64_t duration __asm__("rdi") = milliseconds;
    __asm__ volatile("syscall" : "+a"(result) : "D"(duration) : "rcx", "r11", "memory");
    return (long)result;
}

__attribute__((noreturn)) static void app_exit(void) {
    register uint64_t result __asm__("rax") = SYS_exit;
    register uint64_t status __asm__("rdi") = 0;
    __asm__ volatile("syscall" : "+a"(result) : "D"(status) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

__attribute__((noreturn)) void _start(void) {
    struct kos_gui_surface surface = { .abi_version = KOS_GUI_ABI_VERSION };
    if (vino_gui_surface_get(&surface) < 0) app_exit();
    struct kos_gui_rect window = {
        .x = 28, .y = 28,
        .width = surface.width > 56 ? surface.width - 56 : surface.width,
        .height = surface.height > 56 ? surface.height - 56 : surface.height,
    };
    vino_gui_window_draw(window, "Vino " GUI_PACKAGE_NAME, panel, foreground);
    struct kos_gui_rect content = {
        .x = window.x + 24, .y = window.y + 74,
        .width = window.width > 48 ? window.width - 48 : window.width,
        .height = window.height > 110 ? window.height - 110 : 48,
    };
    vino_gui_panel_draw(content, background);
    (void)vino_gui_text(content.x + 18, content.y + 18,
        "User-space GUI process", 2, foreground, background);
    (void)vino_gui_text(content.x + 18, content.y + 48,
        "Framebuffer and keyboard are accessed through display/input services.",
        1, muted, background);
    struct vino_gui_button close_button = {
        .bounds = { content.x + 18, content.y + 82, 180, 32 },
        .label = "Q  Close",
        .enabled = true,
        .selected = true,
    };
    vino_gui_button_draw(&close_button, foreground, panel, accent);
    (void)vino_gui_text(content.x + 18, content.y + 132,
        "Press Q or Escape to return to CLI.", 1, muted, background);

    for (;;) {
        struct kos_gui_input_event event = { .abi_version = KOS_GUI_ABI_VERSION };
        if (vino_gui_input_read(&event) == 0 && event.type == KOS_GUI_INPUT_KEY
            && (event.key == 'q' || event.key == 'Q' || event.key == 27)) app_exit();
        (void)sleep_ms(12);
    }
}
