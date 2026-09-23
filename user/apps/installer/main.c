#include <stdbool.h>
#include <stdint.h>

#include <uapi/kos/installer.h>
#include <uapi/kos/syscall_numbers.h>
#include <vino/gui.h>
#include <vino/installer.h>

enum {
    UI_MARGIN = 24,
    UI_TEXT_SCALE = 1,
    UI_ROW_HEIGHT = 34,
};

static const struct kos_gui_color color_background = { 10, 16, 29, 0 };
static const struct kos_gui_color color_panel = { 20, 30, 48, 0 };
static const struct kos_gui_color color_active = { 27, 91, 158, 0 };
static const struct kos_gui_color color_text = { 238, 244, 252, 0 };
static const struct kos_gui_color color_muted = { 159, 176, 198, 0 };
static const struct kos_gui_color color_accent = { 96, 190, 255, 0 };
static const struct kos_gui_color color_error = { 255, 127, 127, 0 };

static struct kos_gui_surface surface;
static struct kos_installer_state installer;
static uint32_t selection;
static bool format_confirmation_pending;
static char local_error[KOS_INSTALLER_TEXT_MAX];

static long syscall1(uint64_t number, uint64_t arg0) {
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = arg0;
    __asm__ volatile("syscall" : "+a"(result) : "D"(first) : "rcx", "r11", "memory");
    return (long)result;
}

static long syscall2(uint64_t number, uint64_t arg0, uint64_t arg1) {
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = arg0;
    register uint64_t second __asm__("rsi") = arg1;
    __asm__ volatile("syscall" : "+a"(result) : "D"(first), "S"(second)
                     : "rcx", "r11", "memory");
    return (long)result;
}

__attribute__((noreturn)) static void app_exit(int status) {
    (void)syscall1(SYS_exit, (uint64_t)status);
    __builtin_unreachable();
}

static void string_copy(char *destination, uint32_t capacity, const char *source) {
    if (capacity == 0) return;
    uint32_t index = 0;
    if (source != 0) {
        while (index + 1 < capacity && source[index] != '\0') {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}

static bool strings_equal(const char *left, const char *right) {
    if (left == 0 || right == 0) return false;
    uint32_t index = 0;
    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return false;
        ++index;
    }
    return left[index] == right[index];
}

static void string_append(char *destination, uint32_t capacity, const char *source) {
    uint32_t index = 0;
    while (index < capacity && destination[index] != '\0') ++index;
    if (index == capacity) return;
    uint32_t offset = 0;
    while (index + 1 < capacity && source[offset] != '\0') {
        destination[index++] = source[offset++];
    }
    destination[index] = '\0';
}

static void number_append(char *destination, uint32_t capacity, uint64_t value) {
    char digits[24];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) {
        char digit[2] = { digits[--count], '\0' };
        string_append(destination, capacity, digit);
    }
}

static long send_action(uint32_t action, bool confirm, const char *value) {
    struct kos_installer_action_request request = {
        .abi_version = KOS_INSTALLER_ABI_VERSION,
        .action = action,
        .confirm = confirm ? 1u : 0u,
    };
    string_copy(request.value, sizeof(request.value), value);
    long result = vino_installer_action(&request, &installer);
    if (result < 0) string_copy(local_error, sizeof(local_error), "Action failed; check the selected target and retry.");
    else local_error[0] = '\0';
    return result;
}

static void draw_text(int32_t x, int32_t y, const char *value,
                      struct kos_gui_color foreground,
                      struct kos_gui_color background) {
    (void)vino_gui_text(x, y, value, UI_TEXT_SCALE, foreground, background);
}

static struct kos_gui_rect content_rect(uint32_t y, uint32_t width, uint32_t height) {
    return (struct kos_gui_rect){
        .x = UI_MARGIN + 28,
        .y = (int32_t)y,
        .width = width,
        .height = height,
    };
}

static void draw_shell(void) {
    (void)vino_gui_clear(color_background);
    uint32_t width = surface.width > UI_MARGIN * 2 ? surface.width - UI_MARGIN * 2 : 1;
    uint32_t height = surface.height > UI_MARGIN * 2 ? surface.height - UI_MARGIN * 2 : 1;
    struct kos_gui_rect window = { UI_MARGIN, UI_MARGIN, width, height };
    vino_gui_window_draw(window, "Vino OS Installer", color_panel, color_text);
    draw_text(UI_MARGIN + 28, UI_MARGIN + 50,
              "Vino OS is the default edition; profiles only choose CLI or GUI.",
              color_muted, color_panel);
    const char *footer = "J/K select  Enter continue  C confirm/create  F format  Esc exit";
    if (installer.phase == KOS_INSTALLER_PHASE_FINISHED)
        footer = "Enter/R reboot   Q/Esc return to Vino CLI";
    else if (installer.phase == KOS_INSTALLER_PHASE_ERROR)
        footer = "Q/Esc return to Vino CLI";
    draw_text(UI_MARGIN + 28, (int32_t)(surface.height - UI_MARGIN - 22), footer,
              color_muted, color_panel);
    const char *message = installer.error[0] != '\0' ? installer.error : local_error;
    if (message[0] != '\0')
        draw_text(UI_MARGIN + 28, (int32_t)(surface.height - UI_MARGIN - 42), message,
                  color_error, color_panel);
}

static void draw_list(const char *const *items, uint32_t count, uint32_t start_y,
                      uint32_t row_width) {
    struct vino_gui_list list = {
        .bounds = content_rect(start_y, row_width, count == 0 ? UI_ROW_HEIGHT : count * UI_ROW_HEIGHT),
        .items = items,
        .item_count = count,
        .selected_item = count == 0 ? 0 : selection % count,
        .row_height = UI_ROW_HEIGHT,
    };
    vino_gui_list_draw(&list, color_text, color_panel, color_active);
}

static void make_disk_label(char destination[KOS_GUI_TEXT_MAX],
                            const struct kos_installer_disk *disk) {
    string_copy(destination, KOS_GUI_TEXT_MAX, disk->name);
    string_append(destination, KOS_GUI_TEXT_MAX, "   ");
    number_append(destination, KOS_GUI_TEXT_MAX, disk->capacity_bytes / (1024 * 1024));
    string_append(destination, KOS_GUI_TEXT_MAX, " MiB   ");
    string_append(destination, KOS_GUI_TEXT_MAX,
        disk->table_type == KOS_INSTALLER_TABLE_GPT ? "GPT" :
        disk->table_type == KOS_INSTALLER_TABLE_MBR ? "MBR" : "uninitialized");
    string_append(destination, KOS_GUI_TEXT_MAX, "   ");
    number_append(destination, KOS_GUI_TEXT_MAX, disk->partition_count);
    string_append(destination, KOS_GUI_TEXT_MAX, " partitions");
}

static void make_partition_label(char destination[KOS_GUI_TEXT_MAX],
                                 const struct kos_installer_partition *partition) {
    string_copy(destination, KOS_GUI_TEXT_MAX, partition->name);
    string_append(destination, KOS_GUI_TEXT_MAX, "   ");
    string_append(destination, KOS_GUI_TEXT_MAX, partition->filesystem);
    if (partition->is_efi_system_partition) string_append(destination, KOS_GUI_TEXT_MAX, "   EFI System");
    string_append(destination, KOS_GUI_TEXT_MAX, "   ");
    number_append(destination, KOS_GUI_TEXT_MAX, partition->capacity_bytes / (1024 * 1024));
    string_append(destination, KOS_GUI_TEXT_MAX, " MiB");
}

static void draw_profile_page(void) {
    static const char *const choices[] = { "Vino CLI - OS default, no GUI", "Vino Plus - OS default, GUI selected" };
    draw_text(UI_MARGIN + 28, UI_MARGIN + 116, "Choose installation profile", color_text, color_panel);
    draw_list(choices, 2, UI_MARGIN + 148, surface.width - UI_MARGIN * 2 - 56);
}

static void draw_gui_page(void) {
    struct vino_gui_dropdown dropdown = {
        .bounds = content_rect(UI_MARGIN + 148, 380, 52),
        .label = "GUI package included in this ISO",
        .value = "Default",
        .expanded = false,
    };
    draw_text(UI_MARGIN + 28, UI_MARGIN + 116, "Choose the GUI for Vino Plus", color_text, color_panel);
    vino_gui_dropdown_draw(&dropdown, color_text, color_panel, color_accent);
    draw_text(UI_MARGIN + 28, UI_MARGIN + 226, "Vino OS remains edition=default.", color_muted, color_panel);
}

static void draw_disk_page(void) {
    static char labels[KOS_INSTALLER_MAX_DISKS][KOS_GUI_TEXT_MAX];
    static const char *items[KOS_INSTALLER_MAX_DISKS];
    for (uint32_t index = 0; index < installer.disk_count; ++index) {
        make_disk_label(labels[index], &installer.disks[index]);
        items[index] = labels[index];
    }
    draw_text(UI_MARGIN + 28, UI_MARGIN + 116, "Select a physical target disk", color_text, color_panel);
    draw_list(items, installer.disk_count, UI_MARGIN + 148, surface.width - UI_MARGIN * 2 - 56);
    if (installer.disk_count == 0)
        draw_text(UI_MARGIN + 28, UI_MARGIN + 160, "No readable online disks were found.", color_error, color_panel);
}

static void draw_partition_page(void) {
    static char labels[KOS_INSTALLER_MAX_PARTITIONS][KOS_GUI_TEXT_MAX];
    static const char *items[KOS_INSTALLER_MAX_PARTITIONS];
    for (uint32_t index = 0; index < installer.partition_count; ++index) {
        make_partition_label(labels[index], &installer.partitions[index]);
        items[index] = labels[index];
    }
    draw_text(UI_MARGIN + 28, UI_MARGIN + 116, "Select an EFI System partition, or create one", color_text, color_panel);
    draw_list(items, installer.partition_count, UI_MARGIN + 148, surface.width - UI_MARGIN * 2 - 56);
    if (installer.partition_count == 0)
        draw_text(UI_MARGIN + 28, UI_MARGIN + 160, "No partitions found. C prepares a new FAT32 EFI target.", color_muted, color_panel);
}

static void draw_confirmation_page(void) {
    draw_text(UI_MARGIN + 28, UI_MARGIN + 116, "Review and confirm the target before writing", color_text, color_panel);
    draw_text(UI_MARGIN + 28, UI_MARGIN + 154, "OS edition: default", color_muted, color_panel);
    draw_text(UI_MARGIN + 28, UI_MARGIN + 184, "Profile:", color_muted, color_panel);
    draw_text(UI_MARGIN + 150, UI_MARGIN + 184, installer.selected_profile, color_text, color_panel);
    draw_text(UI_MARGIN + 28, UI_MARGIN + 214, "Disk:", color_muted, color_panel);
    draw_text(UI_MARGIN + 150, UI_MARGIN + 214, installer.selected_disk, color_text, color_panel);
    draw_text(UI_MARGIN + 28, UI_MARGIN + 244, "Partition:", color_muted, color_panel);
    draw_text(UI_MARGIN + 150, UI_MARGIN + 244,
        installer.selected_partition[0] ? installer.selected_partition : "new partition + FAT32",
        color_text, color_panel);
    if (installer.target_confirmed) {
        draw_text(UI_MARGIN + 28, UI_MARGIN + 288, "Target confirmed. Enter starts installation.", color_accent, color_panel);
        draw_text(UI_MARGIN + 28, UI_MARGIN + 318, "Base OS, Limine and selected GUI will be written.", color_muted, color_panel);
    } else {
        draw_text(UI_MARGIN + 28, UI_MARGIN + 288, "C confirms creation/target; F requests destructive FAT32 format.", color_muted, color_panel);
        draw_text(UI_MARGIN + 28, UI_MARGIN + 318, "Installation overwrites files on the selected partition.", color_error, color_panel);
    }
    if (format_confirmation_pending) {
        struct vino_gui_dialog dialog = {
            .bounds = content_rect(UI_MARGIN + 360, surface.width - UI_MARGIN * 2 - 100, 88),
            .title = "Confirm destructive format",
            .message = "Y formats selected partition as FAT32; N cancels.",
            .visible = true,
        };
        vino_gui_dialog_draw(&dialog, color_text, color_panel, color_error);
    }
}

static void draw_progress(void) {
    struct vino_gui_progress_bar progress = {
        .bounds = content_rect(UI_MARGIN + 190, surface.width - UI_MARGIN * 2 - 100, 24),
        .value = installer.phase == KOS_INSTALLER_PHASE_FINISHED ? 100 : 0,
        .maximum = 100,
    };
    vino_gui_progress_draw(&progress, color_panel, color_accent, color_muted);
}

static void draw_state(void) {
    draw_shell();
    switch (installer.phase) {
        case KOS_INSTALLER_PHASE_PROFILE_SELECTION: draw_profile_page(); break;
        case KOS_INSTALLER_PHASE_GUI_SELECTION: draw_gui_page(); break;
        case KOS_INSTALLER_PHASE_DISK_SELECTION: draw_disk_page(); break;
        case KOS_INSTALLER_PHASE_PARTITION_SETUP: draw_partition_page(); break;
        case KOS_INSTALLER_PHASE_CONFIRMATION: draw_confirmation_page(); break;
        case KOS_INSTALLER_PHASE_INSTALLING_OS:
        case KOS_INSTALLER_PHASE_INSTALLING_GUI:
        case KOS_INSTALLER_PHASE_INSTALLING_BOOTLOADER:
            draw_text(UI_MARGIN + 28, UI_MARGIN + 150, "Installing Vino OS...", color_text, color_panel);
            draw_progress();
            break;
        case KOS_INSTALLER_PHASE_FINISHED:
            draw_text(UI_MARGIN + 28, UI_MARGIN + 150, "Installation committed successfully.", color_text, color_panel);
            draw_text(UI_MARGIN + 28, UI_MARGIN + 184, "Press Enter/R to reboot into Vino OS CLI.", color_muted, color_panel);
            draw_progress();
            break;
        case KOS_INSTALLER_PHASE_ERROR: {
            struct vino_gui_dialog dialog = {
                .bounds = content_rect(UI_MARGIN + 140, surface.width - UI_MARGIN * 2 - 100, 100),
                .title = "Installer error",
                .message = installer.error[0] ? installer.error : "Operation failed; inspect disk and retry.",
                .visible = true,
            };
            vino_gui_dialog_draw(&dialog, color_text, color_panel, color_error);
            break;
        }
        default: break;
    }
}

static const struct kos_installer_disk *selected_disk(void) {
    for (uint32_t index = 0; index < installer.disk_count; ++index) {
        if (installer.selected_disk[0] != '\0'
            && strings_equal(installer.selected_disk, installer.disks[index].name))
            return &installer.disks[index];
    }
    return installer.disk_count == 0 ? 0 : &installer.disks[selection % installer.disk_count];
}

static long request_partition(void) {
    const struct kos_installer_disk *disk = selected_disk();
    if (disk == 0 || disk->partition_count != 0) return -1;
    uint32_t table = disk->table_type;
    uint64_t first = 2048;
    uint64_t last;
    if (table == KOS_INSTALLER_TABLE_NONE) {
        table = KOS_INSTALLER_TABLE_GPT;
        if (disk->total_blocks <= 34) return -1;
        last = disk->total_blocks - 34;
    } else {
        if (disk->last_usable_lba < first) return -1;
        last = disk->last_usable_lba;
    }
    if (disk->first_usable_lba > first) first = disk->first_usable_lba;
    if (last < first || last - first < 4095) return -1;
    struct kos_installer_action_request request = {
        .abi_version = KOS_INSTALLER_ABI_VERSION,
        .action = KOS_INSTALLER_REQUEST_PARTITION,
        .table_type = table,
        .filesystem = KOS_INSTALLER_FS_FAT32,
        .start_lba = first,
        .block_count = last - first + 1,
    };
    long result = vino_installer_action(&request, &installer);
    if (result < 0) string_copy(local_error, sizeof(local_error), "Could not prepare a safe partition range.");
    else local_error[0] = '\0';
    return result;
}

static void handle_enter(void) {
    switch (installer.phase) {
        case KOS_INSTALLER_PHASE_PROFILE_SELECTION:
            (void)send_action(KOS_INSTALLER_SELECT_PROFILE, false, selection == 0 ? "cli" : "plus");
            selection = 0;
            break;
        case KOS_INSTALLER_PHASE_GUI_SELECTION:
            (void)send_action(KOS_INSTALLER_SELECT_GUI, false, "default");
            break;
        case KOS_INSTALLER_PHASE_DISK_SELECTION:
            if (installer.disk_count != 0)
                (void)send_action(KOS_INSTALLER_SELECT_DISK, false,
                    installer.disks[selection % installer.disk_count].name);
            selection = 0;
            break;
        case KOS_INSTALLER_PHASE_PARTITION_SETUP:
            if (installer.partition_count != 0)
                (void)send_action(KOS_INSTALLER_SELECT_PARTITION, false,
                    installer.partitions[selection % installer.partition_count].name);
            selection = 0;
            break;
        case KOS_INSTALLER_PHASE_CONFIRMATION:
            if (installer.target_confirmed) (void)send_action(KOS_INSTALLER_EXECUTE, true, 0);
            else string_copy(local_error, sizeof(local_error), "Press C to confirm this target before installation.");
            break;
        case KOS_INSTALLER_PHASE_FINISHED:
            (void)send_action(KOS_INSTALLER_REBOOT, true, 0);
            break;
        default: break;
    }
}

__attribute__((noreturn)) void _start(void) {
    surface.abi_version = KOS_GUI_ABI_VERSION;
    if (vino_gui_surface_get(&surface) < 0 || surface.width < 400 || surface.height < 300)
        app_exit(1);
    struct kos_installer_action_request request = {
        .abi_version = KOS_INSTALLER_ABI_VERSION,
        .action = KOS_INSTALLER_START,
    };
    if (vino_installer_action(&request, &installer) < 0)
        app_exit(2);

    for (;;) {
        draw_state();
        struct kos_gui_input_event event = { .abi_version = KOS_GUI_ABI_VERSION };
        if (vino_gui_input_read(&event) < 0) app_exit(3);
        if (event.type != KOS_GUI_INPUT_KEY) {
            (void)syscall1(SYS_sleep, 12);
            continue;
        }
        uint32_t key = event.key;
        if (format_confirmation_pending) {
            if (key == 'y' || key == 'Y') {
                format_confirmation_pending = false;
                (void)send_action(KOS_INSTALLER_FORMAT_TARGET, true, 0);
            } else if (key == 'n' || key == 'N' || key == 27) {
                format_confirmation_pending = false;
            }
            continue;
        }
        if (key == 27 || key == 'q' || key == 'Q') {
            (void)send_action(KOS_INSTALLER_CLOSE, false, 0);
            app_exit(0);
        }
        if (key == 'j' || key == 'J' || key == 'd' || key == '\t') ++selection;
        else if (key == 'k' || key == 'K' || key == 'u' || key == 8 || key == 127) {
            if (selection > 0) --selection;
            else if (key == 8 || key == 127) (void)send_action(KOS_INSTALLER_BACK, false, 0);
        }
        else if (key == '\n' || key == '\r' || key == ' ') handle_enter();
        else if ((key == 'c' || key == 'C') && installer.phase == KOS_INSTALLER_PHASE_PARTITION_SETUP)
            (void)request_partition();
        else if ((key == 'c' || key == 'C') && installer.phase == KOS_INSTALLER_PHASE_CONFIRMATION)
            (void)send_action(KOS_INSTALLER_CONFIRM_TARGET, true, 0);
        else if ((key == 'f' || key == 'F') && installer.phase == KOS_INSTALLER_PHASE_CONFIRMATION
                 && installer.selected_partition[0] != '\0')
            format_confirmation_pending = true;
    }
}
