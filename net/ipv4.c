#include <stdbool.h>
#include <stdint.h>

#include <kos/net.h>
#include <kos/sync.h>
#include <kos/timer.h>

enum {
    IPV4_HEADER_SIZE = 20,
    IPV4_ETHERTYPE = 0x0800,
    IPV4_DEFAULT_TTL = 64,
};

static struct net_interface_info interface;
static uint16_t next_identification;
static struct kos_spinlock ipv4_lock = KOS_SPINLOCK_INITIALIZER;

void net_icmp_receive(uint32_t source, const uint8_t *packet, uint64_t size);
void net_udp_receive(uint32_t source, uint32_t destination, const uint8_t *packet,
    uint64_t size);
void net_tcp_receive(uint32_t source, uint32_t destination, const uint8_t *packet,
    uint64_t size);

static uint16_t read_u16_be(const uint8_t *data) {
    return ((uint16_t)data[0] << 8) | data[1];
}

static void write_u16_be(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static uint16_t ipv4_checksum(const uint8_t *data, uint64_t size) {
    uint32_t sum = 0;
    for (uint64_t index = 0; index + 1 < size; index += 2) {
        sum += ((uint16_t)data[index] << 8) | data[index + 1];
    }
    if ((size & 1) != 0) {
        sum += (uint16_t)data[size - 1] << 8;
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static void write_u32_be(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

bool net_interface_info(struct net_interface_info *info) {
    if (info == 0) {
        return false;
    }
    uint64_t flags = spinlock_lock_irqsave(&ipv4_lock);
    *info = interface;
    spinlock_unlock_irqrestore(&ipv4_lock, flags);
    return true;
}

bool net_has_external_interface(void) {
    uint64_t flags = spinlock_lock_irqsave(&ipv4_lock);
    bool avail = interface.available;
    spinlock_unlock_irqrestore(&ipv4_lock, flags);
    return avail;
}

bool net_ipv4_initialize_interface(const uint8_t mac_address[NET_ETHERNET_ADDRESS_SIZE]) {
    if (mac_address == 0) {
        return false;
    }
    interface = (struct net_interface_info){
        .available = true,
        .ipv4_address = NET_QEMU_STATIC_ADDRESS,
        .subnet_mask = 0xffffff00u,
        .gateway = NET_QEMU_GATEWAY_ADDRESS,
        .dns_server = NET_QEMU_DNS_ADDRESS,
        .configuration_source = NET_CONFIGURATION_STATIC,
        .dhcp_state = NET_DHCP_IDLE,
    };
    for (uint64_t index = 0; index < NET_ETHERNET_ADDRESS_SIZE; ++index) {
        interface.mac_address[index] = mac_address[index];
    }
    next_identification = (uint16_t)timer_ticks();
    return true;
}

bool net_ipv4_configure_dhcp(uint32_t address, uint32_t subnet_mask, uint32_t gateway,
        uint32_t dns_server, uint64_t lease_seconds) {
    uint64_t flags = spinlock_lock_irqsave(&ipv4_lock);
    if (!interface.available || address == 0 || subnet_mask == 0) {
        spinlock_unlock_irqrestore(&ipv4_lock, flags);
        return false;
    }
    interface.ipv4_address = address;
    interface.subnet_mask = subnet_mask;
    interface.gateway = gateway;
    interface.dns_server = dns_server;
    interface.configuration_source = NET_CONFIGURATION_DHCP;
    interface.dhcp_state = NET_DHCP_BOUND;
    interface.dhcp_lease_seconds = lease_seconds;
    spinlock_unlock_irqrestore(&ipv4_lock, flags);
    return true;
}

void net_ipv4_set_dhcp_state(enum net_dhcp_state state) {
    uint64_t flags = spinlock_lock_irqsave(&ipv4_lock);
    interface.dhcp_state = state;
    spinlock_unlock_irqrestore(&ipv4_lock, flags);
}

void net_ipv4_receive(const uint8_t source[NET_ETHERNET_ADDRESS_SIZE], const uint8_t *packet,
        uint64_t size) {
    (void)source;
    if (packet == 0 || size < IPV4_HEADER_SIZE || (packet[0] >> 4) != 4) {
        ++interface.ipv4_packets_dropped;
        return;
    }
    uint64_t header_size = (packet[0] & 0x0f) * 4u;
    uint64_t total_size = read_u16_be(&packet[2]);
    if (header_size < IPV4_HEADER_SIZE || header_size > size || total_size < header_size
        || total_size > size || ipv4_checksum(packet, header_size) != 0
        || (read_u16_be(&packet[6]) & 0x3fffu) != 0) {
        ++interface.ipv4_packets_dropped;
        return;
    }
    uint32_t source_address = ((uint32_t)packet[12] << 24) | ((uint32_t)packet[13] << 16)
        | ((uint32_t)packet[14] << 8) | packet[15];
    uint32_t destination = ((uint32_t)packet[16] << 24) | ((uint32_t)packet[17] << 16)
        | ((uint32_t)packet[18] << 8) | packet[19];
    bool in_dhcp_negotiation = (interface.dhcp_state == NET_DHCP_DISCOVERING
        || interface.dhcp_state == NET_DHCP_REQUESTING);
    if (!interface.available
        || (destination != interface.ipv4_address && destination != 0xffffffffu
            && !(in_dhcp_negotiation && packet[9] == 17))) {
        ++interface.ipv4_packets_dropped;
        return;
    }
    ++interface.ipv4_packets_received;
    if (packet[9] == 1) {
        net_icmp_receive(source_address, &packet[header_size], total_size - header_size);
    }
    else if (packet[9] == 17) {
        net_udp_receive(source_address, destination, &packet[header_size], total_size - header_size);
    }
    else if (packet[9] == 6) {
        net_tcp_receive(source_address, destination, &packet[header_size], total_size - header_size);
    }
}

bool net_ipv4_send_from(uint8_t protocol, uint32_t source, uint32_t destination,
        const uint8_t *payload, uint64_t payload_size) {
    if (!interface.available || payload == 0 || payload_size == 0
        || payload_size > NET_ETHERNET_FRAME_MAX - 14 - IPV4_HEADER_SIZE
        || (source != 0 && source != interface.ipv4_address)) {
        return false;
    }
    if (destination == interface.ipv4_address || (destination >> 24) == 127) {
        uint32_t loop_src = source != 0 ? source : interface.ipv4_address;
        if (protocol == 6) {
            net_tcp_receive(loop_src, destination, payload, payload_size);
        }
        else if (protocol == 17) {
            net_udp_receive(loop_src, destination, payload, payload_size);
        }
        else if (protocol == 1) {
            net_icmp_receive(loop_src, payload, payload_size);
        }
        return true;
    }
    uint8_t destination_mac[NET_ETHERNET_ADDRESS_SIZE];
    if (destination == 0xffffffffu) {
        for (uint64_t index = 0; index < NET_ETHERNET_ADDRESS_SIZE; ++index) {
            destination_mac[index] = 0xff;
        }
    }
    else {
        uint32_t next_hop = (destination & interface.subnet_mask)
                == (interface.ipv4_address & interface.subnet_mask) ? destination : interface.gateway;
        if (next_hop == 0 || !net_arp_resolve(next_hop, destination_mac,
                NET_ARP_DEFAULT_TIMEOUT_TICKS)) {
            return false;
        }
    }
    uint8_t packet[NET_ETHERNET_FRAME_MAX - 14];
    packet[0] = 0x45;
    packet[1] = 0;
    write_u16_be(&packet[2], (uint16_t)(IPV4_HEADER_SIZE + payload_size));
    write_u16_be(&packet[4], next_identification++);
    write_u16_be(&packet[6], 0x4000);
    packet[8] = IPV4_DEFAULT_TTL;
    packet[9] = protocol;
    packet[10] = 0;
    packet[11] = 0;
    write_u32_be(&packet[12], source);
    write_u32_be(&packet[16], destination);
    write_u16_be(&packet[10], ipv4_checksum(packet, IPV4_HEADER_SIZE));
    for (uint64_t index = 0; index < payload_size; ++index) {
        packet[IPV4_HEADER_SIZE + index] = payload[index];
    }
    return net_ethernet_send(destination_mac, IPV4_ETHERTYPE, packet, IPV4_HEADER_SIZE + payload_size);
}

bool net_ipv4_send(uint8_t protocol, uint32_t destination, const uint8_t *payload,
        uint64_t payload_size) {
    return net_ipv4_send_from(protocol, interface.ipv4_address, destination, payload, payload_size);
}

static char hex_digit(uint8_t value) {
    return value < 10 ? (char)('0' + value) : (char)('A' + value - 10);
}

static uint64_t write_decimal(char *text, uint8_t value) {
    if (value >= 100) {
        text[0] = (char)('0' + value / 100);
        text[1] = (char)('0' + (value / 10) % 10);
        text[2] = (char)('0' + value % 10);
        return 3;
    }
    if (value >= 10) {
        text[0] = (char)('0' + value / 10);
        text[1] = (char)('0' + value % 10);
        return 2;
    }
    text[0] = (char)('0' + value);
    return 1;
}

void net_format_ipv4(uint32_t address, char text[16]) {
    uint64_t position = 0;
    for (uint64_t index = 0; index < 4; ++index) {
        position += write_decimal(&text[position], (uint8_t)(address >> (24 - index * 8)));
        if (index != 3) {
            text[position++] = '.';
        }
    }
    text[position] = '\0';
}

void net_format_mac(const uint8_t address[NET_ETHERNET_ADDRESS_SIZE], char text[18]) {
    for (uint64_t index = 0; index < NET_ETHERNET_ADDRESS_SIZE; ++index) {
        text[index * 3] = hex_digit(address[index] >> 4);
        text[index * 3 + 1] = hex_digit(address[index] & 0x0f);
        if (index != NET_ETHERNET_ADDRESS_SIZE - 1) {
            text[index * 3 + 2] = ':';
        }
    }
    text[17] = '\0';
}
