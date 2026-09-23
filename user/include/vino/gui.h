#ifndef VINO_USER_GUI_H
#define VINO_USER_GUI_H

#include <stdbool.h>
#include <stdint.h>

#include <uapi/kos/gui.h>
#include <uapi/kos/system_info.h>
#include <uapi/kos/syscall_numbers.h>

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
long vino_gui_clear(struct kos_gui_color color);
long vino_gui_fill(struct kos_gui_rect rect, struct kos_gui_color color);
long vino_gui_border(struct kos_gui_rect rect, struct kos_gui_color color);
long vino_gui_text(int32_t x, int32_t y, const char *text, uint32_t scale,
                   struct kos_gui_color foreground, struct kos_gui_color background);
long vino_gui_input_read(struct kos_gui_input_event *event);

long vino_fs_open(const char *path, uint32_t flags);
long vino_fs_close(long descriptor);
long vino_fs_read(long descriptor, void *buffer, uint64_t length);
long vino_fs_write(long descriptor, const void *buffer, uint64_t length);
long vino_fs_stat(const char *path, struct kos_stat *stat);
long vino_fs_getdents(long descriptor, struct kos_dirent *entries, uint64_t capacity);
long vino_system_info_get(struct kos_system_info *info);

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
