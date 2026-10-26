#ifndef KOS_UAPI_GUI_H
#define KOS_UAPI_GUI_H

#include <stdint.h>

#define KOS_GUI_ABI_VERSION 4u
#define KOS_GUI_TEXT_MAX 128u
#define KOS_GUI_SURFACE_POINTER 0x01u
#define KOS_GUI_SURFACE_BACKBUFFER 0x02u
/* BEGIN_FRAME only: skip copying old scanout when the caller paints the full surface. */
#define KOS_GUI_FRAME_DISCARD_PREVIOUS 0x01u
#define KOS_GUI_FRAME_DAMAGE_RECT 0x02u

#define KOS_GUI_RGB(red, green, blue) \
    ((((uint32_t)(red) & 0xffu) << 16) | (((uint32_t)(green) & 0xffu) << 8) \
        | ((uint32_t)(blue) & 0xffu))

enum kos_gui_display_operation {
    KOS_GUI_DISPLAY_GET_SURFACE = 1,
    KOS_GUI_DISPLAY_CLEAR,
    KOS_GUI_DISPLAY_FILL_RECT,
    KOS_GUI_DISPLAY_BORDER_RECT,
    KOS_GUI_DISPLAY_DRAW_TEXT,
    KOS_GUI_DISPLAY_GLASS_RECT,
    KOS_GUI_DISPLAY_DRAW_TEXT_TRANSPARENT,
    KOS_GUI_DISPLAY_BEGIN_FRAME,
    KOS_GUI_DISPLAY_PRESENT_FRAME,
    KOS_GUI_DISPLAY_BACKGROUND_CAPTURE,
    KOS_GUI_DISPLAY_BACKGROUND_RESTORE,
    KOS_GUI_DISPLAY_GET_FRAME_STATS,
    KOS_GUI_DISPLAY_BLIT_RGB24,
};

enum kos_gui_pixel_format {
    KOS_GUI_PIXEL_FORMAT_RGB32 = 1,
};

enum kos_gui_font_id {
    KOS_GUI_FONT_8X8 = 1,
    KOS_GUI_FONT_INTER_8X14 = 2,
};

enum kos_gui_input_type {
    KOS_GUI_INPUT_NONE = 0,
    KOS_GUI_INPUT_KEY = 1,
    KOS_GUI_INPUT_POINTER = 2,
};

/* KOS_GUI_INPUT_KEY.key contains translated ASCII, including these controls. */
enum kos_gui_key {
    KOS_GUI_KEY_BACKSPACE = '\b',
    KOS_GUI_KEY_TAB = '\t',
    KOS_GUI_KEY_ENTER = '\n',
    KOS_GUI_KEY_RETURN = '\r',
    KOS_GUI_KEY_ESCAPE = 27,
    KOS_GUI_KEY_SPACE = ' ',
    KOS_GUI_KEY_DELETE = 127,
};

struct kos_gui_surface {
    uint32_t abi_version;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bits_per_pixel;
    uint32_t pixel_format;
    uint32_t capabilities;
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
    uint8_t alpha;
};

struct kos_gui_display_request {
    uint32_t abi_version;
    uint32_t operation;
    struct kos_gui_rect rect;
    struct kos_gui_color color;
    struct kos_gui_color foreground;
    struct kos_gui_color background;
    uint32_t flags;
    uint32_t scale;
    uint32_t font_id;
    uint32_t radius;
    uint32_t blur_radius;
    char text[KOS_GUI_TEXT_MAX];
};

/* Image requests are separate from the small primitive request ABI. */
struct kos_gui_image_request {
    uint32_t abi_version;
    uint32_t pixel_format;
    struct kos_gui_rect destination;
    uint64_t pixels;
    uint64_t pixel_bytes;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t source_pitch;
    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t crop_width;
    uint32_t crop_height;
};

enum kos_gui_image_pixel_format {
    KOS_GUI_IMAGE_RGB24 = 1,
};

struct kos_gui_frame_stats {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t redraw_count;
    uint64_t draw_operation_count;
    uint64_t last_frame_draw_operations;
    uint64_t last_frame_duration_ms;
    uint64_t total_frame_duration_ms;
    uint64_t max_frame_duration_ms;
    uint64_t aborted_frame_count;
    uint32_t backbuffer_enabled;
    uint32_t background_cache_enabled;
    uint32_t timing_sample_count;
    uint32_t p50_frame_duration_ms;
    uint32_t p95_frame_duration_ms;
    uint32_t p99_frame_duration_ms;
    uint64_t blur_cache_hit_count;
    uint64_t blur_cache_miss_count;
    uint64_t blur_cache_bytes_used;
};

struct kos_gui_input_event {
    uint32_t abi_version;
    uint32_t type;
    uint32_t key;       /* KOS_GUI_INPUT_KEY carries translated ASCII. */
    uint32_t modifiers; /* Reserved; zero until modifier events are exposed. */
    uint64_t timestamp_ms;
    int32_t pointer_x;
    int32_t pointer_y;
    uint32_t pointer_buttons;
    int32_t pointer_wheel;
};

#endif /* KOS_UAPI_GUI_H */
