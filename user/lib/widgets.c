#include <stddef.h>
#include <stdint.h>

#include <vino/gui.h>

static const struct kos_gui_color color_white = { 245, 248, 255, 0 };
static const struct kos_gui_color color_border = { 91, 109, 137, 0 };

static struct kos_gui_rect inset(struct kos_gui_rect rect, uint32_t amount) {
    if ((uint64_t)rect.width <= (uint64_t)amount * 2
        || (uint64_t)rect.height <= (uint64_t)amount * 2) {
        return (struct kos_gui_rect){ rect.x, rect.y, 0, 0 };
    }
    int64_t x = (int64_t)rect.x + amount;
    int64_t y = (int64_t)rect.y + amount;
    if (x > INT32_MAX || y > INT32_MAX) {
        return (struct kos_gui_rect){ rect.x, rect.y, 0, 0 };
    }
    rect.x = (int32_t)x;
    rect.y = (int32_t)y;
    rect.width -= amount * 2;
    rect.height -= amount * 2;
    return rect;
}

static void text_in_rect(struct kos_gui_rect bounds, uint32_t x_offset,
                         uint32_t y_offset, const char *text, uint32_t scale,
                         struct kos_gui_color foreground,
                         struct kos_gui_color background) {
    int64_t x = (int64_t)bounds.x + x_offset;
    int64_t y = (int64_t)bounds.y + y_offset;
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) return;
    (void)vino_gui_text((int32_t)x, (int32_t)y, text, scale, foreground, background);
}

void vino_gui_window_render(const struct vino_gui_window *window) {
    if (window == NULL) return;
    (void)vino_gui_fill(window->bounds, window->background);
    (void)vino_gui_border(window->bounds, color_border);
    struct kos_gui_rect title_bounds = inset(window->bounds, 16);
    if (title_bounds.width != 0 && title_bounds.height != 0) {
        text_in_rect(title_bounds, 0, 0, window->title, 2,
                     window->title_color, window->background);
    }
}

void vino_gui_panel_render(const struct vino_gui_panel *panel) {
    if (panel == NULL) return;
    (void)vino_gui_fill(panel->bounds, panel->color);
    (void)vino_gui_border(panel->bounds, color_border);
}

void vino_gui_window_draw(struct kos_gui_rect bounds, const char *title,
                          struct kos_gui_color background,
                          struct kos_gui_color title_color) {
    const struct vino_gui_window window = {
        .bounds = bounds,
        .title = title,
        .background = background,
        .title_color = title_color,
    };
    vino_gui_window_render(&window);
}

void vino_gui_panel_draw(struct kos_gui_rect bounds, struct kos_gui_color color) {
    const struct vino_gui_panel panel = { .bounds = bounds, .color = color };
    vino_gui_panel_render(&panel);
}

void vino_gui_button_draw(const struct vino_gui_button *button,
                          struct kos_gui_color foreground,
                          struct kos_gui_color normal,
                          struct kos_gui_color active) {
    if (button == NULL) return;
    struct kos_gui_color fill = button->selected ? active : normal;
    if (!button->enabled) fill = (struct kos_gui_color){ 56, 62, 74, 0 };
    (void)vino_gui_fill(button->bounds, fill);
    (void)vino_gui_border(button->bounds, button->selected ? color_white : color_border);
    text_in_rect(button->bounds, 12, 8, button->label, 1, foreground, fill);
}

void vino_gui_dropdown_draw(const struct vino_gui_dropdown *dropdown,
                            struct kos_gui_color foreground,
                            struct kos_gui_color background,
                            struct kos_gui_color accent) {
    if (dropdown == NULL) return;
    (void)vino_gui_fill(dropdown->bounds, background);
    (void)vino_gui_border(dropdown->bounds, dropdown->expanded ? accent : color_border);
    text_in_rect(dropdown->bounds, 8, 6, dropdown->label, 1, accent, background);
    text_in_rect(dropdown->bounds, 8, 22, dropdown->value, 1, foreground, background);
}

void vino_gui_list_draw(const struct vino_gui_list *list,
                        struct kos_gui_color foreground,
                        struct kos_gui_color background,
                        struct kos_gui_color selected) {
    if (list == NULL || list->row_height == 0
        || list->bounds.width <= 4 || list->bounds.height <= 4) return;
    (void)vino_gui_fill(list->bounds, background);
    (void)vino_gui_border(list->bounds, color_border);
    if (list->items == NULL || list->first_visible_item >= list->item_count) return;
    uint32_t visible = (list->bounds.height - 4) / list->row_height;
    uint32_t remaining = list->item_count - list->first_visible_item;
    if (visible > remaining) visible = remaining;
    for (uint32_t index = 0; index < visible; ++index) {
        uint32_t item_index = list->first_visible_item + index;
        int64_t row_x = (int64_t)list->bounds.x + 2;
        int64_t row_y = (int64_t)list->bounds.y + 2
            + (int64_t)index * (int64_t)list->row_height;
        if (row_x < INT32_MIN || row_x > INT32_MAX
            || row_y < INT32_MIN || row_y > INT32_MAX) break;
        struct kos_gui_rect row = {
            .x = (int32_t)row_x,
            .y = (int32_t)row_y,
            .width = list->bounds.width - 4,
            .height = list->row_height,
        };
        struct kos_gui_color row_color = item_index == list->selected_item ? selected : background;
        (void)vino_gui_fill(row, row_color);
        text_in_rect(row, 8, 6, list->items[item_index], 1, foreground, row_color);
    }
}

void vino_gui_dialog_draw(const struct vino_gui_dialog *dialog,
                          struct kos_gui_color foreground,
                          struct kos_gui_color background,
                          struct kos_gui_color accent) {
    if (dialog == NULL || !dialog->visible) return;
    (void)vino_gui_fill(dialog->bounds, background);
    (void)vino_gui_border(dialog->bounds, accent);
    text_in_rect(dialog->bounds, 12, 12, dialog->title, 1, accent, background);
    text_in_rect(dialog->bounds, 12, 36, dialog->message, 1, foreground, background);
}

void vino_gui_progress_draw(const struct vino_gui_progress_bar *progress,
                            struct kos_gui_color background,
                            struct kos_gui_color fill,
                            struct kos_gui_color border) {
    if (progress == NULL || progress->maximum == 0
        || progress->bounds.width <= 4 || progress->bounds.height <= 4) return;
    (void)vino_gui_fill(progress->bounds, background);
    (void)vino_gui_border(progress->bounds, border);
    struct kos_gui_rect inner = inset(progress->bounds, 2);
    if (inner.width == 0 || inner.height == 0) return;
    uint64_t fill_width = (uint64_t)inner.width
        * (progress->value > progress->maximum ? progress->maximum : progress->value)
        / progress->maximum;
    if (fill_width != 0) {
        inner.width = (uint32_t)fill_width;
        struct kos_gui_rect bar = inner;
        (void)vino_gui_fill(bar, fill);
    }
}
