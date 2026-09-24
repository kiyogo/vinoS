#include <stdbool.h>
#include <stdint.h>

#include <kos/framebuffer.h>
#include <kos/heap.h>
#include <kos/gui_canvas.h>
#include <kos/gui_service.h>
#include <kos/keyboard.h>
#include <kos/mouse.h>
#include <kos/timer.h>
#include <kos/uaccess.h>
#include <uapi/kos/gui.h>
#include <uapi/kos/syscall_numbers.h>

static struct kos_gui_frame_stats frame_stats = {
    .abi_version = KOS_GUI_ABI_VERSION,
    .struct_size = sizeof(struct kos_gui_frame_stats),
};
static bool frame_open;
static bool frame_buffered;
static uint64_t frame_started_ms;
static uint64_t frame_draw_operations;
static bool pointer_input_first;

static struct gui_color canvas_color(struct kos_gui_color color) {
    return (struct gui_color){ color.red, color.green, color.blue, color.alpha };
}

static void record_draw_operation(void) {
    if (frame_open && frame_draw_operations != UINT64_MAX) ++frame_draw_operations;
}

static void saturating_increment(uint64_t *value) {
    if (*value != UINT64_MAX) ++*value;
}

static bool valid_rect(const struct kos_gui_rect *rect) {
    if (rect == NULL || rect->x < 0 || rect->y < 0
        || rect->width == 0 || rect->height == 0) return false;
    uint64_t x = (uint32_t)rect->x;
    uint64_t y = (uint32_t)rect->y;
    return x < framebuffer_width() && y < framebuffer_height()
        && rect->width <= framebuffer_width() - x
        && rect->height <= framebuffer_height() - y;
}

static uint64_t display_blit_rgb24(uint64_t user_request) {
    struct kos_gui_image_request request;
    if (!access_ok((const void *)user_request, sizeof(request))
        || copy_from_user(&request, (const void *)user_request, sizeof(request)) != 0) {
        return (uint64_t)-EFAULT;
    }
    if (request.abi_version != KOS_GUI_ABI_VERSION
        || request.pixel_format != KOS_GUI_IMAGE_RGB24
        || !frame_open || !valid_rect(&request.destination)
        || request.source_width == 0 || request.source_height == 0
        || request.source_width > 4096 || request.source_height > 4096
        || request.crop_width == 0 || request.crop_height == 0
        || request.crop_x > request.source_width
        || request.crop_width > request.source_width - request.crop_x
        || request.crop_y > request.source_height
        || request.crop_height > request.source_height - request.crop_y
        || request.source_pitch < (uint64_t)request.source_width * 3u
        || request.source_pitch > 16u * 1024u * 1024u
        || request.source_height > UINT64_MAX / request.source_pitch
        || (uint64_t)request.source_pitch * request.source_height > request.pixel_bytes
        || request.pixel_bytes > 16u * 1024u * 1024u
        || request.pixels == 0 || request.pixels > UINT64_MAX - request.pixel_bytes
        || !access_ok((const void *)request.pixels, (size_t)request.pixel_bytes)) {
        return (uint64_t)-EINVAL;
    }

    uint64_t row_bytes = (uint64_t)request.crop_width * 3u;
    if (row_bytes > SIZE_MAX) return (uint64_t)-EOVERFLOW;
    uint8_t *source_row = (uint8_t *)kmalloc(row_bytes);
    if (source_row == NULL) return (uint64_t)-ENOMEM;

    uint64_t result = 0;
    for (uint64_t row = 0; row < request.destination.height; ++row) {
        uint64_t source_y = request.crop_y
            + row * request.crop_height / request.destination.height;
        uint64_t source_offset = source_y * request.source_pitch
            + (uint64_t)request.crop_x * 3u;
        if (source_offset > request.pixel_bytes
            || row_bytes > request.pixel_bytes - source_offset
            || request.pixels > UINT64_MAX - source_offset
            || !access_ok((const void *)(request.pixels + source_offset),
                          (size_t)row_bytes)
            || copy_from_user(source_row,
                (const void *)(request.pixels + source_offset), (size_t)row_bytes) != 0) {
            result = (uint64_t)-EFAULT;
            break;
        }
        gui_canvas_blit_rgb24_row((uint32_t)request.destination.x,
            (uint64_t)(uint32_t)request.destination.y + row,
            request.destination.width, request.crop_width, source_row);
    }
    (void)kfree(source_row);
    if (result == 0) record_draw_operation();
    return result;
}

uint64_t gui_service_display(uint64_t operation, uint64_t user_request) {
    if (!framebuffer_is_initialized() || !gui_canvas_initialize()) return (uint64_t)-ENODEV;

    if (operation == KOS_GUI_DISPLAY_GET_SURFACE) {
        struct kos_gui_surface surface = {0};
        if (!access_ok((const void *)user_request, sizeof(surface))
            || copy_from_user(&surface, (const void *)user_request, sizeof(surface)) != 0
            || surface.abi_version != KOS_GUI_ABI_VERSION) return (uint64_t)-EFAULT;
        uint64_t pitch = framebuffer_pitch();
        if (framebuffer_width() > UINT32_MAX || framebuffer_height() > UINT32_MAX
            || pitch > UINT32_MAX) return (uint64_t)-EOVERFLOW;
        surface.width = (uint32_t)framebuffer_width();
        surface.height = (uint32_t)framebuffer_height();
        surface.pitch = (uint32_t)pitch;
        surface.bits_per_pixel = 32;
        surface.pixel_format = KOS_GUI_PIXEL_FORMAT_RGB32;
        bool pointer_available = mouse_is_initialized();
        surface.capabilities = pointer_available ? KOS_GUI_SURFACE_POINTER : 0;
        if (framebuffer_backbuffer_available())
            surface.capabilities |= KOS_GUI_SURFACE_BACKBUFFER;
        mouse_set_bounds(surface.width, surface.height);
        int32_t pointer_x = 0;
        int32_t pointer_y = 0;
        mouse_get_position(&pointer_x, &pointer_y);
        framebuffer_cursor_set(pointer_available, pointer_x, pointer_y);
        return copy_to_user((void *)user_request, &surface, sizeof(surface)) == 0
            ? 0 : (uint64_t)-EFAULT;
    }

    if (operation == KOS_GUI_DISPLAY_GET_FRAME_STATS) {
        struct kos_gui_frame_stats requested;
        if (!access_ok((const void *)user_request, sizeof(requested))
            || copy_from_user(&requested, (const void *)user_request,
                              sizeof(requested)) != 0) return (uint64_t)-EFAULT;
        if (requested.abi_version != KOS_GUI_ABI_VERSION
            || requested.struct_size < sizeof(requested)) return (uint64_t)-EINVAL;
        struct kos_gui_frame_stats snapshot = frame_stats;
        snapshot.abi_version = KOS_GUI_ABI_VERSION;
        snapshot.struct_size = sizeof(snapshot);
        snapshot.backbuffer_enabled = framebuffer_backbuffer_available() ? 1u : 0u;
        snapshot.background_cache_enabled =
            framebuffer_background_cache_available() ? 1u : 0u;
        return copy_to_user((void *)user_request, &snapshot, sizeof(snapshot)) == 0
            ? 0 : (uint64_t)-EFAULT;
    }

    if (operation == KOS_GUI_DISPLAY_BLIT_RGB24)
        return display_blit_rgb24(user_request);

    struct kos_gui_display_request request;
    if (!access_ok((const void *)user_request, sizeof(request))
        || copy_from_user(&request, (const void *)user_request, sizeof(request)) != 0) {
        return (uint64_t)-EFAULT;
    }
    if (request.abi_version != KOS_GUI_ABI_VERSION || request.operation != operation) {
        return (uint64_t)-EINVAL;
    }
    if (operation != KOS_GUI_DISPLAY_BEGIN_FRAME && request.flags != 0)
        return (uint64_t)-EINVAL;

    struct gui_color color = canvas_color(request.color);
    switch (operation) {
        case KOS_GUI_DISPLAY_CLEAR:
            gui_canvas_clear(color);
            record_draw_operation();
            return 0;
        case KOS_GUI_DISPLAY_FILL_RECT:
            if (!valid_rect(&request.rect)) return (uint64_t)-EINVAL;
            gui_canvas_fill((struct gui_rect){ (uint64_t)request.rect.x,
                (uint64_t)request.rect.y, request.rect.width, request.rect.height }, color);
            record_draw_operation();
            return 0;
        case KOS_GUI_DISPLAY_BORDER_RECT:
            if (!valid_rect(&request.rect)) return (uint64_t)-EINVAL;
            gui_canvas_border((struct gui_rect){ (uint64_t)request.rect.x,
                (uint64_t)request.rect.y, request.rect.width, request.rect.height },
                1, color);
            record_draw_operation();
            return 0;
        case KOS_GUI_DISPLAY_GLASS_RECT:
            if (!valid_rect(&request.rect) || request.radius > 64
                || request.blur_radius > 30 || request.color.alpha == 0
                || request.foreground.alpha == 0) return (uint64_t)-EINVAL;
            gui_canvas_glass((struct gui_rect){ (uint64_t)request.rect.x,
                (uint64_t)request.rect.y, request.rect.width, request.rect.height },
                request.radius, request.blur_radius,
                (struct gui_color){ request.color.red, request.color.green,
                    request.color.blue, request.color.alpha },
                (struct gui_color){ request.foreground.red, request.foreground.green,
                    request.foreground.blue, request.foreground.alpha });
            record_draw_operation();
            return 0;
        case KOS_GUI_DISPLAY_DRAW_TEXT:
        case KOS_GUI_DISPLAY_DRAW_TEXT_TRANSPARENT: {
            if (request.rect.x < 0 || request.rect.y < 0
                || (uint32_t)request.rect.x >= framebuffer_width()
                || (uint32_t)request.rect.y >= framebuffer_height()
                || request.scale == 0 || request.scale > 4
                || (request.font_id != KOS_GUI_FONT_8X8
                    && request.font_id != KOS_GUI_FONT_INTER_8X14))
                return (uint64_t)-EINVAL;
            bool terminated = false;
            for (uint32_t index = 0; index < KOS_GUI_TEXT_MAX; ++index) {
                if (request.text[index] == '\0') {
                    terminated = true;
                    break;
                }
            }
            if (!terminated) return (uint64_t)-EINVAL;
            if (operation == KOS_GUI_DISPLAY_DRAW_TEXT_TRANSPARENT) {
                gui_canvas_text_transparent_font((uint32_t)request.rect.x,
                    (uint32_t)request.rect.y, request.text, request.scale,
                    request.font_id, canvas_color(request.foreground));
            }
            else {
                gui_canvas_text_font((uint32_t)request.rect.x,
                    (uint32_t)request.rect.y, request.text, request.scale,
                    request.font_id, canvas_color(request.foreground),
                    canvas_color(request.background));
            }
            record_draw_operation();
            return 0;
        }
        case KOS_GUI_DISPLAY_BEGIN_FRAME:
            if (frame_open) return (uint64_t)-EBUSY;
            if ((request.flags & ~KOS_GUI_FRAME_DISCARD_PREVIOUS) != 0)
                return (uint64_t)-EINVAL;
            frame_started_ms = timer_get_uptime_ms();
            framebuffer_cursor_suspend();
            frame_buffered = framebuffer_backbuffer_available();
            if (frame_buffered && !framebuffer_frame_begin(
                    (request.flags & KOS_GUI_FRAME_DISCARD_PREVIOUS) == 0)) {
                frame_buffered = false;
                framebuffer_cursor_resume();
                return (uint64_t)-EBUSY;
            }
            frame_open = true;
            frame_draw_operations = 0;
            return 0;
        case KOS_GUI_DISPLAY_PRESENT_FRAME: {
            if (!frame_open) return (uint64_t)-EINVAL;
            if (frame_buffered) framebuffer_frame_present();
            else framebuffer_cursor_resume();
            uint64_t now = timer_get_uptime_ms();
            uint64_t duration = now >= frame_started_ms ? now - frame_started_ms : 0;
            saturating_increment(&frame_stats.redraw_count);
            if (UINT64_MAX - frame_stats.draw_operation_count < frame_draw_operations)
                frame_stats.draw_operation_count = UINT64_MAX;
            else frame_stats.draw_operation_count += frame_draw_operations;
            frame_stats.last_frame_draw_operations = frame_draw_operations;
            frame_stats.last_frame_duration_ms = duration;
            if (UINT64_MAX - frame_stats.total_frame_duration_ms < duration)
                frame_stats.total_frame_duration_ms = UINT64_MAX;
            else frame_stats.total_frame_duration_ms += duration;
            if (duration > frame_stats.max_frame_duration_ms)
                frame_stats.max_frame_duration_ms = duration;
            frame_stats.backbuffer_enabled = framebuffer_backbuffer_available() ? 1u : 0u;
            frame_stats.background_cache_enabled =
                framebuffer_background_cache_available() ? 1u : 0u;
            frame_open = false;
            frame_buffered = false;
            return 0;
        }
        case KOS_GUI_DISPLAY_BACKGROUND_CAPTURE:
            if (!frame_open) return (uint64_t)-EINVAL;
            return framebuffer_background_capture() ? 0 : (uint64_t)-ENOMEM;
        case KOS_GUI_DISPLAY_BACKGROUND_RESTORE:
            if (!frame_open) return (uint64_t)-EINVAL;
            return framebuffer_background_restore() ? 0 : (uint64_t)-ENOENT;
        default:
            return (uint64_t)-EINVAL;
    }
}

void gui_service_abort_frame(void) {
    framebuffer_frame_abort();
    framebuffer_cursor_set(false, 0, 0);
    if (frame_open) saturating_increment(&frame_stats.aborted_frame_count);
    frame_open = false;
    frame_buffered = false;
    frame_draw_operations = 0;
}

void gui_service_frame_stats_snapshot(struct kos_gui_frame_stats *stats) {
    if (stats == 0) return;
    *stats = frame_stats;
    stats->abi_version = KOS_GUI_ABI_VERSION;
    stats->struct_size = sizeof(*stats);
    stats->backbuffer_enabled = framebuffer_backbuffer_available() ? 1u : 0u;
    stats->background_cache_enabled =
        framebuffer_background_cache_available() ? 1u : 0u;
}

uint64_t gui_service_input(uint64_t user_event) {
    struct kos_gui_input_event event = {
        .abi_version = KOS_GUI_ABI_VERSION,
        .type = KOS_GUI_INPUT_NONE,
        .timestamp_ms = timer_get_uptime_ms(),
    };
    if (!access_ok((const void *)user_event, sizeof(event))
        || copy_from_user(&event, (const void *)user_event, sizeof(event)) != 0
        || event.abi_version != KOS_GUI_ABI_VERSION) return (uint64_t)-EFAULT;
    event.type = KOS_GUI_INPUT_NONE;
    event.key = 0;
    event.modifiers = 0;
    event.timestamp_ms = timer_get_uptime_ms();
    event.pointer_x = 0;
    event.pointer_y = 0;
    event.pointer_buttons = 0;
    event.pointer_wheel = 0;
    struct kos_mouse_event pointer;
    char key = 0;
    bool have_pointer = false;
    bool have_key = false;
    if (pointer_input_first) {
        have_pointer = mouse_read_event(&pointer);
        if (!have_pointer) have_key = keyboard_read_char(&key) && key != '\0';
    }
    else {
        have_key = keyboard_read_char(&key) && key != '\0';
        if (!have_key) have_pointer = mouse_read_event(&pointer);
    }
    pointer_input_first = !pointer_input_first;
    if (have_pointer) {
        event.type = KOS_GUI_INPUT_POINTER;
        event.pointer_x = pointer.x;
        event.pointer_y = pointer.y;
        event.pointer_buttons = pointer.buttons;
        event.pointer_wheel = pointer.wheel;
        framebuffer_cursor_set(true, pointer.x, pointer.y);
    }
    else if (have_key) {
        event.type = KOS_GUI_INPUT_KEY;
        event.key = (uint8_t)key;
    }
    return copy_to_user((void *)user_event, &event, sizeof(event)) == 0
        ? 0 : (uint64_t)-EFAULT;
}
