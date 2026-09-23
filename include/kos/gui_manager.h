#ifndef KOS_GUI_MANAGER_H
#define KOS_GUI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include <kos/installer_contracts.h>

enum gui_manager_status {
    GUI_MANAGER_OK = 0,
    GUI_MANAGER_ERR_INVALID_ARGUMENT = -1,
    GUI_MANAGER_ERR_NOT_FOUND = -2,
    GUI_MANAGER_ERR_ALREADY_INSTALLED = -3,
    GUI_MANAGER_ERR_NOT_INSTALLED = -4,
    GUI_MANAGER_ERR_PACKAGE_MISSING = -5,
    GUI_MANAGER_ERR_IO = -6,
    GUI_MANAGER_ERR_NOT_IMPLEMENTED = -7,
    GUI_MANAGER_ERR_INTEGRITY = -8,
    GUI_MANAGER_ERR_AUTH = -9,
    GUI_MANAGER_ERR_BUSY = -10,
};

bool gui_manager_init(void);
char gui_manager_system_volume_letter(void);
bool gui_manager_default_should_autostart(void);
uint32_t gui_manifest_count(void);
const struct gui_manifest *gui_manifest_at(uint32_t index);
const struct gui_manifest *gui_manifest_find(const char *id);
bool gui_manifest_package_available(const struct gui_manifest *manifest);

uint32_t gui_installed_count(void);
bool gui_is_installed(const char *id);
const char *gui_default_id(void);

int gui_install(const char *id);
int gui_remove(const char *id);
int gui_reinstall(const char *id);
int gui_start(const char *id);
int gui_stop(const char *id);
int gui_restart(const char *id);
int gui_set_default(const char *id);
int gui_status(const char *id);
const char *gui_manager_status_string(int status);

#endif /* KOS_GUI_MANAGER_H */
