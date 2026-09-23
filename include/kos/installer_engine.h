#ifndef KOS_INSTALLER_ENGINE_H
#define KOS_INSTALLER_ENGINE_H

#include <kos/installer_flow.h>

int install_base_os(struct installer_flow *flow, void *context);
int install_gui(struct installer_flow *flow, const struct gui_manifest *gui,
                void *context);
int install_bootloader(struct installer_flow *flow, void *context);

#endif /* KOS_INSTALLER_ENGINE_H */
