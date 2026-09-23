#include <stddef.h>
#include <stdint.h>

#include <vino/gui.h>

static const struct kos_gui_color color_white = { 245, 248, 255, 0 };
static const struct kos_gui_color color_border = { 91, 109, 137, 0 };

static struct kos_gui_rect inset(struct kos_gui_rect rect, uint32_t amount) {
    if (rect.width <= amount * 2 || rect.height <= amount * 2) {
        return (struct kos_gui_rect){ rect.x, rect.y, 0, 0 };
    }
    rect.x += (int32_t)amount;
    rect.y += (int32_t)amount;
    rect.width -= amount * 2;
    rect.height -= amount * 2;
    return rect;
}

void vino_gui_window_draw(struct kos_gui_rect bounds, const char *title,
                          struct kos_gui_color background,
                          struct kos_gui_color title_color) {
    (void)vino_gui_fill(bounds, background);
    (void)vino_gui_border(bounds, color_border);
    struct kos_gui_rect title_bounds = inset(bounds, 16);
    if (title_bounds.width != 0 && title_bounds.height != 0) {
        (void)vino_gui_text(title_bounds.x, title_bounds.y, title, 2,
                            title_color, background);
    }
}

void vino_gui_panel_draw(struct kos_gui_rect bounds, struct kos_gui_color color) {
    (void)vino_gui_fill(bounds, color);
    (void)vino_gui_border(bounds, color_border);
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
    (void)vino_gui_text(button->bounds.x + 12, button->bounds.y + 8,
                        button->label, 1, foreground, fill);
}

void vino_gui_dropdown_draw(const struct vino_gui_dropdown *dropdown,
                            struct kos_gui_color foreground,
                            struct kos_gui_color background,
                            struct kos_gui_color accent) {
    if (dropdown == NULL) return;
    (void)vino_gui_fill(dropdown->bounds, background);
    (void)vino_gui_border(dropdown->bounds, dropdown->expanded ? accent : color_border);
    (void)vino_gui_text(dropdown->bounds.x + 8, dropdown->bounds.y + 6,
                        dropdown->label, 1, accent, background);
    (void)vino_gui_text(dropdown->bounds.x + 8, dropdown->bounds.y + 22,
                        dropdown->value, 1, foreground, background);
}

void vino_gui_list_draw(const struct vino_gui_list *list,
                        struct kos_gui_color foreground,
                        struct kos_gui_color background,
                        struct kos_gui_color selected) {
    if (list == NULL || list->items == NULL || list->row_height == 0
        || list->bounds.width <= 4) return;
    (void)vino_gui_fill(list->bounds, background);
    (void)vino_gui_border(list->bounds, color_border);
    uint32_t visible = list->bounds.height / list->row_height;
    if (visible > list->item_count) visible = list->item_count;
    for (uint32_t index = 0; index < visible; ++index) {
        struct kos_gui_rect row = {
            .x = list->bounds.x + 2,
            .y = list->bounds.y + 2 + (int32_t)(index * list->row_height),
            .width = list->bounds.width - 4,
            .height = list->row_height,
        };
        struct kos_gui_color row_color = index == list->selected_item ? selected : background;
        (void)vino_gui_fill(row, row_color);
        (void)vino_gui_text(row.x + 8, row.y + 6, list->items[index], 1,
                            foreground, row_color);
    }
}

void vino_gui_dialog_draw(const struct vino_gui_dialog *dialog,
                          struct kos_gui_color foreground,
                          struct kos_gui_color background,
                          struct kos_gui_color accent) {
    if (dialog == NULL || !dialog->visible) return;
    (void)vino_gui_fill(dialog->bounds, background);
    (void)vino_gui_border(dialog->bounds, accent);
    (void)vino_gui_text(dialog->bounds.x + 12, dialog->bounds.y + 12,
                        dialog->title, 1, accent, background);
    (void)vino_gui_text(dialog->bounds.x + 12, dialog->bounds.y + 36,
                        dialog->message, 1, foreground, background);
}

void vino_gui_progress_draw(const struct vino_gui_progress_bar *progress,
                            struct kos_gui_color background,
                            struct kos_gui_color fill,
                            struct kos_gui_color border) {
    if (progress == NULL || progress->maximum == 0
        || progress->bounds.width <= 4 || progress->bounds.height <= 4) return;
    (void)vino_gui_fill(progress->bounds, background);
    (void)vino_gui_border(progress->bounds, border);
    uint64_t fill_width = (uint64_t)(progress->bounds.width - 4)
        * (progress->value > progress->maximum ? progress->maximum : progress->value)
        / progress->maximum;
    if (fill_width != 0) {
        struct kos_gui_rect bar = {
            .x = progress->bounds.x + 2,
            .y = progress->bounds.y + 2,
            .width = (uint32_t)fill_width,
            .height = progress->bounds.height - 4,
        };
        (void)vino_gui_fill(bar, fill);
    }
}
