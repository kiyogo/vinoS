#ifndef KOS_UAPI_GUI_CONTROL_H
#define KOS_UAPI_GUI_CONTROL_H

#include <stdint.h>

#define KOS_GUI_CONTROL_ABI_VERSION 1u
#define KOS_GUI_CONTROL_ID_MAX 32u
#define KOS_GUI_CONTROL_TEXT_MAX 96u

enum kos_gui_control_action {
    KOS_GUI_CONTROL_QUERY = 1,
    KOS_GUI_CONTROL_INSTALL_LOCAL,
    KOS_GUI_CONTROL_REINSTALL_LOCAL,
    KOS_GUI_CONTROL_SET_DEFAULT,
    KOS_GUI_CONTROL_INSTALL_REMOTE,
    KOS_GUI_CONTROL_RECOVER_DEFAULT,
};

struct kos_gui_control_request {
    uint32_t abi_version;
    uint32_t action;
    uint32_t installed_mask;
    int32_t result;
    char gui_id[KOS_GUI_CONTROL_ID_MAX];
    char default_gui_id[KOS_GUI_CONTROL_ID_MAX];
    char message[KOS_GUI_CONTROL_TEXT_MAX];
};

#define KOS_GUI_INSTALLED_DEFAULT (1u << 0)
#define KOS_GUI_INSTALLED_MVP     (1u << 1)

#endif /* KOS_UAPI_GUI_CONTROL_H */
