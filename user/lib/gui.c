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

static long display_simple_operation(uint32_t operation, uint32_t flags) {
    struct kos_gui_display_request request = { .operation = operation, .flags = flags };
    return display_request(&request);
}

long vino_gui_frame_begin(bool discard_previous) {
    return display_simple_operation(KOS_GUI_DISPLAY_BEGIN_FRAME,
        discard_previous ? KOS_GUI_FRAME_DISCARD_PREVIOUS : 0);
}

long vino_gui_frame_begin_damage(struct kos_gui_rect damage) {
    struct kos_gui_display_request request = {
        .operation = KOS_GUI_DISPLAY_BEGIN_FRAME,
        .flags = KOS_GUI_FRAME_DISCARD_PREVIOUS | KOS_GUI_FRAME_DAMAGE_RECT,
        .rect = damage,
    };
    return display_request(&request);
}

long vino_gui_frame_present(void) {
    return display_simple_operation(KOS_GUI_DISPLAY_PRESENT_FRAME, 0);
}

long vino_gui_background_capture(void) {
    return display_simple_operation(KOS_GUI_DISPLAY_BACKGROUND_CAPTURE, 0);
}

long vino_gui_background_restore(void) {
    return display_simple_operation(KOS_GUI_DISPLAY_BACKGROUND_RESTORE, 0);
}

long vino_gui_background_restore_rect(struct kos_gui_rect rect) {
    struct kos_gui_display_request request = {
        .operation = KOS_GUI_DISPLAY_BACKGROUND_RESTORE,
        .rect = rect,
    };
    return display_request(&request);
}

long vino_gui_frame_stats_get(struct kos_gui_frame_stats *stats) {
    if (stats == NULL) return -1;
    stats->abi_version = KOS_GUI_ABI_VERSION;
    stats->struct_size = sizeof(*stats);
    return vino_syscall2(SYS_gui_display, KOS_GUI_DISPLAY_GET_FRAME_STATS,
                         (uint64_t)stats);
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

long vino_gui_glass(struct kos_gui_rect rect, struct kos_gui_color fill,
                    struct kos_gui_color border, uint32_t radius,
                    uint32_t blur_radius) {
    struct kos_gui_display_request request = {
        .operation = KOS_GUI_DISPLAY_GLASS_RECT,
        .rect = rect,
        .color = fill,
        .foreground = border,
        .radius = radius,
        .blur_radius = blur_radius,
    };
    return display_request(&request);
}

long vino_gui_text(int32_t x, int32_t y, const char *text, uint32_t scale,
                   struct kos_gui_color foreground, struct kos_gui_color background) {
    return vino_gui_text_font(x, y, text, scale, KOS_GUI_FONT_8X8,
                              foreground, background);
}

long vino_gui_text_transparent(int32_t x, int32_t y, const char *text, uint32_t scale,
                               struct kos_gui_color foreground) {
    return vino_gui_text_transparent_font(x, y, text, scale, KOS_GUI_FONT_8X8,
                                          foreground);
}

long vino_gui_text_transparent_font(int32_t x, int32_t y, const char *text,
                                    uint32_t scale, enum kos_gui_font_id font_id,
                                    struct kos_gui_color foreground) {
    struct kos_gui_display_request request = {
        .operation = KOS_GUI_DISPLAY_DRAW_TEXT_TRANSPARENT,
        .rect = { .x = x, .y = y },
        .foreground = foreground,
        .scale = scale,
        .font_id = (uint32_t)font_id,
    };
    if (!text_copy(request.text, text)) return -1;
    return display_request(&request);
}

long vino_gui_text_font(int32_t x, int32_t y, const char *text, uint32_t scale,
                        enum kos_gui_font_id font_id, struct kos_gui_color foreground,
                        struct kos_gui_color background) {
    struct kos_gui_display_request request = {
        .operation = KOS_GUI_DISPLAY_DRAW_TEXT,
        .rect = { .x = x, .y = y },
        .foreground = foreground,
        .background = background,
        .scale = scale,
        .font_id = font_id,
    };
    if (!text_copy(request.text, text)) return -1;
    return display_request(&request);
}

long vino_gui_blit_rgb24(struct kos_gui_rect destination, const uint8_t *pixels,
                         uint64_t pixel_bytes, uint32_t source_width,
                         uint32_t source_height, uint32_t source_pitch,
                         uint32_t crop_x, uint32_t crop_y, uint32_t crop_width,
                         uint32_t crop_height) {
    struct kos_gui_image_request request = {
        .abi_version = KOS_GUI_ABI_VERSION,
        .pixel_format = KOS_GUI_IMAGE_RGB24,
        .destination = destination,
        .pixels = (uint64_t)pixels,
        .pixel_bytes = pixel_bytes,
        .source_width = source_width,
        .source_height = source_height,
        .source_pitch = source_pitch,
        .crop_x = crop_x,
        .crop_y = crop_y,
        .crop_width = crop_width,
        .crop_height = crop_height,
    };
    return vino_syscall2(SYS_gui_display, KOS_GUI_DISPLAY_BLIT_RGB24,
                         (uint64_t)&request);
}

long vino_gui_input_read(struct kos_gui_input_event *event) {
    if (event == NULL) return -1;
    event->abi_version = KOS_GUI_ABI_VERSION;
    return vino_syscall2(SYS_gui_input, (uint64_t)event, 0);
}

bool vino_gui_rect_contains(const struct kos_gui_rect *rect, int32_t x, int32_t y) {
    if (rect == NULL || x < rect->x || y < rect->y) return false;
    int64_t dx = (int64_t)x - rect->x;
    int64_t dy = (int64_t)y - rect->y;
    return (uint64_t)dx < rect->width && (uint64_t)dy < rect->height;
}
