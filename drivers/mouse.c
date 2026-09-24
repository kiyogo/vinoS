#include <stdbool.h>
#include <stdint.h>

#include <kos/io.h>
#include <kos/log.h>
#include <kos/mouse.h>

enum {
    PS2_DATA_PORT = 0x60,
    PS2_STATUS_PORT = 0x64,
    PS2_COMMAND_PORT = 0x64,
    PS2_STATUS_OUTPUT_FULL = 0x01,
    PS2_STATUS_INPUT_FULL = 0x02,
    PS2_STATUS_AUXILIARY = 0x20,
    PS2_COMMAND_READ_CONFIGURATION = 0x20,
    PS2_COMMAND_WRITE_CONFIGURATION = 0x60,
    PS2_COMMAND_ENABLE_SECOND_PORT = 0xa8,
    PS2_COMMAND_WRITE_TO_SECOND_PORT = 0xd4,
    PS2_CONFIGURATION_SECOND_PORT_INTERRUPT = 0x02,
    PS2_CONFIGURATION_SECOND_PORT_CLOCK_DISABLED = 0x20,
    PS2_MOUSE_ACK = 0xfa,
    PS2_MOUSE_SET_SAMPLE_RATE = 0xf3,
    PS2_MOUSE_GET_ID = 0xf2,
    PS2_MOUSE_ENABLE_STREAMING = 0xf4,
    PS2_MOUSE_WAIT_SPIN_LIMIT = 1000000,
    MOUSE_EVENT_CAPACITY = 64,
    MOUSE_EVENT_MASK = MOUSE_EVENT_CAPACITY - 1,
    MOUSE_MAX_PACKET_SIZE = 4,
};

static struct kos_mouse_event event_queue[MOUSE_EVENT_CAPACITY];
static volatile uint8_t event_head;
static volatile uint8_t event_tail;
static volatile uint32_t mouse_x;
static volatile uint32_t mouse_y;
static volatile uint32_t mouse_max_x;
static volatile uint32_t mouse_max_y;
static volatile uint64_t event_count;
static volatile uint64_t dropped_event_count;
static volatile uint64_t movement_event_count;
static volatile uint64_t button_event_count;
static volatile uint64_t wheel_event_count;
static volatile int32_t last_event_x;
static volatile int32_t last_event_y;
static volatile uint32_t last_event_buttons;
static volatile int32_t last_event_wheel;
static bool last_event_valid;
static uint8_t packet[MOUSE_MAX_PACKET_SIZE];
static uint8_t packet_size;
static uint8_t packet_index;
static uint8_t device_id;
static uint8_t previous_buttons;
static bool bounds_initialized;
static bool initialized;
static volatile bool usb_hid_initialized;

static void saturating_count(volatile uint64_t *counter) {
    if (*counter != UINT64_MAX) ++*counter;
}

static void enqueue_event(uint32_t buttons, int32_t wheel) {
    uint8_t head = event_head;
    uint8_t next = (uint8_t)((head + 1) & MOUSE_EVENT_MASK);
    if (next == event_tail) {
        if (dropped_event_count != UINT64_MAX) ++dropped_event_count;
        return;
    }
    event_queue[head] = (struct kos_mouse_event){
        (int32_t)mouse_x, (int32_t)mouse_y, buttons, wheel,
    };
    if (last_event_valid) {
        if (last_event_x != (int32_t)mouse_x || last_event_y != (int32_t)mouse_y)
            saturating_count(&movement_event_count);
        if (last_event_buttons != buttons) saturating_count(&button_event_count);
    }
    else {
        saturating_count(&movement_event_count);
        if (buttons != 0) saturating_count(&button_event_count);
    }
    if (wheel != 0) saturating_count(&wheel_event_count);
    last_event_x = (int32_t)mouse_x;
    last_event_y = (int32_t)mouse_y;
    last_event_buttons = buttons;
    last_event_wheel = wheel;
    last_event_valid = true;
    __asm__ volatile("" ::: "memory");
    event_head = next;
    if (event_count != UINT64_MAX) ++event_count;
}

static bool wait_input_empty(void) {
    for (uint32_t spin = 0; spin < PS2_MOUSE_WAIT_SPIN_LIMIT; ++spin) {
        if ((io_in8(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0) return true;
    }
    return false;
}

static bool wait_output_full(void) {
    for (uint32_t spin = 0; spin < PS2_MOUSE_WAIT_SPIN_LIMIT; ++spin) {
        if ((io_in8(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) return true;
    }
    return false;
}

static bool controller_command(uint8_t command) {
    if (!wait_input_empty()) return false;
    io_out8(PS2_COMMAND_PORT, command);
    return true;
}

static bool mouse_response(uint8_t *response) {
    if (response == 0 || !wait_output_full()) return false;
    *response = io_in8(PS2_DATA_PORT);
    return true;
}

static bool mouse_command(uint8_t command) {
    uint8_t response;
    if (!controller_command(PS2_COMMAND_WRITE_TO_SECOND_PORT)
        || !wait_input_empty()) {
        log_warnf("PS/2 mouse command 0x%02X could not reach the controller (status=0x%02X).",
                  command, io_in8(PS2_STATUS_PORT));
        return false;
    }
    io_out8(PS2_DATA_PORT, command);
    if (!mouse_response(&response)) {
        log_warnf("PS/2 mouse command 0x%02X timed out (status=0x%02X).",
                  command, io_in8(PS2_STATUS_PORT));
        return false;
    }
    if (response != PS2_MOUSE_ACK) {
        log_warnf("PS/2 mouse command 0x%02X returned 0x%02X, expected ACK.",
                  command, response);
        return false;
    }
    return true;
}

static bool mouse_set_sample_rate(uint8_t rate) {
    return mouse_command(PS2_MOUSE_SET_SAMPLE_RATE) && mouse_command(rate);
}

bool mouse_initialize(void) {
    uint8_t configuration;
    uint8_t id = 0;
    initialized = false;
    event_head = 0;
    event_tail = 0;
    packet_index = 0;
    packet_size = 3;
    previous_buttons = 0;
    device_id = 0;
    event_count = 0;
    dropped_event_count = 0;
    movement_event_count = 0;
    button_event_count = 0;
    wheel_event_count = 0;
    last_event_x = 0;
    last_event_y = 0;
    last_event_buttons = 0;
    last_event_wheel = 0;
    last_event_valid = false;
    if (!controller_command(PS2_COMMAND_ENABLE_SECOND_PORT)) {
        log_warn("PS/2 mouse: failed to enable the auxiliary controller port.");
        return false;
    }
    if (!controller_command(PS2_COMMAND_READ_CONFIGURATION)
        || !mouse_response(&configuration)) {
        log_warnf("PS/2 mouse: failed to read controller configuration (status=0x%02X).",
                  io_in8(PS2_STATUS_PORT));
        return false;
    }
    configuration |= PS2_CONFIGURATION_SECOND_PORT_INTERRUPT;
    configuration &= (uint8_t)~PS2_CONFIGURATION_SECOND_PORT_CLOCK_DISABLED;
    if (!controller_command(PS2_COMMAND_WRITE_CONFIGURATION)
        || !wait_input_empty()) {
        log_warnf("PS/2 mouse: failed to write controller configuration (status=0x%02X).",
                  io_in8(PS2_STATUS_PORT));
        return false;
    }
    io_out8(PS2_DATA_PORT, configuration);

    /* Negotiate the standard IntelliMouse wheel mode when supported. */
    if (mouse_set_sample_rate(200) && mouse_set_sample_rate(100)
        && mouse_set_sample_rate(80) && mouse_command(PS2_MOUSE_GET_ID)
        && mouse_response(&id) && (id == 3 || id == 4)) {
        device_id = id;
        packet_size = 4;
    }
    if (!mouse_command(PS2_MOUSE_ENABLE_STREAMING)) {
        log_warn("PS/2 mouse: streaming could not be enabled.");
        return false;
    }
    initialized = true;
    return true;
}

bool mouse_is_initialized(void) {
    return initialized || usb_hid_initialized;
}

bool mouse_ps2_is_initialized(void) {
    return initialized;
}

bool mouse_usb_hid_is_initialized(void) {
    return usb_hid_initialized;
}

void mouse_set_bounds(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    const uint32_t coordinate_limit = (uint32_t)INT32_MAX + 1u;
    if (width > coordinate_limit) width = coordinate_limit;
    if (height > coordinate_limit) height = coordinate_limit;
    mouse_max_x = width - 1;
    mouse_max_y = height - 1;
    if (!bounds_initialized) {
        mouse_x = width / 2;
        mouse_y = height / 2;
        bounds_initialized = true;
    }
    if (mouse_x > mouse_max_x) mouse_x = mouse_max_x;
    if (mouse_y > mouse_max_y) mouse_y = mouse_max_y;
}

static int32_t signed_wheel(uint8_t value) {
    int32_t wheel = value & 0x0f;
    return (wheel & 0x08) != 0 ? wheel - 16 : wheel;
}

static void decode_packet(void) {
    uint8_t flags = packet[0];
    if ((flags & 0xc0) != 0) return;
    int32_t delta_x = (int8_t)packet[1];
    int32_t delta_y = -(int32_t)(int8_t)packet[2];
    int64_t next_x = (int64_t)mouse_x + delta_x;
    int64_t next_y = (int64_t)mouse_y + delta_y;
    if (next_x < 0) next_x = 0;
    if (next_y < 0) next_y = 0;
    if ((uint64_t)next_x > mouse_max_x) next_x = mouse_max_x;
    if ((uint64_t)next_y > mouse_max_y) next_y = mouse_max_y;
    mouse_x = (uint32_t)next_x;
    mouse_y = (uint32_t)next_y;

    uint8_t buttons = flags & 0x07;
    bool changed = buttons != previous_buttons || delta_x != 0 || delta_y != 0;
    previous_buttons = buttons;
    int32_t wheel = (device_id == 3 || device_id == 4) && packet_size == 4
        ? signed_wheel(packet[3]) : 0;
    if (changed || wheel != 0) enqueue_event(buttons, wheel);
}

static void update_usb_position(int64_t next_x, int64_t next_y,
                                uint32_t buttons, int32_t wheel) {
    if (next_x < 0) next_x = 0;
    if (next_y < 0) next_y = 0;
    if ((uint64_t)next_x > mouse_max_x) next_x = mouse_max_x;
    if ((uint64_t)next_y > mouse_max_y) next_y = mouse_max_y;
    mouse_x = (uint32_t)next_x;
    mouse_y = (uint32_t)next_y;
    buttons &= 0x1fu;
    bool changed = buttons != previous_buttons;
    previous_buttons = (uint8_t)buttons;
    if (changed || wheel != 0) enqueue_event(buttons, wheel);
}

void mouse_usb_relative_report(int32_t delta_x, int32_t delta_y,
                               uint32_t buttons, int32_t wheel) {
    usb_hid_initialized = true;
    if (!bounds_initialized) {
        /* Keep button/wheel input useful before the GUI publishes its surface. */
        update_usb_position(mouse_x, mouse_y, buttons, wheel);
        return;
    }
    int64_t next_x = (int64_t)mouse_x + delta_x;
    int64_t next_y = (int64_t)mouse_y + delta_y;
    bool moved = delta_x != 0 || delta_y != 0;
    uint32_t normalized_buttons = buttons & 0x1fu;
    uint32_t previous = previous_buttons;
    uint32_t previous_x = mouse_x;
    uint32_t previous_y = mouse_y;
    update_usb_position(next_x, next_y, buttons, wheel);
    if (moved && (mouse_x != previous_x || mouse_y != previous_y)
        && normalized_buttons == previous && wheel == 0) {
        enqueue_event(previous_buttons, 0);
    }
}

void mouse_usb_absolute_report(uint32_t x, uint32_t y,
                               uint32_t max_x, uint32_t max_y,
                               uint32_t buttons, int32_t wheel) {
    if (max_x == 0 || max_y == 0) return;
    usb_hid_initialized = true;
    uint64_t scaled_x = bounds_initialized
        ? ((uint64_t)x * mouse_max_x) / max_x : 0;
    uint64_t scaled_y = bounds_initialized
        ? ((uint64_t)y * mouse_max_y) / max_y : 0;
    uint32_t previous_x = mouse_x;
    uint32_t previous_y = mouse_y;
    uint32_t old_buttons = previous_buttons;
    update_usb_position((int64_t)scaled_x, (int64_t)scaled_y, buttons, wheel);
    if ((mouse_x != previous_x || mouse_y != previous_y)
        && (buttons & 0x1fu) == old_buttons && wheel == 0) {
        enqueue_event(old_buttons, 0);
    }
}

void mouse_interrupt(void) {
    if (!initialized || (io_in8(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) == 0) return;
    uint8_t status = io_in8(PS2_STATUS_PORT);
    if ((status & PS2_STATUS_AUXILIARY) == 0) return;
    uint8_t value = io_in8(PS2_DATA_PORT);
    if (packet_index == 0 && (value & 0x08) == 0) return;
    packet[packet_index++] = value;
    if (packet_index == packet_size) {
        packet_index = 0;
        decode_packet();
    }
}

bool mouse_read_event(struct kos_mouse_event *event) {
    if (event == 0) return false;
    uint8_t tail = event_tail;
    if (tail == event_head) return false;
    __asm__ volatile("" ::: "memory");
    *event = event_queue[tail];
    event_tail = (uint8_t)((tail + 1) & MOUSE_EVENT_MASK);
    return true;
}

void mouse_get_position(int32_t *x, int32_t *y) {
    if (x != 0) *x = (int32_t)mouse_x;
    if (y != 0) *y = (int32_t)mouse_y;
}

void mouse_get_stats(struct kos_mouse_stats *stats) {
    if (stats == 0) return;
    *stats = (struct kos_mouse_stats){
        .event_count = event_count,
        .dropped_event_count = dropped_event_count,
        .movement_event_count = movement_event_count,
        .button_event_count = button_event_count,
        .wheel_event_count = wheel_event_count,
        .x = last_event_x,
        .y = last_event_y,
        .buttons = last_event_buttons,
        .wheel = last_event_wheel,
        .ps2_available = initialized ? 1u : 0u,
        .usb_hid_available = usb_hid_initialized ? 1u : 0u,
    };
}
