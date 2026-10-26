#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/kos/syscall_numbers.h>
#include <uapi/kos/system_control.h>
#include <vino/gui.h>
#include <vino/system.h>
#include <vino/vfs.h>

enum {
    APP_TERMINAL = 0,
    APP_FILES,
    APP_SETTINGS,
    APP_NETWORK,
    APP_CONTROL_CENTER,
    APP_NOTIFICATIONS,
    APP_COUNT,
    DESKTOP_ICON_COUNT = 4,
    WINDOW_TITLE_HEIGHT = 40,
    TERMINAL_LINE_COUNT = 13,
    TERMINAL_LINE_LENGTH = 96,
    FILE_ENTRY_COUNT = 32,
    FILE_PATH_LENGTH = 256,
    PREVIEW_SIZE = 768,
    WALLPAPER_MAX_BYTES = 8 * 1024 * 1024,
    WALLPAPER_HEADER_SIZE = 20,
    POWER_DIALOG_CLOSED = 0,
    POWER_DIALOG_CHOOSE = 1,
    POWER_DIALOG_CONFIRM_RESTART = 2,
    POWER_DIALOG_CONFIRM_SHUTDOWN = 3,
};

static const char *const app_names[APP_COUNT] = {
    "Terminal", "Files", "Settings", "Network", "Control Center", "Notifications",
};

static struct kos_gui_surface surface;
static struct kos_system_info system_info;
static bool system_info_available;
static struct kos_gui_color color_wallpaper = { 12, 19, 34, 0 };
static struct kos_gui_color color_panel = { 15, 22, 40, 0 };
static struct kos_gui_color color_panel_high = { 37, 48, 67, 0 };
static struct kos_gui_color color_panel_low = { 10, 16, 29, 0 };
static struct kos_gui_color color_accent = { 102, 200, 255, 0 };
static struct kos_gui_color color_cyan = { 102, 200, 255, 0 };
static struct kos_gui_color color_text = { 255, 255, 255, 0 };
static struct kos_gui_color color_muted = { 139, 155, 180, 0 };
static struct kos_gui_color color_dim = { 196, 208, 224, 0 };
static const struct kos_gui_color app_icon_colors[APP_COUNT] = {
    { 51, 65, 85, 0 }, { 59, 130, 246, 0 }, { 100, 116, 139, 0 },
    { 6, 182, 212, 0 }, { 139, 92, 246, 0 }, { 249, 115, 22, 0 },
};

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
static char file_home_directory[FILE_PATH_LENGTH];
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
static char menu_search[32];
static bool context_menu_open;
static bool about_dialog_open;
static uint32_t power_dialog_state;
static char power_dialog_message[96];
static char network_action_message[96];
static bool move_window_mode;
static uint32_t menu_selected;
static uint32_t context_selected;
static uint32_t menu_animation_step;
static uint32_t theme_variant;
static bool reduced_motion;
static bool strong_window_shadow;
static bool pointer_available;
static int32_t pointer_x;
static int32_t pointer_y;
static uint32_t pointer_buttons;
static int32_t drag_app = -1;
static int32_t drag_offset_x;
static int32_t drag_offset_y;
static bool context_menu_at_pointer;
static int32_t context_menu_x;
static int32_t context_menu_y;
static uint32_t file_last_clicked = UINT32_MAX;
static uint64_t file_last_click_ms;
static uint64_t last_state_poll_second;
static uint64_t displayed_uptime_minute;
static bool wallpaper_cache_valid;
static bool wallpaper_loaded;
static uint8_t wallpaper_asset[WALLPAPER_MAX_BYTES];
static uint32_t wallpaper_asset_size;
static uint32_t wallpaper_width;
static uint32_t wallpaper_height;
static uint32_t wallpaper_pitch;
static uint32_t recent_apps[2];
static uint32_t recent_app_count;
static uint32_t settings_page;
static uint32_t pointer_hover_target;
static uint32_t animation_frame;
static bool visual_action_rendered;
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

static void format_memory_summary(char output[64]) {
    if (!system_info_available) {
        string_copy(output, 64, "Kernel metrics unavailable");
        return;
    }
    char total[24], free_memory[24];
    number_text(system_info.total_memory_bytes / (1024 * 1024), total);
    number_text(system_info.free_memory_bytes / (1024 * 1024), free_memory);
    output[0] = '\0';
    string_append(output, 64, "PMM: ");
    string_append(output, 64, total);
    string_append(output, 64, " MB total / ");
    string_append(output, 64, free_memory);
    string_append(output, 64, " MB free");
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
    (void)background;
    (void)vino_gui_text_transparent_font(x, y, text_scratch, 1,
        KOS_GUI_FONT_INTER_8X14, foreground);
}

static void draw_text_scale(int32_t x, int32_t y, const char *text, uint32_t scale,
                            struct kos_gui_color foreground,
                            struct kos_gui_color background) {
    string_copy(text_scratch, sizeof(text_scratch), text);
    for (uint32_t index = 0; text_scratch[index] != '\0'; ++index) {
        if ((uint8_t)text_scratch[index] < 32 || (uint8_t)text_scratch[index] >= 127)
            text_scratch[index] = '?';
    }
    (void)background;
    (void)vino_gui_text_transparent_font(x, y, text_scratch, scale,
        KOS_GUI_FONT_INTER_8X14, foreground);
}

static void draw_glass(struct kos_gui_rect rect, struct kos_gui_color fill,
                       struct kos_gui_color border, uint32_t radius,
                       uint32_t blur_radius) {
    fill.alpha = 179;
    border.alpha = 23;
    (void)vino_gui_glass(rect, fill, border, radius, blur_radius);
}

static void draw_box(struct kos_gui_rect rect, struct kos_gui_color fill,
                     struct kos_gui_color border) {
    if (rect.width == 0 || rect.height == 0) return;
    uint32_t radius = rect.width < rect.height ? rect.width / 3 : rect.height / 3;
    if (radius > 12) radius = 12;
    /* Small controls/cards should not trigger a full software backdrop blur. */
    draw_glass(rect, fill, border, radius, 0);
}

static bool pointer_inside(struct kos_gui_rect rect) {
    return pointer_available && vino_gui_rect_contains(&rect, pointer_x, pointer_y);
}

static void draw_dot(int32_t x, int32_t y, uint32_t size,
                     struct kos_gui_color color) {
    (void)vino_gui_fill((struct kos_gui_rect){ x, y, size, size }, color);
}

static void draw_hline(int32_t x, int32_t y, uint32_t width, uint32_t thickness,
                       struct kos_gui_color color);

static void draw_round_dot(int32_t x, int32_t y, uint32_t size,
                           struct kos_gui_color color) {
    struct kos_gui_color border = color;
    color.alpha = 255;
    border.alpha = 255;
    (void)vino_gui_glass((struct kos_gui_rect){ x, y, size, size }, color,
                         border, size / 2, 0);
}

static void draw_hline(int32_t x, int32_t y, uint32_t width, uint32_t thickness,
                       struct kos_gui_color color) {
    (void)vino_gui_fill((struct kos_gui_rect){ x, y, width, thickness }, color);
}

static void draw_vline(int32_t x, int32_t y, uint32_t height, uint32_t thickness,
                       struct kos_gui_color color) {
    (void)vino_gui_fill((struct kos_gui_rect){ x, y, thickness, height }, color);
}

static void draw_vino_mark(int32_t x, int32_t y, uint32_t size) {
    uint32_t unit = size / 5;
    if (unit == 0) unit = 1;
    struct kos_gui_color light = { 102, 200, 255, 0 };
    struct kos_gui_color blue = { 29, 94, 183, 0 };
    draw_hline(x, y, unit * 2, unit, light);
    draw_hline(x + unit * 2, y, unit * 2, unit, blue);
    draw_hline(x, y + unit * 2, unit * 2, unit, light);
    draw_hline(x + unit * 2, y + unit * 2, unit * 2, unit, blue);
    draw_hline(x, y + unit * 4, unit * 2, unit, light);
    draw_hline(x + unit * 2, y + unit * 4, unit * 2, unit, blue);
}

static void draw_app_icon(uint32_t app, int32_t x, int32_t y, uint32_t size,
                          bool tile_enabled) {
    if (app >= APP_COUNT || size < 32) return;
    if (tile_enabled) {
        struct kos_gui_rect tile = { x, y, size, size };
        struct kos_gui_color tile_border = { 255, 255, 255, 32 };
        draw_glass(tile, app_icon_colors[app], tile_border, 10, 0);
    }
    struct kos_gui_color glyph = tile_enabled
        ? (struct kos_gui_color){ 255, 255, 255, 0 } : app_icon_colors[app];
    int32_t left = x + (int32_t)size / 4;
    int32_t top = y + (int32_t)size / 4;
    uint32_t inner = size / 2;
    if (app == APP_TERMINAL) {
        struct kos_gui_rect screen = { left, top, inner, inner - 2 };
        (void)vino_gui_border(screen, glyph);
        draw_hline(left + 4, top + 5, 3, 2, glyph);
        draw_vline(left + 6, top + 7, 3, 2, glyph);
        draw_hline(left + 4, top + 10, 5, 2, glyph);
    }
    else if (app == APP_FILES) {
        draw_hline(left + 1, top + 3, inner / 2, 4, glyph);
        draw_hline(left + 1, top + 7, inner - 1, 3, glyph);
        draw_hline(left + 3, top + 10, inner - 5, 2, glyph);
        draw_vline(left + 1, top + 7, inner / 2 - 1, 2, glyph);
        draw_vline(left + (int32_t)inner - 2, top + 8, inner / 2 - 2, 2, glyph);
    }
    else if (app == APP_SETTINGS) {
        uint32_t track_width = inner - 2;
        for (uint32_t row = 0; row < 3; ++row) {
            int32_t track_y = top + 4 + (int32_t)row * 6;
            draw_hline(left + 1, track_y, track_width, 2, glyph);
        }
        draw_vline(left + 6, top + 2, 5, 3, app_icon_colors[app]);
        draw_vline(left + 11, top + 8, 5, 3, app_icon_colors[app]);
        draw_vline(left + 15, top + 14, 4, 3, app_icon_colors[app]);
    }
    else if (app == APP_NETWORK) {
        draw_hline(left + 3, top + 4, inner - 6, 2, glyph);
        draw_hline(left + 6, top + 8, inner - 12, 2, glyph);
        draw_hline(left + 8, top + 12, inner - 16, 2, glyph);
        draw_dot(left + (int32_t)inner / 2 - 1, top + 16, 3, glyph);
    }
    else if (app == APP_CONTROL_CENTER) {
        for (uint32_t row = 0; row < 3; ++row) {
            int32_t track_y = top + 4 + (int32_t)row * 6;
            draw_hline(left + 1, track_y, inner - 2, 2, glyph);
        }
        draw_dot(left + 5, top + 2, 4, glyph);
        draw_dot(left + 12, top + 8, 4, glyph);
        draw_dot(left + 8, top + 14, 4, glyph);
    }
    else {
        draw_hline(left + 7, top + 3, inner - 14, 2, glyph);
        draw_vline(left + 6, top + 5, 8, 2, glyph);
        draw_vline(left + (int32_t)inner - 8, top + 5, 8, 2, glyph);
        draw_hline(left + 6, top + 13, inner - 12, 2, glyph);
        draw_hline(left + 8, top + 16, inner - 16, 2, glyph);
        draw_dot(left + (int32_t)inner / 2 - 1, top + 1, 3, glyph);
    }
}

static void draw_file_glyph(int32_t x, int32_t y, bool directory) {
    if (directory) {
        struct kos_gui_color folder = { 96, 165, 250, 0 };
        draw_hline(x + 1, y + 2, 11, 3, folder);
        draw_hline(x, y + 5, 16, 9, folder);
        draw_hline(x + 2, y + 7, 12, 1,
                   (struct kos_gui_color){ 191, 219, 254, 0 });
    }
    else {
        struct kos_gui_color paper = { 196, 208, 224, 0 };
        draw_vline(x + 3, y + 1, 15, 11, paper);
        draw_hline(x + 5, y + 5, 7, 1, color_panel_high);
        draw_hline(x + 5, y + 8, 7, 1, color_panel_high);
        draw_hline(x + 5, y + 11, 5, 1, color_panel_high);
    }
}

static uint32_t files_quick_directory(uint32_t ordinal) {
    for (uint32_t index = 0; index < file_entry_count; ++index) {
        if (file_entries[index].type != KOS_STAT_DIRECTORY) continue;
        if (ordinal == 0) return index;
        --ordinal;
    }
    return FILE_ENTRY_COUNT;
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

static bool system_info_visual_equal(const struct kos_system_info *left,
                                    const struct kos_system_info *right) {
    if (left->task_count != right->task_count
        || left->total_memory_bytes / (1024 * 1024)
            != right->total_memory_bytes / (1024 * 1024)
        || left->free_memory_bytes / (1024 * 1024)
            != right->free_memory_bytes / (1024 * 1024)
        || left->network_available != right->network_available
        || left->configuration_source != right->configuration_source
        || left->dhcp_state != right->dhcp_state
        || left->ipv4_address != right->ipv4_address
        || left->subnet_mask != right->subnet_mask
        || left->gateway != right->gateway
        || left->dns_server != right->dns_server
        || left->system_volume_letter != right->system_volume_letter) return false;
    for (uint32_t index = 0; index < sizeof(left->mac_address); ++index) {
        if (left->mac_address[index] != right->mac_address[index]) return false;
    }
    for (uint32_t index = 0; index < sizeof(left->device_name); ++index) {
        if (left->device_name[index] != right->device_name[index]) return false;
    }
    return true;
}

static bool refresh_system_info(void) {
    struct kos_system_info previous = system_info;
    bool was_available = system_info_available;
    if (vino_system_info_get(&system_info) < 0) {
        system_info_available = false;
        system_info = (struct kos_system_info){
            .abi_version = KOS_SYSTEM_INFO_ABI_VERSION,
        };
        return was_available || !system_info_visual_equal(&previous, &system_info);
    }
    system_info_available = true;
    return !was_available || !system_info_visual_equal(&previous, &system_info);
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
    if (terminal_directory[0] == '\0')
        terminal_add_line("No mounted system volume is exposed by the kernel.");
}

static void initialize_paths(void) {
    char volume = system_info.system_volume_letter;
    if (volume < 'C' || volume > 'Z') {
        file_directory[0] = '\0';
        file_home_directory[0] = '\0';
        terminal_directory[0] = '\0';
        return;
    }
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
    string_copy(file_home_directory, sizeof(file_home_directory), terminal_directory);
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
    if (file_directory[0] == '\0') {
        string_copy(file_error, sizeof(file_error),
                    "No mounted system volume is reported by the kernel.");
        return;
    }
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

static void files_select_or_open(uint32_t index, uint64_t timestamp_ms) {
    if (index >= file_entry_count) return;
    if (index == file_last_clicked && timestamp_ms >= file_last_click_ms
        && timestamp_ms - file_last_click_ms <= 500) {
        file_selected = index;
        (void)files_enter_selected();
        file_last_clicked = UINT32_MAX;
    }
    else {
        file_selected = index;
        file_last_clicked = index;
        file_last_click_ms = timestamp_ms;
    }
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
    if (!system_info_available) {
        terminal_add_line("Kernel system-info API: unavailable");
        return;
    }
    if (!system_info.network_available) {
        terminal_add_line("NIC: unavailable in kernel network state");
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
        ? system_info.device_name : "Unavailable");
    string_append(line, sizeof(line), "  |  ");
    string_append(line, sizeof(line), dhcp_state_name(system_info.dhcp_state));
    terminal_add_line(line);
}

static void request_network_dhcp(void) {
    (void)refresh_system_info();
    if (!system_info_available) {
        string_copy(network_action_message, sizeof(network_action_message),
                    "Network API unavailable.");
        return;
    }
    if (!system_info.network_available) {
        string_copy(network_action_message, sizeof(network_action_message),
                    "No NIC exposed; DHCP unavailable.");
        return;
    }
    long result = vino_system_control(KOS_SYSTEM_CONTROL_CONNECT_NETWORK, false);
    (void)refresh_system_info();
    string_copy(network_action_message, sizeof(network_action_message), result < 0
        ? "DHCP failed or timed out."
        : "DHCP request completed.");
}

static void execute_power_action(uint32_t action) {
    long result = vino_system_control(action, true);
    if (result < 0) {
        string_copy(power_dialog_message, sizeof(power_dialog_message),
                    "Kernel rejected the request; the desktop is still running.");
    }
    else {
        string_copy(power_dialog_message, sizeof(power_dialog_message),
                    "Kernel accepted the request but did not transition state.");
    }
}

static void open_power_dialog(void) {
    power_dialog_state = POWER_DIALOG_CHOOSE;
    power_dialog_message[0] = '\0';
}

static void handle_power_dialog_key(char key) {
    if (key == 27) {
        power_dialog_state = POWER_DIALOG_CLOSED;
        return;
    }
    if (power_dialog_state == POWER_DIALOG_CHOOSE) {
        if (key == 'r') power_dialog_state = POWER_DIALOG_CONFIRM_RESTART;
        else if (key == 's') power_dialog_state = POWER_DIALOG_CONFIRM_SHUTDOWN;
        return;
    }
    if (key == 'n') {
        power_dialog_state = POWER_DIALOG_CLOSED;
        return;
    }
    if (key == '\n' || key == 'y') {
        execute_power_action(power_dialog_state == POWER_DIALOG_CONFIRM_RESTART
            ? KOS_SYSTEM_CONTROL_RESTART : KOS_SYSTEM_CONTROL_SHUTDOWN);
    }
}

static void handle_power_dialog_pointer(int32_t x, int32_t y) {
    uint32_t width = surface.width > 484 ? 460 : surface.width - 24;
    uint32_t height = 220;
    int32_t left = (int32_t)(surface.width - width) / 2;
    int32_t top = (int32_t)(surface.height - height) / 2;
    if (power_dialog_state == POWER_DIALOG_CHOOSE) {
        struct kos_gui_rect restart, shutdown, cancel;
        if (width >= 430) {
            restart = (struct kos_gui_rect){ left + 18, top + 126, 126, 38 };
            shutdown = (struct kos_gui_rect){ left + 155, top + 126, 126, 38 };
            cancel = (struct kos_gui_rect){ left + 292, top + 126, 126, 38 };
        }
        else {
            uint32_t button_width = width - 36;
            restart = (struct kos_gui_rect){ left + 18, top + 96, button_width, 32 };
            shutdown = (struct kos_gui_rect){ left + 18, top + 136, button_width, 32 };
            cancel = (struct kos_gui_rect){ left + 18, top + 176, button_width, 32 };
        }
        if (vino_gui_rect_contains(&restart, x, y))
            power_dialog_state = POWER_DIALOG_CONFIRM_RESTART;
        else if (vino_gui_rect_contains(&shutdown, x, y))
            power_dialog_state = POWER_DIALOG_CONFIRM_SHUTDOWN;
        else if (vino_gui_rect_contains(&cancel, x, y))
            power_dialog_state = POWER_DIALOG_CLOSED;
        return;
    }
    uint32_t action_width = width >= 430 ? 126 : (width - 48) / 2;
    struct kos_gui_rect confirm = {
        left + (width >= 430 ? 150 : 18), top + 150, action_width, 38,
    };
    struct kos_gui_rect cancel = {
        left + (width >= 430 ? 286 : 30 + action_width),
        top + 150, action_width, 38,
    };
    if (vino_gui_rect_contains(&confirm, x, y)) {
        execute_power_action(power_dialog_state == POWER_DIALOG_CONFIRM_RESTART
            ? KOS_SYSTEM_CONTROL_RESTART : KOS_SYSTEM_CONTROL_SHUTDOWN);
    }
    else if (vino_gui_rect_contains(&cancel, x, y)) {
        power_dialog_state = POWER_DIALOG_CLOSED;
    }
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
            struct kos_gui_control_request request = {
                .abi_version = KOS_GUI_CONTROL_ABI_VERSION,
                .action = KOS_GUI_CONTROL_QUERY,
            };
            if (vino_gui_control(&request) < 0) {
                terminal_add_line("GUI manager query unavailable.");
            }
            else {
                char line[TERMINAL_LINE_LENGTH] = "Default GUI: ";
                string_append(line, sizeof(line), request.default_gui_id[0] != '\0'
                    ? request.default_gui_id : "none");
                terminal_add_line(line);
                string_copy(line, sizeof(line), "Installed GUI packages: ");
                if (request.installed_mask & KOS_GUI_INSTALLED_DEFAULT)
                    string_append(line, sizeof(line), "default ");
                if (request.installed_mask & KOS_GUI_INSTALLED_MVP)
                    string_append(line, sizeof(line), "mvp ");
                if (request.installed_mask == 0)
                    string_append(line, sizeof(line), "none");
                terminal_add_line(line);
            }
        }
        else terminal_add_line("Only 'gui status' is implemented in this shell.");
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
    uint32_t desired_width = 740;
    uint32_t desired_height = 480;
    if (app == APP_FILES) {
        desired_width = 820;
        desired_height = 520;
    }
    else if (app == APP_SETTINGS) {
        desired_width = 740;
        desired_height = 500;
    }
    else if (app == APP_NETWORK) {
        desired_width = 560;
        desired_height = 440;
    }
    else if (app == APP_CONTROL_CENTER) {
        desired_width = 340;
        desired_height = 360;
    }
    else if (app == APP_NOTIFICATIONS) {
        desired_width = 380;
        desired_height = 320;
    }

    uint32_t max_width = surface.width > 32 ? surface.width - 32 : surface.width;
    uint32_t max_height = surface.height > 72 ? surface.height - 72 : surface.height;
    window->width = desired_width < max_width ? desired_width : max_width;
    window->height = desired_height < max_height ? desired_height : max_height;
    if (window->width < 280) window->width = max_width;
    if (window->height < 220) window->height = max_height;

    if (app == APP_CONTROL_CENTER || app == APP_NOTIFICATIONS) {
        window->x = (int32_t)surface.width - (int32_t)window->width - 16;
        window->y = (int32_t)surface.height - (int32_t)window->height - 56;
    }
    else {
        window->x = (int32_t)((surface.width - window->width) / 2);
        if (app == APP_FILES)
            window->y = (int32_t)surface.height / 2 - 280;
        else if (app == APP_SETTINGS)
            window->y = (int32_t)surface.height / 2 - 270;
        else
            window->y = (int32_t)surface.height / 2 - 260;
    }
    if (window->x < 8) window->x = 8;
    if (window->y < 8) window->y = 8;
    uint32_t work_bottom = surface.height > 56 ? surface.height - 56 : surface.height;
    if ((uint64_t)window->y + window->height > work_bottom)
        window->y = (int32_t)(work_bottom - window->height);
    if ((uint64_t)window->x + window->width > surface.width - 8)
        window->x = (int32_t)(surface.width - window->width - 8);
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
    if (recent_app_count == 0 || recent_apps[0] != app) {
        if (recent_app_count != 0) recent_apps[1] = recent_apps[0];
        recent_apps[0] = app;
        if (recent_app_count < 2) ++recent_app_count;
    }
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
        window->x = 0;
        window->y = 0;
        window->width = surface.width;
        window->height = surface.height > 48 ? surface.height - 48 : surface.height;
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

static uint32_t wallpaper_u32(uint32_t offset) {
    return (uint32_t)wallpaper_asset[offset]
        | ((uint32_t)wallpaper_asset[offset + 1] << 8)
        | ((uint32_t)wallpaper_asset[offset + 2] << 16)
        | ((uint32_t)wallpaper_asset[offset + 3] << 24);
}

static bool wallpaper_try_path(char *path) {
    long descriptor = vino_fs_open(path, KOS_O_RDONLY);
    if (descriptor < 0) return false;
    uint32_t total = 0;
    while (total < sizeof(wallpaper_asset)) {
        long bytes = vino_fs_read(descriptor, wallpaper_asset + total,
                                  sizeof(wallpaper_asset) - total);
        if (bytes < 0) {
            (void)vino_fs_close(descriptor);
            return false;
        }
        if (bytes == 0) break;
        total += (uint32_t)bytes;
    }
    (void)vino_fs_close(descriptor);
    if (total < WALLPAPER_HEADER_SIZE) return false;
    static const uint8_t expected_magic[8] = { 'K', 'O', 'S', 'R', 'G', 'B', '2', '4' };
    for (uint32_t index = 0; index < sizeof(expected_magic); ++index) {
        if (wallpaper_asset[index] != expected_magic[index]) return false;
    }
    uint32_t width = wallpaper_u32(8);
    uint32_t height = wallpaper_u32(12);
    uint32_t pitch = wallpaper_u32(16);
    if (width == 0 || height == 0 || width > 4096 || height > 4096
        || pitch < width * 3u || (uint64_t)pitch * height != total - WALLPAPER_HEADER_SIZE)
        return false;
    wallpaper_asset_size = total;
    wallpaper_width = width;
    wallpaper_height = height;
    wallpaper_pitch = pitch;
    return true;
}

static void wallpaper_load(void) {
    char installed_path[] = "C:/VINO/system/gui/default/share/wallpaper.kosrgb";
    char live_path[] = "D:/system/gui/default/share/wallpaper.kosrgb";
    char system_path[] = "C:/system/gui/default/share/wallpaper.kosrgb";
    char volume = system_info.system_volume_letter;
    if (volume >= 'a' && volume <= 'z') volume = (char)(volume - 'a' + 'A');
    if (volume >= 'A' && volume <= 'Z') installed_path[0] = volume;
    wallpaper_loaded = wallpaper_try_path(installed_path)
        || wallpaper_try_path(live_path) || wallpaper_try_path(system_path);
}

static void draw_wallpaper(void) {
    (void)vino_gui_clear(color_wallpaper);
    if (!wallpaper_loaded || surface.width == 0 || surface.height == 0) return;

    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_width = wallpaper_width;
    uint32_t crop_height = wallpaper_height;
    uint64_t image_scaled_width = (uint64_t)wallpaper_width * surface.height;
    uint64_t image_scaled_height = (uint64_t)wallpaper_height * surface.width;
    if (image_scaled_width > image_scaled_height) {
        crop_width = (uint32_t)((uint64_t)wallpaper_height * surface.width
                                / surface.height);
        crop_x = (wallpaper_width - crop_width) / 2;
    }
    else if (image_scaled_width < image_scaled_height) {
        crop_height = (uint32_t)((uint64_t)wallpaper_width * surface.height
                                 / surface.width);
        crop_y = (wallpaper_height - crop_height) / 2;
    }
    if (vino_gui_blit_rgb24((struct kos_gui_rect){
            0, 0, surface.width, surface.height },
            wallpaper_asset + WALLPAPER_HEADER_SIZE,
            wallpaper_asset_size - WALLPAPER_HEADER_SIZE,
            wallpaper_width, wallpaper_height, wallpaper_pitch,
            crop_x, crop_y, crop_width, crop_height) < 0) {
        wallpaper_loaded = false;
        (void)vino_gui_clear(color_wallpaper);
    }
}

static void draw_taskbar(void) {
    const uint32_t bar_height = 48;
    int32_t bar_y = (int32_t)(surface.height - bar_height);
    struct kos_gui_color shadow = { 0, 0, 0, 60 };
    struct kos_gui_color shadow_edge = { 0, 0, 0, 1 };
    (void)vino_gui_glass((struct kos_gui_rect){ 0, bar_y - 5,
        surface.width, 10 }, shadow, shadow_edge, 0, 12);
    struct kos_gui_color taskbar = { 12, 19, 34, 204 };
    struct kos_gui_color taskbar_edge = { 255, 255, 255, 23 };
    (void)vino_gui_glass((struct kos_gui_rect){ 0, bar_y,
        surface.width, bar_height }, taskbar, taskbar_edge, 0, 30);

    struct kos_gui_rect start = { 16, bar_y + 7, 34, 34 };
    if (pointer_inside(start)) {
        struct kos_gui_color hover = { 255, 255, 255, 26 };
        struct kos_gui_color no_border = { 255, 255, 255, 8 };
        (void)vino_gui_glass(start, hover, no_border, 12, 0);
    }
    draw_vino_mark(start.x + 8, start.y + 8, 18);

    uint32_t search_width = surface.width >= 620 ? 210 : surface.width / 3;
    if (search_width < 112) search_width = 112;
    struct kos_gui_rect search = { 60, bar_y + 8, search_width, 32 };
    struct kos_gui_color search_fill = { 255, 255, 255, 18 };
    struct kos_gui_color search_edge = { 255, 255, 255, 23 };
    (void)vino_gui_glass(search, search_fill, search_edge, 18, 0);
    struct kos_gui_color search_color = { 196, 208, 224, 0 };
    draw_dot(search.x + 13, search.y + 10, 9, search_color);
    struct kos_gui_color search_cut = { 12, 19, 34, 0 };
    draw_dot(search.x + 15, search.y + 12, 5, search_cut);
    draw_hline(search.x + 22, search.y + 20, 5, 2, search_color);
    draw_text(search.x + 34, search.y + 9, "Search everything...",
              color_muted, color_panel);

    uint32_t icon_size = surface.width >= 720 ? 38 : 32;
    uint32_t icon_gap = surface.width >= 720 ? 6 : 2;
    uint32_t dock_width = APP_COUNT * icon_size + (APP_COUNT - 1) * icon_gap;
    int32_t dock_x = (int32_t)(surface.width - dock_width) / 2;
    int32_t dock_y = bar_y + (int32_t)(bar_height - 38) / 2;
    for (uint32_t app = 0; app < APP_COUNT; ++app) {
        int32_t x = dock_x + (int32_t)(app * (icon_size + icon_gap));
        struct kos_gui_rect button = { x, dock_y, icon_size, 38 };
        bool active = active_app == (int32_t)app && windows[app].open
            && !windows[app].minimized;
        if (pointer_inside(button) || active) {
            struct kos_gui_color fill = { 255, 255, 255, active ? 30 : 20 };
            struct kos_gui_color edge = { 255, 255, 255, active ? 38 : 10 };
            (void)vino_gui_glass(button, fill, edge, 8, 0);
        }
        uint32_t glyph_size = icon_size - 2;
        draw_app_icon(app, x + 1, dock_y + 1, glyph_size, false);
        if (windows[app].open) {
            int32_t marker_x = x + (int32_t)icon_size / 2 - (active ? 7 : 3);
            draw_dot(marker_x, dock_y + 34, active ? 14 : 6, active ? color_accent : color_dim);
        }
    }

    struct kos_gui_rect notification = {
        (int32_t)surface.width - 210, bar_y + 7, 30, 34,
    };
    draw_app_icon(APP_NOTIFICATIONS, notification.x, notification.y + 1, 32, false);
    struct kos_gui_rect network = {
        (int32_t)surface.width - 174, bar_y + 7, 30, 34,
    };
    draw_app_icon(APP_NETWORK, network.x, network.y + 1, 32, false);
    struct kos_gui_color state_color = system_info.network_available
        ? (system_info.ipv4_address != 0
            ? (struct kos_gui_color){ 74, 222, 128, 0 }
            : (struct kos_gui_color){ 251, 191, 36, 0 })
        : color_muted;
    draw_dot(network.x + 24, network.y + 8, 5, state_color);
    struct kos_gui_rect control = {
        (int32_t)surface.width - 138, bar_y + 7, 34, 34,
    };
    if (pointer_inside(control)) {
        struct kos_gui_color fill = { 255, 255, 255, 24 };
        struct kos_gui_color edge = { 255, 255, 255, 10 };
        (void)vino_gui_glass(control, fill, edge, 8, 0);
    }
    draw_app_icon(APP_CONTROL_CENTER, control.x + 1, control.y + 1, 32, false);

    char uptime[24];
    append_uptime(uptime, sizeof(uptime), sys_gettime());
    struct kos_gui_rect clock = { (int32_t)surface.width - 96, bar_y + 6, 80, 36 };
    if (pointer_inside(clock)) {
        struct kos_gui_color fill = { 255, 255, 255, 24 };
        struct kos_gui_color edge = { 255, 255, 255, 10 };
        (void)vino_gui_glass(clock, fill, edge, 8, 0);
    }
    draw_text(clock.x + 6, clock.y + 3, "Vino OS", color_text, taskbar);
    draw_text(clock.x + 6, clock.y + 19, uptime, color_muted, taskbar);
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
    int32_t body_y = window->y + WINDOW_TITLE_HEIGHT;
    uint32_t side_width = 156;
    uint32_t body_height = window->height - WINDOW_TITLE_HEIGHT;
    struct kos_gui_color side_fill = { 0, 0, 0, 38 };
    struct kos_gui_color side_edge = { 255, 255, 255, 10 };
    (void)vino_gui_glass((struct kos_gui_rect){ window->x + 1, body_y,
        side_width, body_height }, side_fill, side_edge, 0, 0);
    draw_text(window->x + 18, body_y + 13, "Favorites", color_muted, side_fill);
    const char *nav[3] = { "System files", "This PC", "Network status" };
    for (uint32_t index = 0; index < 3; ++index) {
        int32_t row_y = body_y + 36 + (int32_t)index * 34;
        bool selected = index == 0 && string_equal(file_directory, file_home_directory);
        struct kos_gui_rect row = { window->x + 9, row_y,
            side_width - 18, 30 };
        if (selected) {
            struct kos_gui_color fill = { 255, 255, 255, 20 };
            struct kos_gui_color edge = { 255, 255, 255, 8 };
            (void)vino_gui_glass(row, fill, edge, 8, 0);
        }
        draw_text(row.x + 12, row.y + 8, nav[index],
                  selected ? color_text : color_muted, side_fill);
    }
    draw_hline(window->x + 16, body_y + 145, side_width - 32, 1,
               (struct kos_gui_color){ 255, 255, 255, 18 });
    draw_text(window->x + 18, body_y + 160, "Storage", color_muted, side_fill);
    char volume[32] = "System volume unavailable";
    if (system_info.system_volume_letter != 0) {
        string_copy(volume, sizeof(volume), "System volume");
        char letter[5] = { ' ', '(', system_info.system_volume_letter, ')', '\0' };
        string_append(volume, sizeof(volume), letter);
    }
    draw_text(window->x + 18, body_y + 185, volume, color_text, side_fill);

    int32_t content_x = window->x + (int32_t)side_width;
    uint32_t content_width = window->width - side_width;
    struct kos_gui_color toolbar_fill = { 255, 255, 255, 5 };
    struct kos_gui_color toolbar_edge = { 255, 255, 255, 18 };
    (void)vino_gui_glass((struct kos_gui_rect){ content_x, body_y,
        content_width, 38 }, toolbar_fill, toolbar_edge, 0, 0);
    struct kos_gui_color nav_color = file_scroll == 0 ? color_dim : color_text;
    draw_hline(content_x + 15, body_y + 15, 7, 2, nav_color);
    draw_hline(content_x + 15, body_y + 19, 7, 2, nav_color);
    draw_hline(content_x + 15, body_y + 23, 7, 2, nav_color);
    struct kos_gui_rect address = {
        content_x + 42, body_y + 5, content_width - 54, 28,
    };
    draw_box(address, color_panel_low, color_dim);
    draw_text(address.x + 10, address.y + 7, file_directory,
              color_text, color_panel_low);

    int32_t left = content_x + 18;
    int32_t top = body_y + 52;
    draw_text(left, top, "Folders here", color_text, color_panel_low);
    uint32_t card_width = (content_width - 48) / 3;
    for (uint32_t card_index = 0; card_index < 3; ++card_index) {
        uint32_t index = files_quick_directory(card_index);
        if (index >= file_entry_count) continue;
        int32_t card_x = left + (int32_t)(card_index * (card_width + 6));
        int32_t card_y = top + 23;
        struct kos_gui_rect card = { card_x, card_y, card_width, 44 };
        draw_box(card, color_panel_high, color_dim);
        draw_file_glyph(card.x + 8, card.y + 13, true);
        draw_text(card.x + 31, card.y + 15, file_entries[index].name,
                  color_text, color_panel_high);
    }

    int32_t list_y = top + 82;
    draw_text(left, list_y, "Name", color_muted, color_panel_low);
    draw_text(left + (int32_t)content_width - 170, list_y, "Type", color_muted,
              color_panel_low);
    draw_text(left + (int32_t)content_width - 82, list_y, "Size", color_muted,
              color_panel_low);
    list_y += 22;
    uint32_t row_height = 25;
    uint32_t available_height = window->height > 185 ? window->height - 185 : 50;
    uint32_t visible = available_height / row_height;
    if (visible > 12) visible = 12;
    for (uint32_t row = 0; row < visible; ++row) {
        uint32_t index = file_scroll + row;
        if (index >= file_entry_count) break;
        struct kos_gui_rect item = {
            left, list_y + (int32_t)(row * row_height), content_width - 30, row_height,
        };
        struct kos_gui_color fill = file_selected == index
            ? (struct kos_gui_color){ 59, 130, 246, 40 }
            : (struct kos_gui_color){ 0, 0, 0, 16 };
        (void)vino_gui_glass(item, fill,
            (struct kos_gui_color){ 255, 255, 255, 4 }, 5, 0);
        draw_file_glyph(item.x + 5, item.y + 5,
                        file_entries[index].type == KOS_STAT_DIRECTORY);
        draw_text(item.x + 28, item.y + 7, file_entries[index].name,
                  color_text, color_panel_low);
        draw_text(left + (int32_t)content_width - 170, item.y + 7,
            file_entries[index].type == KOS_STAT_DIRECTORY ? "Folder" : "File",
            color_muted, color_panel_low);
        char size[24];
        number_text(file_entries[index].size, size);
        draw_text(left + (int32_t)content_width - 82, item.y + 7, size,
                  color_muted, color_panel_low);
    }
    if (file_error[0] != '\0') {
        draw_text(left + 8, list_y + 18, file_error, color_cyan, color_panel_low);
    }
    else if (file_entry_count == 0) {
        draw_text(left + 8, list_y + 18, "This location is empty.", color_muted,
                  color_panel_low);
    }
    draw_text(left, window->y + (int32_t)window->height - 22,
        "Double-click to open   Backspace up   R refresh",
        color_muted, color_panel_low);
}

static void draw_settings_content(struct vino_window *window) {
    static const char *const pages[6] = {
        "System", "Bluetooth", "Network", "Personalization", "Applications", "About",
    };
    int32_t top = window->y + WINDOW_TITLE_HEIGHT + 10;
    int32_t side_x = window->x + 10;
    uint32_t side_width = 152;
    uint32_t side_height = window->height - WINDOW_TITLE_HEIGHT - 20;
    struct kos_gui_color side_fill = { 0, 0, 0, 42 };
    struct kos_gui_color side_edge = { 255, 255, 255, 10 };
    (void)vino_gui_glass((struct kos_gui_rect){ side_x, top, side_width, side_height },
                         side_fill, side_edge, 12, 0);
    for (uint32_t page = 0; page < 6; ++page) {
        int32_t row_y = top + 8 + (int32_t)page * 36;
        struct kos_gui_rect row = { side_x + 7, row_y, side_width - 14, 31 };
        bool selected = settings_page == page;
        if (selected) {
            struct kos_gui_color fill = { 255, 255, 255, 23 };
            struct kos_gui_color edge = { 255, 255, 255, 9 };
            (void)vino_gui_glass(row, fill, edge, 8, 0);
        }
        draw_text(row.x + 10, row.y + 9, pages[page],
                  selected ? color_text : color_muted, color_panel_low);
    }

    int32_t left = window->x + (int32_t)side_width + 28;
    int32_t right = window->x + (int32_t)window->width - 20;
    uint32_t content_width = right > left ? (uint32_t)(right - left) : 1;
    int32_t y = top + 8;
    draw_text(left, y, pages[settings_page < 6 ? settings_page : 0],
              color_text, color_panel_low);
    y += 34;

    if (settings_page == 0) {
        struct kos_gui_rect card = { left, y, content_width, 72 };
        draw_box(card, color_panel_high, color_dim);
        draw_text(card.x + 14, card.y + 12, "Vino OS Default", color_text,
                  color_panel_high);
        draw_text(card.x + 14, card.y + 39, "Native desktop session",
                  color_muted, color_panel_high);
        card.y += 84;
        draw_box(card, color_panel_high, color_dim);
        char uptime[24] = "Uptime: ";
        append_uptime(uptime, sizeof(uptime), sys_gettime());
        draw_text(card.x + 14, card.y + 14, uptime, color_accent, color_panel_high);
        char task_count[64] = "Active tasks: ";
        char digits[24];
        number_text(system_info.task_count, digits);
        string_append(task_count, sizeof(task_count), system_info_available
            ? digits : "unavailable");
        draw_text(card.x + 14, card.y + 42, task_count, color_muted, color_panel_high);
        char memory[64];
        format_memory_summary(memory);
        draw_text(card.x + 14, card.y + 59, memory, color_muted, color_panel_high);
    }
    else if (settings_page == 1) {
        draw_text(left, y, "Bluetooth controller", color_text, color_panel_low);
        draw_text(left, y + 30, "Unavailable: no kernel Bluetooth driver/service.",
                  color_muted, color_panel_low);
        draw_text(left, y + 58, "No radio state or scan results are simulated.", color_muted,
                  color_panel_low);
    }
    else if (settings_page == 2) {
        refresh_system_info();
        char ip[16];
        format_ipv4(system_info.ipv4_address, ip);
        struct kos_gui_rect card = { left, y, content_width, 74 };
        draw_box(card, color_panel_high, color_dim);
        draw_text(card.x + 14, card.y + 12,
            !system_info_available ? "Kernel network API unavailable"
                : system_info.network_available ? "NIC reported by kernel"
                : "No NIC reported by kernel",
            color_text, color_panel_high);
        draw_text(card.x + 14, card.y + 40,
            !system_info_available ? "Unavailable"
                : system_info.ipv4_address != 0 ? ip : "IPv4 address unavailable",
            color_cyan, color_panel_high);
        draw_text(left, y + 94, "Press N to open Network details.",
                  color_muted, color_panel_low);
    }
    else if (settings_page == 3) {
        struct kos_gui_rect card = { left, y, content_width, 62 };
        draw_box(card, color_panel_high, color_dim);
        draw_text(card.x + 14, card.y + 11, "Accent color", color_text, color_panel_high);
        const char *theme = theme_variant == 0 ? "Ocean blue" : theme_variant == 1
            ? "Lagoon teal" : "Evening violet";
        draw_text(card.x + 14, card.y + 36, theme, color_accent, color_panel_high);
        card.y += 72;
        draw_box(card, color_panel_high, color_dim);
        draw_text(card.x + 14, card.y + 11, "Motion", color_text, color_panel_high);
        draw_text(card.x + 14, card.y + 36,
            reduced_motion ? "Reduced motion" : "Subtle transitions",
            color_accent, color_panel_high);
        card.y += 72;
        draw_box(card, color_panel_high, color_dim);
        draw_text(card.x + 14, card.y + 11, "Window shadow", color_text,
                  color_panel_high);
        draw_text(card.x + 14, card.y + 36,
            strong_window_shadow ? "Strong" : "Soft",
            color_accent, color_panel_high);
        draw_text(left, y + 238, "Wallpaper", color_text, color_panel_low);
        draw_text(left, y + 262, wallpaper_loaded
            ? "Demoweb local image  |  cover scaling" : "Local wallpaper asset unavailable",
            wallpaper_loaded ? color_muted : (struct kos_gui_color){ 251, 146, 60, 0 },
            color_panel_low);
        draw_text(left, y + 294, "1 / 2 / 3 changes desktop-session appearance only.",
                  color_muted, color_panel_low);
        draw_text(left, y + 318, "Not saved; no kernel display-settings service is exposed.",
                  color_muted, color_panel_low);
    }
    else if (settings_page == 4) {
        for (uint32_t app = 0; app < APP_COUNT; ++app) {
            draw_app_icon(app, left, y + (int32_t)(app * 40), 32, true);
            draw_text(left + 44, y + 9 + (int32_t)(app * 40), app_names[app],
                      color_text, color_panel_low);
        }
    }
    else {
        draw_text(left, y, "Vino OS Default", color_text, color_panel_low);
        draw_text(left, y + 30, "Native user-space GUI; no browser or WebView.",
                  color_muted, color_panel_low);
        draw_text(left, y + 58, "Files: VFS. Info: uptime, tasks and PMM memory metrics.",
                  color_muted, color_panel_low);
        draw_text(left, y + 86, "Bluetooth, brightness and audio APIs unavailable.",
                  color_muted, color_panel_low);
        draw_text(left, y + 110, "Notification service unavailable.",
                  color_muted, color_panel_low);
    }
}

static void draw_control_center_content(struct vino_window *window) {
    refresh_system_info();
    int32_t left = window->x + 18;
    int32_t top = window->y + WINDOW_TITLE_HEIGHT + 14;
    draw_text(left, top, "Quick settings", color_text, color_panel_low);
    const char *titles[4] = { "Network", "Personalize", "Notifications", "Motion" };
    const char *details[4] = {
        !system_info_available ? "API unavailable"
            : !system_info.network_available ? "No NIC"
            : system_info.ipv4_address != 0 ? "IPv4 configured" : "NIC available",
        theme_variant == 0 ? "Ocean blue" : theme_variant == 1 ? "Lagoon teal" : "Violet",
        "Unavailable",
        reduced_motion ? "Reduced (session)" : "Subtle (session)",
    };
    const uint32_t icons[4] = {
        APP_NETWORK, APP_SETTINGS, APP_NOTIFICATIONS, APP_CONTROL_CENTER,
    };
    uint32_t cell_width = (window->width - 48) / 2;
    for (uint32_t index = 0; index < 4; ++index) {
        int32_t x = left + (int32_t)(index % 2) * (int32_t)(cell_width + 10);
        int32_t y = top + 30 + (int32_t)(index / 2) * 78;
        struct kos_gui_rect tile = { x, y, cell_width, 68 };
        struct kos_gui_color fill = index == 0 && system_info.network_available
            ? (struct kos_gui_color){ 29, 94, 183, 0 } : color_panel_high;
        draw_box(tile, fill, color_dim);
        draw_app_icon(icons[index], x + 8, y + 17, 34, true);
        draw_text(x + 49, y + 14, titles[index], color_text, fill);
        draw_text(x + 49, y + 39, details[index], color_muted, fill);
    }
    int32_t status_y = top + 192;
    draw_hline(left, status_y, window->width - 36, 1,
               (struct kos_gui_color){ 255, 255, 255, 20 });
    draw_text(left, status_y + 16, "System status", color_text, color_panel_low);
    char uptime[24] = "Uptime: ";
    append_uptime(uptime, sizeof(uptime), sys_gettime());
    draw_text(left, status_y + 40, uptime, color_muted, color_panel_low);
    char tasks[48] = "Tasks: ";
    char digits[24];
    number_text(system_info.task_count, digits);
    string_append(tasks, sizeof(tasks), system_info_available ? digits : "unavailable");
    draw_text(left, status_y + 60, tasks, color_muted, color_panel_low);
    char memory[64];
    format_memory_summary(memory);
    draw_text(left, status_y + 80, memory, color_muted, color_panel_low);
    const char *volume_status = !system_info_available
        ? "System volume state unavailable"
        : system_info.system_volume_letter != 0
            ? "Installed system volume mounted"
            : "No installed volume reported";
    draw_text(left, status_y + 102, volume_status, color_cyan, color_panel_low);
}

static void draw_notifications_content(struct vino_window *window) {
    int32_t left = window->x + 14;
    int32_t top = window->y + WINDOW_TITLE_HEIGHT + 14;
    draw_text(left, top, "Notifications unavailable", color_text, color_panel_low);
    draw_text(left, top + 34, "No kernel notification service is exposed.",
              color_muted, color_panel_low);
    draw_text(left, top + 58, "No demo or placeholder notices are shown.",
              color_muted, color_panel_low);
}

static void draw_network_content(struct vino_window *window) {
    refresh_system_info();
    int32_t left = window->x + 22;
    if (window->height < 220) {
        int32_t y = window->y + WINDOW_TITLE_HEIGHT + 12;
        draw_text(left, y, !system_info_available ? "Kernel network API unavailable"
            : system_info.network_available ? "NIC reported by kernel"
                                            : "No NIC reported by kernel",
            system_info_available && system_info.network_available
                ? color_cyan : color_muted, color_panel_low);
        char ip[16];
        format_ipv4(system_info.ipv4_address, ip);
        draw_text(left, y + 22, system_info.network_available
            ? (system_info.ipv4_address != 0 ? ip : "IPv4 unassigned")
            : "IPv4 unavailable", color_text, color_panel_low);
        draw_text(left, y + 42, dhcp_state_name(system_info.dhcp_state),
                  color_muted, color_panel_low);
        int32_t action_y = window->y + (int32_t)window->height - 58;
        struct kos_gui_rect action = { left + 4, action_y, 164, 34 };
        draw_box(action, system_info.network_available ? color_panel_high : color_panel_low,
                 color_dim);
        draw_text(action.x + 10, action.y + 10,
            system_info.network_available ? "Request DHCP (C)" : "DHCP unavailable",
            system_info.network_available ? color_text : color_muted, color_panel_high);
        draw_text(left + 178, action_y + 10, network_action_message[0] != '\0'
            ? network_action_message : "R refresh",
            color_muted, color_panel_low);
        return;
    }
    if (window->height < 360) {
        int32_t y = window->y + WINDOW_TITLE_HEIGHT + 12;
        draw_text(left, y, !system_info_available ? "Kernel network API unavailable"
            : system_info.network_available ? "NIC reported by kernel"
                                            : "No NIC reported by kernel",
            system_info_available && system_info.network_available
                ? color_cyan : color_muted, color_panel_low);
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
        int32_t action_y = window->y + (int32_t)window->height - 58;
        struct kos_gui_rect action = { left + 4, action_y, 164, 34 };
        draw_box(action, system_info.network_available ? color_panel_high : color_panel_low,
                 color_dim);
        draw_text(action.x + 10, action.y + 10,
            system_info.network_available ? "Request DHCP (C)" : "DHCP unavailable",
            system_info.network_available ? color_text : color_muted, color_panel_high);
        draw_text(left + 178, action_y + 10, network_action_message[0] != '\0'
            ? network_action_message : "R refresh",
            color_muted, color_panel_low);
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
        !system_info_available ? "Kernel network API unavailable"
            : system_info.network_available ? "NIC reported by kernel"
                                            : "No NIC reported by kernel",
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
        system_info.network_available && system_info.device_name[0]
            ? system_info.device_name : "Unavailable",
        system_info.network_available
            ? system_info.ipv4_address != 0 ? ip : "Unassigned" : "Unavailable",
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
    int32_t action_y = window->y + (int32_t)window->height - 58;
    struct kos_gui_rect action = { left + 4, action_y, 164, 34 };
    draw_box(action, system_info.network_available ? color_panel_high : color_panel_low,
             color_dim);
    draw_text(action.x + 10, action.y + 10,
        system_info.network_available ? "Request DHCP (C)" : "DHCP unavailable",
        system_info.network_available ? color_text : color_muted, color_panel_high);
    draw_text(left + 178, action_y + 10, network_action_message[0] != '\0'
        ? network_action_message : "R refresh | Wi-Fi toggle unavailable",
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
    struct kos_gui_color shadow = { 0, 0, 0, strong_window_shadow ? 110 : 82 };
    struct kos_gui_color shadow_border = { 0, 0, 0, 1 };
    (void)vino_gui_glass((struct kos_gui_rect){ x + 8, y + 12,
        bounds.width, bounds.height }, shadow, shadow_border, 20, 0);
    struct kos_gui_color window_fill = { 15, 22, 40, 179 };
    struct kos_gui_color window_border = edge;
    window_border.alpha = (int32_t)app == active_app ? 96 : 23;
    (void)vino_gui_glass(bounds, window_fill, window_border, 20, 30);
    struct kos_gui_color title_tint = { 255, 255, 255, 8 };
    struct kos_gui_color title_border = { 255, 255, 255, 4 };
    (void)vino_gui_glass((struct kos_gui_rect){ x + 1, y + 1,
        bounds.width - 2, WINDOW_TITLE_HEIGHT }, title_tint, title_border, 18, 0);
    draw_hline(x + 1, y + WINDOW_TITLE_HEIGHT, bounds.width - 2, 1,
               (struct kos_gui_color){ 255, 255, 255, 23 });
    draw_round_dot(x + 16, y + 14, 12, (struct kos_gui_color){ 255, 95, 87, 0 });
    draw_round_dot(x + 35, y + 14, 12, (struct kos_gui_color){ 254, 188, 46, 0 });
    draw_round_dot(x + 54, y + 14, 12, (struct kos_gui_color){ 40, 200, 64, 0 });
    uint32_t title_length = string_length(app_names[app]);
    int32_t title_width = (int32_t)(title_length * 8 + 32);
    int32_t title_x = x + ((int32_t)bounds.width - title_width) / 2;
    draw_app_icon(app, title_x, y + 4, 32, false);
    draw_text(title_x + 32, y + 13, app_names[app], color_text, color_panel_high);
    if (app == APP_TERMINAL) draw_terminal_content(&visual_window);
    else if (app == APP_FILES) draw_files_content(&visual_window);
    else if (app == APP_SETTINGS) draw_settings_content(&visual_window);
    else if (app == APP_NETWORK) draw_network_content(&visual_window);
    else if (app == APP_CONTROL_CENTER) draw_control_center_content(&visual_window);
    else if (app == APP_NOTIFICATIONS) draw_notifications_content(&visual_window);
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

static bool menu_app_matches(uint32_t app) {
    if (app >= APP_COUNT) return false;
    if (menu_search[0] == '\0') return true;
    uint32_t query_length = string_length(menu_search);
    uint32_t name_length = string_length(app_names[app]);
    if (query_length > name_length) return false;
    for (uint32_t start = 0; start + query_length <= name_length; ++start) {
        uint32_t offset = 0;
        while (offset < query_length) {
            char left = app_names[app][start + offset];
            char right = menu_search[offset];
            if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
            if (left != right) break;
            ++offset;
        }
        if (offset == query_length) return true;
    }
    return false;
}

static uint32_t menu_app_at(uint32_t visible_index) {
    for (uint32_t app = 0; app < APP_COUNT; ++app) {
        if (!menu_app_matches(app)) continue;
        if (visible_index == 0) return app;
        --visible_index;
    }
    return APP_COUNT;
}

static void draw_start_menu(void) {
    if (!start_menu_open && menu_animation_step == 0) return;
    uint32_t target_height = surface.height > 592 ? 520 : surface.height - 72;
    uint32_t height = target_height * menu_animation_step / 4;
    if (height < 60) height = menu_animation_step == 0 ? 0 : 60;
    uint32_t width = surface.width > 508 ? 480 : surface.width - 28;
    int32_t x = 14;
    int32_t y = (int32_t)surface.height - 56 - (int32_t)height;
    struct kos_gui_color panel_border = { 255, 255, 255, 23 };
    uint32_t blur_radius = menu_animation_step == 4 ? 30 : 0;
    draw_glass((struct kos_gui_rect){ x, y, width, height },
               color_panel, panel_border, 20, blur_radius);

    if (height >= 62) {
        struct kos_gui_rect search = { x + 20, y + 18, width - 40, 38 };
        struct kos_gui_color search_fill = { 255, 255, 255, 15 };
        struct kos_gui_color search_edge = { 255, 255, 255, 20 };
        (void)vino_gui_glass(search, search_fill, search_edge, 12, 0);
        draw_dot(search.x + 13, search.y + 11, 9, color_muted);
        draw_dot(search.x + 15, search.y + 13, 5, color_panel_low);
        draw_hline(search.x + 22, search.y + 20, 5, 2, color_muted);
        draw_text(search.x + 36, search.y + 11,
            menu_search[0] == '\0' ? "Search available apps" : menu_search,
            menu_search[0] == '\0' ? color_muted : color_text, color_panel);
    }

    if (height >= 100) {
        draw_text(x + 24, y + 72, "Available apps", color_text, color_panel);
    }
    uint32_t visible_apps = 0;
    for (uint32_t app = 0; app < APP_COUNT; ++app)
        if (menu_app_matches(app)) ++visible_apps;
    const uint32_t cell_width = (width - 36) / 4;
    const uint32_t cell_height = 78;
    for (uint32_t item = 0; item < visible_apps; ++item) {
        if (height <= 100) continue;
        uint32_t app = menu_app_at(item);
        uint32_t row = item / 4;
        uint32_t column = item % 4;
        int32_t cell_x = x + 18 + (int32_t)(column * cell_width);
        int32_t cell_y = y + 92 + (int32_t)(row * cell_height);
        if ((uint32_t)(cell_y - y) + cell_height > height - 100) continue;
        struct kos_gui_rect cell = { cell_x, cell_y, cell_width, cell_height - 4 };
        if (menu_selected == app || pointer_inside(cell)) {
            struct kos_gui_color fill = { 255, 255, 255,
                menu_selected == app ? 20 : 14 };
            struct kos_gui_color border = { 255, 255, 255, 5 };
            (void)vino_gui_glass(cell, fill, border, 12, 0);
        }
        uint32_t icon_size = 36;
        int32_t icon_x = cell_x + ((int32_t)cell_width - (int32_t)icon_size) / 2;
        draw_app_icon(app, icon_x, cell_y + 3, icon_size, true);
        int32_t label_x = cell_x + 3;
        int32_t label_y = cell_y + 47;
        draw_text(label_x, label_y, app_names[app], color_text, color_panel);
    }

    if (height > 310) {
        int32_t recent_y = y + 280;
        draw_hline(x + 22, recent_y - 12, width - 44, 1,
                   (struct kos_gui_color){ 255, 255, 255, 20 });
        draw_text(x + 24, recent_y, "Recent", color_text, color_panel);
        uint32_t entries = recent_app_count < 2 ? recent_app_count : 2;
        if (entries == 0) {
            draw_text(x + 28, recent_y + 30, "No recently opened apps",
                      color_muted, color_panel);
        }
        for (uint32_t index = 0; index < entries; ++index) {
            uint32_t app = recent_apps[index];
            int32_t item_y = recent_y + 24 + (int32_t)index * 38;
            draw_app_icon(app, x + 26, item_y + 2, 32, true);
            draw_text(x + 70, item_y + 1, app_names[app], color_text, color_panel);
            draw_text(x + 70, item_y + 17, "Opened this session", color_muted,
                      color_panel);
        }
    }

    if (height > 150) {
        int32_t footer_y = y + (int32_t)height - 54;
        draw_hline(x + 18, footer_y - 7, width - 36, 1,
                   (struct kos_gui_color){ 255, 255, 255, 20 });
        draw_vino_mark(x + 28, footer_y + 7, 24);
        draw_text(x + 62, footer_y + 11, "Vino OS Default", color_text, color_panel);
        struct kos_gui_rect power = {
            x + (int32_t)width - 108, footer_y + 5, 94, 38,
        };
        draw_box(power, color_panel_high, color_dim);
        draw_text(power.x + 20, power.y + 12, "Power", color_text, color_panel_high);
    }
}

static uint32_t pointer_hover_at_current_position(void) {
    if (!pointer_available || power_dialog_state != POWER_DIALOG_CLOSED
        || context_menu_open) return 0;

    if (start_menu_open && menu_animation_step != 0) {
        uint32_t target_height = surface.height > 592 ? 520 : surface.height - 72;
        uint32_t height = target_height * menu_animation_step / 4;
        if (height < 60) height = 60;
        uint32_t width = surface.width > 508 ? 480 : surface.width - 28;
        int32_t x = 14;
        int32_t y = (int32_t)surface.height - 56 - (int32_t)height;
        if (height > 100) {
            uint32_t cell_width = (width - 36) / 4;
            const uint32_t cell_height = 78;
            uint32_t item = 0;
            for (uint32_t app = 0; app < APP_COUNT; ++app) {
                if (!menu_app_matches(app)) continue;
                uint32_t cell_y = (uint32_t)(y + 92)
                    + (item / 4) * cell_height;
                if (cell_y >= (uint32_t)y
                    && cell_y - (uint32_t)y + cell_height <= height - 100) {
                    struct kos_gui_rect cell = {
                        x + 18 + (int32_t)((item % 4) * cell_width),
                        (int32_t)cell_y, cell_width, cell_height - 4,
                    };
                    if (app != menu_selected
                        && vino_gui_rect_contains(&cell, pointer_x, pointer_y))
                        return 0x100u + app;
                }
                ++item;
            }
        }
    }

    int32_t bar_y = (int32_t)(surface.height - 48);
    struct kos_gui_rect start = { 16, bar_y + 7, 34, 34 };
    if (vino_gui_rect_contains(&start, pointer_x, pointer_y)) return 1;

    uint32_t icon_size = surface.width >= 720 ? 38 : 32;
    uint32_t icon_gap = surface.width >= 720 ? 6 : 2;
    uint32_t dock_width = APP_COUNT * icon_size + (APP_COUNT - 1) * icon_gap;
    int32_t dock_x = (int32_t)(surface.width - dock_width) / 2;
    int32_t dock_y = bar_y + 5;
    for (uint32_t app = 0; app < APP_COUNT; ++app) {
        struct kos_gui_rect button = {
            dock_x + (int32_t)(app * (icon_size + icon_gap)), dock_y,
            icon_size, 38,
        };
        bool active = active_app == (int32_t)app && windows[app].open
            && !windows[app].minimized;
        if (!active && vino_gui_rect_contains(&button, pointer_x, pointer_y))
            return 2u + app;
    }

    struct kos_gui_rect control = {
        (int32_t)surface.width - 138, bar_y + 7, 34, 34,
    };
    if (vino_gui_rect_contains(&control, pointer_x, pointer_y)) return 0x40u;
    struct kos_gui_rect clock = {
        (int32_t)surface.width - 96, bar_y + 6, 80, 36,
    };
    if (vino_gui_rect_contains(&clock, pointer_x, pointer_y)) return 0x41u;
    return 0;
}

static void draw_context_menu(void) {
    if (!context_menu_open) return;
    int32_t x = context_menu_at_pointer ? context_menu_x
        : active_app >= 0 ? windows[active_app].x + 64 : 184;
    int32_t y = context_menu_at_pointer ? context_menu_y
        : active_app >= 0 ? windows[active_app].y + 74 : 142;
    uint32_t width = surface.width > 244 ? 220 : surface.width - 24;
    uint32_t row_height = surface.height < 320 ? 22 : 31;
    uint32_t row_gap = surface.height < 320 ? 24 : 34;
    uint32_t height = 6 * row_gap + 10;
    if ((uint64_t)x + width > surface.width) x = (int32_t)surface.width - width - 8;
    if (x < 8) x = 8;
    if ((uint64_t)y + height > surface.height - 48) {
        y = (int32_t)surface.height - 48 - (int32_t)height;
    }
    if (y < 8) y = 8;
    draw_box((struct kos_gui_rect){ x, y, width, height }, color_panel, color_accent);
    static const char *const items[] = {
        "Open Terminal", "Open Files", "Open Settings", "Open Network",
        "Control Center", "Notifications" };
    for (uint32_t index = 0; index < APP_COUNT; ++index) {
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

static void draw_power_dialog(void) {
    if (power_dialog_state == POWER_DIALOG_CLOSED) return;
    uint32_t width = surface.width > 484 ? 460 : surface.width - 24;
    uint32_t height = 220;
    int32_t x = (int32_t)(surface.width - width) / 2;
    int32_t y = (int32_t)(surface.height - height) / 2;
    draw_box((struct kos_gui_rect){ x, y, width, height }, color_panel, color_accent);
    draw_text(x + 20, y + 18, "POWER OPTIONS", color_cyan, color_panel);
    if (power_dialog_state == POWER_DIALOG_CHOOSE) {
        draw_text(x + 20, y + 54, "Choose a kernel power action.", color_text, color_panel);
        draw_text(x + 20, y + 78, width >= 430
            ? "The next screen asks for confirmation." : "R restart / S halt",
                  color_muted, color_panel);
        struct kos_gui_rect restart, shutdown, cancel;
        if (width >= 430) {
            restart = (struct kos_gui_rect){ x + 18, y + 126, 126, 38 };
            shutdown = (struct kos_gui_rect){ x + 155, y + 126, 126, 38 };
            cancel = (struct kos_gui_rect){ x + 292, y + 126, 126, 38 };
        }
        else {
            uint32_t button_width = width - 36;
            restart = (struct kos_gui_rect){ x + 18, y + 96, button_width, 32 };
            shutdown = (struct kos_gui_rect){ x + 18, y + 136, button_width, 32 };
            cancel = (struct kos_gui_rect){ x + 18, y + 176, button_width, 32 };
        }
        draw_box(restart, color_panel_high, color_dim);
        draw_box(shutdown, color_panel_high, color_dim);
        draw_box(cancel, color_panel_low, color_dim);
        draw_text(restart.x + 16, restart.y + (width >= 430 ? 12 : 9),
                  "Restart", color_text, color_panel_high);
        draw_text(shutdown.x + 16, shutdown.y + (width >= 430 ? 12 : 9),
                  "Halt system", color_text, color_panel_high);
        draw_text(cancel.x + 16, cancel.y + (width >= 430 ? 12 : 9),
                  "Cancel", color_text, color_panel_low);
        if (width >= 430) draw_text(x + 20, y + 184,
            "Keys: R restart / S halt / Esc cancel",
                  color_muted, color_panel);
        return;
    }
    bool restart = power_dialog_state == POWER_DIALOG_CONFIRM_RESTART;
    draw_text(x + 20, y + 54, width >= 430
        ? (restart ? "Restart Vino OS now?" : "Halt the Vino OS CPU now?")
        : (restart ? "Restart Vino OS?" : "Halt CPU now?"),
              color_text, color_panel);
    draw_text(x + 20, y + 82, restart
        ? (width >= 430 ? "Kernel restart request after confirmation."
                        : "Kernel restart request.")
        : (width >= 430 ? "CPU halt requested; hardware power-off may not occur."
                        : "CPU halt; power-off not guaranteed."),
              color_muted, color_panel);
    uint32_t action_width = width >= 430 ? 126 : (width - 48) / 2;
    struct kos_gui_rect confirm = {
        x + (width >= 430 ? 150 : 18), y + 150, action_width, 38,
    };
    struct kos_gui_rect cancel = {
        x + (width >= 430 ? 286 : 30 + action_width), y + 150, action_width, 38,
    };
    draw_box(confirm, color_panel_high, color_accent);
    draw_box(cancel, color_panel_low, color_dim);
    draw_text(confirm.x + 16, confirm.y + 12, "Confirm", color_text, color_panel_high);
    draw_text(cancel.x + 16, cancel.y + 12, "Cancel", color_text, color_panel_low);
    draw_text(x + 20, y + 202, "Enter/Y confirm  |  N/Esc cancel",
              color_muted, color_panel);
    if (power_dialog_message[0] != '\0')
        draw_text(x + 20, y + 122, power_dialog_message, color_cyan, color_panel);
}

static void render_ui_contents(void) {
    draw_windows();
    draw_taskbar();
    draw_start_menu();
    draw_context_menu();
    draw_preview();
    draw_about();
    draw_power_dialog();
    if (move_window_mode) {
        uint32_t overlay_width = surface.width > 440 ? surface.width - 440 : surface.width - 24;
        int32_t overlay_x = surface.width > 440 ? 220 : 12;
        draw_box((struct kos_gui_rect){ overlay_x, 38, overlay_width, 28 },
                 color_panel_high, color_accent);
        draw_text(overlay_x + 12, 48, "Move window: H/J/K/L  |  G to finish",
                  color_cyan, color_panel_high);
    }
}

static void render_ui(void) {
    (void)vino_gui_frame_begin(true);
    if (!wallpaper_cache_valid || vino_gui_background_restore() < 0) {
        draw_wallpaper();
        wallpaper_cache_valid = vino_gui_background_capture() == 0;
    }
    render_ui_contents();
    (void)vino_gui_frame_present();
    pointer_hover_target = pointer_hover_at_current_position();
}

static struct kos_gui_rect menu_damage_bounds(void) {
    uint32_t height = surface.height > 592 ? 520 : surface.height - 72;
    uint32_t width = surface.width > 508 ? 480 : surface.width - 28;
    int32_t x = 14;
    int32_t y = (int32_t)surface.height - 56 - (int32_t)height;
    return (struct kos_gui_rect){ x, y, width, height };
}

static bool rects_intersect(struct kos_gui_rect left, struct kos_gui_rect right) {
    int64_t left_right = (int64_t)left.x + left.width;
    int64_t left_bottom = (int64_t)left.y + left.height;
    int64_t right_right = (int64_t)right.x + right.width;
    int64_t right_bottom = (int64_t)right.y + right.height;
    return left.x < right_right && right.x < left_right
        && left.y < right_bottom && right.y < left_bottom;
}

static void draw_windows_intersecting(struct kos_gui_rect damage) {
    for (uint32_t order = 0; order < APP_COUNT; ++order) {
        for (uint32_t app = 0; app < APP_COUNT; ++app) {
            struct vino_window *window = &windows[app];
            if (!window->open || window->minimized || window->z_order != order + 1)
                continue;
            struct kos_gui_rect bounds = {
                window->x, window->y - (int32_t)(animation_frame * 7),
                window->width, window->height,
            };
            if (rects_intersect(bounds, damage)) draw_window(app);
        }
    }
}

static void render_menu_region(void) {
    struct kos_gui_rect damage = menu_damage_bounds();
    if (vino_gui_frame_begin_damage(damage) < 0) {
        render_ui();
        return;
    }
    if (vino_gui_background_restore_rect(damage) < 0) {
        (void)vino_gui_frame_present();
        render_ui();
        return;
    }
    /* Menu animation never intersects the bottom bar. Redraw only windows
       beneath its damage and menu overlays, not the entire desktop/taskbar. */
    draw_windows_intersecting(damage);
    draw_start_menu();
    draw_context_menu();
    draw_preview();
    draw_about();
    draw_power_dialog();
    (void)vino_gui_frame_present();
    pointer_hover_target = pointer_hover_at_current_position();
}

static void render_ui_region(struct kos_gui_rect damage) {
    if (vino_gui_frame_begin_damage(damage) < 0) {
        render_ui();
        return;
    }
    if (vino_gui_background_restore_rect(damage) < 0) {
        (void)vino_gui_frame_present();
        render_ui();
        return;
    }
    render_ui_contents();
    (void)vino_gui_frame_present();
    pointer_hover_target = pointer_hover_at_current_position();
}

static void render_hover_region(void) {
    if (start_menu_open && menu_animation_step != 0) {
        render_menu_region();
        return;
    }
    uint32_t bar_height = surface.height < 48 ? surface.height : 48;
    render_ui_region((struct kos_gui_rect){ 0,
        (int32_t)(surface.height - bar_height), surface.width, bar_height });
}

static void animate_menu(bool opening) {
    if (reduced_motion) {
        menu_animation_step = opening ? 4 : 0;
        render_menu_region();
        visual_action_rendered = true;
        return;
    }
    while ((opening && menu_animation_step < 4)
           || (!opening && menu_animation_step > 0)) {
        if (opening) ++menu_animation_step;
        else --menu_animation_step;
        render_menu_region();
        visual_action_rendered = true;
        (void)sys_sleep(16);
    }
}

static void set_start_menu(bool open) {
    if (start_menu_open == open) return;
    if (open) {
        context_menu_open = false;
        menu_search[0] = '\0';
        menu_selected = 0;
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
    context_menu_at_pointer = false;
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
    if (key == '\r') key = '\n';
    if (power_dialog_state != POWER_DIALOG_CLOSED) {
        handle_power_dialog_key(key);
        return;
    }
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
        else if (key == 'p' || key == 'P') {
            set_start_menu(false);
            open_power_dialog();
        }
        else if (key == 'j' || key == 'k') {
            for (uint32_t attempt = 0; attempt < APP_COUNT; ++attempt) {
                if (key == 'j') menu_selected = (menu_selected + 1) % APP_COUNT;
                else menu_selected = menu_selected == 0 ? APP_COUNT - 1 : menu_selected - 1;
                if (menu_app_matches(menu_selected)) break;
            }
        }
        else if (key == '\n') start_menu_launch();
        else if (key >= '1' && key <= '6') {
            menu_selected = (uint32_t)(key - '1');
            start_menu_launch();
        }
        else if (key == '\b') {
            uint32_t length = string_length(menu_search);
            if (length != 0) menu_search[length - 1] = '\0';
            uint32_t first = menu_app_at(0);
            if (first < APP_COUNT) menu_selected = first;
        }
        else if ((uint8_t)key >= 32 && (uint8_t)key < 127
                 && string_length(menu_search) + 1 < sizeof(menu_search)) {
            uint32_t length = string_length(menu_search);
            menu_search[length] = key;
            menu_search[length + 1] = '\0';
            uint32_t first = menu_app_at(0);
            if (first < APP_COUNT) menu_selected = first;
        }
        return;
    }
    if (context_menu_open) {
        if (key == 27 || key == 'c') context_menu_open = false;
        else if (key == 'j' && context_selected + 1 < APP_COUNT) ++context_selected;
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
    if (active_app == APP_SETTINGS && settings_page == 2 && key == 'n') {
        open_app(APP_NETWORK);
        return;
    }
    if (active_app == APP_SETTINGS && settings_page == 3
        && key >= '1' && key <= '3') {
        if (key == '1') {
            theme_variant = (theme_variant + 1) % 3;
            color_set_accent(theme_variant);
            wallpaper_cache_valid = false;
        }
        else if (key == '2') reduced_motion = !reduced_motion;
        else if (key == '3') strong_window_shadow = !strong_window_shadow;
        return;
    }
    if (active_app == APP_NETWORK && key == 'r') {
        refresh_system_info();
        return;
    }
    if (active_app == APP_NETWORK && key == 'c') {
        request_network_dhcp();
        return;
    }
    if (active_app == APP_CONTROL_CENTER && key >= '1' && key <= '4') {
        if (key == '1') open_app(APP_NETWORK);
        else if (key == '2') {
            settings_page = 3;
            open_app(APP_SETTINGS);
        }
        else if (key == '3') open_app(APP_NOTIFICATIONS);
        else reduced_motion = !reduced_motion;
        return;
    }
    if (key >= '1' && key <= '6') {
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

static bool point_hits_window(uint32_t app, int32_t x, int32_t y) {
    const struct vino_window *window = &windows[app];
    struct kos_gui_rect bounds = {
        window->x, window->y, window->width, window->height,
    };
    return window->open && !window->minimized
        && vino_gui_rect_contains(&bounds, x, y);
}

static int32_t top_window_at(int32_t x, int32_t y) {
    int32_t selected = -1;
    for (uint32_t app = 0; app < APP_COUNT; ++app) {
        if (point_hits_window(app, x, y)
            && (selected < 0 || windows[app].z_order > windows[selected].z_order)) {
            selected = (int32_t)app;
        }
    }
    return selected;
}

static void handle_start_menu_pointer(int32_t x, int32_t y) {
    uint32_t height = surface.height > 592 ? 520 : surface.height - 72;
    uint32_t width = surface.width > 508 ? 480 : surface.width - 28;
    int32_t menu_x = 14;
    int32_t menu_y = (int32_t)surface.height - 56 - (int32_t)height;
    if (x < menu_x || (uint32_t)(x - menu_x) >= width
        || y < menu_y || (uint32_t)(y - menu_y) >= height) {
        set_start_menu(false);
        return;
    }
    if (height > 150) {
        struct kos_gui_rect power = {
            menu_x + (int32_t)width - 108,
            menu_y + (int32_t)height - 49,
            94, 38,
        };
        if (vino_gui_rect_contains(&power, x, y)) {
            set_start_menu(false);
            open_power_dialog();
            return;
        }
    }
    uint32_t cell_width = (width - 36) / 4;
    int32_t grid_x = menu_x + 18;
    int32_t grid_y = menu_y + 92;
    if (x >= grid_x && y >= grid_y) {
        uint32_t column = (uint32_t)(x - grid_x) / cell_width;
        uint32_t row = (uint32_t)(y - grid_y) / 78;
        if (column < 4 && row < 3) {
            uint32_t app = menu_app_at(row * 4 + column);
            if (app < APP_COUNT) {
                menu_selected = app;
                start_menu_launch();
                return;
            }
        }
    }
    if (y >= menu_y + 304 && y < menu_y + 388) {
        uint32_t recent = (uint32_t)(y - menu_y - 304) / 38;
        if (recent < recent_app_count && recent < 2) {
            open_app(recent_apps[recent]);
            menu_animation_step = 0;
            return;
        }
    }
}

static void handle_context_menu_pointer(int32_t x, int32_t y) {
    int32_t menu_x = context_menu_x;
    int32_t menu_y = context_menu_y;
    uint32_t width = surface.width > 244 ? 220 : surface.width - 24;
    uint32_t row_height = surface.height < 320 ? 22 : 31;
    uint32_t row_gap = surface.height < 320 ? 24 : 34;
    uint32_t height = 6 * row_gap + 10;
    if (!context_menu_at_pointer) {
        menu_x = active_app >= 0 ? windows[active_app].x + 64 : 184;
        menu_y = active_app >= 0 ? windows[active_app].y + 74 : 142;
        if ((uint64_t)menu_x + width > surface.width)
            menu_x = (int32_t)surface.width - (int32_t)width - 8;
        if (menu_x < 8) menu_x = 8;
        if ((uint64_t)menu_y + height > surface.height - 48)
            menu_y = (int32_t)surface.height - 48 - (int32_t)height;
        if (menu_y < 8) menu_y = 8;
    }
    if ((uint64_t)menu_x + width > surface.width)
        menu_x = (int32_t)surface.width - (int32_t)width - 8;
    if (menu_x < 8) menu_x = 8;
    if ((uint64_t)menu_y + height > surface.height - 48)
        menu_y = (int32_t)surface.height - 48 - (int32_t)height;
    if (menu_y < 8) menu_y = 8;
    if (x < menu_x || (uint32_t)(x - menu_x) >= width
        || y < menu_y || (uint32_t)(y - menu_y) >= height) {
        context_menu_open = false;
        context_menu_at_pointer = false;
        return;
    }
    for (uint32_t item = 0; item < APP_COUNT; ++item) {
        int32_t row_y = menu_y + 5 + (int32_t)item * (int32_t)row_gap;
        if (y >= row_y && (uint32_t)(y - row_y) < row_height) {
            context_selected = item;
            context_menu_activate();
            return;
        }
    }
}

static void handle_window_pointer(uint32_t app, int32_t x, int32_t y,
                                  uint32_t buttons, uint64_t timestamp_ms) {
    struct vino_window *window = &windows[app];
    active_app = (int32_t)app;
    raise_window(app);
    start_menu_open = false;
    menu_animation_step = 0;
    context_menu_open = false;
    context_menu_at_pointer = false;

    if (y < window->y + WINDOW_TITLE_HEIGHT) {
        if (x >= window->x + 14 && x < window->x + 30) {
            close_active_app();
            return;
        }
        if (x >= window->x + 33 && x < window->x + 49) {
            window->minimized = true;
            active_app = -1;
            return;
        }
        if (x >= window->x + 52 && x < window->x + 68) {
            toggle_maximize();
            return;
        }
        if ((buttons & 1u) != 0 && !window->maximized) {
            drag_app = (int32_t)app;
            drag_offset_x = x - window->x;
            drag_offset_y = y - window->y;
        }
        return;
    }

    if (app == APP_FILES) {
        int32_t body_y = window->y + WINDOW_TITLE_HEIGHT;
        if (y < body_y + 38) {
            if (x < window->x + 42) {
                path_parent(file_directory);
                files_reload();
            }
        }
        else if (x < window->x + 156) {
            if (y >= body_y + 36 && y < body_y + 66) {
                string_copy(file_directory, sizeof(file_directory), file_home_directory);
                files_reload();
            }
            else if (y >= body_y + 70 && y < body_y + 100) {
                char volume = system_info.system_volume_letter;
                if (volume >= 'C' && volume <= 'Z') {
                    file_directory[0] = volume;
                    file_directory[1] = ':';
                    file_directory[2] = '/';
                    file_directory[3] = '\0';
                }
                else {
                    string_copy(file_error, sizeof(file_error),
                                "Kernel reports no mounted system volume.");
                }
                files_reload();
            }
            else if (y >= body_y + 104 && y < body_y + 134) {
                open_app(APP_NETWORK);
            }
        }
        else {
            int32_t content_x = window->x + 156;
            int32_t quick_y = body_y + 75;
            uint32_t content_width = window->width - 156;
            uint32_t card_width = (content_width - 48) / 3;
            if (y >= quick_y && y < quick_y + 44 && x >= content_x + 18) {
                uint32_t column = (uint32_t)(x - content_x - 18) / (card_width + 6);
                if (column < 3) {
                    uint32_t index = files_quick_directory(column);
                    if (index < file_entry_count) files_select_or_open(index, timestamp_ms);
                }
            }
            int32_t list_y = body_y + 156;
            if (y >= list_y && x >= content_x + 18
                && x < window->x + (int32_t)window->width - 8) {
                uint32_t index = file_scroll + (uint32_t)(y - list_y) / 25;
                files_select_or_open(index, timestamp_ms);
            }
        }
    }
    else if (app == APP_SETTINGS) {
        int32_t top = window->y + WINDOW_TITLE_HEIGHT + 10;
        if (x >= window->x + 10 && x < window->x + 162 && y >= top + 8) {
            uint32_t page = (uint32_t)(y - top - 8) / 36;
            if (page < 6) settings_page = page;
        }
        else if (settings_page == 2) {
            open_app(APP_NETWORK);
        }
        else if (settings_page == 3) {
            int32_t relative_y = y - (top + 42);
            if (relative_y >= 0 && relative_y < 62) handle_key('1');
            else if (relative_y >= 72 && relative_y < 134) handle_key('2');
            else if (relative_y >= 144 && relative_y < 206) handle_key('3');
        }
    }
    else if (app == APP_CONTROL_CENTER) {
        int32_t top = window->y + WINDOW_TITLE_HEIGHT + 14;
        int32_t tile_y = top + 30;
        uint32_t cell_width = (window->width - 48) / 2;
        if (y >= tile_y && y < tile_y + 146 && x >= window->x + 18) {
            uint32_t row = (uint32_t)(y - tile_y) / 78;
            uint32_t column = (uint32_t)(x - (window->x + 18))
                / (cell_width + 10);
            uint32_t action = row * 2 + column;
            if (column < 2 && action == 0) open_app(APP_NETWORK);
            else if (column < 2 && action == 1) {
                settings_page = 3;
                open_app(APP_SETTINGS);
            }
            else if (column < 2 && action == 2) open_app(APP_NOTIFICATIONS);
            else if (column < 2 && action == 3) reduced_motion = !reduced_motion;
        }
    }
    else if (app == APP_NOTIFICATIONS) {
        /* The kernel has no notification service; this view is informational only. */
    }
    else if (app == APP_NETWORK) {
        int32_t action_y = window->y + (int32_t)window->height - 58;
        struct kos_gui_rect action = { window->x + 26, action_y, 164, 34 };
        if (vino_gui_rect_contains(&action, x, y)) {
            request_network_dhcp();
        }
        else if (y > window->y + (int32_t)window->height - 45) {
            refresh_system_info();
        }
    }
}

static void handle_pointer(const struct kos_gui_input_event *event) {
    if (event == NULL) return;
    pointer_available = true;
    uint32_t previous_buttons = pointer_buttons;
    pointer_x = event->pointer_x;
    pointer_y = event->pointer_y;
    uint32_t buttons = event->pointer_buttons;
    bool left_pressed = (buttons & 1u) != 0 && (previous_buttons & 1u) == 0;
    bool right_pressed = (buttons & 2u) != 0 && (previous_buttons & 2u) == 0;

    if (power_dialog_state != POWER_DIALOG_CLOSED) {
        if (left_pressed) handle_power_dialog_pointer(pointer_x, pointer_y);
        pointer_buttons = buttons;
        return;
    }

    if (drag_app >= 0 && (buttons & 1u) != 0) {
        struct vino_window *window = &windows[drag_app];
        window->x = pointer_x - drag_offset_x;
        window->y = pointer_y - drag_offset_y;
        if (window->x < 8) window->x = 8;
        if (window->y < 8) window->y = 8;
        if ((uint64_t)window->x + window->width > surface.width - 8)
            window->x = (int32_t)(surface.width - window->width - 8);
        if ((uint64_t)window->y + window->height > surface.height - 48)
            window->y = (int32_t)(surface.height - window->height - 48);
    }
    if ((buttons & 1u) == 0) drag_app = -1;
    if (event->pointer_wheel != 0 && active_app == APP_FILES
        && point_hits_window(APP_FILES, pointer_x, pointer_y)) {
        if (event->pointer_wheel > 0 && file_scroll > 0) --file_scroll;
        else if (event->pointer_wheel < 0 && file_scroll + 1 < file_entry_count)
            ++file_scroll;
    }
    if (right_pressed) {
        start_menu_open = false;
        menu_animation_step = 0;
        context_menu_open = true;
        context_menu_at_pointer = true;
        context_menu_x = pointer_x;
        context_menu_y = pointer_y;
        context_selected = 0;
    }
    else if (left_pressed) {
        if (context_menu_open) {
            handle_context_menu_pointer(pointer_x, pointer_y);
        }
        else if (start_menu_open) {
            handle_start_menu_pointer(pointer_x, pointer_y);
        }
        else {
            uint32_t bar_y = surface.height - 48;
            struct kos_gui_rect start = { 16, (int32_t)bar_y + 7, 34, 34 };
            uint32_t search_width = surface.width >= 620 ? 210 : surface.width / 3;
            if (search_width < 112) search_width = 112;
            struct kos_gui_rect search = { 60, (int32_t)bar_y + 8, search_width, 32 };
            struct kos_gui_rect notification = {
                (int32_t)surface.width - 210, (int32_t)bar_y + 7, 30, 34,
            };
            struct kos_gui_rect network = {
                (int32_t)surface.width - 174, (int32_t)bar_y + 7, 30, 34,
            };
            struct kos_gui_rect control = {
                (int32_t)surface.width - 138, (int32_t)bar_y + 7, 34, 34,
            };
            struct kos_gui_rect clock = {
                (int32_t)surface.width - 96, (int32_t)bar_y + 6, 80, 36,
            };
            if (vino_gui_rect_contains(&start, pointer_x, pointer_y)) {
                set_start_menu(true);
            }
            else if (vino_gui_rect_contains(&search, pointer_x, pointer_y)) {
                set_start_menu(true);
            }
            else if (vino_gui_rect_contains(&notification, pointer_x, pointer_y)) {
                open_app(APP_NOTIFICATIONS);
            }
            else if (vino_gui_rect_contains(&network, pointer_x, pointer_y)) {
                open_app(APP_NETWORK);
            }
            else if (vino_gui_rect_contains(&control, pointer_x, pointer_y)
                     || vino_gui_rect_contains(&clock, pointer_x, pointer_y)) {
                open_app(APP_CONTROL_CENTER);
            }
            else {
                uint32_t icon_size = surface.width >= 720 ? 38 : 32;
                uint32_t icon_gap = surface.width >= 720 ? 6 : 2;
                uint32_t dock_width = APP_COUNT * icon_size + (APP_COUNT - 1) * icon_gap;
                int32_t dock_x = (int32_t)(surface.width - dock_width) / 2;
                int32_t dock_y = (int32_t)bar_y + 5;
                bool dock_hit = false;
                for (uint32_t app = 0; app < APP_COUNT; ++app) {
                    struct kos_gui_rect button = {
                        dock_x + (int32_t)(app * (icon_size + icon_gap)), dock_y,
                        icon_size, 38,
                    };
                    if (vino_gui_rect_contains(&button, pointer_x, pointer_y)) {
                        open_app(app);
                        dock_hit = true;
                        break;
                    }
                }
                if (!dock_hit) {
                    int32_t app = top_window_at(pointer_x, pointer_y);
                    if (app >= 0) {
                        handle_window_pointer((uint32_t)app, pointer_x, pointer_y,
                                              buttons, event->timestamp_ms);
                    }
                    else active_app = -1;
                }
            }
        }
    }
    pointer_buttons = buttons;
}

__attribute__((noreturn)) void _start(void) {
    surface.abi_version = KOS_GUI_ABI_VERSION;
    if (vino_gui_surface_get(&surface) < 0 || surface.width < 320
        || surface.height < 240) app_exit_with_status(1);
    pointer_available = (surface.capabilities & KOS_GUI_SURFACE_POINTER) != 0;
    pointer_x = (int32_t)(surface.width / 2);
    pointer_y = (int32_t)(surface.height / 2);
    pointer_hover_target = pointer_hover_at_current_position();
    (void)refresh_system_info();
    wallpaper_load();
    last_state_poll_second = sys_gettime();
    displayed_uptime_minute = last_state_poll_second / 60;
    initialize_paths();
    terminal_init();
    file_entry_count = 0;
    files_reload();
    render_ui();
    for (;;) {
        bool got_input = false;
        bool render_for_input = false;
        bool batch_interaction = false;
        bool batch_dragging = false;
        uint32_t drained_events = 0;
        struct kos_gui_input_event event = { .abi_version = KOS_GUI_ABI_VERSION };
        for (; drained_events < 64; ++drained_events) {
            event.abi_version = KOS_GUI_ABI_VERSION;
            if (vino_gui_input_read(&event) < 0) break;
            if (event.type != KOS_GUI_INPUT_KEY
                && event.type != KOS_GUI_INPUT_POINTER) break;
            got_input = true;
            if (event.type == KOS_GUI_INPUT_KEY) {
                visual_action_rendered = false;
                handle_key((char)event.key);
                if (!visual_action_rendered) render_for_input = true;
                continue;
            }

            bool position_changed = event.pointer_x != pointer_x
                || event.pointer_y != pointer_y;
            bool buttons_changed = event.pointer_buttons != pointer_buttons;
            bool interaction_changed = ((event.pointer_buttons & 3u) != 0
                && (event.pointer_buttons & 3u) != (pointer_buttons & 3u))
                || event.pointer_wheel != 0;
            if (interaction_changed) batch_interaction = true;
            if (!position_changed && !buttons_changed && event.pointer_wheel == 0)
                continue;
            visual_action_rendered = false;
            handle_pointer(&event);
            bool dragging = drag_app >= 0 && (event.pointer_buttons & 1u) != 0;
            if (dragging) batch_dragging = true;
            if (interaction_changed && !visual_action_rendered) {
                render_for_input = true;
            }
            else if (position_changed && dragging) {
                render_for_input = true;
            }
        }

        bool hover_changed = pointer_hover_at_current_position() != pointer_hover_target;
        if (hover_changed)
            render_for_input = true;

        /* The kernel owns the hardware cursor overlay. Plain pointer motion
           therefore does not repaint the desktop; only hover changes, clicks,
           wheel actions, and active window drags reach the desktop renderer. */
        if (render_for_input) {
            if (visual_action_rendered) {
                render_for_input = false;
            }
            else if (hover_changed && !batch_interaction && !batch_dragging) {
                render_hover_region();
            }
            else render_ui();
            while (animation_frame != 0) {
                --animation_frame;
                (void)sys_sleep(16);
                render_ui();
            }
        }
        uint64_t now = sys_gettime();
        if (now - last_state_poll_second >= 1) {
            bool state_changed = refresh_system_info();
            bool uptime_changed = now / 60 != displayed_uptime_minute;
            last_state_poll_second = now;
            if (state_changed || uptime_changed) {
                render_ui();
                displayed_uptime_minute = now / 60;
            }
        }
        if (!got_input && drained_events < 64) (void)sys_sleep(2);
    }
}
