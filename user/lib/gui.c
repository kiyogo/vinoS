#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/kos/syscall_numbers.h>
#include <vino/gui.h>

static long vino_syscall2(uint64_t number, uint64_t arg0, uint64_t arg1) {
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = arg0;
    register uint64_t second __asm__("rsi") = arg1;
    __asm__ volatile("syscall"
        : "+a"(result)
        : "D"(first), "S"(second)
        : "rcx", "r11", "memory");
    return (long)result;
}

static bool text_copy(char destination[KOS_GUI_TEXT_MAX], const char *source) {
    if (source == NULL) return false;
    uint32_t index = 0;
    while (index + 1 < KOS_GUI_TEXT_MAX && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    if (source[index] != '\0') return false;
    destination[index] = '\0';
    return true;
}

static long display_request(struct kos_gui_display_request *request) {
    if (request == NULL) return -1;
    request->abi_version = KOS_GUI_ABI_VERSION;
    return vino_syscall2(SYS_gui_display, (uint64_t)request->operation,
                         (uint64_t)request);
}

long vino_gui_surface_get(struct kos_gui_surface *surface) {
    if (surface == NULL) return -1;
    surface->abi_version = KOS_GUI_ABI_VERSION;
    return vino_syscall2(SYS_gui_display, KOS_GUI_DISPLAY_GET_SURFACE,
                         (uint64_t)surface);
}

long vino_gui_clear(struct kos_gui_color color) {
    struct kos_gui_display_request request = {
        .operation = KOS_GUI_DISPLAY_CLEAR,
        .color = color,
    };
    return display_request(&request);
}

long vino_gui_fill(struct kos_gui_rect rect, struct kos_gui_color color) {
    struct kos_gui_display_request request = {
        .operation = KOS_GUI_DISPLAY_FILL_RECT,
        .rect = rect,
        .color = color,
    };
    return display_request(&request);
}

long vino_gui_border(struct kos_gui_rect rect, struct kos_gui_color color) {
    struct kos_gui_display_request request = {
        .operation = KOS_GUI_DISPLAY_BORDER_RECT,
        .rect = rect,
        .color = color,
    };
    return display_request(&request);
}

long vino_gui_text(int32_t x, int32_t y, const char *text, uint32_t scale,
                   struct kos_gui_color foreground, struct kos_gui_color background) {
    struct kos_gui_display_request request = {
        .operation = KOS_GUI_DISPLAY_DRAW_TEXT,
        .rect = { .x = x, .y = y },
        .foreground = foreground,
        .background = background,
        .scale = scale,
        .font_id = KOS_GUI_FONT_8X8,
    };
    if (!text_copy(request.text, text)) return -1;
    return display_request(&request);
}

long vino_gui_input_read(struct kos_gui_input_event *event) {
    if (event == NULL) return -1;
    event->abi_version = KOS_GUI_ABI_VERSION;
    return vino_syscall2(SYS_gui_input, (uint64_t)event, 0);
}
