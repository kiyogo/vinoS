#include <stdbool.h>
#include <stdint.h>

#include <kos/framebuffer.h>
#include <kos/gui_canvas.h>
#include <kos/gui_service.h>
#include <kos/keyboard.h>
#include <kos/timer.h>
#include <kos/uaccess.h>
#include <uapi/kos/gui.h>
#include <uapi/kos/syscall_numbers.h>

static struct gui_color canvas_color(struct kos_gui_color color) {
    return (struct gui_color){ color.red, color.green, color.blue };
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
        return copy_to_user((void *)user_request, &surface, sizeof(surface)) == 0
            ? 0 : (uint64_t)-EFAULT;
    }

    struct kos_gui_display_request request;
    if (!access_ok((const void *)user_request, sizeof(request))
        || copy_from_user(&request, (const void *)user_request, sizeof(request)) != 0) {
        return (uint64_t)-EFAULT;
    }
    if (request.abi_version != KOS_GUI_ABI_VERSION || request.operation != operation) {
        return (uint64_t)-EINVAL;
    }

    struct gui_color color = canvas_color(request.color);
    switch (operation) {
        case KOS_GUI_DISPLAY_CLEAR:
            gui_canvas_clear(color);
            return 0;
        case KOS_GUI_DISPLAY_FILL_RECT:
            if (!valid_rect(&request.rect)) return (uint64_t)-EINVAL;
            gui_canvas_fill((struct gui_rect){ (uint64_t)request.rect.x,
                (uint64_t)request.rect.y, request.rect.width, request.rect.height }, color);
            return 0;
        case KOS_GUI_DISPLAY_BORDER_RECT:
            if (!valid_rect(&request.rect)) return (uint64_t)-EINVAL;
            gui_canvas_border((struct gui_rect){ (uint64_t)request.rect.x,
                (uint64_t)request.rect.y, request.rect.width, request.rect.height },
                1, color);
            return 0;
        case KOS_GUI_DISPLAY_DRAW_TEXT: {
            if (request.rect.x < 0 || request.rect.y < 0
                || (uint32_t)request.rect.x >= framebuffer_width()
                || (uint32_t)request.rect.y >= framebuffer_height()
                || request.scale == 0 || request.scale > 4
                || request.font_id != KOS_GUI_FONT_8X8) return (uint64_t)-EINVAL;
            bool terminated = false;
            for (uint32_t index = 0; index < KOS_GUI_TEXT_MAX; ++index) {
                if (request.text[index] == '\0') {
                    terminated = true;
                    break;
                }
            }
            if (!terminated) return (uint64_t)-EINVAL;
            gui_canvas_text((uint32_t)request.rect.x, (uint32_t)request.rect.y,
                request.text, request.scale, canvas_color(request.foreground),
                canvas_color(request.background));
            return 0;
        }
        default:
            return (uint64_t)-EINVAL;
    }
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
    char key = 0;
    if (keyboard_read_char(&key) && key != '\0') {
        event.type = KOS_GUI_INPUT_KEY;
        event.key = (uint8_t)key;
    }
    return copy_to_user((void *)user_event, &event, sizeof(event)) == 0
        ? 0 : (uint64_t)-EFAULT;
}
