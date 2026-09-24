#include <stdbool.h>
#include <stdint.h>

#include <kos/gui_repository.h>
#include <kos/installer_contracts.h>
#include <kos/memory.h>

static bool has_text(const char *value) {
    return value != NULL && value[0] != '\0';
}

static bool is_repository_id(const char *value) {
    if (!has_text(value)) return false;
    for (uint64_t index = 0; value[index] != '\0'; ++index) {
        char character = value[index];
        if (!((character >= 'a' && character <= 'z')
              || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9')
              || character == '-' || character == '_' || character == '.')) {
            return false;
        }
    }
    return true;
}

static bool is_sha256_hex(const char *value) {
    if (value == NULL) return false;
    for (uint32_t index = 0; index < 64; ++index) {
        char character = value[index];
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f')
              || (character >= 'A' && character <= 'F'))) return false;
    }
    return value[64] == '\0';
}

static void copy_text(char *destination, uint64_t capacity, const char *source) {
    if (destination == NULL || capacity == 0) return;
    destination[0] = '\0';
    if (source == NULL) return;
    kstrncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
}

void installer_profile_init(struct installer_profile *profile, const char *id,
                            const char *display_name, bool install_gui) {
    if (profile == NULL) return;
    *profile = (struct installer_profile){0};
    copy_text(profile->id, sizeof(profile->id), id);
    copy_text(profile->display_name, sizeof(profile->display_name), display_name);
    profile->install_gui = install_gui;
    copy_text(profile->os_edition, sizeof(profile->os_edition), VINO_OS_EDITION_DEFAULT);
}

bool installer_profile_is_valid(const struct installer_profile *profile) {
    return profile != NULL
        && has_text(profile->id)
        && has_text(profile->display_name)
        && kstrcmp(profile->os_edition, VINO_OS_EDITION_DEFAULT) == 0;
}

bool gui_manifest_is_valid(const struct gui_manifest *manifest) {
    if (manifest == NULL || !has_text(manifest->id) || !has_text(manifest->name)
        || !has_text(manifest->version) || !has_text(manifest->package_app_id)
        || !has_text(manifest->install_root) || !has_text(manifest->entry_point)
        || !has_text(manifest->architecture)) {
        return false;
    }
    if (manifest->source == GUI_PACKAGE_SOURCE_LOCAL_ISO) {
        return has_text(manifest->package_path) && !manifest->requires_network
            && manifest->repository_id[0] == '\0';
    }
    if (manifest->source == GUI_PACKAGE_SOURCE_REMOTE) {
        return manifest->requires_network && manifest->package_path[0] == '\0'
            && is_repository_id(manifest->repository_id)
            && is_sha256_hex(manifest->checksum_sha256)
            && has_text(manifest->signature);
    }
    return false;
}

void partition_plan_init(struct partition_plan *plan) {
    if (plan == NULL) return;
    *plan = (struct partition_plan){0};
    plan->selected_filesystem = PARTITION_PLAN_FILESYSTEM_NONE;
}

bool partition_plan_is_valid(const struct partition_plan *plan) {
    if (plan == NULL || plan->operation_count > INSTALLER_MAX_PARTITION_OPERATIONS) {
        return false;
    }
    if (plan->table_type < PARTITION_TABLE_NONE
        || plan->table_type > PARTITION_TABLE_GPT
        || plan->selected_filesystem < PARTITION_PLAN_FILESYSTEM_NONE
        || plan->selected_filesystem > PARTITION_PLAN_FILESYSTEM_FAT32) {
        return false;
    }
    if (plan->disk_name[0] == '\0') {
        return plan->operation_count == 0 && plan->selected_partition_name[0] == '\0';
    }
    if (plan->disk_block_size == 0 || plan->disk_total_blocks == 0) return false;
    for (uint32_t index = 0; index < plan->operation_count; ++index) {
        const struct partition_plan_operation *operation = &plan->operations[index];
        if (operation->type < PARTITION_PLAN_SELECT_EXISTING
            || operation->type > PARTITION_PLAN_FORMAT
            || operation->filesystem < PARTITION_PLAN_FILESYSTEM_NONE
            || operation->filesystem > PARTITION_PLAN_FILESYSTEM_FAT32) {
            return false;
        }
        if (operation->block_count == 0
            || operation->start_lba > UINT64_MAX - operation->block_count
            || operation->start_lba + operation->block_count > plan->disk_total_blocks) {
            return false;
        }
        if (operation->type == PARTITION_PLAN_FORMAT
            && operation->filesystem == PARTITION_PLAN_FILESYSTEM_NONE) {
            return false;
        }
    }
    bool pending_create = plan->operation_count == 1
        && plan->operations[0].type == PARTITION_PLAN_CREATE;
    if (pending_create
        && (plan->selected_start_lba != plan->operations[0].start_lba
            || plan->selected_block_count != plan->operations[0].block_count
            || plan->selected_filesystem != plan->operations[0].filesystem)) {
        return false;
    }
    if (plan->selected_partition_name[0] == '\0') {
        if (!pending_create
            && (plan->selected_start_lba != 0 || plan->selected_block_count != 0
                || plan->selected_filesystem != PARTITION_PLAN_FILESYSTEM_NONE)) {
            return false;
        }
        if (pending_create && (plan->selected_start_lba == 0
                               || plan->selected_block_count == 0)) {
            return false;
        }
    }
    if (plan->selected_partition_name[0] != '\0'
        && (plan->selected_block_count == 0
            || plan->selected_start_lba > UINT64_MAX - plan->selected_block_count
            || plan->selected_start_lba + plan->selected_block_count > plan->disk_total_blocks)) {
        return false;
    }
    if (plan->selected_filesystem != PARTITION_PLAN_FILESYSTEM_NONE
        && plan->selected_partition_name[0] == '\0' && !pending_create) {
        return false;
    }
    return true;
}

void install_transaction_init(struct install_transaction *transaction) {
    if (transaction == NULL) return;
    *transaction = (struct install_transaction){
        .state = INSTALL_TRANSACTION_NEW,
    };
}

bool install_transaction_is_terminal(const struct install_transaction *transaction) {
    if (transaction == NULL) return false;
    return transaction->state == INSTALL_TRANSACTION_COMMITTED
        || transaction->state == INSTALL_TRANSACTION_ROLLED_BACK
        || transaction->state == INSTALL_TRANSACTION_FAILED;
}

void gui_session_init(struct gui_session *session, const char *gui_id) {
    if (session == NULL) return;
    *session = (struct gui_session){
        .state = GUI_SESSION_STOPPED,
    };
    copy_text(session->gui_id, sizeof(session->gui_id), gui_id);
}

bool gui_session_is_valid(const struct gui_session *session) {
    if (session == NULL || session->state < GUI_SESSION_STOPPED
        || session->state > GUI_SESSION_FAILED) return false;
    if (session->state == GUI_SESSION_STOPPED) return true;
    return session->session_id != 0 && has_text(session->gui_id);
}

void pending_gui_install_init(struct pending_gui_install *pending) {
    if (pending == NULL) return;
    *pending = (struct pending_gui_install){
        .state = PENDING_GUI_NONE,
    };
}

bool pending_gui_install_is_valid(const struct pending_gui_install *pending) {
    if (pending == NULL || pending->state < PENDING_GUI_NONE
        || pending->state > PENDING_GUI_CANCELLED) return false;
    if (pending->state == PENDING_GUI_NONE) {
        return pending->gui_id[0] == '\0' && pending->repository_id[0] == '\0';
    }
    if (!has_text(pending->gui_id)
        || (pending->repository_id[0] != '\0'
            && !is_repository_id(pending->repository_id))) return false;
    if (pending->state >= PENDING_GUI_REQUESTED
        && pending->state <= PENDING_GUI_COMPLETED) {
        return is_repository_id(pending->repository_id);
    }
    return true;
}

void installer_state_init(struct installer_state *state) {
    if (state == NULL) return;
    *state = (struct installer_state){
        .phase = INSTALLER_FLOW_PROFILE_SELECTION,
    };
    copy_text(state->selected_profile_id, sizeof(state->selected_profile_id), "cli");
    partition_plan_init(&state->partition_plan);
    install_transaction_init(&state->transaction);
    pending_gui_install_init(&state->pending_gui);
}

bool installer_state_is_valid(const struct installer_state *state) {
    if (state == NULL || state->phase < INSTALLER_FLOW_WELCOME
        || state->phase > INSTALLER_FLOW_ERROR
        || state->selected_profile_id[0] == '\0'
        || state->transaction.state < INSTALL_TRANSACTION_NEW
        || state->transaction.state > INSTALL_TRANSACTION_FAILED
        || !partition_plan_is_valid(&state->partition_plan)
        || !pending_gui_install_is_valid(&state->pending_gui)) {
        return false;
    }
    return true;
}

bool installer_contract_self_test(void) {
    struct installer_profile profile;
    installer_profile_init(&profile, "plus", "Vino Plus", true);
    if (!installer_profile_is_valid(&profile)) return false;
    kstrncpy(profile.os_edition, "plus", sizeof(profile.os_edition) - 1);
    if (installer_profile_is_valid(&profile)) return false;

    struct gui_manifest local_gui = {
        .id = "default",
        .name = "Default",
        .version = "1.0.0",
        .source = GUI_PACKAGE_SOURCE_LOCAL_ISO,
        .package_path = "C:/GUI/DEFAULT.KPKG",
        .package_app_id = "com.vino.gui.default",
        .install_root = "D:/Apps/com.vino.gui.default",
        .entry_point = "bin/gui-default.elf",
        .architecture = "x86_64",
    };
    if (!gui_manifest_is_valid(&local_gui)) return false;

    struct gui_manifest remote_gui = {
        .id = "future",
        .name = "Future",
        .version = "1.0.0",
        .source = GUI_PACKAGE_SOURCE_REMOTE,
        .requires_network = true,
        .package_app_id = "com.vino.gui.future",
        .install_root = "system/gui/future",
        .entry_point = "bin/gui-future.elf",
        .architecture = "x86_64",
    };
    if (gui_manifest_is_valid(&remote_gui)) return false;
    kstrncpy(remote_gui.repository_id, "test-repository",
             sizeof(remote_gui.repository_id) - 1);
    kstrncpy(remote_gui.checksum_sha256,
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
             sizeof(remote_gui.checksum_sha256) - 1);
    kstrncpy(remote_gui.signature, "test-fixture",
             sizeof(remote_gui.signature) - 1);
    if (!gui_manifest_is_valid(&remote_gui)) return false;

    struct partition_plan plan;
    partition_plan_init(&plan);
    kstrncpy(plan.disk_name, "disk0", sizeof(plan.disk_name) - 1);
    plan.disk_block_size = 512;
    plan.disk_total_blocks = 4096;
    plan.operation_count = 1;
    plan.operations[0] = (struct partition_plan_operation){
        .type = PARTITION_PLAN_SELECT_EXISTING,
        .start_lba = 128,
        .block_count = 2048,
    };
    if (!partition_plan_is_valid(&plan)) return false;

    plan.operations[0] = (struct partition_plan_operation){
        .type = PARTITION_PLAN_CREATE,
        .start_lba = 2048,
        .block_count = 2048,
        .filesystem = PARTITION_PLAN_FILESYSTEM_FAT32,
    };
    plan.selected_start_lba = 2048;
    plan.selected_block_count = 2048;
    plan.selected_filesystem = PARTITION_PLAN_FILESYSTEM_FAT32;
    if (!partition_plan_is_valid(&plan)) return false;
    plan.operations[0].block_count = 1024;
    if (partition_plan_is_valid(&plan)) return false;

    plan.operations[0] = (struct partition_plan_operation){
        .type = PARTITION_PLAN_SELECT_EXISTING,
        .start_lba = 128,
        .block_count = 2048,
    };
    plan.selected_start_lba = 0;
    plan.selected_block_count = 0;
    plan.selected_filesystem = PARTITION_PLAN_FILESYSTEM_NONE;
    plan.operations[0].start_lba = UINT64_MAX;
    if (partition_plan_is_valid(&plan)) return false;

    struct install_transaction transaction;
    install_transaction_init(&transaction);
    if (transaction.cli_fallback_ready || install_transaction_is_terminal(&transaction)) {
        return false;
    }

    struct gui_session session;
    gui_session_init(&session, "default");
    if (!gui_session_is_valid(&session)) return false;
    session.state = GUI_SESSION_RUNNING;
    if (gui_session_is_valid(&session)) return false;

    struct pending_gui_install pending;
    pending_gui_install_init(&pending);
    if (!pending_gui_install_is_valid(&pending)) return false;
    pending.state = PENDING_GUI_REQUESTED;
    kstrncpy(pending.gui_id, "future", sizeof(pending.gui_id) - 1);
    if (pending_gui_install_is_valid(&pending)) return false;
    kstrncpy(pending.repository_id, "test-repository",
             sizeof(pending.repository_id) - 1);
    if (!pending_gui_install_is_valid(&pending)) return false;

    struct installer_state state;
    installer_state_init(&state);
    return installer_state_is_valid(&state) && gui_remote_interfaces_self_test();
}
