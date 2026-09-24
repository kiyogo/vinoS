#ifndef KOS_GUI_SERVICE_H
#define KOS_GUI_SERVICE_H

#include <stdint.h>

#include <uapi/kos/gui.h>

/* Kernel adapters for framebuffer/keyboard drivers; no storage or VFS writes. */
uint64_t gui_service_display(uint64_t operation, uint64_t user_request);
uint64_t gui_service_input(uint64_t user_event);
void gui_service_abort_frame(void);
void gui_service_frame_stats_snapshot(struct kos_gui_frame_stats *stats);

#endif /* KOS_GUI_SERVICE_H */
