#ifndef KOS_UAPI_SYSTEM_CONTROL_H
#define KOS_UAPI_SYSTEM_CONTROL_H

#include <stdint.h>

enum kos_system_control_action {
    KOS_SYSTEM_CONTROL_CONNECT_NETWORK = 1,
    KOS_SYSTEM_CONTROL_RESTART,
    KOS_SYSTEM_CONTROL_SHUTDOWN,
};

/* Prevent accidental destructive actions from an uninitialized request. */
#define KOS_SYSTEM_CONTROL_CONFIRM_TOKEN 0x56494E4Fu

#endif /* KOS_UAPI_SYSTEM_CONTROL_H */
