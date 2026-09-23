#include <stdbool.h>
#include <stdint.h>
#include <kos/block_cache.h>
#include <kos/gui_manager.h>
#include <kos/gui_repository.h>
#include <kos/heap.h>
#include <kos/installer.h>
#include <kos/log.h>
#include <kos/memory.h>
#include <kos/pkg_db.h>
#include <kos/process.h>
#include <kos/vfs.h>
#include <kos/volume.h>
#include <uapi/kos/syscall_numbers.h>

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
        .install_root = "system/gui/default",
        .entry_point = "bin/gui-default.elf",
        .architecture = "x86_64",
        .min_os_version = "0.1",
        .signature = "unsigned; detached SHA-256 verified",
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
        .install_root = "system/gui/mvp",
        .entry_point = "bin/gui-mvp.elf",
        .architecture = "x86_64",
        .min_os_version = "0.1",
        .signature = "unsigned; detached SHA-256 verified",
    },
};

static const struct package_verifier_ops local_package_verifier_ops = {
    .verify_checksum = package_verifier_sha256_hex,
};
static const struct package_verifier local_package_verifier = {
    .ops = &local_package_verifier_ops,
};

static bool manager_initialized;
static char default_gui_id[GUI_MAX_ID_LENGTH];
static struct gui_session active_gui_session;
static struct process *active_gui_process;
static char active_gui_entry_path[GUI_MAX_PATH_LENGTH];
static uint32_t next_gui_session_id = 1;
static char persistent_volume_letter;
static bool recovery_default_requested;

static bool find_installed_vino_volume(char *letter_out);

static bool read_selected_gui_id(char output[GUI_MAX_ID_LENGTH]) {
    if (output == NULL) return false;
    output[0] = '\0';
    char volume = 0;
    if (!find_installed_vino_volume(&volume)) return false;

    char path[GUI_MAX_PATH_LENGTH];
    int path_size = snprintf(path, sizeof(path),
        "%c:/VINO/SYSTEM/GUI/SELECTED.TXT", volume);
    const uint8_t *data = NULL;
    uint64_t size = 0;
    if (path_size <= 0 || (uint64_t)path_size >= sizeof(path)
        || !vfs_read_file(path, &data, &size) || data == NULL) return false;

    static const char prefix[] = "gui_id=";
    bool found = false;
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t line_end = offset;
        while (line_end < size && data[line_end] != '\n') ++line_end;
        uint64_t content_end = line_end;
        if (content_end > offset && data[content_end - 1] == '\r') --content_end;
        uint64_t value_offset = offset + sizeof(prefix) - 1;
        if (content_end >= value_offset
            && kmemcmp(data + offset, prefix, sizeof(prefix) - 1) == 0) {
            uint64_t value_size = content_end - value_offset;
            if (value_size != 0 && value_size < GUI_MAX_ID_LENGTH) {
                bool valid = true;
                for (uint64_t index = 0; index < value_size; ++index) {
                    uint8_t character = data[value_offset + index];
                    if (!((character >= 'a' && character <= 'z')
                          || (character >= '0' && character <= '9')
                          || character == '-' || character == '_')) {
                        valid = false;
                        break;
                    }
                    output[index] = (char)character;
                }
                if (valid) {
                    output[value_size] = '\0';
                    found = true;
                }
            }
            break;
        }
        offset = line_end < size ? line_end + 1 : size;
    }
    if (size != 0) kfree((void *)data);
    if (!found) output[0] = '\0';
    return found;
}

static bool gui_registry_is_isolated(void) {
    for (uint32_t first = 0; first < gui_manifest_count(); ++first) {
        const struct gui_manifest *left = &gui_registry[first];
        for (uint32_t second = first + 1; second < gui_manifest_count(); ++second) {
            const struct gui_manifest *right = &gui_registry[second];
            uint64_t left_size = kstrlen(left->install_root);
            uint64_t right_size = kstrlen(right->install_root);
            bool roots_overlap = kstrcmp(left->install_root, right->install_root) == 0
                || (left_size < right_size
                    && kmemcmp(left->install_root, right->install_root, left_size) == 0
                    && right->install_root[left_size] == '/')
                || (right_size < left_size
                    && kmemcmp(right->install_root, left->install_root, right_size) == 0
                    && left->install_root[right_size] == '/');
            if (roots_overlap
                || kstrcmp(left->package_app_id, right->package_app_id) == 0) {
                log_errorf("GUI Manager: registry namespaces overlap ('%s', '%s').",
                           left->id, right->id);
                return false;
            }
        }
    }
    return true;
}

static bool persist_gui_selection(const struct gui_manifest *manifest) {
    char volume = 0;
    if (!find_installed_vino_volume(&volume)) return false;

    char path[GUI_MAX_PATH_LENGTH];
    char selection[160];
    int path_size = snprintf(path, sizeof(path),
        "%c:/VINO/SYSTEM/GUI/SELECTED.TXT", volume);
    int selection_size = manifest == NULL
        ? snprintf(selection, sizeof(selection), "gui_id=none\nversion=0\n")
        : snprintf(selection, sizeof(selection), "gui_id=%s\nversion=%s\n",
                   manifest->id, manifest->version);
    return path_size > 0 && (uint64_t)path_size < sizeof(path)
        && selection_size > 0 && (uint64_t)selection_size < sizeof(selection)
        && vfs_write_file(path, (const uint8_t *)selection,
                          (uint64_t)selection_size)
        && block_cache_sync();
}

static bool buffer_has_line(const uint8_t *buffer, uint64_t size,
                            const char *expected_line) {
    if (buffer == NULL || expected_line == NULL) return false;
    uint64_t expected_size = kstrlen(expected_line);
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t line_end = offset;
        while (line_end < size && buffer[line_end] != '\n') ++line_end;
        uint64_t content_end = line_end;
        if (content_end > offset && buffer[content_end - 1] == '\r') --content_end;
        if (content_end - offset == expected_size) {
            uint64_t index = 0;
            while (index < expected_size
                   && buffer[offset + index] == (uint8_t)expected_line[index]) ++index;
            if (index == expected_size) return true;
        }
        offset = line_end < size ? line_end + 1 : size;
    }
    return false;
}

static bool volume_metadata_has(const char *relative_path, const char *line) {
    char volume = 0;
    if (relative_path == NULL || line == NULL
        || !find_installed_vino_volume(&volume)) return false;
    char path[GUI_MAX_PATH_LENGTH];
    int path_size = snprintf(path, sizeof(path), "%c:/VINO/%s", volume, relative_path);
    const uint8_t *data = NULL;
    uint64_t size = 0;
    if (path_size <= 0 || (uint64_t)path_size >= sizeof(path)
        || !vfs_read_file(path, &data, &size) || data == NULL) return false;
    bool matches = buffer_has_line(data, size, line);
    if (size != 0) kfree((void *)data);
    return matches;
}

static bool selected_gui_is(const char *id) {
    if (id == NULL || *id == '\0') return false;
    char selected_id[GUI_MAX_ID_LENGTH];
    return read_selected_gui_id(selected_id) && kstrcmp(selected_id, id) == 0;
}

static void gui_process_entry(void *argument) {
    const char *path = (const char *)argument;
    int result = process_exec(path, 0, 0);
    struct process *process = process_get_current();
    if (process != 0 && process->pid > 1) process_exit(process, result);
    thread_exit(result);
}

static bool resolve_gui_entry_path(const struct gui_manifest *manifest,
                                   char *path, uint64_t capacity) {
    if (manifest == 0 || path == 0 || capacity == 0) return false;
    char volume = 0;
    int written;
    if (find_installed_vino_volume(&volume)) {
        written = snprintf(path, capacity, "%c:/VINO/%s/%s", volume,
                           manifest->install_root, manifest->entry_point);
    } else {
        written = snprintf(path, capacity, "D:/%s/%s", manifest->install_root,
                           manifest->entry_point);
    }
    return written > 0 && (uint64_t)written < capacity;
}

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

static bool package_checksum_matches(const struct gui_manifest *manifest,
                                     const uint8_t *package_data,
                                     uint64_t package_size) {
    if (manifest == NULL || package_data == NULL || package_size == 0) return false;
    char checksum_path[GUI_MAX_PATH_LENGTH];
    int path_size = snprintf(checksum_path, sizeof(checksum_path), "%s.SHA256",
                             manifest->package_path);
    if (path_size <= 0 || (uint64_t)path_size >= sizeof(checksum_path)) return false;

    const uint8_t *checksum_file = NULL;
    uint64_t checksum_file_size = 0;
    if (!vfs_read_file(checksum_path, &checksum_file, &checksum_file_size)
        || checksum_file == NULL) {
        return false;
    }

    bool valid_checksum_file = checksum_file_size >= 64;
    for (uint64_t index = 64;
         valid_checksum_file && index < checksum_file_size; ++index) {
        if (checksum_file[index] != '\r' && checksum_file[index] != '\n') {
            valid_checksum_file = false;
        }
    }
    char expected_checksum[GUI_MAX_CHECKSUM_LENGTH] = {0};
    if (valid_checksum_file) kmemcpy(expected_checksum, checksum_file, 64);
    if (checksum_file_size != 0) kfree((void *)checksum_file);
    return valid_checksum_file
        && package_verifier_verify_checksum(&local_package_verifier,
                                             expected_checksum, package_data,
                                             package_size)
            == PACKAGE_VERIFICATION_OK;
}

static bool find_installed_vino_volume(char *letter_out) {
    if (letter_out == NULL) return false;
    for (uint32_t index = 0; index < volume_manager_get_count(); ++index) {
        const struct volume_entry *volume = volume_manager_get_at(index);
        if (volume == NULL || !volume->is_mounted
            || kstrcmp(volume->fs_type, "FAT32") != 0) continue;
        char metadata_path[GUI_MAX_PATH_LENGTH];
        int path_size = snprintf(metadata_path, sizeof(metadata_path),
                                 "%c:/VINO/SYSTEM/OS.META", volume->letter);
        const uint8_t *metadata_data = NULL;
        uint64_t metadata_size = 0;
        bool metadata_valid = path_size > 0 && (uint64_t)path_size < sizeof(metadata_path)
            && vfs_read_file(metadata_path, &metadata_data, &metadata_size)
            && buffer_has_line(metadata_data, metadata_size, "os_edition=default")
            && buffer_has_line(metadata_data, metadata_size, "install_state=committed");
        if (metadata_data != NULL && metadata_size != 0) kfree((void *)metadata_data);
        if (metadata_valid) {
            *letter_out = volume->letter;
            return true;
        }
    }
    return false;
}

static bool resolve_gui_install_paths(const struct gui_manifest *manifest,
                                      char *install_path, uint64_t install_capacity,
                                      char *data_path, uint64_t data_capacity) {
    if (manifest == NULL || install_path == NULL || data_path == NULL
        || install_capacity == 0 || data_capacity == 0) return false;
    char volume = 0;
    int install_size;
    int data_size;
    if (find_installed_vino_volume(&volume)) {
        install_size = snprintf(install_path, install_capacity, "%c:/VINO/%s",
                                volume, manifest->install_root);
        data_size = snprintf(data_path, data_capacity,
                             "%c:/VINO/SYSTEM/GUI/DATA/%s", volume, manifest->id);
    }
    else {
        install_size = snprintf(install_path, install_capacity, "D:/%s",
                                manifest->install_root);
        data_size = snprintf(data_path, data_capacity, "D:/system/gui/data/%s",
                             manifest->id);
    }
    return install_size > 0 && (uint64_t)install_size < install_capacity
        && data_size > 0 && (uint64_t)data_size < data_capacity;
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
    if (!package_checksum_matches(manifest, package_data, package_size)) {
        log_errorf("GUI Manager: package checksum failed for '%s'.", manifest->id);
        return GUI_MANAGER_ERR_INTEGRITY;
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
    char install_path[GUI_MAX_PATH_LENGTH];
    char data_path[GUI_MAX_PATH_LENGTH];
    if (!resolve_gui_install_paths(manifest, install_path, sizeof(install_path),
                                   data_path, sizeof(data_path))) {
        return GUI_MANAGER_ERR_IO;
    }
    struct installer_options options = {
        .target_install_path = install_path,
        .target_data_path = data_path,
    };
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
    if (result == INSTALLER_ERR_INTEGRITY_FAIL) {
        return GUI_MANAGER_ERR_INTEGRITY;
    }
    if (result == INSTALLER_ERR_AUTH_FAIL) {
        return GUI_MANAGER_ERR_AUTH;
    }
    if (result == INSTALLER_ERR_IO || result == INSTALLER_ERR_STAGING_FAIL
        || result == INSTALLER_ERR_COMMIT_FAIL) {
        return GUI_MANAGER_ERR_IO;
    }
    return GUI_MANAGER_ERR_IO;
}

bool gui_manager_init(void) {
    for (uint32_t index = 0; index < gui_manifest_count(); ++index) {
        if (!gui_manifest_is_valid(&gui_registry[index])) {
            log_errorf("GUI Manager: invalid manifest for registry entry %u", index);
            return false;
        }
    }
    if (!gui_registry_is_isolated()) return false;
    manager_initialized = true;
    persistent_volume_letter = 0;
    (void)find_installed_vino_volume(&persistent_volume_letter);
    default_gui_id[0] = '\0';
    active_gui_process = 0;
    recovery_default_requested = false;
    gui_session_init(&active_gui_session, "");
    char selected_id[GUI_MAX_ID_LENGTH];
    if (read_selected_gui_id(selected_id) && find_manifest(selected_id) != NULL) {
        kstrncpy(default_gui_id, selected_id, sizeof(default_gui_id) - 1);
        default_gui_id[sizeof(default_gui_id) - 1] = '\0';
    }
    log_info("GUI Manager: manifest registry initialized (Vino OS edition=default).");
    return true;
}

char gui_manager_system_volume_letter(void) {
    if (persistent_volume_letter >= 'E' && persistent_volume_letter <= 'Z') {
        return persistent_volume_letter;
    }
    char letter = 0;
    if (!find_installed_vino_volume(&letter)) return 0;
    persistent_volume_letter = letter;
    return letter;
}

bool gui_manager_should_autostart_selected(void) {
    return manager_initialized && default_gui_id[0] != '\0'
        && find_manifest(default_gui_id) != NULL
        && volume_metadata_has("SYSTEM/OS.META", "installer_profile=plus");
}

int gui_manager_start_selected_with_fallback(void) {
    if (!gui_manager_should_autostart_selected()) return GUI_MANAGER_ERR_NOT_FOUND;
    if (kstrcmp(default_gui_id, "default") == 0)
        return gui_start_with_fallback("default");
    return gui_start(default_gui_id);
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
    return vfs_read_file(manifest->package_path, &data, &size) && data != NULL
        && size != 0 && package_checksum_matches(manifest, data, size);
}

bool gui_is_installed(const char *id) {
    const struct gui_manifest *manifest = find_manifest(id);
    if (!manager_initialized || manifest == NULL) return false;
    if (pkg_db_find(manifest->package_app_id) != NULL) return true;
    char entry_path[GUI_MAX_PATH_LENGTH];
    struct vfs_metadata entry_metadata;
    return resolve_gui_entry_path(manifest, entry_path, sizeof(entry_path))
        && vfs_stat(entry_path, &entry_metadata)
        && entry_metadata.type == VFS_NODE_FILE;
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

bool gui_session_snapshot(struct gui_session *session) {
    if (!manager_initialized || session == NULL) return false;
    *session = active_gui_session;
    return true;
}

const char *gui_session_state_string(enum gui_session_state state) {
    switch (state) {
        case GUI_SESSION_STOPPED: return "stopped";
        case GUI_SESSION_STARTING: return "starting";
        case GUI_SESSION_RUNNING: return "running";
        case GUI_SESSION_STOPPING: return "stopping";
        case GUI_SESSION_CRASHED: return "crashed";
        case GUI_SESSION_FAILED: return "failed";
        default: return "unknown";
    }
}

int gui_install(const char *id) {
    const struct gui_manifest *manifest = find_manifest(id);
    if (!manager_initialized || manifest == NULL) {
        return GUI_MANAGER_ERR_NOT_FOUND;
    }
    if (manifest->source == GUI_PACKAGE_SOURCE_REMOTE) {
        return GUI_MANAGER_ERR_NOT_IMPLEMENTED;
    }
    if (pkg_db_find(manifest->package_app_id) != NULL) {
        return GUI_MANAGER_ERR_ALREADY_INSTALLED;
    }
    /* The registry is volatile; an on-disk package can be safely refreshed from
       its verified local KPKG when no ownership record survived reboot. */
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

    if (active_gui_process != NULL
        && kstrcmp(active_gui_session.gui_id, manifest->id) == 0) {
        int stop_result = gui_stop(id);
        if (stop_result != GUI_MANAGER_OK) return stop_result;
    }

    if (pkg_db_find(manifest->package_app_id) == NULL) {
        int register_result = install_manifest(manifest, false);
        if (register_result != GUI_MANAGER_OK) return register_result;
    }

    bool was_default = kstrcmp(default_gui_id, manifest->id) == 0
        || selected_gui_is(manifest->id);
    if (was_default) {
        if (!persist_gui_selection(NULL)) return GUI_MANAGER_ERR_IO;
        default_gui_id[0] = '\0';
    }

    int result = installer_uninstall(manifest->package_app_id, false);
    if (result != INSTALLER_OK) {
        if (was_default && persist_gui_selection(manifest)) {
            kstrncpy(default_gui_id, manifest->id, sizeof(default_gui_id) - 1);
            default_gui_id[sizeof(default_gui_id) - 1] = '\0';
        }
        return result == INSTALLER_ERR_NOT_FOUND ? GUI_MANAGER_ERR_NOT_INSTALLED : GUI_MANAGER_ERR_IO;
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
    if (!gui_is_installed(id) || pkg_db_find(manifest->package_app_id) == NULL) {
        return gui_install(id);
    }
    return install_manifest(manifest, true);
}

int gui_start(const char *id) {
    const struct gui_manifest *manifest = find_manifest(id);
    if (manifest == 0) return GUI_MANAGER_ERR_NOT_FOUND;
    if (!gui_is_installed(id)) {
        return GUI_MANAGER_ERR_NOT_INSTALLED;
    }
    if (active_gui_process != 0) {
        if (active_gui_process->state == PROCESS_STATE_ACTIVE)
            return kstrcmp(active_gui_session.gui_id, id) == 0
                ? GUI_MANAGER_OK : GUI_MANAGER_ERR_BUSY;
        int exit_code = active_gui_process->exit_code;
        if (process_wait(active_gui_process, 0, 0)) active_gui_process = 0;
        active_gui_session.state = exit_code == 0 ? GUI_SESSION_STOPPED : GUI_SESSION_CRASHED;
        active_gui_session.last_error = exit_code;
    }
    if (!resolve_gui_entry_path(manifest, active_gui_entry_path,
                                sizeof(active_gui_entry_path))) return GUI_MANAGER_ERR_IO;

    struct process *process = process_create("vino-gui-session");
    if (process == 0) return GUI_MANAGER_ERR_IO;
    gui_session_init(&active_gui_session, id);
    active_gui_session.session_id = next_gui_session_id++;
    if (active_gui_session.session_id == 0)
        active_gui_session.session_id = next_gui_session_id++;
    active_gui_session.process_id = (uint32_t)process->pid;
    active_gui_session.state = GUI_SESSION_STARTING;
    if (thread_create(process, gui_process_entry, active_gui_entry_path,
                      "gui-session") == 0) {
        process_exit(process, -ENOMEM);
        (void)process_wait(process, 0, 0);
        active_gui_session.state = GUI_SESSION_FAILED;
        active_gui_session.last_error = -ENOMEM;
        return GUI_MANAGER_ERR_IO;
    }
    active_gui_process = process;
    active_gui_session.state = GUI_SESSION_RUNNING;
    log_infof("GUI session %u: started '%s' in process %u",
              active_gui_session.session_id, id, active_gui_session.process_id);

    int exit_code = 0;
    if (!process_wait(process, &exit_code, UINT64_MAX)) {
        active_gui_session.state = GUI_SESSION_FAILED;
        active_gui_session.last_error = -EIO;
        return GUI_MANAGER_ERR_IO;
    }
    active_gui_process = 0;
    active_gui_session.state = exit_code == 0 ? GUI_SESSION_STOPPED : GUI_SESSION_CRASHED;
    active_gui_session.last_error = exit_code;
    bool retry_default = recovery_default_requested && kstrcmp(id, "mvp") == 0;
    if (retry_default) recovery_default_requested = false;
    if (exit_code != 0) {
        log_errorf("GUI session '%s' exited with %d", id, exit_code);
        if (retry_default) return gui_start("default");
        return GUI_MANAGER_ERR_IO;
    }
    log_infof("GUI session '%s' stopped; CLI remains available", id);
    if (retry_default) return gui_start("default");
    return GUI_MANAGER_OK;
}

int gui_start_with_fallback(const char *id) {
    int result = gui_start(id);
    if (id == NULL || kstrcmp(id, "default") != 0
        || result == GUI_MANAGER_OK || !gui_is_installed("mvp")) {
        return result;
    }
    log_errorf("Default GUI failed to start (%s); launching MVP recovery.",
               gui_manager_status_string(result));
    return gui_start("mvp");
}

int gui_stop(const char *id) {
    if (!gui_is_installed(id)) {
        return GUI_MANAGER_ERR_NOT_INSTALLED;
    }
    if (active_gui_process == 0 || kstrcmp(active_gui_session.gui_id, id) != 0)
        return GUI_MANAGER_ERR_NOT_FOUND;
    if (active_gui_process->state == PROCESS_STATE_ACTIVE) {
        active_gui_session.state = GUI_SESSION_STOPPING;
        process_exit(active_gui_process, 0);
    }
    int exit_code = 0;
    if (!process_wait(active_gui_process, &exit_code, UINT64_MAX))
        return GUI_MANAGER_ERR_IO;
    active_gui_process = 0;
    active_gui_session.state = GUI_SESSION_STOPPED;
    active_gui_session.last_error = 0;
    return GUI_MANAGER_OK;
}

int gui_restart(const char *id) {
    if (!gui_is_installed(id)) {
        return GUI_MANAGER_ERR_NOT_INSTALLED;
    }
    if (active_gui_process != 0 && kstrcmp(active_gui_session.gui_id, id) == 0) {
        int stopped = gui_stop(id);
        if (stopped != GUI_MANAGER_OK) return stopped;
    }
    return gui_start_with_fallback(id);
}

int gui_set_default(const char *id) {
    const struct gui_manifest *manifest = find_manifest(id);
    if (manifest == NULL) return GUI_MANAGER_ERR_NOT_FOUND;
    if (!gui_is_installed(id)) {
        return GUI_MANAGER_ERR_NOT_INSTALLED;
    }
    if (!persist_gui_selection(manifest)) return GUI_MANAGER_ERR_IO;
    kstrncpy(default_gui_id, id, sizeof(default_gui_id) - 1);
    default_gui_id[sizeof(default_gui_id) - 1] = '\0';
    return GUI_MANAGER_OK;
}

int gui_manager_request_default_recovery(uint64_t requester_pid) {
    if (!manager_initialized || requester_pid == 0 || active_gui_process == NULL
        || active_gui_process->pid != requester_pid
        || active_gui_process->state != PROCESS_STATE_ACTIVE
        || kstrcmp(active_gui_session.gui_id, "mvp") != 0) {
        return GUI_MANAGER_ERR_BUSY;
    }
    if (!gui_is_installed("default")) return GUI_MANAGER_ERR_NOT_INSTALLED;
    int result = gui_set_default("default");
    if (result != GUI_MANAGER_OK) return result;
    recovery_default_requested = true;
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
        case GUI_MANAGER_ERR_INTEGRITY: return "GUI package SHA-256 verification failed";
        case GUI_MANAGER_ERR_AUTH: return "GUI package signature verification failed";
        case GUI_MANAGER_ERR_BUSY: return "Another GUI session owns the display";
        default: return "Unknown GUI manager status";
    }
}
