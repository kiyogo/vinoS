#ifndef VINO_USER_INSTALLER_H
#define VINO_USER_INSTALLER_H

#include <uapi/kos/installer.h>

/* Sends one installer action to the privileged installer engine and refreshes state. */
long vino_installer_action(const struct kos_installer_action_request *request,
                           struct kos_installer_state *state);

#endif /* VINO_USER_INSTALLER_H */
