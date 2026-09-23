#ifndef KOS_UAPI_INSTALLER_H
#define KOS_UAPI_INSTALLER_H

#include <stdint.h>

#define KOS_INSTALLER_ABI_VERSION 1u
#define KOS_INSTALLER_TEXT_MAX 128u
#define KOS_INSTALLER_NAME_MAX 32u
#define KOS_INSTALLER_MAX_DISKS 8u
#define KOS_INSTALLER_MAX_PARTITIONS 16u

enum kos_installer_action {
    KOS_INSTALLER_START = 1,
    KOS_INSTALLER_CLOSE,
    KOS_INSTALLER_SELECT_PROFILE,
    KOS_INSTALLER_SELECT_GUI,
    KOS_INSTALLER_SELECT_DISK,
    KOS_INSTALLER_SELECT_PARTITION,
    KOS_INSTALLER_REQUEST_PARTITION,
    KOS_INSTALLER_CONFIRM_TARGET,
    KOS_INSTALLER_FORMAT_TARGET,
    KOS_INSTALLER_EXECUTE,
    KOS_INSTALLER_BACK,
    KOS_INSTALLER_REBOOT,
};

enum kos_installer_phase {
    KOS_INSTALLER_PHASE_WELCOME = 0,
    KOS_INSTALLER_PHASE_PROFILE_SELECTION,
    KOS_INSTALLER_PHASE_GUI_SELECTION,
    KOS_INSTALLER_PHASE_DISK_SELECTION,
    KOS_INSTALLER_PHASE_PARTITION_SETUP,
    KOS_INSTALLER_PHASE_CONFIRMATION,
    KOS_INSTALLER_PHASE_INSTALLING_OS,
    KOS_INSTALLER_PHASE_INSTALLING_GUI,
    KOS_INSTALLER_PHASE_INSTALLING_BOOTLOADER,
    KOS_INSTALLER_PHASE_FINISHED,
    KOS_INSTALLER_PHASE_ERROR,
};

enum kos_installer_partition_table {
    KOS_INSTALLER_TABLE_NONE = 0,
    KOS_INSTALLER_TABLE_MBR = 1,
    KOS_INSTALLER_TABLE_GPT = 2,
};

enum kos_installer_filesystem {
    KOS_INSTALLER_FS_NONE = 0,
    KOS_INSTALLER_FS_FAT32 = 1,
};

struct kos_installer_action_request {
    uint32_t abi_version;
    uint32_t action;
    uint32_t confirm;
    uint32_t table_type;
    uint32_t filesystem;
    uint32_t reserved;
    uint64_t start_lba;
    uint64_t block_count;
    char value[KOS_INSTALLER_NAME_MAX];
};

struct kos_installer_disk {
    char name[KOS_INSTALLER_NAME_MAX];
    uint64_t capacity_bytes;
    uint64_t total_blocks;
    uint64_t block_size;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint32_t table_type;
    uint32_t partition_count;
};

struct kos_installer_partition {
    char name[KOS_INSTALLER_NAME_MAX];
    char filesystem[16];
    uint64_t start_lba;
    uint64_t block_count;
    uint64_t capacity_bytes;
    uint32_t is_efi_system_partition;
    uint32_t reserved;
};

struct kos_installer_state {
    uint32_t abi_version;
    uint32_t phase;
    int32_t result;
    uint32_t disk_count;
    uint32_t partition_count;
    uint32_t selected_filesystem;
    uint32_t transaction_flags;
    uint32_t target_confirmed;
    uint32_t reserved;
    char selected_profile[KOS_INSTALLER_NAME_MAX];
    char selected_gui[KOS_INSTALLER_NAME_MAX];
    char selected_disk[KOS_INSTALLER_NAME_MAX];
    char selected_partition[KOS_INSTALLER_NAME_MAX];
    char error[KOS_INSTALLER_TEXT_MAX];
    struct kos_installer_disk disks[KOS_INSTALLER_MAX_DISKS];
    struct kos_installer_partition partitions[KOS_INSTALLER_MAX_PARTITIONS];
};

enum kos_installer_transaction_flag {
    KOS_INSTALLER_TX_BASE_OS = 1u << 0,
    KOS_INSTALLER_TX_GUI = 1u << 1,
    KOS_INSTALLER_TX_BOOTLOADER = 1u << 2,
    KOS_INSTALLER_TX_CLI_FALLBACK = 1u << 3,
};

#endif /* KOS_UAPI_INSTALLER_H */
