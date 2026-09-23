#include <stdbool.h>
#include <stdint.h>

#include <kos/installer_flow.h>
#include <kos/log.h>
#include <kos/memory.h>

static const struct installer_profile profiles[] = {
    {
        .id = "cli",
        .display_name = "Vino CLI",
        .install_gui = false,
        .os_edition = "default",
    },
    {
        .id = "plus",
        .display_name = "Vino Plus",
        .install_gui = true,
        .os_edition = "default",
    },
};

static bool strings_equal(const char *left, const char *right) {
    return left != NULL && right != NULL && kstrcmp(left, right) == 0;
}

static void set_error(struct installer_flow *flow, const char *message) {
    if (flow == NULL) {
        return;
    }
    kstrncpy(flow->error, message, sizeof(flow->error) - 1);
    flow->error[sizeof(flow->error) - 1] = '\0';
    flow->state = INSTALLER_FLOW_ERROR;
}

uint32_t installer_profile_count(void) {
    return (uint32_t)(sizeof(profiles) / sizeof(profiles[0]));
}

const struct installer_profile *installer_profile_at(uint32_t index) {
    return index < installer_profile_count() ? &profiles[index] : NULL;
}

const struct installer_profile *installer_profile_find(const char *id) {
    for (uint32_t index = 0; index < installer_profile_count(); ++index) {
        if (strings_equal(profiles[index].id, id)) {
            return &profiles[index];
        }
    }
    return NULL;
}

bool installer_flow_init(struct installer_flow *flow) {
    if (flow == NULL) {
        return false;
    }
    *flow = (struct installer_flow){
        .state = INSTALLER_FLOW_PROFILE_SELECTION,
        .profile = &profiles[0],
    };
    return true;
}

int installer_flow_select_profile(struct installer_flow *flow, const char *id) {
    if (flow == NULL) {
        return INSTALLER_FLOW_ERR_INVALID_ARGUMENT;
    }
    const struct installer_profile *profile = installer_profile_find(id);
    if (profile == NULL) {
        set_error(flow, "Unknown installer profile");
        return INSTALLER_FLOW_ERR_INVALID_PROFILE;
    }
    flow->profile = profile;
    flow->selected_gui = NULL;
    flow->selected_disk = NULL;
    flow->selected_partition = NULL;
    flow->error[0] = '\0';
    flow->state = profile->install_gui ? INSTALLER_FLOW_GUI_SELECTION
                                       : INSTALLER_FLOW_DISK_SELECTION;
    log_infof("Installer: selected profile=%s, os_edition=%s, install_gui=%s",
              profile->id, profile->os_edition, profile->install_gui ? "true" : "false");
    return INSTALLER_FLOW_OK;
}

int installer_flow_select_gui(struct installer_flow *flow, const char *id) {
    if (flow == NULL || flow->profile == NULL) {
        return INSTALLER_FLOW_ERR_INVALID_ARGUMENT;
    }
    if (!flow->profile->install_gui) {
        set_error(flow, "Vino CLI does not select a GUI");
        return INSTALLER_FLOW_ERR_GUI_NOT_ALLOWED;
    }
    const struct gui_manifest *manifest = gui_manifest_find(id);
    if (manifest == NULL || !manifest->installer_visible) {
        set_error(flow, "Selected GUI is not available in the installer");
        return INSTALLER_FLOW_ERR_GUI_REQUIRED;
    }
    flow->selected_gui = manifest;
    flow->state = INSTALLER_FLOW_DISK_SELECTION;
    log_infof("Installer: selected gui=%s, source=%s",
              manifest->id,
              manifest->source == GUI_PACKAGE_SOURCE_LOCAL_ISO ? "iso" : "network");
    return INSTALLER_FLOW_OK;
}

int installer_flow_select_disk(struct installer_flow *flow, const char *name) {
    if (flow == NULL || name == NULL || flow->profile == NULL) {
        return INSTALLER_FLOW_ERR_INVALID_ARGUMENT;
    }
    struct block_device *device = block_device_find_by_name(name);
    if (device == NULL || device->parent != NULL) {
        set_error(flow, "Install target disk was not found");
        return INSTALLER_FLOW_ERR_DISK_NOT_FOUND;
    }
    flow->selected_disk = device;
    flow->selected_partition = NULL;
    flow->state = INSTALLER_FLOW_PARTITION_SETUP;
    log_infof("Installer: selected disk=%s capacity=%lu bytes",
              device->name, device->capacity_bytes);
    return INSTALLER_FLOW_OK;
}

int installer_flow_select_partition(struct installer_flow *flow, const char *name) {
    if (flow == NULL || name == NULL || flow->selected_disk == NULL) {
        return INSTALLER_FLOW_ERR_INVALID_ARGUMENT;
    }
    struct block_device *partition = block_device_find_by_name(name);
    if (partition == NULL || partition->parent != flow->selected_disk) {
        set_error(flow, "Install target partition was not found on the selected disk");
        return INSTALLER_FLOW_ERR_PARTITION_NOT_FOUND;
    }
    flow->selected_partition = partition;
    flow->state = INSTALLER_FLOW_CONFIRMATION;
    log_infof("Installer: selected partition=%s start_lba=%lu blocks=%lu",
              partition->name, partition->start_lba, partition->total_blocks);
    return INSTALLER_FLOW_OK;
}

bool installer_flow_is_valid(const struct installer_flow *flow) {
    if (flow == NULL || flow->profile == NULL || flow->selected_disk == NULL
        || flow->selected_partition == NULL) {
        return false;
    }
    if (flow->profile->install_gui && flow->selected_gui == NULL) {
        return false;
    }
    return flow->selected_partition->parent == flow->selected_disk
        && flow->selected_partition->total_blocks != 0;
}

int installer_flow_next(struct installer_flow *flow) {
    if (flow == NULL) {
        return INSTALLER_FLOW_ERR_INVALID_ARGUMENT;
    }
    switch (flow->state) {
        case INSTALLER_FLOW_WELCOME:
            flow->state = INSTALLER_FLOW_PROFILE_SELECTION;
            return INSTALLER_FLOW_OK;
        case INSTALLER_FLOW_PROFILE_SELECTION:
            flow->state = flow->profile != NULL && flow->profile->install_gui
                ? INSTALLER_FLOW_GUI_SELECTION : INSTALLER_FLOW_DISK_SELECTION;
            return INSTALLER_FLOW_OK;
        case INSTALLER_FLOW_GUI_SELECTION:
            if (flow->selected_gui == NULL) {
                set_error(flow, "Vino Plus requires a GUI selection");
                return INSTALLER_FLOW_ERR_GUI_REQUIRED;
            }
            flow->state = INSTALLER_FLOW_DISK_SELECTION;
            return INSTALLER_FLOW_OK;
        case INSTALLER_FLOW_DISK_SELECTION:
            if (flow->selected_disk == NULL) {
                set_error(flow, "Select a disk before continuing");
                return INSTALLER_FLOW_ERR_DISK_NOT_FOUND;
            }
            flow->state = INSTALLER_FLOW_PARTITION_SETUP;
            return INSTALLER_FLOW_OK;
        case INSTALLER_FLOW_PARTITION_SETUP:
            if (flow->selected_partition == NULL) {
                set_error(flow, "Select a partition before continuing");
                return INSTALLER_FLOW_ERR_PARTITION_NOT_FOUND;
            }
            flow->state = INSTALLER_FLOW_CONFIRMATION;
            return INSTALLER_FLOW_OK;
        case INSTALLER_FLOW_CONFIRMATION:
            return installer_flow_is_valid(flow) ? INSTALLER_FLOW_OK : INSTALLER_FLOW_ERR_INVALID_STATE;
        default:
            return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
}

int installer_flow_back(struct installer_flow *flow) {
    if (flow == NULL) {
        return INSTALLER_FLOW_ERR_INVALID_ARGUMENT;
    }
    switch (flow->state) {
        case INSTALLER_FLOW_PROFILE_SELECTION: flow->state = INSTALLER_FLOW_WELCOME; break;
        case INSTALLER_FLOW_GUI_SELECTION: flow->state = INSTALLER_FLOW_PROFILE_SELECTION; break;
        case INSTALLER_FLOW_DISK_SELECTION:
            flow->state = flow->profile != NULL && flow->profile->install_gui
                ? INSTALLER_FLOW_GUI_SELECTION : INSTALLER_FLOW_PROFILE_SELECTION;
            break;
        case INSTALLER_FLOW_PARTITION_SETUP: flow->state = INSTALLER_FLOW_DISK_SELECTION; break;
        case INSTALLER_FLOW_CONFIRMATION: flow->state = INSTALLER_FLOW_PARTITION_SETUP; break;
        default: return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    return INSTALLER_FLOW_OK;
}

int installer_flow_execute(struct installer_flow *flow, const struct installer_engine *engine) {
    if (flow == NULL || engine == NULL || !installer_flow_is_valid(flow)
        || engine->install_base_os == NULL || engine->install_bootloader == NULL) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }

    flow->state = INSTALLER_FLOW_INSTALLING_OS;
    int result = engine->install_base_os(flow, engine->context);
    if (result != 0) {
        set_error(flow, "Base Vino OS installation failed; target remains uncommitted");
        return result;
    }

    if (flow->profile->install_gui) {
        if (flow->selected_gui->source == GUI_PACKAGE_SOURCE_REMOTE) {
            flow->state = INSTALLER_FLOW_INSTALLING_GUI;
            if (engine->install_gui == NULL) {
                set_error(flow, "Remote GUI requires MVP/network handoff");
                return INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
            }
        }
        else if (engine->install_gui == NULL) {
            set_error(flow, "Local GUI package installation backend is not configured");
            return INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
        }
        else {
            flow->state = INSTALLER_FLOW_INSTALLING_GUI;
        }
        result = engine->install_gui(flow, flow->selected_gui, engine->context);
        if (result != 0) {
            /* Base OS is deliberately left bootable in CLI mode. */
            set_error(flow, "GUI installation failed; Vino OS can fall back to CLI");
            return result;
        }
    }

    flow->state = INSTALLER_FLOW_INSTALLING_BOOTLOADER;
    result = engine->install_bootloader(flow, engine->context);
    if (result != 0) {
        set_error(flow, "Bootloader installation failed");
        return result;
    }
    flow->state = INSTALLER_FLOW_FINISHED;
    return INSTALLER_FLOW_OK;
}

const char *installer_flow_state_name(enum installer_flow_state state) {
    switch (state) {
        case INSTALLER_FLOW_WELCOME: return "WELCOME";
        case INSTALLER_FLOW_PROFILE_SELECTION: return "PROFILE_SELECTION";
        case INSTALLER_FLOW_GUI_SELECTION: return "GUI_SELECTION";
        case INSTALLER_FLOW_DISK_SELECTION: return "DISK_SELECTION";
        case INSTALLER_FLOW_PARTITION_SETUP: return "PARTITION_SETUP";
        case INSTALLER_FLOW_CONFIRMATION: return "CONFIRMATION";
        case INSTALLER_FLOW_INSTALLING_OS: return "INSTALLING_OS";
        case INSTALLER_FLOW_INSTALLING_GUI: return "INSTALLING_GUI";
        case INSTALLER_FLOW_INSTALLING_BOOTLOADER: return "INSTALLING_BOOTLOADER";
        case INSTALLER_FLOW_FINISHED: return "FINISHED";
        case INSTALLER_FLOW_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char *installer_flow_status_string(int status) {
    switch (status) {
        case INSTALLER_FLOW_OK: return "OK";
        case INSTALLER_FLOW_ERR_INVALID_ARGUMENT: return "Invalid argument";
        case INSTALLER_FLOW_ERR_INVALID_PROFILE: return "Invalid installer profile";
        case INSTALLER_FLOW_ERR_GUI_REQUIRED: return "A GUI selection is required";
        case INSTALLER_FLOW_ERR_GUI_NOT_ALLOWED: return "This profile does not accept a GUI";
        case INSTALLER_FLOW_ERR_DISK_NOT_FOUND: return "Disk not found";
        case INSTALLER_FLOW_ERR_PARTITION_NOT_FOUND: return "Partition not found";
        case INSTALLER_FLOW_ERR_INVALID_STATE: return "Invalid installer state";
        case INSTALLER_FLOW_ERR_NOT_IMPLEMENTED: return "Installer backend is not implemented";
        default: return "Unknown installer flow status";
    }
}
