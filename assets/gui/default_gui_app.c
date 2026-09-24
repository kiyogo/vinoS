#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/kos/syscall_numbers.h>
#include <vino/gui.h>
#include <vino/system.h>
#include <vino/vfs.h>

enum {
    APP_TERMINAL = 0,
    APP_FILES,
    APP_SETTINGS,
    APP_NETWORK,
    APP_COUNT,
    WINDOW_TITLE_HEIGHT = 38,
    TERMINAL_LINE_COUNT = 13,
    TERMINAL_LINE_LENGTH = 96,
    FILE_ENTRY_COUNT = 32,
    FILE_PATH_LENGTH = 256,
    PREVIEW_SIZE = 768,
};

static const char *const app_names[APP_COUNT] = {
    "Terminal", "Files", "Settings", "Network",
};
static const char *const app_symbols[APP_COUNT] = {
    ">_", "<>", "*", "~",
};

static struct kos_gui_surface surface;
static struct kos_system_info system_info;
static struct kos_gui_color color_wallpaper = { 10, 18, 36, 0 };
static struct kos_gui_color color_wallpaper_light = { 20, 48, 78, 0 };
static struct kos_gui_color color_panel = { 17, 31, 53, 0 };
static struct kos_gui_color color_panel_high = { 24, 42, 68, 0 };
static struct kos_gui_color color_panel_low = { 12, 24, 43, 0 };
static struct kos_gui_color color_accent = { 91, 194, 255, 0 };
static struct kos_gui_color color_cyan = { 135, 226, 245, 0 };
static struct kos_gui_color color_text = { 240, 247, 255, 0 };
static struct kos_gui_color color_muted = { 157, 178, 204, 0 };
static struct kos_gui_color color_dim = { 91, 110, 139, 0 };
static struct kos_gui_color color_shadow = { 5, 10, 22, 0 };

struct vino_window {
    bool open;
    bool minimized;
    bool maximized;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    int32_t restore_x;
    int32_t restore_y;
    uint32_t restore_width;
    uint32_t restore_height;
    uint32_t z_order;
};

static struct vino_window windows[APP_COUNT];
static struct kos_dirent file_entries[FILE_ENTRY_COUNT];
static uint32_t file_entry_count;
static uint32_t file_selected;
static uint32_t file_scroll;
static char file_directory[FILE_PATH_LENGTH];
static char file_error[96];
static char file_preview[PREVIEW_SIZE];
static uint32_t file_preview_size;
static bool file_preview_open;
static char terminal_lines[TERMINAL_LINE_COUNT][TERMINAL_LINE_LENGTH];
static uint32_t terminal_line_count;
static char terminal_input[TERMINAL_LINE_LENGTH];
static uint32_t terminal_input_size;
static char terminal_directory[FILE_PATH_LENGTH];
static int32_t active_app = -1;
static bool start_menu_open;
static bool context_menu_open;
static bool about_dialog_open;
static bool move_window_mode;
static uint32_t menu_selected;
static uint32_t context_selected;
static uint32_t menu_animation_step;
static uint32_t theme_variant;
static bool reduced_motion;
static bool compact_panels;
static uint64_t last_redraw_ms;
static uint32_t animation_frame;
static char text_scratch[KOS_GUI_TEXT_MAX];

static void open_app(uint32_t app);

static long sys_sleep(uint32_t milliseconds) {
    register uint64_t result __asm__("rax") = SYS_sleep;
    register uint64_t duration __asm__("rdi") = milliseconds;
    __asm__ volatile("syscall" : "+a"(result) : "D"(duration) : "rcx", "r11", "memory");
    return (long)result;
}

__attribute__((noreturn)) static void app_exit_with_status(uint64_t exit_status) {
    register uint64_t result __asm__("rax") = SYS_exit;
    register uint64_t status __asm__("rdi") = exit_status;
    __asm__ volatile("syscall" : "+a"(result) : "D"(status) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

__attribute__((noreturn)) static void app_exit(void) {
    app_exit_with_status(0);
}

static uint64_t sys_gettime(void) {
    register uint64_t result __asm__("rax") = SYS_gettime;
    __asm__ volatile("syscall" : "+a"(result) : : "rcx", "r11", "memory");
    return result;
}

static uint32_t string_length(const char *text) {
    uint32_t length = 0;
    if (text != NULL) while (text[length] != '\0') ++length;
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

static bool string_equal_fold(const char *left, const char *right) {
    if (left == NULL || right == NULL) return false;
    uint32_t index = 0;
    while (left[index] != '\0' && right[index] != '\0') {
        char left_character = left[index];
        char right_character = right[index];
        if (left_character >= 'A' && left_character <= 'Z')
            left_character = (char)(left_character - 'A' + 'a');
        if (right_character >= 'A' && right_character <= 'Z')
            right_character = (char)(right_character - 'A' + 'a');
        if (left_character != right_character) return false;
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
        reversed[count++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(reversed));
    uint32_t index = 0;
    while (count != 0) output[index++] = reversed[--count];
    output[index] = '\0';
}

static void color_set_accent(uint32_t variant) {
    if (variant == 1) color_accent = (struct kos_gui_color){ 90, 222, 205, 0 };
    else if (variant == 2) color_accent = (struct kos_gui_color){ 177, 156, 255, 0 };
    else color_accent = (struct kos_gui_color){ 91, 194, 255, 0 };
}

static void draw_text(int32_t x, int32_t y, const char *text,
                      struct kos_gui_color foreground,
                      struct kos_gui_color background) {
    string_copy(text_scratch, sizeof(text_scratch), text);
    for (uint32_t index = 0; text_scratch[index] != '\0'; ++index) {
        if ((uint8_t)text_scratch[index] < 32 || (uint8_t)text_scratch[index] >= 127)
            text_scratch[index] = '?';
    }
    (void)vino_gui_text(x, y, text_scratch, 1, foreground, background);
}

static void draw_text_scale(int32_t x, int32_t y, const char *text, uint32_t scale,
                            struct kos_gui_color foreground,
                            struct kos_gui_color background) {
    string_copy(text_scratch, sizeof(text_scratch), text);
    for (uint32_t index = 0; text_scratch[index] != '\0'; ++index) {
        if ((uint8_t)text_scratch[index] < 32 || (uint8_t)text_scratch[index] >= 127)
            text_scratch[index] = '?';
    }
    (void)vino_gui_text(x, y, text_scratch, scale, foreground, background);
}

static void draw_box(struct kos_gui_rect rect, struct kos_gui_color fill,
                     struct kos_gui_color border) {
    if (rect.width == 0 || rect.height == 0) return;
    (void)vino_gui_fill(rect, fill);
    (void)vino_gui_border(rect, border);
}

static void draw_dot(int32_t x, int32_t y, uint32_t size,
                     struct kos_gui_color color) {
    (void)vino_gui_fill((struct kos_gui_rect){ x, y, size, size }, color);
}

static void append_uptime(char *line, uint32_t capacity, uint64_t seconds) {
    char digits[24];
    uint64_t hours = seconds / 3600;
    uint64_t minutes = (seconds / 60) % 60;
    number_text(hours, digits);
    string_append(line, capacity, digits);
    string_append(line, capacity, "h ");
    number_text(minutes, digits);
    if (minutes < 10) string_append(line, capacity, "0");
    string_append(line, capacity, digits);
    string_append(line, capacity, "m");
}

static void format_ipv4(uint32_t address, char output[16]) {
    uint8_t octets[4] = {
        (uint8_t)(address >> 24), (uint8_t)(address >> 16),
        (uint8_t)(address >> 8), (uint8_t)address,
    };
    output[0] = '\0';
    char number[24];
    for (uint32_t index = 0; index < 4; ++index) {
        number_text(octets[index], number);
        string_append(output, 16, number);
        if (index != 3) string_append(output, 16, ".");
    }
}

static void format_mac(char output[18]) {
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t index = 0; index < 6; ++index) {
        output[index * 3] = hex[system_info.mac_address[index] >> 4];
        output[index * 3 + 1] = hex[system_info.mac_address[index] & 15];
        if (index != 5) output[index * 3 + 2] = ':';
    }
    output[17] = '\0';
}

static const char *dhcp_state_name(uint32_t state) {
    switch (state) {
        case 1: return "Discovering";
        case 2: return "Requesting lease";
        case 3: return "Lease active";
        case 4: return "DHCP failed";
        default: return "Idle / static";
    }
}

static void refresh_system_info(void) {
    if (vino_system_info_get(&system_info) < 0) {
        system_info.network_available = 0;
    }
}

static bool path_exists(const char *path, uint32_t expected_type) {
    struct kos_stat metadata = {0};
    return vino_fs_stat(path, &metadata) == 0 && metadata.type == expected_type;
}

static void path_parent(char path[FILE_PATH_LENGTH]) {
    uint32_t length = string_length(path);
    while (length > 3 && path[length - 1] == '/') path[--length] = '\0';
    if (length <= 3) return;
    while (length > 3 && path[length - 1] != '/') --length;
    if (length < 3) length = 3;
    path[length] = '\0';
}

static bool path_join(char output[FILE_PATH_LENGTH], const char *base,
                      const char *leaf) {
    if (base == NULL || leaf == NULL || output == NULL) return false;
    uint32_t base_length = string_length(base);
    uint32_t leaf_length = string_length(leaf);
    if (base_length + leaf_length + 2 > FILE_PATH_LENGTH) return false;
    string_copy(output, FILE_PATH_LENGTH, base);
    if (base_length != 0 && output[base_length - 1] != '/') {
        output[base_length++] = '/';
        output[base_length] = '\0';
    }
    string_copy(output + base_length, FILE_PATH_LENGTH - base_length, leaf);
    return true;
}

static void terminal_add_line(const char *line) {
    if (terminal_line_count == TERMINAL_LINE_COUNT) {
        for (uint32_t index = 1; index < TERMINAL_LINE_COUNT; ++index) {
            string_copy(terminal_lines[index - 1], TERMINAL_LINE_LENGTH,
                        terminal_lines[index]);
        }
        --terminal_line_count;
    }
    string_copy(terminal_lines[terminal_line_count], TERMINAL_LINE_LENGTH, line);
    ++terminal_line_count;
}

static void terminal_add_text(const char *text) {
    char line[TERMINAL_LINE_LENGTH];
    uint32_t line_size = 0;
    if (text == NULL) return;
    for (uint32_t index = 0; text[index] != '\0'; ++index) {
        char character = text[index];
        if (character == '\r') continue;
        if (character == '\n' || line_size + 1 >= sizeof(line)) {
            line[line_size] = '\0';
            terminal_add_line(line);
            line_size = 0;
            if (character == '\n') continue;
        }
        if (((uint8_t)character >= 32 && (uint8_t)character < 127) || character == '\t') {
            line[line_size++] = character == '\t' ? ' ' : character;
        }
    }
    if (line_size != 0) {
        line[line_size] = '\0';
        terminal_add_line(line);
    }
}

static void terminal_init(void) {
    terminal_line_count = 0;
    terminal_input_size = 0;
    terminal_input[0] = '\0';
    terminal_add_line("Vino OS Default  |  native shell");
    terminal_add_line("Type help for commands. Esc returns to the desktop.");
}

static void initialize_paths(void) {
    char volume = system_info.system_volume_letter;
    if (volume < 'C' || volume > 'Z') volume = 'D';
    terminal_directory[0] = volume;
    terminal_directory[1] = ':';
    terminal_directory[2] = '/';
    terminal_directory[3] = '\0';
    char candidate[FILE_PATH_LENGTH];
    if (path_join(candidate, terminal_directory, "VINO")
        && path_exists(candidate, KOS_STAT_DIRECTORY)) {
        string_copy(terminal_directory, sizeof(terminal_directory), candidate);
    }
    string_copy(file_directory, sizeof(file_directory), terminal_directory);
}

static bool list_directory(const char *path, struct kos_dirent *entries,
                           uint32_t capacity, uint32_t *count_out) {
    if (path == NULL || entries == NULL || count_out == NULL || capacity == 0) return false;
    *count_out = 0;
    long descriptor = vino_fs_open(path, KOS_O_RDONLY | KOS_O_DIRECTORY);
    if (descriptor < 0) return false;
    long bytes = vino_fs_getdents(descriptor, entries,
                                  (uint64_t)capacity * sizeof(entries[0]));
    (void)vino_fs_close(descriptor);
    if (bytes < 0) return false;
    uint64_t record_size = sizeof(entries[0]);
    *count_out = (uint32_t)((uint64_t)bytes / record_size);
    if (*count_out > capacity) *count_out = capacity;
    return true;
}

static void files_reload(void) {
    file_entry_count = 0;
    file_selected = 0;
    file_scroll = 0;
    file_error[0] = '\0';
    if (!list_directory(file_directory, file_entries, FILE_ENTRY_COUNT,
                        &file_entry_count)) {
        string_copy(file_error, sizeof(file_error), "Cannot open this VFS directory.");
    }
}

static bool files_enter_selected(void) {
    if (file_selected >= file_entry_count) return false;
    struct kos_dirent *entry = &file_entries[file_selected];
    if (entry->name_length == 0
        || (entry->name[0] == '.'
            && (entry->name_length == 1
                || (entry->name_length == 2 && entry->name[1] == '.')))) return false;
    char target[FILE_PATH_LENGTH];
    if (!path_join(target, file_directory, entry->name)) return false;
    if (entry->type == KOS_STAT_DIRECTORY) {
        string_copy(file_directory, sizeof(file_directory), target);
        files_reload();
        return true;
    }
    long descriptor = vino_fs_open(target, KOS_O_RDONLY);
    if (descriptor < 0) {
        string_copy(file_error, sizeof(file_error), "Cannot open selected file.");
        return false;
    }
    long bytes = vino_fs_read(descriptor, file_preview, PREVIEW_SIZE - 1);
    (void)vino_fs_close(descriptor);
    if (bytes < 0) {
        string_copy(file_error, sizeof(file_error), "VFS read failed for selected file.");
        return false;
    }
    file_preview_size = (uint32_t)bytes;
    file_preview[file_preview_size] = '\0';
    file_preview_open = true;
    return true;
}

static void terminal_list_path(const char *path) {
    struct kos_dirent entries[12];
    uint32_t count = 0;
    if (!list_directory(path, entries, 12, &count)) {
        terminal_add_line("ls: cannot read directory");
        return;
    }
    if (count == 0) terminal_add_line("(empty directory)");
    for (uint32_t index = 0; index < count; ++index) {
        char line[TERMINAL_LINE_LENGTH];
        string_copy(line, sizeof(line),
                    entries[index].type == KOS_STAT_DIRECTORY ? "[dir] " : "      ");
        string_append(line, sizeof(line), entries[index].name);
        terminal_add_line(line);
    }
}

static void terminal_cat_path(const char *path) {
    long descriptor = vino_fs_open(path, KOS_O_RDONLY);
    if (descriptor < 0) {
        terminal_add_line("cat: file not found or not readable");
        return;
    }
    char buffer[641];
    long bytes = vino_fs_read(descriptor, buffer, sizeof(buffer) - 1);
    (void)vino_fs_close(descriptor);
    if (bytes < 0) {
        terminal_add_line("cat: VFS read failed");
        return;
    }
    buffer[(uint32_t)bytes] = '\0';
    terminal_add_text(buffer);
    if (bytes == 0) terminal_add_line("(empty file)");
    else if (bytes == (long)(sizeof(buffer) - 1))
        terminal_add_line("[preview may be truncated]");
}

static void terminal_show_network(void) {
    refresh_system_info();
    if (!system_info.network_available) {
        terminal_add_line("Network interface: unavailable");
        return;
    }
    char ip[16];
    format_ipv4(system_info.ipv4_address, ip);
    char line[TERMINAL_LINE_LENGTH] = "IPv4: ";
    string_append(line, sizeof(line), ip);
    terminal_add_line(line);
    line[0] = '\0';
    string_copy(line, sizeof(line), "Interface: ");
    string_append(line, sizeof(line), system_info.device_name[0]
        ? system_info.device_name : "eth0");
    string_append(line, sizeof(line), "  |  ");
    string_append(line, sizeof(line), dhcp_state_name(system_info.dhcp_state));
    terminal_add_line(line);
}

static void terminal_execute(void) {
    char command[TERMINAL_LINE_LENGTH];
    string_copy(command, sizeof(command), terminal_input);
    char prompt[TERMINAL_LINE_LENGTH];
    string_copy(prompt, sizeof(prompt), "> ");
    string_append(prompt, sizeof(prompt), command);
    terminal_add_line(prompt);
    terminal_input_size = 0;
    terminal_input[0] = '\0';
    if (command[0] == '\0') return;

    char *argument = command;
    while (*argument != '\0' && *argument != ' ') ++argument;
    if (*argument != '\0') {
        *argument++ = '\0';
        while (*argument == ' ') ++argument;
    }
    if (string_equal(command, "help")) {
        terminal_add_line("help clear pwd ls [path] cd [path] cat <file>");
        terminal_add_line("uptime network open <app> gui status exit");
    }
    else if (string_equal(command, "clear")) terminal_line_count = 0;
    else if (string_equal(command, "pwd")) terminal_add_line(terminal_directory);
    else if (string_equal(command, "uptime")) {
        char line[TERMINAL_LINE_LENGTH] = "Uptime: ";
        append_uptime(line, sizeof(line), sys_gettime());
        terminal_add_line(line);
    }
    else if (string_equal(command, "network")) terminal_show_network();
    else if (string_equal(command, "ls")) {
        char target[FILE_PATH_LENGTH];
        const char *path = terminal_directory;
        if (argument[0] != '\0') {
            if (argument[1] == ':' && argument[2] == '/') path = argument;
            else if (!path_join(target, terminal_directory, argument)) path = "";
            else path = target;
        }
        terminal_list_path(path);
    }
    else if (string_equal(command, "cd")) {
        if (argument[0] == '\0' || string_equal(argument, "..")) {
            if (argument[0] != '\0') path_parent(terminal_directory);
        }
        else {
            char target[FILE_PATH_LENGTH];
            const char *path = argument[1] == ':' && argument[2] == '/'
                ? argument : (path_join(target, terminal_directory, argument) ? target : "");
            if (path_exists(path, KOS_STAT_DIRECTORY)) {
                string_copy(terminal_directory, sizeof(terminal_directory), path);
            }
            else terminal_add_line("cd: directory not found");
        }
    }
    else if (string_equal(command, "cat")) {
        char target[FILE_PATH_LENGTH];
        const char *path = argument;
        if (argument[0] != '\0' && !(argument[1] == ':' && argument[2] == '/')) {
            path = path_join(target, terminal_directory, argument) ? target : "";
        }
        if (path[0] == '\0') terminal_add_line("usage: cat <file>");
        else terminal_cat_path(path);
    }
    else if (string_equal(command, "open")) {
        uint32_t app = APP_COUNT;
        for (uint32_t index = 0; index < APP_COUNT; ++index) {
            if (string_equal_fold(argument, app_names[index])) app = index;
        }
        if (app < APP_COUNT) {
            terminal_add_line("Opening application window...");
            open_app(app);
        }
        else terminal_add_line("open: use Terminal, Files, Settings, or Network");
    }
    else if (string_equal(command, "gui")) {
        if (string_equal(argument, "status")) {
            terminal_add_line(system_info.system_volume_letter
                ? "Default GUI package is selected on the installed Vino volume."
                : "Running in the live CLI / RAM environment.");
        }
        else terminal_add_line("The native Default desktop is the active GUI session.");
    }
    else if (string_equal(command, "echo")) terminal_add_line(argument);
    else if (string_equal(command, "exit")) {
        windows[APP_TERMINAL].open = false;
        active_app = -1;
    }
    else terminal_add_line("Command not found. Type help.");
}

static void window_set_geometry(uint32_t app) {
    struct vino_window *window = &windows[app];
    uint32_t margin_x = surface.width > 80 ? 40 : 8;
    uint32_t margin_y = surface.height > 150 ? 54 : 8;
    window->width = surface.width > 900 ? 760 : surface.width - margin_x * 2;
    window->height = surface.height > 680 ? 500 : surface.height - 142;
    if (window->width < 360) window->width = surface.width > 32 ? surface.width - 32 : surface.width;
    if (window->height < 260) window->height = surface.height > 98 ? surface.height - 98 : surface.height;
    window->x = (int32_t)((surface.width - window->width) / 2) + (int32_t)(app * 22);
    window->y = (int32_t)margin_y + (int32_t)(app * 18);
    if ((uint64_t)window->x + window->width > surface.width - 8) {
        window->x = (int32_t)(surface.width - window->width - 8);
    }
    uint32_t work_bottom = surface.height > 64 ? surface.height - 60 : surface.height;
    if ((uint64_t)window->y + window->height > work_bottom) {
        window->y = (int32_t)(work_bottom - window->height);
    }
    if (window->y < 38) window->y = 38;
}

static void compact_window_order(void) {
    uint32_t order[APP_COUNT];
    uint32_t count = 0;
    for (uint32_t app = 0; app < APP_COUNT; ++app) {
        if (!windows[app].open) continue;
        uint32_t position = count;
        while (position != 0
               && windows[order[position - 1]].z_order > windows[app].z_order) {
            order[position] = order[position - 1];
            --position;
        }
        order[position] = app;
        ++count;
    }
    for (uint32_t index = 0; index < count; ++index)
        windows[order[index]].z_order = index + 1;
}

static void raise_window(uint32_t app) {
    windows[app].z_order = APP_COUNT + 1;
    compact_window_order();
}

static void open_app(uint32_t app) {
    if (app >= APP_COUNT) return;
    struct vino_window *window = &windows[app];
    if (!window->open) {
        window->open = true;
        window->minimized = false;
        window->maximized = false;
        window_set_geometry(app);
        if (app == APP_TERMINAL && terminal_line_count == 0) terminal_init();
        if (app == APP_FILES && file_entry_count == 0) files_reload();
    }
    window->minimized = false;
    raise_window(app);
    active_app = (int32_t)app;
    start_menu_open = false;
    context_menu_open = false;
    move_window_mode = false;
    animation_frame = reduced_motion ? 0 : 3;
}

static void close_active_app(void) {
    if (active_app < 0 || active_app >= APP_COUNT) return;
    windows[active_app].open = false;
    windows[active_app].minimized = false;
    windows[active_app].maximized = false;
    active_app = -1;
    for (uint32_t index = 0; index < APP_COUNT; ++index) {
        if (windows[index].open && !windows[index].minimized
            && (active_app < 0 || windows[index].z_order > windows[active_app].z_order)) {
            active_app = (int32_t)index;
        }
    }
}

static void toggle_maximize(void) {
    if (active_app < 0 || active_app >= APP_COUNT) return;
    struct vino_window *window = &windows[active_app];
    if (!window->maximized) {
        window->restore_x = window->x;
        window->restore_y = window->y;
        window->restore_width = window->width;
        window->restore_height = window->height;
        window->x = 12;
        window->y = 42;
        window->width = surface.width - 24;
        window->height = surface.height - 110;
        window->maximized = true;
    }
    else {
        window->x = window->restore_x;
        window->y = window->restore_y;
        window->width = window->restore_width;
        window->height = window->restore_height;
        window->maximized = false;
    }
}

static void cycle_focus(int direction) {
    int32_t candidate = active_app;
    for (uint32_t attempt = 0; attempt < APP_COUNT; ++attempt) {
        candidate += direction;
        if (candidate < 0) candidate = APP_COUNT - 1;
        if (candidate >= APP_COUNT) candidate = 0;
        if (windows[candidate].open && !windows[candidate].minimized) {
            active_app = candidate;
            raise_window((uint32_t)candidate);
            return;
        }
    }
    active_app = -1;
}

static void draw_wallpaper(void) {
    (void)vino_gui_clear(color_wallpaper);
    uint32_t top_height = surface.height * 2 / 3;
    uint32_t band_height = top_height / 16;
    for (uint32_t band = 0; band < 16; ++band) {
        struct kos_gui_color band_color = {
            (uint8_t)(color_wallpaper.red + (color_wallpaper_light.red - color_wallpaper.red) * band / 16),
            (uint8_t)(color_wallpaper.green + (color_wallpaper_light.green - color_wallpaper.green) * band / 16),
            (uint8_t)(color_wallpaper.blue + (color_wallpaper_light.blue - color_wallpaper.blue) * band / 16),
            0,
        };
        uint32_t y = band * band_height;
        uint32_t height = band == 15 ? top_height - y : band_height;
        if (height != 0) {
            (void)vino_gui_fill((struct kos_gui_rect){ 0, (int32_t)y,
                surface.width, height }, band_color);
        }
    }
    uint32_t glow_width = surface.width / 3;
    if (glow_width > 0 && surface.height > 110) {
        struct kos_gui_color glow = theme_variant == 1
            ? (struct kos_gui_color){ 18, 65, 72, 0 }
            : theme_variant == 2 ? (struct kos_gui_color){ 47, 40, 83, 0 }
                                 : (struct kos_gui_color){ 22, 59, 91, 0 };
        (void)vino_gui_fill((struct kos_gui_rect){
            (int32_t)(surface.width - glow_width), 42, glow_width,
            surface.height - 112 }, glow);
    }
}

static void draw_top_bar(void) {
    struct kos_gui_rect top = { 0, 0, surface.width, 34 };
    (void)vino_gui_fill(top, color_panel_low);
    (void)vino_gui_fill((struct kos_gui_rect){ 0, 33, surface.width, 1 }, color_accent);
    draw_dot(14, 10, 14, color_accent);
    draw_text(19, 13, "V", color_wallpaper, color_accent);
    draw_text_scale(40, 10, "Vino OS", 1, color_text, color_panel_low);
    draw_text(100, 13, surface.width >= 520 ? "Default  /  Native desktop" : "Default",
              color_muted, color_panel_low);
    char status[96] = "up ";
    append_uptime(status, sizeof(status), sys_gettime());
    if (system_info.network_available) string_append(status, sizeof(status), "   ");
    int32_t status_x = (int32_t)surface.width - (int32_t)string_length(status) * 8 - 22;
    if (status_x < 190) status_x = 190;
    draw_text(status_x, 13, status, color_muted, color_panel_low);
    if (system_info.network_available) {
        draw_dot(status_x - 15, 13, 7, (system_info.ipv4_address != 0)
            ? (struct kos_gui_color){ 104, 224, 169, 0 }
            : (struct kos_gui_color){ 240, 190, 96, 0 });
    }
}

static void draw_desktop_icons(void) {
    if (surface.height < 440) {
        draw_text(18, (int32_t)surface.height - 72,
                  "1-4 launch   S start   C menu", color_muted, color_wallpaper);
        return;
    }
    static const uint32_t icon_apps[4] = {
        APP_TERMINAL, APP_FILES, APP_SETTINGS, APP_NETWORK,
    };
    for (uint32_t index = 0; index < 4; ++index) {
        int32_t x = 26;
        int32_t y = 64 + (int32_t)index * 84;
        struct kos_gui_rect icon = { x, y, 144, 66 };
        (void)vino_gui_fill(icon, color_panel);
        (void)vino_gui_border(icon, color_dim);
        draw_box((struct kos_gui_rect){ x + 9, y + 10, 42, 42 }, color_panel_high,
                 color_accent);
        draw_text(x + 18, y + 26, app_symbols[icon_apps[index]], color_accent,
                  color_panel_high);
        draw_text(x + 60, y + 17, app_names[icon_apps[index]], color_text, color_panel);
        char shortcut[8] = { (char)('1' + index), '\0' };
        draw_text(x + 60, y + 39, shortcut, color_muted, color_panel);
    }
    if (surface.width < 600) return;
    struct kos_gui_rect info_card = {
        (int32_t)surface.width - 270, 64, 238, 88,
    };
    draw_box(info_card, color_panel, color_dim);
    draw_text(info_card.x + 16, info_card.y + 14, "WELCOME TO VINO OS",
              color_cyan, color_panel);
    draw_text(info_card.x + 16, info_card.y + 38, "A quiet space for your work.",
              color_text, color_panel);
    draw_text(info_card.x + 16, info_card.y + 62, "Choose an app from the dock below.",
              color_muted, color_panel);
}

static void draw_dock(void) {
    uint32_t bar_height = 58;
    int32_t bar_y = (int32_t)(surface.height - bar_height);
    (void)vino_gui_fill((struct kos_gui_rect){ 0, bar_y, surface.width, bar_height },
                        color_panel_low);
    (void)vino_gui_fill((struct kos_gui_rect){ 0, bar_y, surface.width, 1 }, color_dim);

    uint32_t start_width = surface.width >= 520 ? 102 : 44;
    struct kos_gui_rect start = { 10, bar_y + 7, start_width, 44 };
    struct kos_gui_color start_fill = start_menu_open ? color_panel_high : color_panel_low;
    draw_box(start, start_fill, start_menu_open ? color_accent : color_dim);
    draw_dot(start.x + 8, start.y + 12, 18, color_accent);
    draw_text(start.x + 14, start.y + 17, "V", color_wallpaper, color_accent);
    if (start_width >= 90)
        draw_text(start.x + 34, start.y + 18, "Start  S", color_text, start_fill);

    uint32_t left_reserve = surface.width >= 520 ? 124 : 60;
    uint32_t right_reserve = surface.width >= 520 ? 104 : 10;
    uint32_t available_width = surface.width > left_reserve + right_reserve + 20
        ? surface.width - left_reserve - right_reserve - 20 : surface.width - 36;
    uint32_t dock_width = available_width < 486 ? available_width : 486;
    if (dock_width < 40) dock_width = 40;
    uint32_t dock_height = 46;
    int32_t dock_x = (int32_t)left_reserve
        + (int32_t)(available_width - dock_width) / 2;
    int32_t dock_y = bar_y + 6;
    draw_box((struct kos_gui_rect){ dock_x, dock_y, dock_width, dock_height },
             color_panel_low, color_dim);
    uint32_t button_width = (dock_width - 24) / APP_COUNT;
    for (uint32_t app = 0; app < APP_COUNT; ++app) {
        int32_t x = dock_x + 6 + (int32_t)(app * button_width);
        struct kos_gui_color fill = active_app == (int32_t)app
            ? color_panel_high : color_panel_low;
        (void)vino_gui_fill((struct kos_gui_rect){ x, dock_y + 5,
            button_width - 3, 36 }, fill);
        if (active_app == (int32_t)app) {
            uint32_t pulse = !reduced_motion && ((sys_gettime() / 2) & 1) ? 2 : 1;
            (void)vino_gui_fill((struct kos_gui_rect){ x + 3, dock_y + 5,
                button_width - 9, pulse }, color_accent);
        }
        draw_text(x + 9, dock_y + 18, app_symbols[app], color_accent, fill);
        if (button_width >= 95) {
            draw_text(x + 34, dock_y + 18, app_names[app], color_text, fill);
        }
        else {
            char shortcut[2] = { (char)('1' + app), '\0' };
            draw_text(x + 34, dock_y + 18, shortcut, color_text, fill);
        }
        if (windows[app].open) draw_dot(x + (int32_t)button_width - 12,
                                        dock_y + 19, 4, color_cyan);
    }
    if (surface.width >= 520) {
        struct kos_gui_rect status = { (int32_t)surface.width - 92, bar_y + 7, 82, 44 };
        draw_box(status, color_panel, color_dim);
        struct kos_gui_color status_color = system_info.network_available
            ? (system_info.ipv4_address != 0
                ? (struct kos_gui_color){ 104, 224, 169, 0 }
                : (struct kos_gui_color){ 240, 190, 96, 0 })
            : color_dim;
        draw_dot(status.x + 10, status.y + 17, 8, status_color);
        draw_text(status.x + 24, status.y + 18,
                  system_info.network_available ? "Online" : "Offline",
                  color_muted, color_panel);
    }
}

static void draw_terminal_content(struct vino_window *window) {
    int32_t left = window->x + 18;
    int32_t top = window->y + WINDOW_TITLE_HEIGHT + 16;
    uint32_t visible = (window->height - WINDOW_TITLE_HEIGHT - 52) / 18;
    if (visible > TERMINAL_LINE_COUNT) visible = TERMINAL_LINE_COUNT;
    uint32_t output_rows = visible == 0 ? 0 : visible - 1;
    uint32_t output_count = terminal_line_count < output_rows
        ? terminal_line_count : output_rows;
    uint32_t start = terminal_line_count - output_count;
    for (uint32_t row = 0; row < output_count; ++row) {
        draw_text(left, top + (int32_t)(row * 18), terminal_lines[start + row],
                  row == output_count - 1 ? color_cyan : color_text, color_panel_low);
    }
    int32_t prompt_y = top + (int32_t)(output_rows * 18);
    char prompt[TERMINAL_LINE_LENGTH] = "> ";
    string_append(prompt, sizeof(prompt), terminal_input);
    if (!reduced_motion && ((sys_gettime() / 2) & 1)
        && string_length(prompt) + 1 < sizeof(prompt)) string_append(prompt, sizeof(prompt), "_");
    draw_text(left, prompt_y, prompt, color_accent, color_panel_low);
    draw_text(window->x + 18, window->y + (int32_t)window->height - 23,
              "Enter run  |  Backspace edit  |  Esc return to desktop",
              color_muted, color_panel_low);
}

static void draw_files_content(struct vino_window *window) {
    int32_t left = window->x + 18;
    bool compact = window->height < 300;
    int32_t top = window->y + WINDOW_TITLE_HEIGHT + (compact ? 8 : 14);
    int32_t list_y;
    if (compact) {
        draw_text(left + 2, top, file_directory, color_cyan, color_panel_low);
        list_y = top + 20;
    }
    else {
        struct kos_gui_rect address = {
            left, top, window->width - 36, 30,
        };
        draw_box(address, color_panel_high, color_dim);
        draw_text(address.x + 10, address.y + 10, file_directory,
                  color_cyan, color_panel_high);
        list_y = top + 42;
    }
    uint32_t row_height = 24;
    uint32_t available_height = window->height > (compact ? 84u : 150u)
        ? window->height - (compact ? 84u : 150u) : row_height;
    uint32_t visible = available_height / row_height;
    if (visible > 10) visible = 10;
    if (visible > file_entry_count) visible = file_entry_count;
    for (uint32_t row = 0; row < visible; ++row) {
        uint32_t index = file_scroll + row;
        if (index >= file_entry_count) break;
        struct kos_gui_rect item = {
            left, list_y + (int32_t)(row * row_height), window->width - 36, row_height,
        };
        struct kos_gui_color fill = file_selected == index
            ? color_panel_high : color_panel_low;
        (void)vino_gui_fill(item, fill);
        if (file_selected == index) {
            (void)vino_gui_fill((struct kos_gui_rect){ item.x, item.y, 3, item.height },
                                color_accent);
        }
        const char *type = file_entries[index].type == KOS_STAT_DIRECTORY ? "DIR  " : "FILE ";
        draw_text(item.x + 10, item.y + 8, type,
                  file_entries[index].type == KOS_STAT_DIRECTORY ? color_cyan : color_muted,
                  fill);
        draw_text(item.x + 58, item.y + 8, file_entries[index].name, color_text, fill);
        char size[24];
        number_text(file_entries[index].size, size);
        draw_text(item.x + (int32_t)item.width - 78, item.y + 8, size, color_muted, fill);
    }
    if (file_error[0] != '\0') {
        draw_text(left + 8, list_y + 14, file_error, color_cyan, color_panel_low);
    }
    else if (file_entry_count == 0) {
        draw_text(left + 8, list_y + 14, "This directory is empty.", color_muted,
                  color_panel_low);
    }
    draw_text(left, window->y + (int32_t)window->height - 24,
              "J/K select   Enter open   Backspace up   R refresh   V Vino volume",
              color_muted, color_panel_low);
}

static void draw_settings_content(struct vino_window *window) {
    int32_t left = window->x + 22;
    if (window->height < 360) {
        int32_t y = window->y + WINDOW_TITLE_HEIGHT + 12;
        draw_text(left, y, "1 Accent: Ocean / Teal / Violet", color_accent,
                  color_panel_low);
        draw_text(left, y + 25, reduced_motion ? "2 Motion: Reduced" : "2 Motion: Subtle",
                  color_text, color_panel_low);
        draw_text(left, y + 50, compact_panels ? "3 Panels: Compact" : "3 Panels: Comfortable",
                  color_text, color_panel_low);
        return;
    }
    int32_t y = window->y + WINDOW_TITLE_HEIGHT + 20;
    draw_text(left, y, "PERSONALIZATION", color_cyan, color_panel_low);
    y += 34;
    struct kos_gui_rect card = { left, y, window->width - 44, 62 };
    draw_box(card, color_panel_high, color_dim);
    draw_text(card.x + 14, card.y + 12, "01   Accent color", color_text, color_panel_high);
    const char *theme = theme_variant == 0 ? "Ocean blue" : theme_variant == 1
        ? "Lagoon teal" : "Evening violet";
    draw_text(card.x + 14, card.y + 36, theme, color_accent, color_panel_high);
    y += 76;
    card.y = y;
    draw_box(card, color_panel_high, color_dim);
    draw_text(card.x + 14, card.y + 12, "02   Motion", color_text, color_panel_high);
    draw_text(card.x + 14, card.y + 36, reduced_motion ? "Reduced" : "Subtle transitions",
              color_accent, color_panel_high);
    y += 76;
    card.y = y;
    draw_box(card, color_panel_high, color_dim);
    draw_text(card.x + 14, card.y + 12, "03   Panel density", color_text, color_panel_high);
    draw_text(card.x + 14, card.y + 36, compact_panels ? "Compact" : "Comfortable",
              color_accent, color_panel_high);
    draw_text(left, window->y + (int32_t)window->height - 24,
              "Press 1, 2, or 3 to adjust. Changes apply to this session.",
              color_muted, color_panel_low);
}

static void draw_network_content(struct vino_window *window) {
    refresh_system_info();
    int32_t left = window->x + 22;
    if (window->height < 360) {
        int32_t y = window->y + WINDOW_TITLE_HEIGHT + 12;
        draw_text(left, y, system_info.network_available
            ? "Network interface ready" : "No network interface",
            system_info.network_available ? color_cyan : color_muted, color_panel_low);
        y += 22;
        char ip[16], gateway[16], dns[16];
        format_ipv4(system_info.ipv4_address, ip);
        format_ipv4(system_info.gateway, gateway);
        format_ipv4(system_info.dns_server, dns);
        draw_text(left, y, system_info.network_available ? ip : "Address unavailable",
                  color_text, color_panel_low);
        draw_text(left, y + 20, system_info.network_available ? gateway : "Gateway -",
                  color_muted, color_panel_low);
        draw_text(left, y + 40, system_info.network_available ? dns : "DNS -",
                  color_muted, color_panel_low);
        draw_text(left, y + 64, dhcp_state_name(system_info.dhcp_state),
                  color_cyan, color_panel_low);
        return;
    }
    int32_t y = window->y + WINDOW_TITLE_HEIGHT + 20;
    struct kos_gui_rect state = { left, y, window->width - 44, 46 };
    draw_box(state, color_panel_high, color_dim);
    struct kos_gui_color status = system_info.network_available
        ? (struct kos_gui_color){ 104, 224, 169, 0 }
        : (struct kos_gui_color){ 232, 151, 133, 0 };
    draw_dot(state.x + 14, state.y + 18, 10, status);
    draw_text(state.x + 34, state.y + 19,
        system_info.network_available ? "Interface ready" : "No network interface",
        color_text, color_panel_high);
    y += 64;
    char ip[16], mask[16], gateway[16], dns[16], mac[18];
    format_ipv4(system_info.ipv4_address, ip);
    format_ipv4(system_info.subnet_mask, mask);
    format_ipv4(system_info.gateway, gateway);
    format_ipv4(system_info.dns_server, dns);
    format_mac(mac);
    const char *labels[] = { "Device", "Address", "Subnet", "Gateway", "DNS", "Hardware" };
    const char *values[] = {
        system_info.device_name[0] ? system_info.device_name : "eth0",
        system_info.network_available ? ip : "-",
        system_info.network_available ? mask : "-",
        system_info.network_available ? gateway : "-",
        system_info.network_available ? dns : "-",
        system_info.network_available ? mac : "-",
    };
    for (uint32_t index = 0; index < 6; ++index) {
        draw_text(left + 8, y + (int32_t)(index * 28), labels[index], color_muted,
                  color_panel_low);
        draw_text(left + 132, y + (int32_t)(index * 28), values[index], color_text,
                  color_panel_low);
    }
    char detail[96] = "DHCP  |  ";
    string_append(detail, sizeof(detail), dhcp_state_name(system_info.dhcp_state));
    draw_text(left + 8, y + 180, detail, color_cyan, color_panel_low);
    draw_text(left + 8, window->y + (int32_t)window->height - 24,
              "R refresh status   |   Network settings are read-only in this build",
              color_muted, color_panel_low);
}

static void draw_window(uint32_t app) {
    struct vino_window *window = &windows[app];
    if (!window->open || window->minimized) return;
    int32_t x = window->x;
    int32_t y = window->y - (int32_t)(animation_frame * 7);
    struct vino_window visual_window = *window;
    visual_window.y = y;
    struct kos_gui_rect bounds = {
        visual_window.x, visual_window.y, visual_window.width, visual_window.height,
    };
    struct kos_gui_color edge = (int32_t)app == active_app ? color_accent : color_dim;
    if (compact_panels && bounds.width > 16 && bounds.height > 16) {
        (void)vino_gui_fill((struct kos_gui_rect){ x + 6, y + 7,
            bounds.width, bounds.height }, color_shadow);
    }
    (void)vino_gui_fill(bounds, color_panel_low);
    (void)vino_gui_border(bounds, edge);
    (void)vino_gui_fill((struct kos_gui_rect){ x + 1, y + 1,
        bounds.width - 2, WINDOW_TITLE_HEIGHT }, color_panel_high);
    (void)vino_gui_fill((struct kos_gui_rect){ x + 1, y + WINDOW_TITLE_HEIGHT,
        bounds.width - 2, 1 }, edge);
    draw_text(x + 17, y + 15, app_names[app], color_text, color_panel_high);
    draw_text(x + (int32_t)bounds.width - 92, y + 15, "m  z  x", color_muted,
              color_panel_high);
    struct kos_gui_rect content = {
        x + 1, y + WINDOW_TITLE_HEIGHT + 1, bounds.width - 2,
        bounds.height - WINDOW_TITLE_HEIGHT - 2,
    };
    (void)vino_gui_fill(content, color_panel_low);
    if (app == APP_TERMINAL) draw_terminal_content(&visual_window);
    else if (app == APP_FILES) draw_files_content(&visual_window);
    else if (app == APP_SETTINGS) draw_settings_content(&visual_window);
    else if (app == APP_NETWORK) draw_network_content(&visual_window);
}

static void draw_windows(void) {
    compact_window_order();
    for (uint32_t order = 0; order < APP_COUNT; ++order) {
        for (uint32_t app = 0; app < APP_COUNT; ++app) {
            if (windows[app].open && !windows[app].minimized
                && windows[app].z_order == order + 1) draw_window(app);
        }
    }
}

static void draw_start_menu(void) {
    if (!start_menu_open && menu_animation_step == 0) return;
    uint32_t target_height = surface.height < 400 ? surface.height - 106 : 310;
    uint32_t height = target_height * menu_animation_step / 4;
    if (height < 56) height = menu_animation_step == 0 ? 0 : 56;
    uint32_t width = surface.width > 360 ? 310 : surface.width - 24;
    int32_t x = 14;
    int32_t y = (int32_t)surface.height - 58 - (int32_t)height - 8;
    draw_box((struct kos_gui_rect){ x, y, width, height }, color_panel,
             color_accent);
    draw_text(x + 18, y + 15, "VINO OS", color_cyan, color_panel);
    static const char *const items[] = { "Terminal", "Files", "Settings", "Network" };
    uint32_t row_height = surface.height < 400 ? 20 : 34;
    uint32_t row_gap = surface.height < 400 ? 22 : 42;
    uint32_t heading = surface.height < 400 ? 32 : 48;
    for (uint32_t index = 0; index < APP_COUNT; ++index) {
        int32_t row_y = y + (int32_t)heading + (int32_t)index * (int32_t)row_gap;
        if ((uint32_t)(row_y - y) + row_height > height) continue;
        struct kos_gui_rect row = { x + 10, row_y, width - 20, row_height };
        struct kos_gui_color fill = menu_selected == index ? color_panel_high : color_panel;
        (void)vino_gui_fill(row, fill);
        draw_text(row.x + 12, row.y + (int32_t)(row_height / 2 - 7), items[index],
                  menu_selected == index ? color_cyan : color_text, fill);
        char shortcut[3] = { (char)('1' + index), '\0', '\0' };
        draw_text(row.x + (int32_t)row.width - 26,
                  row.y + (int32_t)(row_height / 2 - 7), shortcut,
                  color_muted, fill);
    }
    if (height > 278) draw_text(x + 18, y + (int32_t)height - 22,
        "Esc close   Enter launch", color_muted, color_panel);
}

static void draw_context_menu(void) {
    if (!context_menu_open) return;
    int32_t x = active_app >= 0 ? windows[active_app].x + 64 : 184;
    int32_t y = active_app >= 0 ? windows[active_app].y + 74 : 142;
    uint32_t width = surface.width > 244 ? 220 : surface.width - 24;
    uint32_t row_height = surface.height < 320 ? 22 : 31;
    uint32_t row_gap = surface.height < 320 ? 24 : 34;
    uint32_t height = 5 * row_gap + 10;
    if ((uint64_t)x + width > surface.width) x = (int32_t)surface.width - width - 8;
    if (x < 8) x = 8;
    if ((uint64_t)y + height > surface.height - 60) {
        y = (int32_t)surface.height - 60 - (int32_t)height;
    }
    if (y < 38) y = 38;
    draw_box((struct kos_gui_rect){ x, y, width, height }, color_panel, color_accent);
    static const char *const items[] = {
        "Open Terminal", "Open Files", "Open Settings", "Open Network", "Desktop" };
    for (uint32_t index = 0; index < 5; ++index) {
        struct kos_gui_rect row = { x + 7, y + 5 + (int32_t)index * (int32_t)row_gap,
            width - 14, row_height };
        struct kos_gui_color fill = context_selected == index
            ? color_panel_high : color_panel;
        (void)vino_gui_fill(row, fill);
        draw_text(row.x + 8, row.y + (int32_t)(row_height / 2 - 7), items[index],
            context_selected == index ? color_cyan : color_text, fill);
    }
}

static void draw_preview(void) {
    if (!file_preview_open) return;
    uint32_t width = surface.width > 760 ? 690 : surface.width - 48;
    uint32_t height = surface.height > 560 ? 390 : surface.height - 120;
    int32_t x = (int32_t)((surface.width - width) / 2);
    int32_t y = (int32_t)((surface.height - height) / 2);
    draw_box((struct kos_gui_rect){ x, y, width, height }, color_panel, color_accent);
    draw_text(x + 18, y + 16, "FILE PREVIEW", color_cyan, color_panel);
    uint32_t line = 0;
    uint32_t column = 0;
    uint32_t max_lines = height > 72 ? (height - 72) / 20 : 1;
    if (max_lines > 14) max_lines = 14;
    char text_line[TERMINAL_LINE_LENGTH];
    int32_t text_y = y + 52;
    for (uint32_t index = 0; index < file_preview_size && line < max_lines; ++index) {
        char character = file_preview[index];
        if (character == '\r') continue;
        if (character == '\n' || column + 1 >= sizeof(text_line)) {
            text_line[column] = '\0';
            draw_text(x + 18, text_y + (int32_t)(line * 20), text_line,
                      color_text, color_panel);
            ++line;
            column = 0;
            if (character == '\n') continue;
        }
        text_line[column++] = ((uint8_t)character >= 32 && (uint8_t)character < 127)
            ? character : ' ';
    }
    if (column != 0 && line < max_lines) {
        text_line[column] = '\0';
        draw_text(x + 18, text_y + (int32_t)(line * 20), text_line,
                  color_text, color_panel);
    }
    draw_text(x + 18, y + (int32_t)height - 25,
              "Read-only preview  |  Esc close", color_muted, color_panel);
}

static void draw_about(void) {
    if (!about_dialog_open) return;
    uint32_t width = surface.width > 520 ? 480 : surface.width - 40;
    uint32_t height = 184;
    int32_t x = (int32_t)((surface.width - width) / 2);
    int32_t y = (int32_t)((surface.height - height) / 2);
    draw_box((struct kos_gui_rect){ x, y, width, height }, color_panel,
             color_accent);
    draw_text(x + 20, y + 18, "ABOUT VINO OS DEFAULT", color_cyan, color_panel);
    draw_text(x + 20, y + 54, "Native user-space desktop  |  No browser engine",
              color_text, color_panel);
    draw_text(x + 20, y + 82, "Display + input services  |  VFS-backed Files",
              color_text, color_panel);
    draw_text(x + 20, y + 110, "Kernel edition remains default; GUI is a package.",
              color_muted, color_panel);
    draw_text(x + 20, y + 150, "Esc close", color_muted, color_panel);
}

static void render_ui(void) {
    draw_wallpaper();
    draw_desktop_icons();
    draw_windows();
    draw_top_bar();
    draw_dock();
    draw_start_menu();
    draw_context_menu();
    draw_preview();
    draw_about();
    if (move_window_mode) {
        uint32_t overlay_width = surface.width > 440 ? surface.width - 440 : surface.width - 24;
        int32_t overlay_x = surface.width > 440 ? 220 : 12;
        draw_box((struct kos_gui_rect){ overlay_x, 38, overlay_width, 28 },
                 color_panel_high, color_accent);
        draw_text(overlay_x + 12, 48, "Move window: H/J/K/L  |  G to finish",
                  color_cyan, color_panel_high);
    }
}

static void animate_menu(bool opening) {
    if (reduced_motion) {
        menu_animation_step = opening ? 4 : 0;
        render_ui();
        return;
    }
    while ((opening && menu_animation_step < 4)
           || (!opening && menu_animation_step > 0)) {
        if (opening) ++menu_animation_step;
        else --menu_animation_step;
        render_ui();
        (void)sys_sleep(14);
    }
}

static void set_start_menu(bool open) {
    if (start_menu_open == open) return;
    if (open) {
        context_menu_open = false;
        start_menu_open = true;
        animate_menu(true);
    }
    else {
        start_menu_open = false;
        animate_menu(false);
    }
}

static void start_menu_launch(void) {
    open_app(menu_selected % APP_COUNT);
    menu_animation_step = 0;
}

static void context_menu_activate(void) {
    if (context_selected < APP_COUNT) open_app(context_selected);
    else active_app = -1;
    context_menu_open = false;
}

static void move_active_window(char direction) {
    if (active_app < 0 || active_app >= APP_COUNT) return;
    struct vino_window *window = &windows[active_app];
    if (window->maximized) return;
    if (direction == 'h' && window->x > 8) window->x -= 16;
    else if (direction == 'l' && (uint64_t)window->x + window->width + 16 < surface.width)
        window->x += 16;
    else if (direction == 'k' && window->y > 42) window->y -= 16;
    else if (direction == 'j' && (uint64_t)window->y + window->height + 76 < surface.height)
        window->y += 16;
}

static void handle_file_key(char key) {
    if (file_preview_open) {
        if (key == 27) file_preview_open = false;
        return;
    }
    if (key == 'j' && file_entry_count != 0 && file_selected + 1 < file_entry_count)
        ++file_selected;
    else if (key == 'k' && file_selected > 0) --file_selected;
    else if (key == '\b') {
        path_parent(file_directory);
        files_reload();
    }
    else if (key == '\n') (void)files_enter_selected();
    else if (key == 'r') files_reload();
    else if (key == 'v') {
        char volume = system_info.system_volume_letter;
        if (volume >= 'C' && volume <= 'Z') {
            file_directory[0] = volume;
            file_directory[1] = ':';
            file_directory[2] = '/';
            file_directory[3] = '\0';
            files_reload();
        }
    }
    else if (key == 'p') (void)files_enter_selected();
}

static void handle_key(char key) {
    if (about_dialog_open) {
        if (key == 27 || key == '\n') about_dialog_open = false;
        return;
    }
    if (file_preview_open) {
        handle_file_key(key);
        return;
    }
    if (start_menu_open) {
        if (key == 27 || key == 's') set_start_menu(false);
        else if (key == 'j' && menu_selected + 1 < APP_COUNT) ++menu_selected;
        else if (key == 'k' && menu_selected > 0) --menu_selected;
        else if (key == '\n') start_menu_launch();
        else if (key >= '1' && key <= '4') {
            menu_selected = (uint32_t)(key - '1');
            start_menu_launch();
        }
        return;
    }
    if (context_menu_open) {
        if (key == 27 || key == 'c') context_menu_open = false;
        else if (key == 'j' && context_selected < 4) ++context_selected;
        else if (key == 'k' && context_selected > 0) --context_selected;
        else if (key == '\n') context_menu_activate();
        return;
    }
    if (active_app == APP_TERMINAL) {
        if (key == 27) {
            active_app = -1;
            return;
        }
        if (key == '\n') {
            terminal_execute();
            return;
        }
        if (key == '\b') {
            if (terminal_input_size != 0) terminal_input[--terminal_input_size] = '\0';
            return;
        }
        if (key == '\t') return;
        if ((uint8_t)key >= 32 && terminal_input_size + 1 < sizeof(terminal_input)) {
            terminal_input[terminal_input_size++] = key;
            terminal_input[terminal_input_size] = '\0';
        }
        return;
    }
    if (key == 27) {
        active_app = -1;
        move_window_mode = false;
        return;
    }
    if (move_window_mode) {
        if (key == 'g') move_window_mode = false;
        else move_active_window(key);
        return;
    }
    if (active_app == APP_FILES
        && (key == 'j' || key == 'k' || key == '\n' || key == '\b'
            || key == 'r' || key == 'v' || key == 'p')) {
        handle_file_key(key);
        uint32_t visible = windows[APP_FILES].height < 300 ? 2
            : (windows[APP_FILES].height - 90) / 24;
        if (visible > 10) visible = 10;
        if (file_selected < file_scroll) file_scroll = file_selected;
        else if (file_selected >= file_scroll + visible && visible != 0)
            file_scroll = file_selected - visible + 1;
        return;
    }
    if (active_app == APP_SETTINGS && key >= '1' && key <= '3') {
        if (key == '1') {
            theme_variant = (theme_variant + 1) % 3;
            color_set_accent(theme_variant);
        }
        else if (key == '2') reduced_motion = !reduced_motion;
        else if (key == '3') compact_panels = !compact_panels;
        return;
    }
    if (active_app == APP_NETWORK && key == 'r') {
        refresh_system_info();
        return;
    }
    if (key >= '1' && key <= '4') {
        open_app((uint32_t)(key - '1'));
        return;
    }
    if (key == 's') {
        set_start_menu(true);
        return;
    }
    if (key == 'c') {
        context_selected = 0;
        context_menu_open = true;
        return;
    }
    if (key == '?') {
        about_dialog_open = true;
        return;
    }
    if (key == '\t' || key == ']') {
        cycle_focus(1);
        return;
    }
    if (key == '[') {
        cycle_focus(-1);
        return;
    }
    if (key == 'x') close_active_app();
    else if (key == 'm' && active_app >= 0) {
        windows[active_app].minimized = true;
        active_app = -1;
    }
    else if (key == 'z') toggle_maximize();
    else if (key == 'g' && active_app >= 0) move_window_mode = true;
    else if (key == 'q' && active_app < 0) app_exit();
}

__attribute__((noreturn)) void _start(void) {
    surface.abi_version = KOS_GUI_ABI_VERSION;
    if (vino_gui_surface_get(&surface) < 0 || surface.width < 320
        || surface.height < 240) app_exit_with_status(1);
    refresh_system_info();
    initialize_paths();
    terminal_init();
    file_entry_count = 0;
    files_reload();
    render_ui();
    for (;;) {
        struct kos_gui_input_event event = { .abi_version = KOS_GUI_ABI_VERSION };
        if (vino_gui_input_read(&event) == 0 && event.type == KOS_GUI_INPUT_KEY) {
            handle_key((char)event.key);
            if (animation_frame != 0) --animation_frame;
            render_ui();
            if (animation_frame != 0) {
                (void)sys_sleep(14);
                --animation_frame;
                render_ui();
            }
            last_redraw_ms = sys_gettime() * 1000;
        }
        uint64_t now = sys_gettime() * 1000;
        if (now - last_redraw_ms >= (reduced_motion ? 1000 : 250)) {
            refresh_system_info();
            render_ui();
            last_redraw_ms = now;
        }
        (void)sys_sleep(24);
    }
}
