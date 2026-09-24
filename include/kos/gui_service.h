#ifndef KOS_GUI_SERVICE_H
#define KOS_GUI_SERVICE_H

#include <stdint.h>

/* Kernel adapters for framebuffer/keyboard drivers; no storage or VFS writes. */
uint64_t gui_service_display(uint64_t operation, uint64_t user_request);
uint64_t gui_service_input(uint64_t user_event);

#endif /* KOS_GUI_SERVICE_H */
