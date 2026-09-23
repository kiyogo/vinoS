#ifndef KOS_INSTALLER_CONTRACTS_H
#define KOS_INSTALLER_CONTRACTS_H

#include <stdbool.h>
#include <stdint.h>

#include <kos/block.h>
#include <kos/partition.h>

/* Vino CLI and Vino Plus are installer profiles, never OS editions. */
#define VINO_OS_EDITION_DEFAULT "default"

#define GUI_MAX_ID_LENGTH 32
#define GUI_MAX_NAME_LENGTH 64
#define GUI_MAX_VERSION_LENGTH 32
#define GUI_MAX_PATH_LENGTH 128
#define GUI_MAX_URL_LENGTH 256
#define GUI_MAX_ARCH_LENGTH 16
#define GUI_MAX_CHECKSUM_LENGTH 65
#define GUI_MAX_SIGNATURE_LENGTH 512

#define INSTALLER_FLOW_ERROR_LENGTH 128
#define INSTALLER_MAX_PARTITION_OPERATIONS 16
#define INSTALLER_MAX_REPOSITORY_LENGTH GUI_MAX_URL_LENGTH

enum gui_package_source {
    GUI_PACKAGE_SOURCE_LOCAL_ISO = 0,
    GUI_PACKAGE_SOURCE_REMOTE = 1,
};

/* Installer-visible GUI metadata and future repository compatibility fields. */
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
    char install_root[GUI_MAX_PATH_LENGTH];
    char entry_point[GUI_MAX_PATH_LENGTH];
    char architecture[GUI_MAX_ARCH_LENGTH];
    char min_os_version[GUI_MAX_VERSION_LENGTH];
    char download_url[GUI_MAX_URL_LENGTH];
    char checksum_sha256[GUI_MAX_CHECKSUM_LENGTH];
    char signature[GUI_MAX_SIGNATURE_LENGTH];
};

/* A profile selects installation behavior only; its OS edition is always default. */
struct installer_profile {
    char id[GUI_MAX_ID_LENGTH];
    char display_name[GUI_MAX_NAME_LENGTH];
    bool install_gui;
    char os_edition[GUI_MAX_VERSION_LENGTH];
};

enum installer_flow_state {
    INSTALLER_FLOW_WELCOME = 0,
    INSTALLER_FLOW_PROFILE_SELECTION,
    INSTALLER_FLOW_GUI_SELECTION,
    INSTALLER_FLOW_DISK_SELECTION,
    INSTALLER_FLOW_PARTITION_SETUP,
    INSTALLER_FLOW_CONFIRMATION,
    INSTALLER_FLOW_INSTALLING_OS,
    INSTALLER_FLOW_INSTALLING_GUI,
    INSTALLER_FLOW_INSTALLING_BOOTLOADER,
    INSTALLER_FLOW_FINISHED,
    INSTALLER_FLOW_ERROR,
};

enum installer_flow_status {
    INSTALLER_FLOW_OK = 0,
    INSTALLER_FLOW_ERR_INVALID_ARGUMENT = -1,
    INSTALLER_FLOW_ERR_INVALID_PROFILE = -2,
    INSTALLER_FLOW_ERR_GUI_REQUIRED = -3,
    INSTALLER_FLOW_ERR_GUI_NOT_ALLOWED = -4,
    INSTALLER_FLOW_ERR_DISK_NOT_FOUND = -5,
    INSTALLER_FLOW_ERR_PARTITION_NOT_FOUND = -6,
    INSTALLER_FLOW_ERR_INVALID_STATE = -7,
    INSTALLER_FLOW_ERR_NOT_IMPLEMENTED = -8,
};

enum partition_plan_operation_type {
    PARTITION_PLAN_SELECT_EXISTING = 0,
    PARTITION_PLAN_CREATE,
    PARTITION_PLAN_DELETE,
    PARTITION_PLAN_FORMAT,
};

enum partition_plan_filesystem {
    PARTITION_PLAN_FILESYSTEM_NONE = 0,
    PARTITION_PLAN_FILESYSTEM_FAT32,
};

struct partition_plan_operation {
    enum partition_plan_operation_type type;
    uint32_t partition_index;
    uint64_t start_lba;
    uint64_t block_count;
    enum partition_plan_filesystem filesystem;
    bool bootable;
};

/* A plan is declarative: the UI can build it, but only the engine may apply it. */
struct partition_plan {
    char disk_name[BLOCK_NAME_MAX];
    uint64_t disk_block_size;
    uint64_t disk_total_blocks;
    enum partition_table_type table_type;
    uint32_t operation_count;
    struct partition_plan_operation operations[INSTALLER_MAX_PARTITION_OPERATIONS];
    char selected_partition_name[BLOCK_NAME_MAX];
    uint64_t selected_start_lba;
    uint64_t selected_block_count;
    enum partition_plan_filesystem selected_filesystem;
    bool destructive;
    bool confirmed;
};

enum install_transaction_state {
    INSTALL_TRANSACTION_NEW = 0,
    INSTALL_TRANSACTION_PREPARED,
    INSTALL_TRANSACTION_VALIDATED,
    INSTALL_TRANSACTION_APPLYING,
    INSTALL_TRANSACTION_VERIFYING,
    INSTALL_TRANSACTION_COMMITTED,
    INSTALL_TRANSACTION_ROLLED_BACK,
    INSTALL_TRANSACTION_FAILED,
};

/* Tracks one install attempt and the CLI fallback guarantee. */
struct install_transaction {
    uint64_t transaction_id;
    enum install_transaction_state state;
    char target_disk_name[BLOCK_NAME_MAX];
    char target_partition_name[BLOCK_NAME_MAX];
    char target_mount_path[GUI_MAX_PATH_LENGTH];
    bool base_os_applied;
    bool gui_applied;
    bool bootloader_applied;
    bool cli_fallback_ready;
    int32_t last_error;
    char error[INSTALLER_FLOW_ERROR_LENGTH];
};

enum gui_session_state {
    GUI_SESSION_STOPPED = 0,
    GUI_SESSION_STARTING,
    GUI_SESSION_RUNNING,
    GUI_SESSION_STOPPING,
    GUI_SESSION_CRASHED,
    GUI_SESSION_FAILED,
};

/* Runtime identity for one GUI instance; it does not own kernel devices. */
struct gui_session {
    uint32_t session_id;
    char gui_id[GUI_MAX_ID_LENGTH];
    uint32_t process_id;
    uint32_t display_id;
    enum gui_session_state state;
    uint32_t restart_count;
    int32_t last_error;
    char error[INSTALLER_FLOW_ERROR_LENGTH];
};

enum pending_gui_install_state {
    PENDING_GUI_NONE = 0,
    PENDING_GUI_REQUESTED,
    PENDING_GUI_WAITING_NETWORK,
    PENDING_GUI_DOWNLOADING,
    PENDING_GUI_VERIFYING,
    PENDING_GUI_INSTALLING,
    PENDING_GUI_COMPLETED,
    PENDING_GUI_FAILED,
    PENDING_GUI_CANCELLED,
};

/* Persistable handoff state for a future remote GUI installed by MVP. */
struct pending_gui_install {
    enum pending_gui_install_state state;
    char gui_id[GUI_MAX_ID_LENGTH];
    char repository[INSTALLER_MAX_REPOSITORY_LENGTH];
    char version[GUI_MAX_VERSION_LENGTH];
    char checksum_sha256[GUI_MAX_CHECKSUM_LENGTH];
    char signature[GUI_MAX_SIGNATURE_LENGTH];
    uint32_t retry_count;
    int32_t last_error;
    char error[INSTALLER_FLOW_ERROR_LENGTH];
};

/* Canonical installer state used by the flow and future persistence layer. */
struct installer_state {
    enum installer_flow_state phase;
    char selected_profile_id[GUI_MAX_ID_LENGTH];
    char selected_gui_id[GUI_MAX_ID_LENGTH];
    struct partition_plan partition_plan;
    struct install_transaction transaction;
    struct pending_gui_install pending_gui;
    char error[INSTALLER_FLOW_ERROR_LENGTH];
};

void installer_profile_init(struct installer_profile *profile, const char *id,
                            const char *display_name, bool install_gui);
bool installer_profile_is_valid(const struct installer_profile *profile);
bool gui_manifest_is_valid(const struct gui_manifest *manifest);

void partition_plan_init(struct partition_plan *plan);
bool partition_plan_is_valid(const struct partition_plan *plan);

void install_transaction_init(struct install_transaction *transaction);
bool install_transaction_is_terminal(const struct install_transaction *transaction);

void gui_session_init(struct gui_session *session, const char *gui_id);
bool gui_session_is_valid(const struct gui_session *session);

void pending_gui_install_init(struct pending_gui_install *pending);
bool pending_gui_install_is_valid(const struct pending_gui_install *pending);

void installer_state_init(struct installer_state *state);
bool installer_state_is_valid(const struct installer_state *state);

bool installer_contract_self_test(void);

#endif /* KOS_INSTALLER_CONTRACTS_H */
