#include <stdbool.h>
#include <stdint.h>

#include <kos/gui_manager.h>
#include <kos/installer.h>
#include <kos/log.h>
#include <kos/memory.h>
#include <kos/pkg_db.h>
#include <kos/vfs.h>

static const struct gui_manifest gui_registry[] = {
    {
        .id = "default",
        .name = "Default",
        .version = "1.0.0",
        .source = GUI_PACKAGE_SOURCE_LOCAL_ISO,
        .requires_network = false,
        .installer_visible = true,
        .recovery_gui = false,
        .package_path = "C:/GUI/DEFAULT.KPKG",
        .package_app_id = "com.vino.gui.default",
        .entry_point = "bin/gui-default.elf",
    },
    {
        .id = "mvp",
        .name = "MVP",
        .version = "1.0.0",
        .source = GUI_PACKAGE_SOURCE_LOCAL_ISO,
        .requires_network = false,
        .installer_visible = false,
        .recovery_gui = true,
        .package_path = "C:/GUI/MVP.KPKG",
        .package_app_id = "com.vino.gui.mvp",
        .entry_point = "bin/gui-mvp.elf",
    },
};

static bool manager_initialized;
static char default_gui_id[GUI_MAX_ID_LENGTH];

struct gui_memory_reader {
    const uint8_t *data;
    uint64_t size;
};

static bool gui_memory_read(void *context, uint64_t offset, void *buffer, size_t size) {
    struct gui_memory_reader *reader = (struct gui_memory_reader *)context;
    if (reader == NULL || buffer == NULL || offset > reader->size
        || (uint64_t)size > reader->size - offset) {
        return false;
    }
    kmemcpy(buffer, reader->data + offset, size);
    return true;
}

static uint64_t gui_memory_size(void *context) {
    struct gui_memory_reader *reader = (struct gui_memory_reader *)context;
    return reader == NULL ? 0 : reader->size;
}

static const struct gui_manifest *find_manifest(const char *id) {
    if (id == NULL || *id == '\0') {
        return NULL;
    }
    for (uint32_t index = 0; index < gui_manifest_count(); ++index) {
        const struct gui_manifest *manifest = &gui_registry[index];
        if (kstrcmp(manifest->id, id) == 0) {
            return manifest;
        }
    }
    return NULL;
}

static int install_manifest(const struct gui_manifest *manifest, bool update) {
    const uint8_t *package_data = NULL;
    uint64_t package_size = 0;
    if (!vfs_read_file(manifest->package_path, &package_data, &package_size)
        || package_data == NULL || package_size == 0) {
        return GUI_MANAGER_ERR_PACKAGE_MISSING;
    }

    struct gui_memory_reader memory_reader = {
        .data = package_data,
        .size = package_size,
    };
    struct kpkg_reader reader = {
        .context = &memory_reader,
        .read = gui_memory_read,
        .get_size = gui_memory_size,
    };
    struct installer_options options = {0};
    int result = update ? installer_update(reader, &options) : installer_install(reader, &options);
    if (result == INSTALLER_OK) {
        return GUI_MANAGER_OK;
    }
    if (result == INSTALLER_ERR_ALREADY_INSTALLED) {
        return GUI_MANAGER_ERR_ALREADY_INSTALLED;
    }
    if (result == INSTALLER_ERR_NOT_FOUND) {
        return GUI_MANAGER_ERR_NOT_INSTALLED;
    }
    if (result == INSTALLER_ERR_IO || result == INSTALLER_ERR_STAGING_FAIL
        || result == INSTALLER_ERR_COMMIT_FAIL) {
        return GUI_MANAGER_ERR_IO;
    }
    return GUI_MANAGER_ERR_IO;
}

bool gui_manager_init(void) {
    manager_initialized = true;
    default_gui_id[0] = '\0';
    log_info("GUI Manager: manifest registry initialized (Vino OS edition=default).");
    return true;
}

uint32_t gui_manifest_count(void) {
    return (uint32_t)(sizeof(gui_registry) / sizeof(gui_registry[0]));
}

const struct gui_manifest *gui_manifest_at(uint32_t index) {
    return index < gui_manifest_count() ? &gui_registry[index] : NULL;
}

const struct gui_manifest *gui_manifest_find(const char *id) {
    return find_manifest(id);
}

bool gui_manifest_package_available(const struct gui_manifest *manifest) {
    const uint8_t *data = NULL;
    uint64_t size = 0;
    if (manifest == NULL || manifest->source != GUI_PACKAGE_SOURCE_LOCAL_ISO) {
        return false;
    }
    return vfs_read_file(manifest->package_path, &data, &size) && data != NULL && size != 0;
}

bool gui_is_installed(const char *id) {
    const struct gui_manifest *manifest = find_manifest(id);
    return manager_initialized && manifest != NULL && pkg_db_find(manifest->package_app_id) != NULL;
}

uint32_t gui_installed_count(void) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < gui_manifest_count(); ++index) {
        if (gui_is_installed(gui_registry[index].id)) {
            ++count;
        }
    }
    return count;
}

const char *gui_default_id(void) {
    return default_gui_id[0] == '\0' ? NULL : default_gui_id;
}

int gui_install(const char *id) {
    const struct gui_manifest *manifest = find_manifest(id);
    if (!manager_initialized || manifest == NULL) {
        return GUI_MANAGER_ERR_NOT_FOUND;
    }
    if (manifest->source == GUI_PACKAGE_SOURCE_REMOTE) {
        return GUI_MANAGER_ERR_NOT_IMPLEMENTED;
    }
    if (gui_is_installed(id)) {
        return GUI_MANAGER_ERR_ALREADY_INSTALLED;
    }
    log_infof("GUI Manager: installing '%s' from %s", manifest->id, manifest->package_path);
    return install_manifest(manifest, false);
}

int gui_remove(const char *id) {
    const struct gui_manifest *manifest = find_manifest(id);
    if (!manager_initialized || manifest == NULL) {
        return GUI_MANAGER_ERR_NOT_FOUND;
    }
    if (!gui_is_installed(id)) {
        return GUI_MANAGER_ERR_NOT_INSTALLED;
    }
    int result = installer_uninstall(manifest->package_app_id, false);
    if (result != INSTALLER_OK) {
        return result == INSTALLER_ERR_NOT_FOUND ? GUI_MANAGER_ERR_NOT_INSTALLED : GUI_MANAGER_ERR_IO;
    }
    if (kstrcmp(default_gui_id, manifest->id) == 0) {
        default_gui_id[0] = '\0';
    }
    log_infof("GUI Manager: removed '%s'; Vino OS remains bootable in CLI mode.", manifest->id);
    return GUI_MANAGER_OK;
}

int gui_reinstall(const char *id) {
    const struct gui_manifest *manifest = find_manifest(id);
    if (!manager_initialized || manifest == NULL) {
        return GUI_MANAGER_ERR_NOT_FOUND;
    }
    if (manifest->source == GUI_PACKAGE_SOURCE_REMOTE) {
        return GUI_MANAGER_ERR_NOT_IMPLEMENTED;
    }
    if (!gui_is_installed(id)) {
        return gui_install(id);
    }
    return install_manifest(manifest, true);
}

int gui_start(const char *id) {
    if (!gui_is_installed(id)) {
        return GUI_MANAGER_ERR_NOT_INSTALLED;
    }
    /* Process/display-session ownership is intentionally outside the kernel registry. */
    return GUI_MANAGER_ERR_NOT_IMPLEMENTED;
}

int gui_stop(const char *id) {
    if (!gui_is_installed(id)) {
        return GUI_MANAGER_ERR_NOT_INSTALLED;
    }
    return GUI_MANAGER_ERR_NOT_IMPLEMENTED;
}

int gui_restart(const char *id) {
    if (!gui_is_installed(id)) {
        return GUI_MANAGER_ERR_NOT_INSTALLED;
    }
    return GUI_MANAGER_ERR_NOT_IMPLEMENTED;
}

int gui_set_default(const char *id) {
    if (!gui_is_installed(id)) {
        return GUI_MANAGER_ERR_NOT_INSTALLED;
    }
    kstrncpy(default_gui_id, id, sizeof(default_gui_id) - 1);
    default_gui_id[sizeof(default_gui_id) - 1] = '\0';
    return GUI_MANAGER_OK;
}

int gui_status(const char *id) {
    const struct gui_manifest *manifest = find_manifest(id);
    if (!manager_initialized || manifest == NULL) {
        return GUI_MANAGER_ERR_NOT_FOUND;
    }
    return gui_is_installed(id) ? GUI_MANAGER_OK : GUI_MANAGER_ERR_NOT_INSTALLED;
}

const char *gui_manager_status_string(int status) {
    switch (status) {
        case GUI_MANAGER_OK: return "OK";
        case GUI_MANAGER_ERR_INVALID_ARGUMENT: return "Invalid argument";
        case GUI_MANAGER_ERR_NOT_FOUND: return "GUI not found";
        case GUI_MANAGER_ERR_ALREADY_INSTALLED: return "GUI is already installed";
        case GUI_MANAGER_ERR_NOT_INSTALLED: return "GUI is not installed";
        case GUI_MANAGER_ERR_PACKAGE_MISSING: return "GUI package is missing from the ISO";
        case GUI_MANAGER_ERR_IO: return "GUI package installation I/O failed";
        case GUI_MANAGER_ERR_NOT_IMPLEMENTED: return "GUI process/session backend is not implemented";
        default: return "Unknown GUI manager status";
    }
}
