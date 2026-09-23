#include <stdbool.h>
#include <stdint.h>

#include <kos/block_cache.h>
#include <kos/cpu.h>
#include <kos/gui_manager.h>
#include <kos/installer_engine.h>
#include <kos/installer_flow.h>
#include <kos/installer_service.h>
#include <kos/installer_storage.h>
#include <kos/memory.h>
#include <kos/process.h>
#include <kos/uaccess.h>
#include <uapi/kos/installer.h>
#include <uapi/kos/syscall_numbers.h>

static struct installer_flow service_flow;
static uint64_t service_owner_pid;
static bool service_started;
static uint8_t service_busy;

static void copy_field(char *destination, uint64_t capacity, const char *source) {
    if (capacity == 0) return;
    destination[0] = '\0';
    if (source == NULL) return;
    kstrncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
}

static bool has_terminator(const char *text, uint64_t capacity) {
    for (uint64_t index = 0; index < capacity; ++index) {
        if (text[index] == '\0') return true;
    }
    return false;
}

static void serialize_state(struct kos_installer_state *output, int result) {
    *output = (struct kos_installer_state){
        .abi_version = KOS_INSTALLER_ABI_VERSION,
        .phase = (uint32_t)service_flow.state.phase,
        .result = result,
        .selected_filesystem = (uint32_t)service_flow.state.partition_plan.selected_filesystem,
        .target_confirmed = service_flow.state.partition_plan.confirmed ? 1u : 0u,
    };
    copy_field(output->selected_profile, sizeof(output->selected_profile),
               service_flow.state.selected_profile_id);
    copy_field(output->selected_gui, sizeof(output->selected_gui),
               service_flow.state.selected_gui_id);
    copy_field(output->selected_disk, sizeof(output->selected_disk),
               service_flow.state.partition_plan.disk_name);
    copy_field(output->selected_partition, sizeof(output->selected_partition),
               service_flow.state.partition_plan.selected_partition_name);
    copy_field(output->error, sizeof(output->error), service_flow.state.error);
    if (service_flow.state.transaction.base_os_applied)
        output->transaction_flags |= KOS_INSTALLER_TX_BASE_OS;
    if (service_flow.state.transaction.gui_applied)
        output->transaction_flags |= KOS_INSTALLER_TX_GUI;
    if (service_flow.state.transaction.bootloader_applied)
        output->transaction_flags |= KOS_INSTALLER_TX_BOOTLOADER;
    if (service_flow.state.transaction.cli_fallback_ready)
        output->transaction_flags |= KOS_INSTALLER_TX_CLI_FALLBACK;

    uint32_t disks = installer_storage_disk_count();
    if (disks > KOS_INSTALLER_MAX_DISKS) disks = KOS_INSTALLER_MAX_DISKS;
    for (uint32_t index = 0; index < disks; ++index) {
        struct installer_disk_info disk;
        struct partition_table_info table;
        if (!installer_storage_disk_at(index, &disk)
            || !partition_table_inspect(disk.device, &table)) continue;
        struct kos_installer_disk *entry = &output->disks[output->disk_count++];
        copy_field(entry->name, sizeof(entry->name), disk.name);
        entry->capacity_bytes = disk.capacity_bytes;
        entry->total_blocks = disk.total_blocks;
        entry->block_size = disk.block_size;
        entry->first_usable_lba = table.first_usable_lba;
        entry->last_usable_lba = table.last_usable_lba;
        entry->table_type = (uint32_t)table.type;
        entry->partition_count = disk.partition_count;
    }

    if (service_flow.selected_disk != NULL) {
        uint32_t count = installer_storage_partition_count(service_flow.selected_disk);
        if (count > KOS_INSTALLER_MAX_PARTITIONS) count = KOS_INSTALLER_MAX_PARTITIONS;
        for (uint32_t index = 0; index < count; ++index) {
            struct installer_partition_info partition;
            if (!installer_storage_partition_at(service_flow.selected_disk, index, &partition)
                || partition.parent != service_flow.selected_disk) continue;
            struct kos_installer_partition *entry =
                &output->partitions[output->partition_count++];
            copy_field(entry->name, sizeof(entry->name), partition.name);
            copy_field(entry->filesystem, sizeof(entry->filesystem), partition.filesystem);
            entry->start_lba = partition.start_lba;
            entry->block_count = partition.block_count;
            entry->capacity_bytes = partition.capacity_bytes;
            entry->is_efi_system_partition =
                (partition.partition_type_mbr == 0xef
                 || (partition.device->has_type_guid
                     && kmemcmp(partition.device->partition_type_guid,
                         (const uint8_t[]){ 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8,
                             0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e,
                             0xc9, 0x3b }, 16) == 0)) ? 1u : 0u;
        }
    }
}

static int dispatch_action(const struct kos_installer_action_request *request,
                           struct process *caller) {
    switch (request->action) {
        case KOS_INSTALLER_START:
            if (caller == NULL || caller->pid <= 1) return -EPERM;
            if (service_owner_pid != 0 && service_owner_pid != caller->pid) {
                struct process *owner = process_find_by_pid(service_owner_pid);
                if (owner != NULL && owner->state == PROCESS_STATE_ACTIVE) return -EBUSY;
            }
            if (!installer_flow_init(&service_flow)) return -EIO;
            service_owner_pid = caller->pid;
            service_started = true;
            return installer_flow_next(&service_flow);

        case KOS_INSTALLER_CLOSE:
            if (!service_started || caller == NULL || service_owner_pid != caller->pid)
                return -EPERM;
            service_started = false;
            service_owner_pid = 0;
            return 0;

        default:
            if (!service_started || caller == NULL || service_owner_pid != caller->pid)
                return -EPERM;
            break;
    }

    switch (request->action) {
        case KOS_INSTALLER_SELECT_PROFILE:
            return installer_flow_select_profile(&service_flow, request->value);
        case KOS_INSTALLER_SELECT_GUI:
            return installer_flow_select_gui(&service_flow, request->value);
        case KOS_INSTALLER_SELECT_DISK:
            return installer_flow_select_disk(&service_flow, request->value);
        case KOS_INSTALLER_SELECT_PARTITION:
            return installer_flow_select_partition(&service_flow, request->value);
        case KOS_INSTALLER_REQUEST_PARTITION: {
            enum partition_table_type table = (enum partition_table_type)request->table_type;
            enum partition_plan_filesystem filesystem =
                (enum partition_plan_filesystem)request->filesystem;
            return installer_flow_request_partition_creation(&service_flow, table,
                request->start_lba, request->block_count, filesystem);
        }
        case KOS_INSTALLER_CONFIRM_TARGET:
            if (request->confirm == 0) return -EPERM;
            if (service_flow.state.partition_plan.operation_count == 1
                && service_flow.state.partition_plan.operations[0].type == PARTITION_PLAN_CREATE) {
                return installer_flow_apply_partition_creation(&service_flow);
            }
            if (service_flow.selected_partition == NULL
                || service_flow.state.phase != INSTALLER_FLOW_CONFIRMATION) return -EINVAL;
            service_flow.state.partition_plan.confirmed = true;
            return 0;
        case KOS_INSTALLER_FORMAT_TARGET:
            if (request->confirm == 0) return -EPERM;
            return installer_flow_format_selected_partition(&service_flow, true);
        case KOS_INSTALLER_EXECUTE: {
            if (request->confirm == 0 || !service_flow.state.partition_plan.confirmed
                || service_flow.selected_partition == NULL
                || service_flow.state.partition_plan.selected_filesystem
                    != PARTITION_PLAN_FILESYSTEM_FAT32) return -EPERM;
            struct installer_engine engine = {
                .install_base_os = install_base_os,
                .install_gui = install_gui,
                .install_bootloader = install_bootloader,
            };
            int result = installer_flow_execute(&service_flow, &engine);
            if (result == 0 && !block_cache_sync()) {
                service_flow.state.transaction.state = INSTALL_TRANSACTION_FAILED;
                service_flow.state.transaction.last_error = -EIO;
                copy_field(service_flow.state.error, sizeof(service_flow.state.error),
                           "Final disk flush failed; reboot cancelled");
                service_flow.state.phase = INSTALLER_FLOW_ERROR;
                return -EIO;
            }
            return result;
        }
        case KOS_INSTALLER_BACK:
            return installer_flow_back(&service_flow);
        case KOS_INSTALLER_REBOOT:
            if (request->confirm == 0 || service_flow.state.phase != INSTALLER_FLOW_FINISHED)
                return -EPERM;
            cpu_reboot();
        default:
            return -EINVAL;
    }
}

uint64_t installer_service_dispatch(uint64_t user_request, uint64_t user_state) {
    struct kos_installer_action_request request;
    if (!access_ok((const void *)user_request, sizeof(request))
        || !access_ok((const void *)user_state, sizeof(struct kos_installer_state))
        || copy_from_user(&request, (const void *)user_request, sizeof(request)) != 0) {
        return (uint64_t)-EFAULT;
    }
    if (request.abi_version != KOS_INSTALLER_ABI_VERSION
        || !has_terminator(request.value, sizeof(request.value))) return (uint64_t)-EINVAL;

    if (__atomic_test_and_set(&service_busy, __ATOMIC_ACQUIRE)) return (uint64_t)-EBUSY;
    struct process *caller = process_get_current();
    int result = dispatch_action(&request, caller);
    struct kos_installer_state state = { .abi_version = KOS_INSTALLER_ABI_VERSION,
                                         .result = result };
    if (service_started) serialize_state(&state, result);
    if (copy_to_user((void *)user_state, &state, sizeof(state)) != 0) {
        __atomic_clear(&service_busy, __ATOMIC_RELEASE);
        return (uint64_t)-EFAULT;
    }
    __atomic_clear(&service_busy, __ATOMIC_RELEASE);
    return (uint64_t)(int64_t)result;
}
