#include <stdbool.h>
#include <stdint.h>

#include <kos/block.h>
#include <kos/gui_canvas.h>
#include <kos/installer_flow.h>
#include <kos/installer_ui.h>
#include <kos/keyboard.h>
#include <kos/task.h>

enum {
    UI_MARGIN = 48,
    UI_TEXT_SCALE = 2,
    UI_ROW_HEIGHT = 32,
};

static const struct gui_color color_background = { 12, 19, 34 };
static const struct gui_color color_panel = { 20, 30, 51 };
static const struct gui_color color_panel_active = { 29, 94, 183 };
static const struct gui_color color_border = { 102, 200, 255 };
static const struct gui_color color_text = { 255, 255, 255 };
static const struct gui_color color_muted = { 196, 208, 224 };
static const struct gui_color color_error = { 248, 113, 113 };

static void text(uint64_t x, uint64_t y, const char *value, struct gui_color color) {
    gui_canvas_text(x, y, value, UI_TEXT_SCALE, color, color_background);
}

static void panel_text(uint64_t x, uint64_t y, const char *value, struct gui_color color) {
    gui_canvas_text(x, y, value, UI_TEXT_SCALE, color, color_panel);
}

static void draw_shell(const struct installer_flow *flow) {
    gui_canvas_clear(color_background);
    struct gui_rect panel = {
        .x = UI_MARGIN,
        .y = UI_MARGIN,
        .width = gui_canvas_width() > UI_MARGIN * 2 ? gui_canvas_width() - UI_MARGIN * 2 : 1,
        .height = gui_canvas_height() > UI_MARGIN * 2 ? gui_canvas_height() - UI_MARGIN * 2 : 1,
    };
    gui_canvas_fill(panel, color_panel);
    gui_canvas_border(panel, 2, color_border);
    text(panel.x + 32, panel.y + 28, "Vino OS Installer", color_text);
    text(panel.x + 32, panel.y + 62, "System: Vino OS   Edition: default", color_muted);
    text(panel.x + 32, panel.y + panel.height - 48,
         "Tab/J/K: select   Enter: continue   Esc: quit", color_muted);
    if (flow != NULL && flow->state.phase == INSTALLER_FLOW_ERROR) {
        text(panel.x + 32, panel.y + panel.height - 82, flow->state.error, color_error);
    }
}

static void draw_profile_page(const struct installer_flow *flow, uint32_t selected) {
    draw_shell(flow);
    text(UI_MARGIN + 32, UI_MARGIN + 120, "Select installation type", color_text);
    for (uint32_t index = 0; index < installer_profile_count(); ++index) {
        const struct installer_profile *profile = installer_profile_at(index);
        struct gui_rect option = {
            .x = UI_MARGIN + 32,
            .y = UI_MARGIN + 160 + index * UI_ROW_HEIGHT,
            .width = 360,
            .height = UI_ROW_HEIGHT - 4,
        };
        gui_canvas_fill(option, index == selected ? color_panel_active : color_panel);
        panel_text(option.x + 14, option.y + 6, profile->display_name, color_text);
    }
}

static void draw_gui_page(const struct installer_flow *flow) {
    draw_shell(flow);
    text(UI_MARGIN + 32, UI_MARGIN + 120, "Select GUI", color_text);
    const struct gui_manifest *manifest = gui_manifest_find("default");
    if (manifest == NULL || !manifest->installer_visible) {
        text(UI_MARGIN + 32, UI_MARGIN + 160, "No installer-visible GUI is registered.", color_error);
        return;
    }
    struct gui_rect option = { UI_MARGIN + 32, UI_MARGIN + 160, 360, UI_ROW_HEIGHT - 4 };
    gui_canvas_fill(option, color_panel_active);
    panel_text(option.x + 14, option.y + 6, "Default", color_text);
    panel_text(option.x + 14, option.y + 36, "Local package from ISO", color_muted);
}

static uint32_t disk_count(void) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < block_device_count(); ++index) {
        struct block_device *device = block_device_get_at(index);
        if (device != NULL && device->parent == NULL) ++count;
    }
    return count;
}

static struct block_device *disk_at(uint32_t selected) {
    uint32_t seen = 0;
    for (uint32_t index = 0; index < block_device_count(); ++index) {
        struct block_device *device = block_device_get_at(index);
        if (device == NULL || device->parent != NULL) continue;
        if (seen++ == selected) return device;
    }
    return NULL;
}

static uint32_t partition_count(const struct block_device *disk) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < block_device_count(); ++index) {
        struct block_device *device = block_device_get_at(index);
        if (device != NULL && device->parent == disk) ++count;
    }
    return count;
}

static struct block_device *partition_at(const struct block_device *disk, uint32_t selected) {
    uint32_t seen = 0;
    for (uint32_t index = 0; index < block_device_count(); ++index) {
        struct block_device *device = block_device_get_at(index);
        if (device == NULL || device->parent != disk) continue;
        if (seen++ == selected) return device;
    }
    return NULL;
}

static void draw_disk_page(const struct installer_flow *flow, uint32_t selected) {
    draw_shell(flow);
    text(UI_MARGIN + 32, UI_MARGIN + 120, "Select disk", color_text);
    uint32_t count = disk_count();
    if (count == 0) {
        text(UI_MARGIN + 32, UI_MARGIN + 160, "No installable disks detected.", color_error);
        text(UI_MARGIN + 32, UI_MARGIN + 196, "The installer does not use a fake disk.", color_muted);
        return;
    }
    for (uint32_t index = 0; index < count; ++index) {
        struct block_device *device = disk_at(index);
        struct gui_rect option = { UI_MARGIN + 32, UI_MARGIN + 160 + index * UI_ROW_HEIGHT, 600, UI_ROW_HEIGHT - 4 };
        gui_canvas_fill(option, index == selected ? color_panel_active : color_panel);
        panel_text(option.x + 14, option.y + 6, device->name, color_text);
    }
}

static void draw_partition_page(const struct installer_flow *flow, uint32_t selected) {
    draw_shell(flow);
    text(UI_MARGIN + 32, UI_MARGIN + 120, "Select install partition", color_text);
    uint32_t count = partition_count(flow->selected_disk);
    if (count == 0) {
        text(UI_MARGIN + 32, UI_MARGIN + 160, "No partition was detected on this disk.", color_error);
        text(UI_MARGIN + 32, UI_MARGIN + 196, "Partition creation/formatting backend is not enabled yet.", color_muted);
        return;
    }
    for (uint32_t index = 0; index < count; ++index) {
        struct block_device *partition = partition_at(flow->selected_disk, index);
        struct gui_rect option = { UI_MARGIN + 32, UI_MARGIN + 160 + index * UI_ROW_HEIGHT, 600, UI_ROW_HEIGHT - 4 };
        gui_canvas_fill(option, index == selected ? color_panel_active : color_panel);
        panel_text(option.x + 14, option.y + 6, partition->name, color_text);
    }
}

static void draw_review_page(const struct installer_flow *flow) {
    draw_shell(flow);
    text(UI_MARGIN + 32, UI_MARGIN + 120, "Review installation", color_text);
    text(UI_MARGIN + 32, UI_MARGIN + 160, "System: Vino OS", color_muted);
    text(UI_MARGIN + 32, UI_MARGIN + 192, flow->profile->install_gui ? "Mode: Vino Plus preset" : "Mode: Vino CLI", color_muted);
    text(UI_MARGIN + 32, UI_MARGIN + 224, flow->selected_gui == NULL ? "GUI: None" : "GUI: Default", color_muted);
    text(UI_MARGIN + 32, UI_MARGIN + 256, flow->selected_disk->name, color_muted);
    text(UI_MARGIN + 32, UI_MARGIN + 288, flow->selected_partition->name, color_muted);
    text(UI_MARGIN + 32, UI_MARGIN + 340, "Enter will validate the engine; it will not write a disk without a backend.", color_error);
}

static int unavailable_base_os(struct installer_flow *flow, void *context) {
    (void)flow;
    (void)context;
    return INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
}

static int unavailable_gui(struct installer_flow *flow, const struct gui_manifest *gui, void *context) {
    (void)flow;
    (void)gui;
    (void)context;
    return INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
}

static int unavailable_bootloader(struct installer_flow *flow, void *context) {
    (void)flow;
    (void)context;
    return INSTALLER_FLOW_ERR_NOT_IMPLEMENTED;
}

static void draw_state(const struct installer_flow *flow, uint32_t selected) {
    switch (flow->state.phase) {
        case INSTALLER_FLOW_PROFILE_SELECTION: draw_profile_page(flow, selected); break;
        case INSTALLER_FLOW_GUI_SELECTION: draw_gui_page(flow); break;
        case INSTALLER_FLOW_DISK_SELECTION: draw_disk_page(flow, selected); break;
        case INSTALLER_FLOW_PARTITION_SETUP: draw_partition_page(flow, selected); break;
        case INSTALLER_FLOW_ERROR:
            draw_shell(flow);
            text(UI_MARGIN + 32, UI_MARGIN + 120, "Installer stopped", color_error);
            text(UI_MARGIN + 32, UI_MARGIN + 160, "Esc or Q returns to the existing CLI.", color_muted);
            break;
        default: draw_review_page(flow); break;
    }
}

bool installer_ui_run(void) {
    if (!gui_canvas_initialize()) return false;
    struct installer_flow flow;
    if (!installer_flow_init(&flow)) return false;
    uint32_t selected = 0;
    draw_state(&flow, selected);

    for (;;) {
        char key;
        if (!keyboard_read_char(&key)) {
            task_yield();
            continue;
        }
        if (key == 27) return true;
        if (key == 'j' || key == 'd' || key == '\t') {
            ++selected;
        }
        else if (key == 'k' || key == 'u' || key == '\b') {
            if (selected > 0) --selected;
        }
        else if (key == '\n' || key == ' ') {
            int result = INSTALLER_FLOW_OK;
            if (flow.state.phase == INSTALLER_FLOW_PROFILE_SELECTION) {
                const struct installer_profile *profile = installer_profile_at(selected % installer_profile_count());
                result = installer_flow_select_profile(&flow, profile->id);
                selected = 0;
            }
            else if (flow.state.phase == INSTALLER_FLOW_GUI_SELECTION) {
                result = installer_flow_select_gui(&flow, "default");
                selected = 0;
            }
            else if (flow.state.phase == INSTALLER_FLOW_DISK_SELECTION) {
                struct block_device *disk = disk_at(selected % (disk_count() == 0 ? 1 : disk_count()));
                result = disk == NULL ? INSTALLER_FLOW_ERR_DISK_NOT_FOUND
                                       : installer_flow_select_disk(&flow, disk->name);
                selected = 0;
            }
            else if (flow.state.phase == INSTALLER_FLOW_PARTITION_SETUP) {
                uint32_t count = partition_count(flow.selected_disk);
                struct block_device *partition = partition_at(flow.selected_disk, selected % (count == 0 ? 1 : count));
                result = partition == NULL ? INSTALLER_FLOW_ERR_PARTITION_NOT_FOUND
                                            : installer_flow_select_partition(&flow, partition->name);
                selected = 0;
            }
            else if (flow.state.phase == INSTALLER_FLOW_CONFIRMATION) {
                struct installer_engine engine = {
                    .install_base_os = unavailable_base_os,
                    .install_gui = unavailable_gui,
                    .install_bootloader = unavailable_bootloader,
                };
                result = installer_flow_execute(&flow, &engine);
            }
            if (result != INSTALLER_FLOW_OK && flow.state.phase != INSTALLER_FLOW_ERROR) {
                flow.state.phase = INSTALLER_FLOW_ERROR;
            }
        }
        else if (key == 'q') {
            return true;
        }
        draw_state(&flow, selected);
    }
}
