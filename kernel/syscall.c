#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kos/console.h>
#include <kos/block_cache.h>
#include <kos/cpu.h>
#include <kos/file.h>
#include <kos/gui_manager.h>
#include <kos/gui_service.h>
#include <kos/handle.h>
#include <kos/installer_service.h>
#include <kos/keyboard.h>
#include <kos/log.h>
#include <kos/memory.h>
#include <kos/net.h>
#include <kos/net_device.h>
#include <kos/process.h>
#include <kos/serial.h>
#include <kos/socket.h>
#include <kos/syscall.h>
#include <kos/task.h>
#include <kos/timer.h>
#include <kos/uaccess.h>
#include <kos/vfs.h>
#include <kos/vmm.h>
#include <uapi/kos/gui_control.h>
#include <uapi/kos/system_info.h>
#include <uapi/kos/system_control.h>

static long syscall_copy_path(char path[VFS_MAX_PATH], const char *user_path) {
    long result = strncpy_from_user(path, user_path, VFS_MAX_PATH);
    if (result < 0) return -EFAULT;
    if (result == VFS_MAX_PATH && path[VFS_MAX_PATH - 1] != '\0') return -ENAMETOOLONG;
    path[VFS_MAX_PATH - 1] = '\0';
    return 0;
}

static bool syscall_text_is_terminated(const char *text, uint64_t capacity) {
    if (text == NULL) return false;
    for (uint64_t index = 0; index < capacity; ++index) {
        if (text[index] == '\0') return true;
    }
    return false;
}

static bool syscall_ui_read_only(void) {
    struct process *proc = process_get_current();
    return proc != NULL && (proc->flags & PROCESS_FLAG_UI_READ_ONLY) != 0;
}

static uint64_t syscall_gui_control(uint64_t user_request) {
    struct kos_gui_control_request request;
    if (!access_ok((const void *)user_request, sizeof(request))
        || copy_from_user(&request, (const void *)user_request, sizeof(request)) != 0
        || request.abi_version != KOS_GUI_CONTROL_ABI_VERSION
        || !syscall_text_is_terminated(request.gui_id, sizeof(request.gui_id))) {
        return (uint64_t)-EFAULT;
    }

    int result = GUI_MANAGER_ERR_INVALID_ARGUMENT;
    const char *message = NULL;
    switch (request.action) {
        case KOS_GUI_CONTROL_QUERY:
            result = GUI_MANAGER_OK;
            break;
        case KOS_GUI_CONTROL_INSTALL_LOCAL:
            result = gui_install(request.gui_id);
            break;
        case KOS_GUI_CONTROL_REINSTALL_LOCAL:
            result = gui_reinstall(request.gui_id);
            break;
        case KOS_GUI_CONTROL_SET_DEFAULT:
            result = gui_set_default(request.gui_id);
            break;
        case KOS_GUI_CONTROL_INSTALL_REMOTE:
            result = GUI_MANAGER_ERR_NOT_IMPLEMENTED;
            message = "No trusted remote catalog or pinned KPKG key is configured.";
            break;
        case KOS_GUI_CONTROL_RECOVER_DEFAULT: {
            struct process *caller = process_get_current();
            result = gui_manager_request_default_recovery(
                caller == NULL ? 0 : caller->pid);
            if (result == GUI_MANAGER_OK)
                message = "Default selected; closing MVP and retrying Default.";
            break;
        }
        default:
            result = GUI_MANAGER_ERR_INVALID_ARGUMENT;
            break;
    }

    request.result = result;
    request.installed_mask = 0;
    if (gui_is_installed("default"))
        request.installed_mask |= KOS_GUI_INSTALLED_DEFAULT;
    if (gui_is_installed("mvp"))
        request.installed_mask |= KOS_GUI_INSTALLED_MVP;
    const char *default_id = gui_default_id();
    kstrncpy(request.default_gui_id, default_id == NULL ? "" : default_id,
             sizeof(request.default_gui_id) - 1);
    request.default_gui_id[sizeof(request.default_gui_id) - 1] = '\0';
    if (message == NULL) message = gui_manager_status_string(result);
    kstrncpy(request.message, message, sizeof(request.message) - 1);
    request.message[sizeof(request.message) - 1] = '\0';
    if (copy_to_user((void *)user_request, &request, sizeof(request)) != 0)
        return (uint64_t)-EFAULT;
    return (uint64_t)(int64_t)result;
}

static uint64_t syscall_system_control(uint64_t action, uint64_t confirmation) {
    if (action == KOS_SYSTEM_CONTROL_CONNECT_NETWORK) {
        return net_dhcp_acquire((uint64_t)timer_frequency_hz() * 5)
            ? 0 : (uint64_t)-ETIMEDOUT;
    }
    if (action != KOS_SYSTEM_CONTROL_RESTART
        && action != KOS_SYSTEM_CONTROL_SHUTDOWN) return (uint64_t)-EINVAL;
    if (confirmation != KOS_SYSTEM_CONTROL_CONFIRM_TOKEN) return (uint64_t)-EPERM;
    if (!block_cache_sync()) return (uint64_t)-EIO;
    if (action == KOS_SYSTEM_CONTROL_RESTART) cpu_reboot();
    cpu_halt();
}

static uint64_t syscall_console_write(uint64_t user_address, uint64_t length) {
    char buffer[256];
    uint64_t written = 0;
    while (written < length) {
        uint64_t chunk = length - written;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        if (copy_from_user(buffer, (const void *)(user_address + written), chunk) != 0) {
            return written == 0 ? (uint64_t)-EFAULT : written;
        }
        for (uint64_t index = 0; index < chunk; index++) {
            serial_write_char(buffer[index]);
            if (console_is_initialized()) console_write_char(buffer[index]);
        }
        written += chunk;
    }
    return written;
}

static uint64_t syscall_console_read(uint64_t user_address, uint64_t length) {
    if (!access_ok((const void *)user_address, length)) return (uint64_t)-EFAULT;
    uint64_t read_bytes = 0;
    while (read_bytes < length) {
        char character = 0;
        if (keyboard_read_char(&character) && character != 0) {
            if (copy_to_user((void *)(user_address + read_bytes), &character, 1) != 0) {
                return read_bytes == 0 ? (uint64_t)-EFAULT : read_bytes;
            }
            read_bytes++;
            if (character == '\n') break;
        } else {
            task_yield();
        }
    }
    return read_bytes;
}

struct syscall_getdents_context {
    uint64_t user_address;
    uint64_t capacity;
    uint64_t written;
    int error;
};

static bool syscall_getdents_entry(const struct vfs_dir_entry *entry, void *user_arg) {
    struct syscall_getdents_context *context = user_arg;
    if (entry == 0 || context == 0) return false;
    if (context->capacity - context->written < sizeof(struct kos_dirent)) return false;

    struct kos_dirent dirent = {0};
    dirent.size = entry->size;
    dirent.type = entry->type == VFS_NODE_DIRECTORY ? KOS_STAT_DIRECTORY : KOS_STAT_FILE;
    dirent.name_length = kstrlen(entry->name);
    if (dirent.name_length >= KOS_DIRENT_NAME_MAX) dirent.name_length = KOS_DIRENT_NAME_MAX - 1;
    kstrncpy(dirent.name, entry->name, KOS_DIRENT_NAME_MAX - 1);
    if (copy_to_user((void *)(context->user_address + context->written), &dirent, sizeof(dirent)) != 0) {
        context->error = -EFAULT;
        return false;
    }
    context->written += sizeof(dirent);
    return true;
}

static struct socket *syscall_lookup_socket(handle_t handle, uint32_t rights) {
    struct process *proc = process_get_current();
    return proc ? handle_lookup_type(proc, handle, KOS_OBJ_SOCKET, rights) : 0;
}

static int syscall_install_socket(struct socket *socket, uint32_t rights, handle_t *handle_out) {
    struct process *proc = process_get_current();
    if (proc == 0 || socket == 0 || handle_out == 0) return -EMFILE;
    handle_t handle = handle_create(proc, socket, rights);
    if (handle == KOS_INVALID_HANDLE) return -EMFILE;
    *handle_out = handle;
    return 0;
}

static uint32_t syscall_poll_once(struct kos_pollfd *pollfds, uint64_t count) {
    uint32_t ready_count = 0;
    for (uint64_t index = 0; index < count; ++index) {
        pollfds[index].revents = 0;
        struct socket *socket = syscall_lookup_socket((handle_t)pollfds[index].fd, KOS_RIGHT_NONE);
        if (socket == 0) {
            pollfds[index].revents = KOS_POLLNVAL;
            ++ready_count;
            continue;
        }
        uint32_t revents = 0;
        if (kos_poll(socket, (uint32_t)(uint16_t)pollfds[index].events, &revents) != 0) {
            pollfds[index].revents = KOS_POLLERR;
        } else {
            pollfds[index].revents = (int16_t)revents;
        }
        if (pollfds[index].revents != 0) ++ready_count;
    }
    return ready_count;
}

uint64_t kos_syscall_dispatch_frame(struct syscall_frame *frame) {
    if (frame == NULL) {
        return (uint64_t)-EINVAL;
    }

    uint64_t nr = frame->rax;
    uint64_t arg0 = frame->rdi;
    uint64_t arg1 = frame->rsi;
    uint64_t arg2 = frame->rdx;
    uint64_t arg3 = frame->r10;
    uint64_t arg4 = frame->r8;
    uint64_t arg5 = frame->r9;

    return kos_syscall_dispatch(nr, arg0, arg1, arg2, arg3, arg4, arg5);
}

uint64_t kos_syscall_dispatch(uint64_t number, uint64_t arg0, uint64_t arg1,
                              uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg3; (void)arg4; (void)arg5;

    switch (number) {
        case SYS_exit: {
            struct process *proc = process_get_current();
            if (proc != NULL && proc->pid > 1) {
                process_exit(proc, (int)arg0);
            }
            return 0;
        }

        case SYS_getpid: {
            struct process *proc = process_get_current();
            return proc ? (uint64_t)proc->pid : 1;
        }

        case SYS_getppid: {
            struct process *proc = process_get_current();
            return proc ? (uint64_t)proc->ppid : 0;
        }

        case SYS_yield:
            task_yield();
            return 0;

        case SYS_sleep:
            task_sleep((uint32_t)arg0);
            return 0;

        case SYS_gettime:
            return timer_uptime_seconds();

        case SYS_sysinfo:
            return task_count();

        case SYS_system_info: {
            uint32_t user_version = 0;
            if (!access_ok((const void *)arg0, sizeof(struct kos_system_info))
                || copy_from_user(&user_version, (const void *)arg0,
                                  sizeof(user_version)) != 0
                || user_version != KOS_SYSTEM_INFO_ABI_VERSION) {
                return (uint64_t)-EFAULT;
            }
            struct kos_system_info info = {
                .abi_version = KOS_SYSTEM_INFO_ABI_VERSION,
                .task_count = (uint32_t)task_count(),
                .uptime_seconds = timer_uptime_seconds(),
                .system_volume_letter = gui_manager_system_volume_letter(),
            };
            struct net_interface_info network = {0};
            if (net_interface_info(&network) && network.available) {
                info.network_available = 1;
                info.configuration_source = (uint32_t)network.configuration_source;
                info.dhcp_state = (uint32_t)network.dhcp_state;
                info.ipv4_address = network.ipv4_address;
                info.subnet_mask = network.subnet_mask;
                info.gateway = network.gateway;
                info.dns_server = network.dns_server;
                kmemcpy(info.mac_address, network.mac_address, sizeof(info.mac_address));
                struct net_device *device = net_device_get_default();
                if (device != NULL) {
                    kstrncpy(info.device_name, device->name, sizeof(info.device_name) - 1);
                    info.device_name[sizeof(info.device_name) - 1] = '\0';
                }
            }
            return copy_to_user((void *)arg0, &info, sizeof(info)) == 0
                ? 0 : (uint64_t)-EFAULT;
        }

        case SYS_gui_control:
            return syscall_gui_control(arg0);

        case SYS_system_control:
            return syscall_system_control(arg0, arg1);

        case SYS_write: {
            uint64_t fd = arg0;
            uint64_t uaddr = arg1;
            uint64_t len = arg2;
            if (len > 65536) len = 65536;
            if (len == 0) return 0;
            if (fd == 1 || fd == 2) return syscall_console_write(uaddr, len);
            if (fd <= 2) return (uint64_t)-EBADF;
            if (syscall_ui_read_only()) return (uint64_t)-EPERM;

            struct process *proc = process_get_current();
            struct kos_file *file = proc ? handle_lookup_type(proc, (handle_t)fd, KOS_OBJ_FILE, KOS_RIGHT_WRITE) : 0;
            if (file == 0) return (uint64_t)-EBADF;
            char buffer[256];
            uint64_t total = 0;
            while (total < len) {
                uint64_t chunk = len - total;
                if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
                if (copy_from_user(buffer, (const void *)(uaddr + total), chunk) != 0) {
                    return total == 0 ? (uint64_t)-EFAULT : total;
                }
                uint64_t written = 0;
                int error = kos_file_write(file, buffer, chunk, &written);
                total += written;
                if (error != 0) return total == 0 ? (uint64_t)-error : total;
                if (written != chunk) break;
            }
            return total;
        }

        case SYS_read: {
            uint64_t fd = arg0;
            uint64_t uaddr = arg1;
            uint64_t max_len = arg2;
            if (max_len == 0) return 0;
            if (max_len > 65536) max_len = 65536;
            if (fd == 0) return syscall_console_read(uaddr, max_len);
            if (fd <= 2) return (uint64_t)-EBADF;

            struct process *proc = process_get_current();
            struct kos_file *file = proc ? handle_lookup_type(proc, (handle_t)fd, KOS_OBJ_FILE, KOS_RIGHT_READ) : 0;
            if (file == 0) return (uint64_t)-EBADF;
            char buffer[256];
            uint64_t total = 0;
            while (total < max_len) {
                uint64_t chunk = max_len - total;
                if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
                uint64_t read_bytes = 0;
                int error = kos_file_read(file, buffer, chunk, &read_bytes);
                if (error != 0) return total == 0 ? (uint64_t)-error : total;
                if (read_bytes == 0) break;
                if (copy_to_user((void *)(uaddr + total), buffer, read_bytes) != 0) {
                    return total == 0 ? (uint64_t)-EFAULT : total;
                }
                total += read_bytes;
            }
            return total;
        }

        case SYS_open: {
            uint32_t flags = (uint32_t)arg1;
            if (syscall_ui_read_only()
                && ((flags & KOS_O_ACCMODE) != KOS_O_RDONLY
                    || (flags & (KOS_O_CREAT | KOS_O_TRUNC | KOS_O_APPEND)) != 0)) {
                return (uint64_t)-EPERM;
            }
            char path[VFS_MAX_PATH];
            long error = syscall_copy_path(path, (const char *)arg0);
            if (error != 0) return (uint64_t)error;
            int file_error = 0;
            struct kos_file *file = kos_file_open(path, flags, &file_error);
            if (file == 0) return (uint64_t)file_error;
            uint32_t rights = (file->can_read ? KOS_RIGHT_READ : 0)
                | (file->can_write ? KOS_RIGHT_WRITE : 0);
            struct process *proc = process_get_current();
            handle_t handle = proc ? handle_create(proc, file, rights) : KOS_INVALID_HANDLE;
            kos_object_unref(&file->header);
            return handle == KOS_INVALID_HANDLE ? (uint64_t)-EMFILE : (uint64_t)handle;
        }

        case SYS_close: {
            if (arg0 <= 2) return 0;
            struct process *proc = process_get_current();
            if (proc != NULL && handle_close(proc, (handle_t)arg0)) {
                return 0;
            }
            return (uint64_t)-EBADF;
        }

        case SYS_stat: {
            char path[VFS_MAX_PATH];
            long error = syscall_copy_path(path, (const char *)arg0);
            if (error != 0) return (uint64_t)error;
            struct vfs_metadata metadata;
            if (!vfs_stat(path, &metadata)) return (uint64_t)-ENOENT;
            struct kos_stat stat = {
                .size = metadata.size,
                .type = metadata.type == VFS_NODE_DIRECTORY ? KOS_STAT_DIRECTORY : KOS_STAT_FILE,
                .attributes = metadata.attributes,
            };
            return copy_to_user((void *)arg1, &stat, sizeof(stat)) == 0 ? 0 : (uint64_t)-EFAULT;
        }

        case SYS_fstat: {
            struct process *proc = process_get_current();
            struct kos_file *file = proc ? handle_lookup_type(proc, (handle_t)arg0, KOS_OBJ_FILE, KOS_RIGHT_NONE) : 0;
            if (file == 0) return (uint64_t)-EBADF;
            struct vfs_metadata metadata;
            if (kos_file_stat(file, &metadata) != 0) return (uint64_t)-EIO;
            struct kos_stat stat = {
                .size = metadata.size,
                .type = metadata.type == VFS_NODE_DIRECTORY ? KOS_STAT_DIRECTORY : KOS_STAT_FILE,
                .attributes = metadata.attributes,
            };
            return copy_to_user((void *)arg1, &stat, sizeof(stat)) == 0 ? 0 : (uint64_t)-EFAULT;
        }

        case SYS_lseek: {
            struct process *proc = process_get_current();
            struct kos_file *file = proc ? handle_lookup_type(proc, (handle_t)arg0, KOS_OBJ_FILE, KOS_RIGHT_NONE) : 0;
            if (file == 0) return (uint64_t)-EBADF;
            uint64_t position = 0;
            int error = kos_file_seek(file, (int64_t)arg1, (uint32_t)arg2, &position);
            return error == 0 ? position : (uint64_t)error;
        }

        case SYS_unlink: {
            if (syscall_ui_read_only()) return (uint64_t)-EPERM;
            char path[VFS_MAX_PATH];
            long error = syscall_copy_path(path, (const char *)arg0);
            if (error != 0) return (uint64_t)error;
            struct vfs_metadata metadata;
            if (!vfs_stat(path, &metadata)) return (uint64_t)-ENOENT;
            if (metadata.type == VFS_NODE_DIRECTORY) return (uint64_t)-EISDIR;
            return vfs_unlink_file(path) ? 0 : (uint64_t)-EIO;
        }

        case SYS_mkdir: {
            if (syscall_ui_read_only()) return (uint64_t)-EPERM;
            char path[VFS_MAX_PATH];
            long error = syscall_copy_path(path, (const char *)arg0);
            if (error != 0) return (uint64_t)error;
            if (vfs_stat(path, &(struct vfs_metadata){0})) return (uint64_t)-EEXIST;
            return vfs_mkdir(path) ? 0 : (uint64_t)-EIO;
        }

        case SYS_rename: {
            if (syscall_ui_read_only()) return (uint64_t)-EPERM;
            char old_path[VFS_MAX_PATH];
            char new_path[VFS_MAX_PATH];
            long error = syscall_copy_path(old_path, (const char *)arg0);
            if (error != 0) return (uint64_t)error;
            error = syscall_copy_path(new_path, (const char *)arg1);
            if (error != 0) return (uint64_t)error;
            if (!vfs_stat(old_path, &(struct vfs_metadata){0})) return (uint64_t)-ENOENT;
            if (vfs_stat(new_path, &(struct vfs_metadata){0})) return (uint64_t)-EEXIST;
            return vfs_rename(old_path, new_path) ? 0 : (uint64_t)-EIO;
        }

        case SYS_getdents: {
            if (arg2 < sizeof(struct kos_dirent)) return (uint64_t)-EINVAL;
            if (!access_ok((void *)arg1, arg2)) return (uint64_t)-EFAULT;
            struct process *proc = process_get_current();
            struct kos_file *file = proc ? handle_lookup_type(proc, (handle_t)arg0, KOS_OBJ_FILE, KOS_RIGHT_READ) : 0;
            if (file == 0) return (uint64_t)-EBADF;
            struct syscall_getdents_context context = {
                .user_address = arg1,
                .capacity = arg2,
            };
            int error = kos_file_getdents(file, syscall_getdents_entry, &context);
            if (context.error != 0) return (uint64_t)context.error;
            if (error != 0) return (uint64_t)error;
            return context.written;
        }

        case SYS_socket: {
            struct socket *sock = NULL;
            int ret = kos_socket((int)arg0, (int)arg1, (int)arg2, &sock);
            if (ret != 0 || sock == NULL) {
                return (uint64_t)(ret != 0 ? ret : -EINVAL);
            }
            handle_t handle = KOS_INVALID_HANDLE;
            ret = syscall_install_socket(sock, KOS_RIGHT_READ | KOS_RIGHT_WRITE, &handle);
            kos_object_unref(&sock->header);
            if (ret != 0) {
                return (uint64_t)ret;
            }
            return (uint64_t)handle;
        }

        case SYS_connect: {
            struct sockaddr kaddr;
            if (arg2 < sizeof(struct sockaddr_in)) return (uint64_t)-EINVAL;
            int err = copy_from_user(&kaddr, (const void *)arg1, sizeof(kaddr));
            if (err != 0) return (uint64_t)-EFAULT;
            struct socket *sock = syscall_lookup_socket((handle_t)arg0, KOS_RIGHT_WRITE);
            if (sock == NULL) return (uint64_t)-EBADF;
            return (uint64_t)kos_connect(sock, &kaddr, (uint32_t)arg2);
        }

        case SYS_bind: {
            if (arg2 < sizeof(struct sockaddr_in)) return (uint64_t)-EINVAL;
            struct sockaddr_in address;
            if (copy_from_user(&address, (const void *)arg1, sizeof(address)) != 0) {
                return (uint64_t)-EFAULT;
            }
            struct socket *sock = syscall_lookup_socket((handle_t)arg0, KOS_RIGHT_WRITE);
            if (sock == 0) return (uint64_t)-EBADF;
            return (uint64_t)kos_bind(sock, (const struct sockaddr *)&address, (uint32_t)arg2);
        }

        case SYS_listen: {
            struct socket *sock = syscall_lookup_socket((handle_t)arg0, KOS_RIGHT_WRITE);
            if (sock == 0) return (uint64_t)-EBADF;
            return (uint64_t)kos_listen(sock, (int)arg1);
        }

        case SYS_accept: {
            struct socket *listener = syscall_lookup_socket((handle_t)arg0, KOS_RIGHT_READ);
            if (listener == 0) return (uint64_t)-EBADF;
            if (arg1 != 0 && arg2 == 0) return (uint64_t)-EFAULT;

            uint32_t address_length = 0;
            if (arg2 != 0 && copy_from_user(&address_length, (const void *)arg2,
                    sizeof(address_length)) != 0) {
                return (uint64_t)-EFAULT;
            }
            if (arg1 != 0 && address_length < sizeof(struct sockaddr_in)) {
                return (uint64_t)-EINVAL;
            }

            struct sockaddr_in address = {0};
            uint32_t kernel_address_length = sizeof(address);
            struct socket *client = 0;
            int error = kos_accept(listener, arg1 != 0 ? (struct sockaddr *)&address : 0,
                arg2 != 0 ? &kernel_address_length : 0, &client);
            if (error != 0) return (uint64_t)error;

            handle_t handle = KOS_INVALID_HANDLE;
            error = syscall_install_socket(client, KOS_RIGHT_READ | KOS_RIGHT_WRITE, &handle);
            kos_object_unref(&client->header);
            if (error != 0) return (uint64_t)error;
            if (arg1 != 0 && copy_to_user((void *)arg1, &address, sizeof(address)) != 0) {
                struct process *proc = process_get_current();
                if (proc != 0) (void)handle_close(proc, handle);
                return (uint64_t)-EFAULT;
            }
            if (arg2 != 0 && copy_to_user((void *)arg2, &kernel_address_length,
                    sizeof(kernel_address_length)) != 0) {
                struct process *proc = process_get_current();
                if (proc != 0) (void)handle_close(proc, handle);
                return (uint64_t)-EFAULT;
            }
            return (uint64_t)handle;
        }

        case SYS_setsockopt: {
            if (arg4 > 256) return (uint64_t)-EINVAL;
            uint8_t value[256];
            if (arg4 != 0 && copy_from_user(value, (const void *)arg3, (size_t)arg4) != 0) {
                return (uint64_t)-EFAULT;
            }
            struct socket *sock = syscall_lookup_socket((handle_t)arg0, KOS_RIGHT_WRITE);
            if (sock == 0) return (uint64_t)-EBADF;
            return (uint64_t)kos_setsockopt(sock, (int)arg1, (int)arg2, value, (uint32_t)arg4);
        }

        case SYS_getsockopt: {
            struct socket *sock = syscall_lookup_socket((handle_t)arg0, KOS_RIGHT_READ);
            if (sock == 0) return (uint64_t)-EBADF;
            if (arg4 == 0) return (uint64_t)-EFAULT;
            uint32_t value_length = 0;
            if (copy_from_user(&value_length, (const void *)arg4, sizeof(value_length)) != 0) {
                return (uint64_t)-EFAULT;
            }
            if (value_length > 256) return (uint64_t)-EINVAL;
            uint8_t value[256];
            int error = kos_getsockopt(sock, (int)arg1, (int)arg2, value, &value_length);
            if (error != 0) return (uint64_t)error;
            if (arg3 == 0 || copy_to_user((void *)arg3, value, value_length) != 0
                || copy_to_user((void *)arg4, &value_length, sizeof(value_length)) != 0) {
                return (uint64_t)-EFAULT;
            }
            return 0;
        }

        case SYS_poll: {
            if (arg1 > 64) return (uint64_t)-EINVAL;
            size_t bytes = (size_t)(arg1 * sizeof(struct kos_pollfd));
            if (arg1 != 0 && !access_ok((const void *)arg0, bytes)) {
                return (uint64_t)-EFAULT;
            }
            struct kos_pollfd pollfds[64];
            if (arg1 != 0 && copy_from_user(pollfds, (const void *)arg0, bytes) != 0) {
                return (uint64_t)-EFAULT;
            }
            int64_t timeout_ms = (int64_t)arg2;
            uint64_t start_tick = timer_ticks();
            for (;;) {
                uint32_t ready = syscall_poll_once(pollfds, arg1);
                if (ready != 0 || timeout_ms == 0) break;
                if (timeout_ms >= 0
                    && timer_ticks() - start_tick >= timer_ms_to_ticks((uint64_t)timeout_ms)) {
                    break;
                }
                net_poll();
                if (task_is_multitasking_active()) task_sleep(1);
                else cpu_wait_for_interrupt();
            }
            uint32_t result = syscall_poll_once(pollfds, arg1);
            if (arg1 != 0 && copy_to_user((void *)arg0, pollfds, bytes) != 0) {
                return (uint64_t)-EFAULT;
            }
            return result;
        }

        case SYS_sendto: {
            uint64_t len = arg2;
            if (len > 65536) len = 65536;
            char kbuf[1024];
            if (len > sizeof(kbuf)) len = sizeof(kbuf);
            int err = copy_from_user(kbuf, (const void *)arg1, len);
            if (err != 0) return (uint64_t)-EFAULT;
            struct process *proc = process_get_current();
            struct socket *sock = proc ? (struct socket *)handle_lookup(proc, (handle_t)arg0, KOS_RIGHT_WRITE) : NULL;
            if (sock == NULL) return (uint64_t)-EBADF;
            return (uint64_t)kos_send(sock, kbuf, (size_t)len, (int)arg3);
        }

        case SYS_recvfrom: {
            uint64_t len = arg2;
            if (len > 65536) len = 65536;
            char kbuf[1024];
            if (len > sizeof(kbuf)) len = sizeof(kbuf);
            struct process *proc = process_get_current();
            struct socket *sock = proc ? (struct socket *)handle_lookup(proc, (handle_t)arg0, KOS_RIGHT_READ) : NULL;
            if (sock == NULL) return (uint64_t)-EBADF;
            int ret = kos_recv(sock, kbuf, (size_t)len, (int)arg3);
            if (ret > 0) {
                int err = copy_to_user((void *)arg1, kbuf, (size_t)ret);
                if (err != 0) return (uint64_t)-EFAULT;
            }
            return (uint64_t)ret;
        }

        case SYS_gui_display:
            return gui_service_display(arg0, arg1);

        case SYS_gui_input:
            return gui_service_input(arg0);

        case SYS_installer:
            return installer_service_dispatch(arg0, arg1);

        default:
            return (uint64_t)-ENOSYS;
    }
}

#ifdef KOS_CI_SMOKE
bool syscall_ui_sandbox_self_test(void) {
    struct process *proc = process_get_current();
    if (proc == NULL) return false;
    uint32_t previous_flags = proc->flags;
    proc->flags |= PROCESS_FLAG_UI_READ_ONLY;

    bool passed = kos_syscall_dispatch(SYS_write, 3, 0, 1, 0, 0, 0)
            == (uint64_t)-EPERM
        && kos_syscall_dispatch(SYS_open, 0, KOS_O_WRONLY, 0, 0, 0, 0)
            == (uint64_t)-EPERM
        && kos_syscall_dispatch(SYS_open, 0, KOS_O_RDONLY | KOS_O_CREAT, 0, 0, 0, 0)
            == (uint64_t)-EPERM
        && kos_syscall_dispatch(SYS_unlink, 0, 0, 0, 0, 0, 0)
            == (uint64_t)-EPERM
        && kos_syscall_dispatch(SYS_mkdir, 0, 0, 0, 0, 0, 0)
            == (uint64_t)-EPERM
        && kos_syscall_dispatch(SYS_rename, 0, 0, 0, 0, 0, 0)
            == (uint64_t)-EPERM;

    proc->flags = previous_flags;
    if (passed) log_info("[CI SMOKE] PASS: UI process filesystem mutation syscalls are denied.");
    else log_error("[CI SMOKE] FAIL: UI filesystem write sandbox policy.");
    return passed;
}
#endif

const char *kos_syscall_name(uint64_t number) {
    switch (number) {
        case SYS_exit:     return "exit";
        case SYS_fork:     return "fork";
        case SYS_read:     return "read";
        case SYS_write:    return "write";
        case SYS_open:     return "open";
        case SYS_close:    return "close";
        case SYS_waitpid:  return "waitpid";
        case SYS_execve:   return "execve";
        case SYS_getpid:   return "getpid";
        case SYS_getppid:  return "getppid";
        case SYS_yield:    return "yield";
        case SYS_sleep:    return "sleep";
        case SYS_socket:   return "socket";
        case SYS_connect:  return "connect";
        case SYS_accept:   return "accept";
        case SYS_sendto:   return "sendto";
        case SYS_recvfrom: return "recvfrom";
        case SYS_bind:     return "bind";
        case SYS_listen:   return "listen";
        case SYS_poll:     return "poll";
        case SYS_setsockopt: return "setsockopt";
        case SYS_getsockopt: return "getsockopt";
        case SYS_stat:     return "stat";
        case SYS_fstat:    return "fstat";
        case SYS_lseek:    return "lseek";
        case SYS_unlink:   return "unlink";
        case SYS_mkdir:    return "mkdir";
        case SYS_getdents: return "getdents";
        case SYS_rename:   return "rename";
        case SYS_gettime:  return "gettime";
        case SYS_sysinfo:  return "sysinfo";
        case SYS_system_info: return "system_info";
        case SYS_gui_control: return "gui_control";
        case SYS_system_control: return "system_control";
        case SYS_gui_display: return "gui_display";
        case SYS_gui_input: return "gui_input";
        case SYS_installer: return "installer";
        default:           return "unknown";
    }
}
