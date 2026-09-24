#ifndef KOS_MOUSE_H
#define KOS_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

struct kos_mouse_event {
    int32_t x;
    int32_t y;
    uint32_t buttons;
    int32_t wheel;
};

struct kos_mouse_stats {
    uint64_t event_count;
    uint64_t dropped_event_count;
    uint64_t movement_event_count;
    uint64_t button_event_count;
    uint64_t wheel_event_count;
    int32_t x;
    int32_t y;
    uint32_t buttons;
    int32_t wheel;
    uint32_t ps2_available;
    uint32_t usb_hid_available;
};

bool mouse_initialize(void);
bool mouse_is_initialized(void);
bool mouse_ps2_is_initialized(void);
bool mouse_usb_hid_is_initialized(void);
void mouse_interrupt(void);
void mouse_set_bounds(uint32_t width, uint32_t height);
void mouse_get_position(int32_t *x, int32_t *y);
bool mouse_read_event(struct kos_mouse_event *event);
void mouse_usb_relative_report(int32_t delta_x, int32_t delta_y,
                               uint32_t buttons, int32_t wheel);
void mouse_usb_absolute_report(uint32_t x, uint32_t y,
                               uint32_t max_x, uint32_t max_y,
                               uint32_t buttons, int32_t wheel);
void mouse_get_stats(struct kos_mouse_stats *stats);

#endif /* KOS_MOUSE_H */
