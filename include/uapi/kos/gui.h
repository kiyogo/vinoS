#ifndef KOS_UAPI_GUI_H
#define KOS_UAPI_GUI_H

#include <stdint.h>

#define KOS_GUI_ABI_VERSION 1u
#define KOS_GUI_TEXT_MAX 128u

#define KOS_GUI_RGB(red, green, blue) \
    ((((uint32_t)(red) & 0xffu) << 16) | (((uint32_t)(green) & 0xffu) << 8) \
        | ((uint32_t)(blue) & 0xffu))

enum kos_gui_display_operation {
    KOS_GUI_DISPLAY_GET_SURFACE = 1,
    KOS_GUI_DISPLAY_CLEAR,
    KOS_GUI_DISPLAY_FILL_RECT,
    KOS_GUI_DISPLAY_BORDER_RECT,
    KOS_GUI_DISPLAY_DRAW_TEXT,
};

enum kos_gui_pixel_format {
    KOS_GUI_PIXEL_FORMAT_RGB32 = 1,
};

enum kos_gui_font_id {
    KOS_GUI_FONT_8X8 = 1,
};

enum kos_gui_input_type {
    KOS_GUI_INPUT_NONE = 0,
    KOS_GUI_INPUT_KEY = 1,
};

struct kos_gui_surface {
    uint32_t abi_version;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bits_per_pixel;
    uint32_t pixel_format;
};

struct kos_gui_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

struct kos_gui_color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t reserved;
};

struct kos_gui_display_request {
    uint32_t abi_version;
    uint32_t operation;
    struct kos_gui_rect rect;
    struct kos_gui_color color;
    struct kos_gui_color foreground;
    struct kos_gui_color background;
    uint32_t scale;
    uint32_t font_id;
    char text[KOS_GUI_TEXT_MAX];
};

struct kos_gui_input_event {
    uint32_t abi_version;
    uint32_t type;
    uint32_t key;
    uint32_t modifiers;
    uint64_t timestamp_ms;
};

#endif /* KOS_UAPI_GUI_H */
