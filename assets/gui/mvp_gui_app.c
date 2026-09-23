#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/kos/gui_control.h>
#include <uapi/kos/syscall_numbers.h>
#include <uapi/kos/system_control.h>
#include <vino/gui.h>

enum {
    PAGE_HOME = 0,
    PAGE_NETWORK,
    PAGE_SHELL,
    PAGE_FILES,
    PAGE_INSTALL,
    PAGE_REMOTE,
    PAGE_RECOVERY,
    PAGE_POWER,
    PAGE_COUNT,
    FILE_ENTRY_COUNT = 20,
    FILE_PATH_LENGTH = 256,
    FILE_PREVIEW_SIZE = 512,
};

static const char *const page_names[PAGE_COUNT] = {
    "MVP Home", "Network", "Open shell", "File manager", "Install local GUI",
    "Remote GUI", "Recovery", "Restart / shutdown",
};

static struct kos_gui_surface surface;
static struct kos_system_info system_info;
static struct kos_gui_control_request gui_state;
static struct kos_gui_color color_background = { 9, 17, 34, 0 };
static struct kos_gui_color color_wallpaper = { 16, 39, 68, 0 };
static struct kos_gui_color color_panel = { 18, 32, 54, 0 };
static struct kos_gui_color color_panel_high = { 27, 47, 74, 0 };
static struct kos_gui_color color_accent = { 91, 194, 255, 0 };
static struct kos_gui_color color_cyan = { 135, 226, 245, 0 };
static struct kos_gui_color color_text = { 240, 247, 255, 0 };
static struct kos_gui_color color_muted = { 157, 178, 204, 0 };

static struct kos_dirent file_entries[FILE_ENTRY_COUNT];
static uint32_t file_entry_count;
static uint32_t file_selection;
static uint32_t file_scroll;
static uint32_t page_selection;
static uint32_t home_selection = PAGE_NETWORK;
static uint32_t selected_gui;
static uint32_t power_selection;
static uint32_t power_confirmation;
static uint32_t file_preview_size;
static bool file_preview_open;
static char file_directory[FILE_PATH_LENGTH];
static char file_message[96];
static char file_preview[FILE_PREVIEW_SIZE];
static char status_message[96];
static int32_t last_gui_result;
static uint64_t last_refresh;
static char text_buffer[KOS_GUI_TEXT_MAX];

static long sleep_ms(uint32_t milliseconds) {
    register uint64_t result __asm__("rax") = SYS_sleep;
    register uint64_t duration __asm__("rdi") = milliseconds;
    __asm__ volatile("syscall" : "+a"(result) : "D"(duration)
                     : "rcx", "r11", "memory");
    return (long)result;
}

static uint64_t uptime_seconds(void) {
    register uint64_t result __asm__("rax") = SYS_gettime;
    __asm__ volatile("syscall" : "+a"(result) : : "rcx", "r11", "memory");
    return result;
}

__attribute__((noreturn)) static void app_exit(void) {
    register uint64_t result __asm__("rax") = SYS_exit;
    register uint64_t status __asm__("rdi") = 0;
    __asm__ volatile("syscall" : "+a"(result) : "D"(status)
                     : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static uint32_t string_length(const char *value) {
    uint32_t length = 0;
    if (value != NULL) while (value[length] != '\0') ++length;
    return length;
}

static bool string_equal(const char *left, const char *right) {
    if (left == NULL || right == NULL) return false;
    uint32_t index = 0;
    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return false;
        ++index;
    }
    return left[index] == right[index];
}

static void string_copy(char *destination, uint32_t capacity, const char *source) {
    if (destination == NULL || capacity == 0) return;
    uint32_t index = 0;
    if (source != NULL) {
        while (index + 1 < capacity && source[index] != '\0') {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}

static void string_append(char *destination, uint32_t capacity, const char *source) {
    uint32_t length = string_length(destination);
    if (length < capacity) string_copy(destination + length, capacity - length, source);
}

static void number_text(uint64_t value, char output[24]) {
    char reversed[24];
    uint32_t count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < sizeof(reversed));
    uint32_t index = 0;
    while (count != 0) output[index++] = reversed[--count];
    output[index] = '\0';
}

static void draw_text(int32_t x, int32_t y, const char *text,
                      struct kos_gui_color foreground,
                      struct kos_gui_color background) {
    string_copy(text_buffer, sizeof(text_buffer), text);
    for (uint32_t index = 0; text_buffer[index] != '\0'; ++index) {
        if ((uint8_t)text_buffer[index] < 32 || (uint8_t)text_buffer[index] >= 127)
            text_buffer[index] = '?';
    }
    (void)vino_gui_text(x, y, text_buffer, 1, foreground, background);
}

static void draw_box(int32_t x, int32_t y, uint32_t width, uint32_t height,
                     struct kos_gui_color fill, struct kos_gui_color border) {
    (void)vino_gui_fill((struct kos_gui_rect){ x, y, width, height }, fill);
    if (width > 1 && height > 1)
        (void)vino_gui_border((struct kos_gui_rect){ x, y, width, height }, border);
}

static void draw_rule(int32_t x, int32_t y, uint32_t width) {
    (void)vino_gui_fill((struct kos_gui_rect){ x, y, width, 1 }, color_cyan);
}

static bool is_compact(void) {
    return surface.width < 560 || surface.height < 380;
}

static void format_ipv4(uint32_t address, char output[16]) {
    uint8_t octets[4] = {
        (uint8_t)(address >> 24), (uint8_t)(address >> 16),
        (uint8_t)(address >> 8), (uint8_t)address,
    };
    output[0] = '\0';
    for (uint32_t index = 0; index < 4; ++index) {
        char number[24];
        number_text(octets[index], number);
        string_append(output, 16, number);
        if (index != 3) string_append(output, 16, ".");
    }
}

static void refresh_state(void) {
    system_info = (struct kos_system_info){
        .abi_version = KOS_SYSTEM_INFO_ABI_VERSION,
    };
    if (vino_system_info_get(&system_info) < 0) {
        system_info.network_available = 0;
        system_info.system_volume_letter = 0;
    }
    gui_state = (struct kos_gui_control_request){
        .abi_version = KOS_GUI_CONTROL_ABI_VERSION,
        .action = KOS_GUI_CONTROL_QUERY,
    };
    (void)vino_gui_control(&gui_state);
}

static bool path_exists(const char *path, uint32_t expected_type) {
    struct kos_stat metadata = {0};
    return vino_fs_stat(path, &metadata) == 0 && metadata.type == expected_type;
}

static bool path_join(char output[FILE_PATH_LENGTH], const char *base,
                      const char *leaf) {
    uint32_t base_size = string_length(base);
    uint32_t leaf_size = string_length(leaf);
    if (base == NULL || leaf == NULL || base_size + leaf_size + 2 > FILE_PATH_LENGTH)
        return false;
    string_copy(output, FILE_PATH_LENGTH, base);
    if (base_size != 0 && output[base_size - 1] != '/') {
        output[base_size++] = '/';
        output[base_size] = '\0';
    }
    string_copy(output + base_size, FILE_PATH_LENGTH - base_size, leaf);
    return true;
}

static void path_parent(char path[FILE_PATH_LENGTH]) {
    uint32_t length = string_length(path);
    while (length > 3 && path[length - 1] == '/') path[--length] = '\0';
    if (length <= 3) return;
    while (length > 3 && path[length - 1] != '/') --length;
    if (length < 3) length = 3;
    path[length] = '\0';
}

static bool list_directory(const char *path, struct kos_dirent *entries,
                           uint32_t capacity, uint32_t *count_out) {
    if (path == NULL || entries == NULL || capacity == 0 || count_out == NULL)
        return false;
    *count_out = 0;
    long descriptor = vino_fs_open(path, KOS_O_RDONLY | KOS_O_DIRECTORY);
    if (descriptor < 0) return false;
    long bytes = vino_fs_getdents(descriptor, entries,
                                  (uint64_t)capacity * sizeof(entries[0]));
    (void)vino_fs_close(descriptor);
    if (bytes < 0) return false;
    *count_out = (uint32_t)((uint64_t)bytes / sizeof(entries[0]));
    if (*count_out > capacity) *count_out = capacity;
    return true;
}

static void files_reload(void) {
    file_entry_count = 0;
    file_selection = 0;
    file_scroll = 0;
    file_message[0] = '\0';
    if (!list_directory(file_directory, file_entries, FILE_ENTRY_COUNT,
                        &file_entry_count)) {
        string_copy(file_message, sizeof(file_message), "Cannot open this VFS folder.");
    }
}

static void initialize_paths(void) {
    char volume = system_info.system_volume_letter;
    if (volume < 'C' || volume > 'Z') volume = 'D';
    file_directory[0] = volume;
    file_directory[1] = ':';
    file_directory[2] = '/';
    file_directory[3] = '\0';
    char candidate[FILE_PATH_LENGTH];
    if (path_join(candidate, file_directory, "VINO")
        && path_exists(candidate, KOS_STAT_DIRECTORY)) {
        string_copy(file_directory, sizeof(file_directory), candidate);
    }
    files_reload();
}

static bool enter_file(void) {
    if (file_selection >= file_entry_count) return false;
    struct kos_dirent *entry = &file_entries[file_selection];
    if (entry->name_length == 0 || entry->name[0] == '.') return false;
    char target[FILE_PATH_LENGTH];
    if (!path_join(target, file_directory, entry->name)) return false;
    if (entry->type == KOS_STAT_DIRECTORY) {
        string_copy(file_directory, sizeof(file_directory), target);
        files_reload();
        return true;
    }
    long descriptor = vino_fs_open(target, KOS_O_RDONLY);
    if (descriptor < 0) {
        string_copy(file_message, sizeof(file_message), "Selected file is not readable.");
        return false;
    }
    long bytes = vino_fs_read(descriptor, file_preview, FILE_PREVIEW_SIZE - 1);
    (void)vino_fs_close(descriptor);
    if (bytes < 0) {
        string_copy(file_message, sizeof(file_message), "VFS read failed.");
        return false;
    }
    file_preview_size = (uint32_t)bytes;
    file_preview[file_preview_size] = '\0';
    file_preview_open = true;
    return true;
}

static void set_page(uint32_t page) {
    if (page >= PAGE_COUNT) return;
    if (page == PAGE_SHELL) app_exit();
    file_preview_open = false;
    page_selection = page;
    home_selection = page;
    if (page == PAGE_FILES) files_reload();
    if (page == PAGE_NETWORK) refresh_state();
}

static void gui_command(uint32_t action, const char *id) {
    struct kos_gui_control_request request = {
        .abi_version = KOS_GUI_CONTROL_ABI_VERSION,
        .action = action,
    };
    string_copy(request.gui_id, sizeof(request.gui_id), id);
    long result = vino_gui_control(&request);
    last_gui_result = result < 0 ? (int32_t)result : request.result;
    string_copy(status_message, sizeof(status_message), request.message);
    refresh_state();
}

static void connect_network(void) {
    long result = vino_system_control(KOS_SYSTEM_CONTROL_CONNECT_NETWORK, false);
    if (result == 0) string_copy(status_message, sizeof(status_message),
                                 "Network DHCP request completed.");
    else string_copy(status_message, sizeof(status_message),
                     "Network connection failed; check link or DHCP.");
    refresh_state();
}

static void draw_background(void) {
    (void)vino_gui_clear(color_background);
    uint32_t bands = surface.height / 4;
    if (bands > 48) bands = 48;
    for (uint32_t index = 0; index < bands; ++index) {
        uint32_t y = index * surface.height / bands;
        uint32_t next = (index + 1) * surface.height / bands;
        uint32_t mix = bands == 0 ? 0 : index * 18 / bands;
        struct kos_gui_color shade = {
            (uint8_t)(color_wallpaper.red + mix),
            (uint8_t)(color_wallpaper.green + mix),
            (uint8_t)(color_wallpaper.blue + mix), 0,
        };
        (void)vino_gui_fill((struct kos_gui_rect){ 0, (int32_t)y, surface.width,
            next - y + 1 }, shade);
    }
}

static void draw_header(void) {
    draw_box(0, 0, surface.width, 48, color_panel, color_accent);
    draw_text(18, 13, "VINO", color_accent, color_panel);
    draw_text(68, 13, "MVP  /  RECOVERY", color_text, color_panel);
    const char *net = system_info.network_available
        && system_info.ipv4_address != 0 ? "ONLINE" : "OFFLINE";
    uint32_t net_x = surface.width > 430 ? surface.width - 92 : surface.width - 78;
    draw_text((int32_t)net_x, 13, net, color_cyan, color_panel);
}

static void draw_footer(const char *help) {
    draw_rule(16, (int32_t)surface.height - 29, surface.width - 32);
    draw_text(18, (int32_t)surface.height - 20, help, color_muted, color_background);
}

static void draw_home(void) {
    bool compact = is_compact();
    if (compact) {
        char line[64] = "Network: ";
        string_append(line, sizeof(line), system_info.network_available
            && system_info.ipv4_address != 0 ? "connected" : "not connected");
        draw_text(18, 58, line, color_cyan, color_background);
        draw_text(18, 74, "Recovery tools are separate from Default.",
                  color_muted, color_background);
        uint32_t row_y = 92;
        for (uint32_t index = 1; index < PAGE_COUNT; ++index) {
            bool selected = home_selection == index;
            draw_box(16, (int32_t)row_y, surface.width - 32, 15,
                selected ? color_panel_high : color_panel,
                selected ? color_accent : color_panel);
            char label[48];
            label[0] = (char)('0' + index);
            label[1] = ' ';
            string_copy(label + 2, sizeof(label) - 2, page_names[index]);
            draw_text(24, (int32_t)row_y + 5, label,
                      selected ? color_text : color_muted,
                      selected ? color_panel_high : color_panel);
            row_y += 16;
        }
    }
    else {
        uint32_t left_width = 205;
        uint32_t card_x = left_width + 32;
        draw_box(20, 70, left_width, surface.height - 116,
                 color_panel, color_accent);
        draw_text(36, 84, "RECOVERY MENU", color_cyan, color_panel);
        uint32_t row_y = 112;
        for (uint32_t index = 1; index < PAGE_COUNT; ++index) {
            bool selected = home_selection == index;
            draw_box(28, (int32_t)row_y, left_width - 16, 36,
                selected ? color_panel_high : color_panel,
                selected ? color_accent : color_panel);
            char label[56];
            label[0] = (char)('0' + index);
            label[1] = ' ';
            string_copy(label + 2, sizeof(label) - 2, page_names[index]);
            draw_text(38, (int32_t)row_y + 13, label,
                      selected ? color_text : color_muted,
                      selected ? color_panel_high : color_panel);
            row_y += 43;
        }
        uint32_t card_width = surface.width - card_x - 20;
        draw_box((int32_t)card_x, 70, card_width, surface.height - 116,
                 color_panel, color_accent);
        draw_text((int32_t)card_x + 20, 92, "A small native rescue environment",
                  color_text, color_panel);
        draw_rule((int32_t)card_x + 20, 116, card_width - 40);
        draw_text((int32_t)card_x + 20, 138,
                  "Open CLI, browse VFS, repair GUI packages, or retry Default.",
                  color_muted, color_panel);
        draw_text((int32_t)card_x + 20, 166,
                  "MVP is its own package; it does not load the Default desktop.",
                  color_muted, color_panel);
        char volume[64] = "System volume: ";
        if (system_info.system_volume_letter >= 'C') {
            char letter[2] = { system_info.system_volume_letter, '\0' };
            string_append(volume, sizeof(volume), letter);
            string_append(volume, sizeof(volume), ":");
        }
        else string_append(volume, sizeof(volume), "live / RAM");
        draw_text((int32_t)card_x + 20, 202, volume, color_cyan, color_panel);
        char package_line[72] = "Default ";
        string_append(package_line, sizeof(package_line),
            gui_state.installed_mask & KOS_GUI_INSTALLED_DEFAULT ? "installed" : "not installed");
        string_append(package_line, sizeof(package_line), "   MVP ");
        string_append(package_line, sizeof(package_line),
            gui_state.installed_mask & KOS_GUI_INSTALLED_MVP ? "installed" : "not installed");
        draw_text((int32_t)card_x + 20, 230, package_line, color_muted, color_panel);
        if (status_message[0] != '\0')
            draw_text((int32_t)card_x + 20, 258, status_message, color_cyan, color_panel);
    }
    draw_footer("1-7 open tool   J/K select   Enter open   C CLI shell");
}

static void draw_page_frame(const char *title, const char *subtitle) {
    uint32_t x = 18;
    uint32_t y = is_compact() ? 58 : 70;
    draw_box((int32_t)x, (int32_t)y, surface.width - 36,
             surface.height - y - 42, color_panel, color_accent);
    draw_text((int32_t)x + 16, (int32_t)y + 14, title, color_text, color_panel);
    draw_text((int32_t)x + 16, (int32_t)y + 33, subtitle, color_muted, color_panel);
    draw_rule((int32_t)x + 16, (int32_t)y + 54, surface.width - 68);
}

static void draw_network_page(void) {
    draw_page_frame("Network", "Connect through the kernel network service; status is live.");
    int32_t y = is_compact() ? 122 : 145;
    char line[80];
    string_copy(line, sizeof(line), "Interface: ");
    string_append(line, sizeof(line), system_info.device_name[0]
        ? system_info.device_name : "unavailable");
    draw_text(38, y, line, color_cyan, color_panel); y += 22;
    string_copy(line, sizeof(line), "DHCP: ");
    string_append(line, sizeof(line), system_info.dhcp_state == 3 ? "lease active"
        : system_info.dhcp_state == 4 ? "failed" : "idle / working");
    draw_text(38, y, line, color_text, color_panel); y += 22;
    char ip[16];
    format_ipv4(system_info.ipv4_address, ip);
    string_copy(line, sizeof(line), "IPv4: "); string_append(line, sizeof(line),
        system_info.ipv4_address == 0 ? "not assigned" : ip);
    draw_text(38, y, line, color_text, color_panel); y += 22;
    if (!is_compact()) {
        format_ipv4(system_info.gateway, ip);
        string_copy(line, sizeof(line), "Gateway: "); string_append(line, sizeof(line), ip);
        draw_text(38, y, line, color_muted, color_panel); y += 22;
    }
    draw_text(38, y, "Press C to request/renew a DHCP lease.", color_cyan, color_panel);
    if (status_message[0] != '\0')
        draw_text(38, y + 20, status_message, color_muted, color_panel);
    draw_footer("C connect   R refresh   Esc home");
}

static void draw_files_page(void) {
    draw_page_frame("File manager", "Read-only VFS browser. No file writes from this UI.");
    draw_text(38, is_compact() ? 119 : 142, file_directory, color_cyan, color_panel);
    if (file_preview_open) {
        draw_box(34, is_compact() ? 137 : 162, surface.width - 68,
                 surface.height - (is_compact() ? 184 : 210), color_background, color_accent);
        uint32_t limit = file_preview_size;
        if (limit > 240) limit = 240;
        uint32_t row = 0;
        uint32_t column = 0;
        uint32_t max_rows = is_compact() ? 3 : 5;
        uint32_t line_limit = surface.width < 500 ? 36 : 72;
        char line[80];
        line[0] = '\0';
        for (uint32_t index = 0; index < limit && row < max_rows; ++index) {
            char character = file_preview[index];
            if (character == '\r') continue;
            if (character == '\n' || column >= line_limit) {
                line[column] = '\0';
                draw_text(42, (is_compact() ? 143 : 170) + (int32_t)row * 14,
                          line, color_text, color_background);
                ++row;
                column = 0;
                line[0] = '\0';
                if (character == '\n') continue;
            }
            if ((uint8_t)character < 32 || (uint8_t)character >= 127) character = '?';
            line[column++] = character;
        }
        if (column != 0 && row < max_rows) {
            line[column] = '\0';
            draw_text(42, (is_compact() ? 143 : 170) + (int32_t)row * 14,
                      line, color_text, color_background);
        }
    }
    else {
        uint32_t visible = is_compact() ? 5 : 9;
        uint32_t row_height = is_compact() ? 14 : 20;
        uint32_t base_y = is_compact() ? 138 : 164;
        for (uint32_t row = 0; row < visible; ++row) {
            uint32_t index = file_scroll + row;
            if (index >= file_entry_count) break;
            bool selected = index == file_selection;
            if (selected) draw_box(34, (int32_t)(base_y + row * row_height),
                surface.width - 68, row_height, color_panel_high, color_accent);
            char line[64];
            string_copy(line, sizeof(line),
                file_entries[index].type == KOS_STAT_DIRECTORY ? "[DIR] " : "       ");
            string_append(line, sizeof(line), file_entries[index].name);
            draw_text(42, (int32_t)(base_y + row * row_height + 4), line,
                selected ? color_text : color_muted,
                selected ? color_panel_high : color_panel);
        }
        if (file_entry_count == 0) draw_text(42, (int32_t)base_y,
            "Folder is empty.", color_muted, color_panel);
    }
    if (file_message[0] != '\0')
        draw_text(38, (int32_t)surface.height - 48, file_message,
                  color_cyan, color_panel);
    draw_footer(file_preview_open ? "Esc close preview   B back to folder"
                                  : "J/K select   Enter open   B parent   R reload   Esc home");
}

static void draw_install_page(void) {
    draw_page_frame("Install local GUI", "Packages are verified and installed by the kernel package manager.");
    bool compact = is_compact();
    int32_t y = compact ? 121 : 151;
    int32_t spacing = compact ? 33 : 43;
    uint32_t row_height = compact ? 28 : 34;
    const char *names[2] = { "Default desktop", "MVP recovery" };
    for (uint32_t index = 0; index < 2; ++index) {
        bool selected = selected_gui == index;
        uint32_t installed = index == 0 ? KOS_GUI_INSTALLED_DEFAULT : KOS_GUI_INSTALLED_MVP;
        draw_box(34, y + (int32_t)index * spacing, surface.width - 68, row_height,
            selected ? color_panel_high : color_panel,
            selected ? color_accent : color_panel);
        char line[72];
        string_copy(line, sizeof(line), names[index]);
        string_append(line, sizeof(line), gui_state.installed_mask & installed
            ? "  [installed]" : "  [available on ISO]");
        draw_text(44, y + (int32_t)index * spacing + (compact ? 10 : 12), line,
            selected ? color_text : color_muted,
            selected ? color_panel_high : color_panel);
    }
    draw_text(38, y + spacing * 2 + 5, "I install / repair   S set Default for next boot",
              color_cyan, color_panel);
    if (!compact)
        draw_text(38, y + spacing * 2 + 28, status_message, color_muted, color_panel);
    draw_footer("D/M choose package   I install or repair   S set Default   Esc home");
}

static void draw_remote_page(void) {
    draw_page_frame("Remote GUI", "Secure remote package installation requires a trusted catalog.");
    int32_t y = is_compact() ? 119 : 150;
    draw_text(38, y, selected_gui == 0 ? "Target package: Default (D)" : "Target package: MVP (M)",
              color_cyan, color_panel);
    draw_text(38, y + 18, "Remote installation is pending configuration.", color_text, color_panel);
    draw_text(38, y + 34, "No remote GUI repository, transport, or signing key is configured.",
              color_text, color_panel);
    draw_text(38, y + 50, "The TLS verifier has no CA trust root configured.",
              color_muted, color_panel);
    draw_text(38, y + 66, "MVP refuses remote executables until trust is configured.",
              color_muted, color_panel);
    draw_text(38, y + 82, status_message, color_cyan, color_panel);
    draw_footer("D/M choose package   R check policy   Esc home");
}

static void draw_recovery_page(void) {
    draw_page_frame("Recovery", "MVP remains available if the Default session cannot start.");
    int32_t y = is_compact() ? 120 : 151;
    draw_text(38, y, gui_state.installed_mask & KOS_GUI_INSTALLED_DEFAULT
        ? "Default package is installed." : "Default package is not installed.",
        color_text, color_panel);
    draw_text(38, y + 24, "R reinstalls Default from the local verified package.",
              color_cyan, color_panel);
    draw_text(38, y + 42, "T retries Default now; the kernel falls back here on failure.",
              color_muted, color_panel);
    draw_text(38, y + 62, status_message, color_cyan, color_panel);
    draw_footer("R repair Default   T retry Default   C open CLI shell   Esc home");
}

static void draw_power_page(void) {
    draw_page_frame("Restart / shutdown", "Every power action syncs block-cache data first.");
    int32_t y = is_compact() ? 120 : 151;
    draw_text(38, y, power_selection == 0 ? "> Restart" : "  Restart",
              power_selection == 0 ? color_cyan : color_muted, color_panel);
    draw_text(38, y + 24, power_selection == 1 ? "> Shutdown (halt)" : "  Shutdown (halt)",
              power_selection == 1 ? color_cyan : color_muted, color_panel);
    draw_text(38, y + 50, power_confirmation == 0
        ? "Choose action with J/K; press Enter, then Y to confirm."
        : "Confirm power action? Press Y to continue or N to cancel.",
        color_text, color_panel);
    draw_text(38, y + 68, "Shutdown halts CPUs; firmware power-off is not implemented yet.",
              color_muted, color_panel);
    draw_footer("J/K choose   Enter continue   Y confirm   N cancel   Esc home");
}

static void render(void) {
    draw_background();
    draw_header();
    switch (page_selection) {
        case PAGE_HOME: draw_home(); break;
        case PAGE_NETWORK: draw_network_page(); break;
        case PAGE_FILES: draw_files_page(); break;
        case PAGE_INSTALL: draw_install_page(); break;
        case PAGE_REMOTE: draw_remote_page(); break;
        case PAGE_RECOVERY: draw_recovery_page(); break;
        case PAGE_POWER: draw_power_page(); break;
        default: draw_home(); break;
    }
}

static void update_file_selection(void) {
    uint32_t visible = is_compact() ? 5 : 9;
    if (file_selection < file_scroll) file_scroll = file_selection;
    else if (file_selection >= file_scroll + visible)
        file_scroll = file_selection - visible + 1;
}

static void handle_file_key(char key) {
    if (file_preview_open) {
        if (key == 27 || key == 'b') file_preview_open = false;
        return;
    }
    if (key == 'j' && file_selection + 1 < file_entry_count) ++file_selection;
    else if (key == 'k' && file_selection > 0) --file_selection;
    else if (key == 'b' || key == '\b') {
        path_parent(file_directory);
        files_reload();
    }
    else if (key == 'r') files_reload();
    else if (key == '\n') (void)enter_file();
    update_file_selection();
}

static void repair_selected_gui(void) {
    const char *id = selected_gui == 0 ? "default" : "mvp";
    gui_command(KOS_GUI_CONTROL_REINSTALL_LOCAL, id);
    if (selected_gui == 0 && last_gui_result == 0)
        gui_command(KOS_GUI_CONTROL_SET_DEFAULT, "default");
}

static void confirm_power_action(void) {
    uint32_t action = power_selection == 0 ? KOS_SYSTEM_CONTROL_RESTART
                                           : KOS_SYSTEM_CONTROL_SHUTDOWN;
    long result = vino_system_control(action, true);
    string_copy(status_message, sizeof(status_message), result == 0
        ? "Power request completed." : "Power request failed; storage was not shut down.");
    power_confirmation = 0;
}

static void handle_key(char key) {
    if (page_selection == PAGE_FILES) {
        if (key == 27 && !file_preview_open) {
            page_selection = PAGE_HOME;
            return;
        }
        handle_file_key(key);
        return;
    }
    if (page_selection == PAGE_POWER && power_confirmation != 0) {
        if (key == 'y' || key == 'Y') confirm_power_action();
        else if (key == 'n' || key == 'N' || key == 27) power_confirmation = 0;
        return;
    }
    if (key == 27 || key == '0') {
        page_selection = PAGE_HOME;
        file_preview_open = false;
        return;
    }
    if (page_selection == PAGE_HOME) {
        if (key == 'c' || key == 'C') app_exit();
        if (key >= '1' && key < '0' + PAGE_COUNT) {
            set_page((uint32_t)(key - '0'));
            return;
        }
        if (key == 'j' && home_selection + 1 < PAGE_COUNT) ++home_selection;
        else if (key == 'k' && home_selection > 1) --home_selection;
        else if (key == '\n') set_page(home_selection);
        return;
    }
    switch (page_selection) {
        case PAGE_NETWORK:
            if (key == 'c' || key == 'C') connect_network();
            else if (key == 'r') refresh_state();
            break;
        case PAGE_INSTALL:
            if (key == 'd' || key == 'D') selected_gui = 0;
            else if (key == 'm' || key == 'M') selected_gui = 1;
            else if (key == 'j' || key == 'k') selected_gui = 1 - selected_gui;
            else if (key == 'i' || key == 'I') repair_selected_gui();
            else if ((key == 's' || key == 'S') && selected_gui == 0)
                gui_command(KOS_GUI_CONTROL_SET_DEFAULT, "default");
            break;
        case PAGE_REMOTE: {
            if (key == 'd' || key == 'D') selected_gui = 0;
            else if (key == 'm' || key == 'M') selected_gui = 1;
            else if (key == 'j' || key == 'k') selected_gui = 1 - selected_gui;
            else if (key == 'r' || key == 'R')
                gui_command(KOS_GUI_CONTROL_INSTALL_REMOTE,
                            selected_gui == 0 ? "default" : "mvp");
            break;
        }
        case PAGE_RECOVERY:
            if (key == 'r' || key == 'R') repair_selected_gui();
            else if (key == 't' || key == 'T') {
                gui_command(KOS_GUI_CONTROL_RECOVER_DEFAULT, "default");
                if (last_gui_result == 0) app_exit();
            }
            else if (key == 'c' || key == 'C') app_exit();
            break;
        case PAGE_POWER:
            if (key == 'j' || key == 'k') power_selection = 1 - power_selection;
            else if (key == '\n') power_confirmation = 1;
            break;
        default:
            break;
    }
}

__attribute__((noreturn)) void _start(void) {
    surface.abi_version = KOS_GUI_ABI_VERSION;
    if (vino_gui_surface_get(&surface) < 0 || surface.width < 320
        || surface.height < 240) app_exit();
    refresh_state();
    initialize_paths();
    last_refresh = uptime_seconds();
    render();
    for (;;) {
        struct kos_gui_input_event event = { .abi_version = KOS_GUI_ABI_VERSION };
        if (vino_gui_input_read(&event) == 0 && event.type == KOS_GUI_INPUT_KEY) {
            handle_key((char)event.key);
            render();
        }
        uint64_t now = uptime_seconds();
        if (now != last_refresh) {
            refresh_state();
            render();
            last_refresh = now;
        }
        (void)sleep_ms(30);
    }
}
