#ifndef VINO_USER_SYSTEM_H
#define VINO_USER_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>

#include <uapi/kos/gui_control.h>
#include <uapi/kos/system_info.h>

long vino_system_info_get(struct kos_system_info *info);
long vino_gui_control(struct kos_gui_control_request *request);
long vino_system_control(uint32_t action, bool confirmed);

#endif /* VINO_USER_SYSTEM_H */
