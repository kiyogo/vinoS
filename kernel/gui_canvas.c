#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kos/framebuffer.h>
#include <kos/gui_canvas.h>
#include <uapi/kos/gui.h>

#include <kos/gui_font_inter_8x14.h>

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

void gui_canvas_glass(struct gui_rect rect, uint64_t radius, uint64_t blur_radius,
                     struct gui_color fill, struct gui_color border) {
    if (!canvas_initialized || rect.width == 0 || rect.height == 0) return;
    framebuffer_glass_rect(rect.x, rect.y, rect.width, rect.height, radius, blur_radius,
        fill.red, fill.green, fill.blue, fill.alpha,
        border.red, border.green, border.blue, border.alpha);
}

static void canvas_text(uint64_t x, uint64_t y, const char *text, uint64_t scale,
                        struct gui_color foreground, struct gui_color background,
                        bool transparent_background, uint32_t font_id) {
    if (!canvas_initialized || text == NULL || scale == 0 || scale > 16) {
        return;
    }
    bool inter_font = font_id == KOS_GUI_FONT_INTER_8X14;
    const uint64_t source_width = 8;
    const uint64_t source_height = inter_font ? KOS_GUI_FONT_INTER_HEIGHT : 8;
    const uint64_t glyph_height = source_height * scale;
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
        if (inter_font && (glyph < KOS_GUI_FONT_INTER_FIRST
                           || glyph >= KOS_GUI_FONT_INTER_FIRST
                               + KOS_GUI_FONT_INTER_COUNT)) glyph = '?';
        else if (!inter_font && (glyph < 32 || glyph > 126)) glyph = '?';
        const struct kos_gui_font_inter_glyph *inter_glyph = inter_font
            ? &kos_gui_font_inter_8x14[glyph - KOS_GUI_FONT_INTER_FIRST] : NULL;
        uint64_t glyph_source_width = inter_font ? inter_glyph->advance : source_width;
        uint64_t glyph_width = glyph_source_width * scale;
        if (!transparent_background) {
            framebuffer_fill_rect(cursor_x, cursor_y, glyph_width, glyph_height, background_pixel);
        }
        for (uint64_t row = 0; row < source_height; ++row) {
            uint16_t bits = inter_font ? inter_glyph->rows[row]
                : (uint8_t)font8x8_basic[glyph][row];
            uint64_t column = 0;
            while (column < glyph_source_width) {
                while (column < glyph_source_width && (bits & (1u << column)) == 0) ++column;
                uint64_t run_start = column;
                while (column < glyph_source_width && (bits & (1u << column)) != 0) ++column;
                if (column != run_start) {
                    framebuffer_fill_rect(cursor_x + run_start * scale,
                        cursor_y + row * scale, (column - run_start) * scale,
                        scale, foreground_pixel);
                }
            }
        }
        cursor_x += glyph_width;
    }
}

void gui_canvas_text(uint64_t x, uint64_t y, const char *text, uint64_t scale,
                     struct gui_color foreground, struct gui_color background) {
    canvas_text(x, y, text, scale, foreground, background, false,
                KOS_GUI_FONT_8X8);
}

void gui_canvas_text_font(uint64_t x, uint64_t y, const char *text, uint64_t scale,
                          uint32_t font_id, struct gui_color foreground,
                          struct gui_color background) {
    canvas_text(x, y, text, scale, foreground, background, false, font_id);
}

void gui_canvas_text_transparent(uint64_t x, uint64_t y, const char *text, uint64_t scale,
                                 struct gui_color foreground) {
    canvas_text(x, y, text, scale, foreground, (struct gui_color){0, 0, 0, 0}, true,
                KOS_GUI_FONT_8X8);
}

void gui_canvas_text_transparent_font(uint64_t x, uint64_t y, const char *text,
                                      uint64_t scale, uint32_t font_id,
                                      struct gui_color foreground) {
    canvas_text(x, y, text, scale, foreground, (struct gui_color){0, 0, 0, 0}, true,
                font_id);
}

void gui_canvas_blit_rgb24_row(uint64_t x, uint64_t y, uint64_t destination_width,
                               uint32_t source_width, const uint8_t *pixels) {
    if (canvas_initialized) {
        framebuffer_blit_rgb24_row(x, y, destination_width, source_width, pixels);
    }
}
