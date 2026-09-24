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

bool mouse_initialize(void);
bool mouse_is_initialized(void);
void mouse_interrupt(void);
void mouse_set_bounds(uint32_t width, uint32_t height);
bool mouse_read_event(struct kos_mouse_event *event);

#endif /* KOS_MOUSE_H */
