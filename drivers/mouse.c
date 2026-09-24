#include <stdbool.h>
#include <stdint.h>

#include <kos/io.h>
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
static uint8_t packet[MOUSE_MAX_PACKET_SIZE];
static uint8_t packet_size;
static uint8_t packet_index;
static uint8_t device_id;
static uint8_t previous_buttons;
static bool bounds_initialized;
static bool initialized;

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
        || !wait_input_empty()) return false;
    io_out8(PS2_DATA_PORT, command);
    return mouse_response(&response) && response == PS2_MOUSE_ACK;
}

static bool mouse_set_sample_rate(uint8_t rate) {
    return mouse_command(PS2_MOUSE_SET_SAMPLE_RATE) && mouse_command(rate);
}

static void enqueue_event(int32_t wheel) {
    uint8_t head = event_head;
    uint8_t next = (uint8_t)((head + 1) & MOUSE_EVENT_MASK);
    if (next == event_tail) return;
    event_queue[head] = (struct kos_mouse_event){
        (int32_t)mouse_x, (int32_t)mouse_y, previous_buttons, wheel,
    };
    __asm__ volatile("" ::: "memory");
    event_head = next;
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
    if (!controller_command(PS2_COMMAND_ENABLE_SECOND_PORT)
        || !controller_command(PS2_COMMAND_READ_CONFIGURATION)
        || !mouse_response(&configuration)) return false;
    configuration |= PS2_CONFIGURATION_SECOND_PORT_INTERRUPT;
    configuration &= (uint8_t)~PS2_CONFIGURATION_SECOND_PORT_CLOCK_DISABLED;
    if (!controller_command(PS2_COMMAND_WRITE_CONFIGURATION)
        || !wait_input_empty()) return false;
    io_out8(PS2_DATA_PORT, configuration);

    /* Negotiate the standard IntelliMouse wheel mode when supported. */
    if (mouse_set_sample_rate(200) && mouse_set_sample_rate(100)
        && mouse_set_sample_rate(80) && mouse_command(PS2_MOUSE_GET_ID)
        && mouse_response(&id) && (id == 3 || id == 4)) {
        device_id = id;
        packet_size = 4;
    }
    if (!mouse_command(PS2_MOUSE_ENABLE_STREAMING)) return false;
    initialized = true;
    return true;
}

bool mouse_is_initialized(void) {
    return initialized;
}

void mouse_set_bounds(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
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
    if (changed || wheel != 0) enqueue_event(wheel);
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
