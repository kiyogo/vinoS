#ifndef KOS_GUI_MANAGER_H
#define KOS_GUI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#define GUI_MAX_ID_LENGTH 32
#define GUI_MAX_NAME_LENGTH 64
#define GUI_MAX_VERSION_LENGTH 32
#define GUI_MAX_PATH_LENGTH 128

enum gui_package_source {
    GUI_PACKAGE_SOURCE_LOCAL_ISO = 0,
    GUI_PACKAGE_SOURCE_REMOTE = 1,
};

enum gui_manager_status {
    GUI_MANAGER_OK = 0,
    GUI_MANAGER_ERR_INVALID_ARGUMENT = -1,
    GUI_MANAGER_ERR_NOT_FOUND = -2,
    GUI_MANAGER_ERR_ALREADY_INSTALLED = -3,
    GUI_MANAGER_ERR_NOT_INSTALLED = -4,
    GUI_MANAGER_ERR_PACKAGE_MISSING = -5,
    GUI_MANAGER_ERR_IO = -6,
    GUI_MANAGER_ERR_NOT_IMPLEMENTED = -7,
};

/* A descriptor is the single source of truth for installer-visible GUIs. */
struct gui_manifest {
    char id[GUI_MAX_ID_LENGTH];
    char name[GUI_MAX_NAME_LENGTH];
    char version[GUI_MAX_VERSION_LENGTH];
    enum gui_package_source source;
    bool requires_network;
    bool installer_visible;
    bool recovery_gui;
    char package_path[GUI_MAX_PATH_LENGTH];
    char package_app_id[GUI_MAX_ID_LENGTH];
    char entry_point[GUI_MAX_PATH_LENGTH];
};

bool gui_manager_init(void);
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
