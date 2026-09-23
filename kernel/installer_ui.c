#include <kos/installer_ui.h>
#include <kos/process.h>
#include <uapi/kos/syscall_numbers.h>

static void installer_process_entry(void *argument) {
    const char *path = (const char *)argument;
    int result = process_exec(path, 0, 0);
    struct process *process = process_get_current();
    if (process != 0 && process->pid > 1) process_exit(process, result);
    thread_exit(result);
}

bool installer_ui_run(void) {
    static const char application_path[] = "C:/SYSTEM/GUI/INSTALLER.ELF";
    struct process *child = process_create("vino-installer-ui");
    if (child == 0) return false;
    if (thread_create(child, installer_process_entry, (void *)application_path,
                      "installer-ui") == 0) {
        process_exit(child, -ENOMEM);
        (void)process_wait(child, 0, 0);
        return false;
    }
    int exit_code = -ENOEXEC;
    return process_wait(child, &exit_code, UINT64_MAX) && exit_code == 0;
}
