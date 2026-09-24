#ifndef VINO_USER_GUI_H
#define VINO_USER_GUI_H

#include <stdbool.h>
#include <stdint.h>

#include <uapi/kos/gui.h>

struct vino_gui_window {
    struct kos_gui_rect bounds;
    const char *title;
    struct kos_gui_color background;
    struct kos_gui_color title_color;
};

struct vino_gui_panel {
    struct kos_gui_rect bounds;
    struct kos_gui_color color;
};

struct vino_gui_button {
    struct kos_gui_rect bounds;
    const char *label;
    bool enabled;
    bool selected;
};

struct vino_gui_list {
    struct kos_gui_rect bounds;
    const char *const *items;
    uint32_t item_count;
    uint32_t selected_item;
    uint32_t row_height;
    uint32_t first_visible_item;
};

struct vino_gui_dropdown {
    struct kos_gui_rect bounds;
    const char *label;
    const char *value;
    bool expanded;
};

struct vino_gui_dialog {
    struct kos_gui_rect bounds;
    const char *title;
    const char *message;
    bool visible;
};

struct vino_gui_progress_bar {
    struct kos_gui_rect bounds;
    uint32_t value;
    uint32_t maximum;
};

long vino_gui_surface_get(struct kos_gui_surface *surface);
long vino_gui_frame_begin(bool discard_previous);
long vino_gui_frame_present(void);
long vino_gui_background_capture(void);
long vino_gui_background_restore(void);
long vino_gui_frame_stats_get(struct kos_gui_frame_stats *stats);
long vino_gui_clear(struct kos_gui_color color);
long vino_gui_fill(struct kos_gui_rect rect, struct kos_gui_color color);
long vino_gui_border(struct kos_gui_rect rect, struct kos_gui_color color);
long vino_gui_glass(struct kos_gui_rect rect, struct kos_gui_color fill,
                    struct kos_gui_color border, uint32_t radius,
                    uint32_t blur_radius);
long vino_gui_text(int32_t x, int32_t y, const char *text, uint32_t scale,
                   struct kos_gui_color foreground, struct kos_gui_color background);
long vino_gui_text_transparent(int32_t x, int32_t y, const char *text, uint32_t scale,
                               struct kos_gui_color foreground);
long vino_gui_text_transparent_font(int32_t x, int32_t y, const char *text,
                                    uint32_t scale, enum kos_gui_font_id font_id,
                                    struct kos_gui_color foreground);
long vino_gui_text_font(int32_t x, int32_t y, const char *text, uint32_t scale,
                        enum kos_gui_font_id font_id, struct kos_gui_color foreground,
                        struct kos_gui_color background);
long vino_gui_blit_rgb24(struct kos_gui_rect destination, const uint8_t *pixels,
                         uint64_t pixel_bytes, uint32_t source_width,
                         uint32_t source_height, uint32_t source_pitch,
                         uint32_t crop_x, uint32_t crop_y, uint32_t crop_width,
                         uint32_t crop_height);
long vino_gui_input_read(struct kos_gui_input_event *event);
bool vino_gui_rect_contains(const struct kos_gui_rect *rect, int32_t x, int32_t y);

void vino_gui_window_render(const struct vino_gui_window *window);
void vino_gui_panel_render(const struct vino_gui_panel *panel);
void vino_gui_window_draw(struct kos_gui_rect bounds, const char *title,
                          struct kos_gui_color background,
                          struct kos_gui_color title_color);
void vino_gui_panel_draw(struct kos_gui_rect bounds, struct kos_gui_color color);
void vino_gui_button_draw(const struct vino_gui_button *button,
                          struct kos_gui_color foreground,
                          struct kos_gui_color normal,
                          struct kos_gui_color active);
void vino_gui_dropdown_draw(const struct vino_gui_dropdown *dropdown,
                            struct kos_gui_color foreground,
                            struct kos_gui_color background,
                            struct kos_gui_color accent);
void vino_gui_list_draw(const struct vino_gui_list *list,
                        struct kos_gui_color foreground,
                        struct kos_gui_color background,
                        struct kos_gui_color selected);
void vino_gui_dialog_draw(const struct vino_gui_dialog *dialog,
                          struct kos_gui_color foreground,
                          struct kos_gui_color background,
                          struct kos_gui_color accent);
void vino_gui_progress_draw(const struct vino_gui_progress_bar *progress,
                            struct kos_gui_color background,
                            struct kos_gui_color fill,
                            struct kos_gui_color border);

#endif /* VINO_USER_GUI_H */
