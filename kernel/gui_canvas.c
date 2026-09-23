#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kos/framebuffer.h>
#include <kos/gui_canvas.h>

extern char font8x8_basic[128][8];

static bool canvas_initialized;

static uint32_t make_color(struct gui_color color) {
    return framebuffer_make_color(color.red, color.green, color.blue);
}

bool gui_canvas_initialize(void) {
    canvas_initialized = framebuffer_is_initialized();
    return canvas_initialized;
}

bool gui_canvas_is_initialized(void) {
    return canvas_initialized;
}

uint64_t gui_canvas_width(void) {
    return framebuffer_width();
}

uint64_t gui_canvas_height(void) {
    return framebuffer_height();
}

void gui_canvas_clear(struct gui_color color) {
    if (canvas_initialized) {
        framebuffer_fill(make_color(color));
    }
}

void gui_canvas_fill(struct gui_rect rect, struct gui_color color) {
    if (canvas_initialized) {
        framebuffer_fill_rect(rect.x, rect.y, rect.width, rect.height, make_color(color));
    }
}

void gui_canvas_border(struct gui_rect rect, uint64_t thickness, struct gui_color color) {
    if (!canvas_initialized || thickness == 0 || rect.width == 0 || rect.height == 0) {
        return;
    }
    if (thickness > rect.width) thickness = rect.width;
    if (thickness > rect.height) thickness = rect.height;
    uint32_t pixel = make_color(color);
    framebuffer_fill_rect(rect.x, rect.y, rect.width, thickness, pixel);
    framebuffer_fill_rect(rect.x, rect.y + rect.height - thickness, rect.width, thickness, pixel);
    framebuffer_fill_rect(rect.x, rect.y, thickness, rect.height, pixel);
    framebuffer_fill_rect(rect.x + rect.width - thickness, rect.y, thickness, rect.height, pixel);
}

void gui_canvas_text(uint64_t x, uint64_t y, const char *text, uint64_t scale,
                     struct gui_color foreground, struct gui_color background) {
    if (!canvas_initialized || text == NULL || scale == 0 || scale > 16) {
        return;
    }
    const uint64_t glyph_width = 8 * scale;
    const uint64_t glyph_height = 8 * scale;
    uint64_t cursor_x = x;
    uint64_t cursor_y = y;
    uint32_t foreground_pixel = make_color(foreground);
    uint32_t background_pixel = make_color(background);

    while (*text != '\0') {
        unsigned char glyph = (unsigned char)*text++;
        if (glyph == '\n') {
            cursor_x = x;
            cursor_y += glyph_height;
            continue;
        }
        if (glyph == '\r') {
            cursor_x = x;
            continue;
        }
        if (glyph < 32 || glyph > 126) {
            glyph = '?';
        }
        framebuffer_fill_rect(cursor_x, cursor_y, glyph_width, glyph_height, background_pixel);
        for (uint64_t row = 0; row < 8; ++row) {
            uint8_t bits = (uint8_t)font8x8_basic[glyph][row];
            for (uint64_t column = 0; column < 8; ++column) {
                if ((bits & (1u << column)) != 0) {
                    framebuffer_fill_rect(cursor_x + column * scale, cursor_y + row * scale,
                                          scale, scale, foreground_pixel);
                }
            }
        }
        cursor_x += glyph_width;
    }
}
