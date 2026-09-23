#ifndef KOS_GUI_CANVAS_H
#define KOS_GUI_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

struct gui_color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

struct gui_rect {
    uint64_t x;
    uint64_t y;
    uint64_t width;
    uint64_t height;
};

bool gui_canvas_initialize(void);
bool gui_canvas_is_initialized(void);
uint64_t gui_canvas_width(void);
uint64_t gui_canvas_height(void);
void gui_canvas_clear(struct gui_color color);
void gui_canvas_fill(struct gui_rect rect, struct gui_color color);
void gui_canvas_border(struct gui_rect rect, uint64_t thickness, struct gui_color color);
void gui_canvas_text(uint64_t x, uint64_t y, const char *text, uint64_t scale,
                     struct gui_color foreground, struct gui_color background);

#endif /* KOS_GUI_CANVAS_H */
