#ifndef KOS_INSTALLER_FLOW_H
#define KOS_INSTALLER_FLOW_H

#include <stdbool.h>
#include <stdint.h>

#include <kos/installer_contracts.h>
#include <kos/installer_storage.h>
#include <kos/gui_manager.h>

struct installer_flow {
    struct installer_state state;
    const struct installer_profile *profile;
    const struct gui_manifest *selected_gui;
    struct block_device *selected_disk;
    struct block_device *selected_partition;
};

struct installer_engine {
    int (*install_base_os)(struct installer_flow *flow, void *context);
    int (*install_gui)(struct installer_flow *flow, const struct gui_manifest *gui, void *context);
    int (*install_bootloader)(struct installer_flow *flow, void *context);
    void *context;
};

uint32_t installer_profile_count(void);
const struct installer_profile *installer_profile_at(uint32_t index);
const struct installer_profile *installer_profile_find(const char *id);

bool installer_flow_init(struct installer_flow *flow);
int installer_flow_select_profile(struct installer_flow *flow, const char *id);
int installer_flow_select_gui(struct installer_flow *flow, const char *id);
int installer_flow_select_disk(struct installer_flow *flow, const char *name);
int installer_flow_select_partition(struct installer_flow *flow, const char *name);
int installer_flow_delete_partition(struct installer_flow *flow, const char *name,
                                    bool confirmed);
int installer_flow_request_partition_creation(struct installer_flow *flow,
                                              enum partition_table_type table_type,
                                              uint64_t start_lba, uint64_t block_count,
                                              enum partition_plan_filesystem filesystem);
int installer_flow_apply_partition_creation(struct installer_flow *flow);
int installer_flow_format_selected_partition(struct installer_flow *flow, bool confirmed);
int installer_flow_next(struct installer_flow *flow);
int installer_flow_back(struct installer_flow *flow);
bool installer_flow_is_valid(const struct installer_flow *flow);
int installer_flow_execute(struct installer_flow *flow, const struct installer_engine *engine);
const char *installer_flow_state_name(enum installer_flow_state state);
const char *installer_flow_status_string(int status);

#endif /* KOS_INSTALLER_FLOW_H */
