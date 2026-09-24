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
    kstrncpy(flow->state.error, message, sizeof(flow->state.error) - 1);
    flow->state.error[sizeof(flow->state.error) - 1] = '\0';
    flow->state.phase = INSTALLER_FLOW_ERROR;
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
    *flow = (struct installer_flow){0};
    installer_state_init(&flow->state);
    flow->profile = &profiles[0];
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
    if (!installer_profile_is_valid(profile)) {
        set_error(flow, "Installer profile violates the Vino OS contract");
        return INSTALLER_FLOW_ERR_INVALID_PROFILE;
    }
    flow->profile = profile;
    flow->selected_gui = NULL;
    flow->selected_disk = NULL;
    flow->selected_partition = NULL;
    kstrncpy(flow->state.selected_profile_id, profile->id,
             sizeof(flow->state.selected_profile_id) - 1);
    flow->state.selected_profile_id[sizeof(flow->state.selected_profile_id) - 1] = '\0';
    flow->state.selected_gui_id[0] = '\0';
    flow->state.error[0] = '\0';
    partition_plan_init(&flow->state.partition_plan);
    install_transaction_init(&flow->state.transaction);
    pending_gui_install_init(&flow->state.pending_gui);
    flow->state.phase = profile->install_gui ? INSTALLER_FLOW_GUI_SELECTION
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
    if (!gui_manifest_is_valid(manifest)) {
        set_error(flow, "Selected GUI manifest is invalid");
        return INSTALLER_FLOW_ERR_GUI_REQUIRED;
    }
    flow->selected_gui = manifest;
    kstrncpy(flow->state.selected_gui_id, manifest->id,
             sizeof(flow->state.selected_gui_id) - 1);
    flow->state.selected_gui_id[sizeof(flow->state.selected_gui_id) - 1] = '\0';
    flow->state.phase = INSTALLER_FLOW_DISK_SELECTION;
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
    struct installer_disk_info disk_info;
    bool found = false;
    for (uint32_t index = 0; index < installer_storage_disk_count(); ++index) {
        if (installer_storage_disk_at(index, &disk_info)
            && disk_info.device == device) {
            found = true;
            break;
        }
    }
    if (!found) {
        set_error(flow, "Selected disk has an unreadable partition table");
        return INSTALLER_FLOW_ERR_DISK_NOT_FOUND;
    }
    flow->selected_disk = device;
    flow->selected_partition = NULL;
    partition_plan_init(&flow->state.partition_plan);
    kstrncpy(flow->state.partition_plan.disk_name, device->name,
             sizeof(flow->state.partition_plan.disk_name) - 1);
    flow->state.partition_plan.disk_name[sizeof(flow->state.partition_plan.disk_name) - 1] = '\0';
    flow->state.partition_plan.disk_block_size = device->block_size;
    flow->state.partition_plan.disk_total_blocks = device->total_blocks;
    flow->state.partition_plan.table_type = disk_info.table_type;
    flow->state.phase = INSTALLER_FLOW_PARTITION_SETUP;
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
    struct installer_partition_info partition_info;
    bool found = false;
    for (uint32_t index = 0; index < installer_storage_partition_count(flow->selected_disk); ++index) {
        if (installer_storage_partition_at(flow->selected_disk, index, &partition_info)
            && partition_info.device == partition) {
            found = true;
            break;
        }
    }
    if (!found) {
        set_error(flow, "Selected partition has invalid geometry");
        return INSTALLER_FLOW_ERR_PARTITION_NOT_FOUND;
    }
    flow->selected_partition = partition;
    kstrncpy(flow->state.partition_plan.selected_partition_name, partition->name,
             sizeof(flow->state.partition_plan.selected_partition_name) - 1);
    flow->state.partition_plan.selected_partition_name[
        sizeof(flow->state.partition_plan.selected_partition_name) - 1] = '\0';
    flow->state.partition_plan.selected_start_lba = partition->start_lba;
    flow->state.partition_plan.selected_block_count = partition->total_blocks;
    flow->state.partition_plan.selected_filesystem =
        kstrcmp(partition_info.filesystem, "FAT32") == 0
            ? PARTITION_PLAN_FILESYSTEM_FAT32 : PARTITION_PLAN_FILESYSTEM_NONE;
    flow->state.partition_plan.operation_count = 1;
    flow->state.partition_plan.operations[0] = (struct partition_plan_operation){
        .type = PARTITION_PLAN_SELECT_EXISTING,
        .partition_index = partition->partition_index,
        .start_lba = partition->start_lba,
        .block_count = partition->total_blocks,
        .filesystem = flow->state.partition_plan.selected_filesystem,
    };
    flow->state.partition_plan.confirmed = false;
    flow->state.phase = INSTALLER_FLOW_CONFIRMATION;
    log_infof("Installer: selected partition=%s start_lba=%lu blocks=%lu",
              partition->name, partition->start_lba, partition->total_blocks);
    return INSTALLER_FLOW_OK;
}

int installer_flow_delete_partition(struct installer_flow *flow, const char *name,
                                    bool confirmed) {
    if (flow == NULL || name == NULL || flow->selected_disk == NULL) {
        return INSTALLER_FLOW_ERR_INVALID_ARGUMENT;
    }
    if (!confirmed || flow->state.phase != INSTALLER_FLOW_PARTITION_SETUP
        || flow->selected_disk->state != BLOCK_DEVICE_ONLINE
        || flow->selected_disk->type == BLOCK_TYPE_PARTITION
        || flow->selected_disk->parent != NULL
        || flow->selected_disk->block_size != BLOCK_DEFAULT_SECTOR_SIZE
        || flow->selected_disk->total_blocks == 0
        || block_device_find_by_name(flow->selected_disk->name) != flow->selected_disk) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }

    struct block_device *partition = block_device_find_by_name(name);
    if (partition == NULL || partition->type != BLOCK_TYPE_PARTITION
        || partition->parent != flow->selected_disk) {
        return INSTALLER_FLOW_ERR_PARTITION_NOT_FOUND;
    }
    bool listed = false;
    uint32_t count = installer_storage_partition_count(flow->selected_disk);
    for (uint32_t index = 0; index < count; ++index) {
        struct installer_partition_info info;
        if (installer_storage_partition_at(flow->selected_disk, index, &info)
            && info.device == partition && info.parent == flow->selected_disk) {
            listed = true;
            break;
        }
    }
    if (!listed || !installer_storage_delete_partition(partition, confirmed)) {
        set_error(flow, "Partition deletion failed; the partition table was not updated");
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }

    partition_plan_init(&flow->state.partition_plan);
    kstrncpy(flow->state.partition_plan.disk_name, flow->selected_disk->name,
             sizeof(flow->state.partition_plan.disk_name) - 1);
    flow->state.partition_plan.disk_name[
        sizeof(flow->state.partition_plan.disk_name) - 1] = '\0';
    flow->state.partition_plan.disk_block_size = flow->selected_disk->block_size;
    flow->state.partition_plan.disk_total_blocks = flow->selected_disk->total_blocks;
    struct installer_disk_info disk_info;
    for (uint32_t index = 0; index < installer_storage_disk_count(); ++index) {
        if (installer_storage_disk_at(index, &disk_info)
            && disk_info.device == flow->selected_disk) {
            flow->state.partition_plan.table_type = disk_info.table_type;
            break;
        }
    }
    flow->selected_partition = NULL;
    flow->state.phase = INSTALLER_FLOW_PARTITION_SETUP;
    return INSTALLER_FLOW_OK;
}

int installer_flow_request_partition_creation(struct installer_flow *flow,
                                              enum partition_table_type table_type,
                                              uint64_t start_lba, uint64_t block_count,
                                              enum partition_plan_filesystem filesystem) {
    if (flow == NULL || flow->selected_disk == NULL
        || flow->state.phase != INSTALLER_FLOW_PARTITION_SETUP
        || (table_type != PARTITION_TABLE_MBR && table_type != PARTITION_TABLE_GPT)
        || (filesystem != PARTITION_PLAN_FILESYSTEM_NONE
            && filesystem != PARTITION_PLAN_FILESYSTEM_FAT32)) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    if (start_lba == 0 && block_count == 0) {
        if (!installer_storage_find_free_extent(flow->selected_disk, table_type,
                INSTALLER_MIN_TARGET_BLOCKS, &start_lba, &block_count)) {
            set_error(flow, "No suitably sized, aligned free partition extent was found");
            return INSTALLER_FLOW_ERR_INVALID_STATE;
        }
    }
    if (start_lba == 0 || block_count == 0
        || start_lba > UINT64_MAX - block_count
        || start_lba + block_count > flow->selected_disk->total_blocks) {
        set_error(flow, "Requested partition range is outside the selected disk");
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    flow->selected_partition = NULL;
    flow->state.partition_plan.table_type = table_type;
    flow->state.partition_plan.selected_partition_name[0] = '\0';
    flow->state.partition_plan.selected_start_lba = start_lba;
    flow->state.partition_plan.selected_block_count = block_count;
    flow->state.partition_plan.selected_filesystem = filesystem;
    flow->state.partition_plan.operation_count = 1;
    flow->state.partition_plan.operations[0] = (struct partition_plan_operation){
        .type = PARTITION_PLAN_CREATE,
        .start_lba = start_lba,
        .block_count = block_count,
        .filesystem = filesystem,
        .bootable = true,
    };
    flow->state.partition_plan.confirmed = false;
    flow->state.phase = INSTALLER_FLOW_CONFIRMATION;
    return INSTALLER_FLOW_OK;
}

int installer_flow_apply_partition_creation(struct installer_flow *flow) {
    if (flow == NULL || flow->selected_disk == NULL
        || !installer_flow_is_valid(flow)
        || flow->state.phase != INSTALLER_FLOW_CONFIRMATION
        || flow->state.partition_plan.operation_count != 1
        || flow->state.partition_plan.operations[0].type != PARTITION_PLAN_CREATE
        || !flow->state.partition_plan.confirmed) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    const struct partition_plan_operation operation = flow->state.partition_plan.operations[0];
    if (!installer_storage_create_partition(flow->selected_disk,
            flow->state.partition_plan.table_type, operation.start_lba,
            operation.block_count, operation.bootable, true)) {
        set_error(flow, "Partition creation failed; no install target was selected");
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }

    struct block_device *created = NULL;
    for (uint32_t index = 0; index < installer_storage_partition_count(flow->selected_disk); ++index) {
        struct installer_partition_info info;
        if (installer_storage_partition_at(flow->selected_disk, index, &info)
            && info.start_lba == operation.start_lba
            && info.block_count == operation.block_count) {
            created = info.device;
            break;
        }
    }
    if (created == NULL
        || (operation.filesystem == PARTITION_PLAN_FILESYSTEM_FAT32
            && !installer_storage_format_partition(created,
                PARTITION_PLAN_FILESYSTEM_FAT32, true))) {
        set_error(flow, "Partition was created but FAT32 formatting failed");
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    int result = installer_flow_select_partition(flow, created->name);
    if (result == INSTALLER_FLOW_OK) flow->state.partition_plan.confirmed = true;
    return result;
}

int installer_flow_format_selected_partition(struct installer_flow *flow, bool confirmed) {
    if (flow == NULL || flow->selected_partition == NULL
        || flow->state.phase != INSTALLER_FLOW_CONFIRMATION
        || !confirmed
        || !installer_storage_format_partition(flow->selected_partition,
            PARTITION_PLAN_FILESYSTEM_FAT32, confirmed)) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    flow->state.partition_plan.selected_filesystem = PARTITION_PLAN_FILESYSTEM_FAT32;
    flow->state.partition_plan.operations[0].filesystem = PARTITION_PLAN_FILESYSTEM_FAT32;
    flow->state.partition_plan.confirmed = true;
    return INSTALLER_FLOW_OK;
}

bool installer_flow_is_valid(const struct installer_flow *flow) {
    if (flow == NULL || flow->profile == NULL || flow->selected_disk == NULL) {
        return false;
    }
    if (!installer_state_is_valid(&flow->state)
        || !installer_profile_is_valid(flow->profile)
        || kstrcmp(flow->state.selected_profile_id, flow->profile->id) != 0) {
        return false;
    }
    if (flow->profile->install_gui && flow->selected_gui == NULL) {
        return false;
    }
    if (flow->profile->install_gui
        && (flow->state.selected_gui_id[0] == '\0'
            || kstrcmp(flow->state.selected_gui_id, flow->selected_gui->id) != 0
            || !gui_manifest_is_valid(flow->selected_gui))) {
        return false;
    }
    if (flow->selected_disk->state != BLOCK_DEVICE_ONLINE
        || flow->selected_disk->type == BLOCK_TYPE_PARTITION
        || flow->selected_disk->parent != NULL
        || flow->selected_disk->block_size != BLOCK_DEFAULT_SECTOR_SIZE
        || flow->selected_disk->total_blocks == 0
        || block_device_find_by_name(flow->state.partition_plan.disk_name)
            != flow->selected_disk
        || kstrcmp(flow->state.partition_plan.disk_name, flow->selected_disk->name) != 0
        || flow->state.partition_plan.disk_block_size != flow->selected_disk->block_size
        || flow->state.partition_plan.disk_total_blocks != flow->selected_disk->total_blocks) {
        return false;
    }
    if (flow->selected_partition == NULL) {
        return flow->state.phase == INSTALLER_FLOW_CONFIRMATION
            && flow->state.partition_plan.operation_count == 1
            && flow->state.partition_plan.operations[0].type == PARTITION_PLAN_CREATE;
    }
    if (block_device_find_by_name(flow->state.partition_plan.selected_partition_name)
            != flow->selected_partition
        || flow->selected_partition->state != BLOCK_DEVICE_ONLINE
        || flow->selected_partition->type != BLOCK_TYPE_PARTITION
        || flow->selected_partition->parent != flow->selected_disk
        || flow->selected_partition->block_size != flow->selected_disk->block_size
        || flow->selected_partition->start_lba == 0
        || flow->selected_partition->total_blocks == 0
        || flow->selected_partition->start_lba
            > UINT64_MAX - flow->selected_partition->total_blocks
        || flow->selected_partition->start_lba + flow->selected_partition->total_blocks
            > flow->selected_disk->total_blocks) {
        return false;
    }
    return flow->state.partition_plan.selected_start_lba == flow->selected_partition->start_lba
        && flow->state.partition_plan.selected_block_count == flow->selected_partition->total_blocks
        && kstrcmp(flow->state.partition_plan.selected_partition_name,
                   flow->selected_partition->name) == 0;
}

int installer_flow_next(struct installer_flow *flow) {
    if (flow == NULL) {
        return INSTALLER_FLOW_ERR_INVALID_ARGUMENT;
    }
    switch (flow->state.phase) {
        case INSTALLER_FLOW_WELCOME:
            flow->state.phase = INSTALLER_FLOW_PROFILE_SELECTION;
            return INSTALLER_FLOW_OK;
        case INSTALLER_FLOW_PROFILE_SELECTION:
            flow->state.phase = flow->profile != NULL && flow->profile->install_gui
                ? INSTALLER_FLOW_GUI_SELECTION : INSTALLER_FLOW_DISK_SELECTION;
            return INSTALLER_FLOW_OK;
        case INSTALLER_FLOW_GUI_SELECTION:
            if (flow->selected_gui == NULL) {
                set_error(flow, "Vino Plus requires a GUI selection");
                return INSTALLER_FLOW_ERR_GUI_REQUIRED;
            }
            flow->state.phase = INSTALLER_FLOW_DISK_SELECTION;
            return INSTALLER_FLOW_OK;
        case INSTALLER_FLOW_DISK_SELECTION:
            if (flow->selected_disk == NULL) {
                set_error(flow, "Select a disk before continuing");
                return INSTALLER_FLOW_ERR_DISK_NOT_FOUND;
            }
            flow->state.phase = INSTALLER_FLOW_PARTITION_SETUP;
            return INSTALLER_FLOW_OK;
        case INSTALLER_FLOW_PARTITION_SETUP:
            if (flow->selected_partition == NULL) {
                set_error(flow, "Select a partition before continuing");
                return INSTALLER_FLOW_ERR_PARTITION_NOT_FOUND;
            }
            flow->state.phase = INSTALLER_FLOW_CONFIRMATION;
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
    switch (flow->state.phase) {
        case INSTALLER_FLOW_PROFILE_SELECTION: flow->state.phase = INSTALLER_FLOW_WELCOME; break;
        case INSTALLER_FLOW_GUI_SELECTION: flow->state.phase = INSTALLER_FLOW_PROFILE_SELECTION; break;
        case INSTALLER_FLOW_DISK_SELECTION:
            flow->state.phase = flow->profile != NULL && flow->profile->install_gui
                ? INSTALLER_FLOW_GUI_SELECTION : INSTALLER_FLOW_PROFILE_SELECTION;
            break;
        case INSTALLER_FLOW_PARTITION_SETUP: flow->state.phase = INSTALLER_FLOW_DISK_SELECTION; break;
        case INSTALLER_FLOW_CONFIRMATION: flow->state.phase = INSTALLER_FLOW_PARTITION_SETUP; break;
        default: return INSTALLER_FLOW_ERR_INVALID_STATE;
    }
    return INSTALLER_FLOW_OK;
}

int installer_flow_execute(struct installer_flow *flow, const struct installer_engine *engine) {
    if (flow == NULL || engine == NULL || !installer_flow_is_valid(flow)
        || flow->state.phase != INSTALLER_FLOW_CONFIRMATION
        || !flow->state.partition_plan.confirmed
        || flow->selected_partition == NULL
        || flow->state.partition_plan.selected_filesystem != PARTITION_PLAN_FILESYSTEM_FAT32
        || engine->install_base_os == NULL || engine->install_bootloader == NULL) {
        return INSTALLER_FLOW_ERR_INVALID_STATE;
    }

    flow->state.transaction.state = INSTALL_TRANSACTION_VALIDATED;
    kstrncpy(flow->state.transaction.target_disk_name, flow->selected_disk->name,
             sizeof(flow->state.transaction.target_disk_name) - 1);
    flow->state.transaction.target_disk_name[
        sizeof(flow->state.transaction.target_disk_name) - 1] = '\0';
    kstrncpy(flow->state.transaction.target_partition_name, flow->selected_partition->name,
             sizeof(flow->state.transaction.target_partition_name) - 1);
    flow->state.transaction.target_partition_name[
        sizeof(flow->state.transaction.target_partition_name) - 1] = '\0';
    flow->state.transaction.state = INSTALL_TRANSACTION_APPLYING;
    flow->state.transaction.cli_fallback_ready = false;
    flow->state.phase = INSTALLER_FLOW_INSTALLING_OS;
    int result = engine->install_base_os(flow, engine->context);
    if (result != 0) {
        set_error(flow, "Base Vino OS installation failed; target remains uncommitted");
        flow->state.transaction.state = INSTALL_TRANSACTION_FAILED;
        flow->state.transaction.last_error = result;
        return result;
    }
    flow->state.transaction.base_os_applied = true;

    /* Establish a bootable CLI system before attempting the optional GUI package. */
    flow->state.phase = INSTALLER_FLOW_INSTALLING_BOOTLOADER;
    result = engine->install_bootloader(flow, engine->context);
    if (result != 0) {
        set_error(flow, "Bootloader installation failed; the target is not bootable yet");
        flow->state.transaction.state = INSTALL_TRANSACTION_FAILED;
        flow->state.transaction.last_error = result;
        return result;
    }
    flow->state.transaction.bootloader_applied = true;
    flow->state.transaction.cli_fallback_ready = true;

    if (flow->profile->install_gui) {
        if (flow->selected_gui->source == GUI_PACKAGE_SOURCE_REMOTE) {
            flow->state.phase = INSTALLER_FLOW_INSTALLING_GUI;
            if (engine->install_gui == NULL) {
                set_error(flow, "Remote GUI requires MVP/network handoff");
                flow->state.transaction.state = INSTALL_TRANSACTION_FAILED;
                flow->state.transaction.last_error = INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
                return INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
            }
        }
        else if (engine->install_gui == NULL) {
            set_error(flow, "Local GUI package installation backend is not configured");
            flow->state.transaction.state = INSTALL_TRANSACTION_FAILED;
            flow->state.transaction.last_error = INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
            return INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
        }
        else {
            flow->state.phase = INSTALLER_FLOW_INSTALLING_GUI;
        }
        result = engine->install_gui(flow, flow->selected_gui, engine->context);
        if (result != 0) {
            /* Base OS and Limine stay installed so the machine can use CLI mode. */
            set_error(flow, "GUI installation failed; Vino OS remains bootable in CLI mode");
            flow->state.transaction.state = INSTALL_TRANSACTION_FAILED;
            flow->state.transaction.last_error = result;
            return result;
        }
        flow->state.transaction.gui_applied = true;
    }

    flow->state.transaction.state = INSTALL_TRANSACTION_VERIFYING;
    flow->state.transaction.state = INSTALL_TRANSACTION_COMMITTED;
    flow->state.phase = INSTALLER_FLOW_FINISHED;
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
