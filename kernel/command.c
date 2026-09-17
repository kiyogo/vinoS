#include <stdbool.h>

#include <kos/command.h>
#include <kos/console.h>
#include <kos/cpu.h>
#include <kos/driver.h>
#include <kos/e1000.h>
#include <kos/elf.h>
#include <kos/interrupts.h>
#include <kos/keyboard.h>
#include <kos/log.h>
#include <kos/net.h>
#include <kos/pci.h>
#include <kos/pic.h>
#include <kos/pmm.h>
#include <kos/serial.h>
#include <kos/system.h>
#include <kos/timer.h>
#include <kos/task.h>
#include <kos/vfs.h>
#include <kos/heap.h>
#include <kos/ramfs.h>

typedef void (*command_handler)(const char *arguments);

struct command_entry {
    const char *name;
    command_handler handler;
};

static void command_help(const char *arguments) {
    (void)arguments;
    log_info("help clear meminfo uptime sysinfo netinfo ifconfig arp arping ping dhcp udptest tcptest nslookup curl httpd netstat echo reboot irqinfo kbdinfo drivers lspci taskinfo tasktest taskdemo vol ls cat elfinfo");
}

static void command_clear(const char *arguments) {
    (void)arguments;
    console_clear();
    serial_write("\x1b[2J\x1b[H");
}

static void command_meminfo(const char *arguments) {
    (void)arguments;
    log_info("meminfo");
    log_info_u64("  Total RAM bytes: ", pmm_total_memory_bytes());
    log_info_u64("  Usable RAM bytes: ", pmm_usable_memory_bytes());
    log_info_u64("  PMM tracked bytes: ", pmm_tracked_memory_bytes());
    log_info_u64("  Kernel/modules reserved bytes: ", pmm_kernel_memory_bytes());
    log_info_u64("  Framebuffer reserved bytes: ", pmm_framebuffer_memory_bytes());
    log_info_u64("  Bootloader reclaimable bytes: ", pmm_bootloader_reclaimable_bytes());
    log_info_u64("  Frame size bytes: ", pmm_frame_size());
    log_info_u64("  Free frames: ", pmm_free_frame_count());
    log_info_u64("  Allocated frames: ", pmm_allocated_frame_count());
}

static void command_uptime(const char *arguments) {
    (void)arguments;
    log_info_u64_suffix("uptime: ", timer_uptime_seconds(), " seconds");
}

static void command_sysinfo(const char *arguments) {
    (void)arguments;
    struct kos_system_info info;
    system_info_snapshot(&info);
    log_info("sysinfo");
    log_info("  Kernel: KOS v0.1");
    log_info("  Architecture: x86_64");
    log_info_u64("  Uptime seconds: ", info.uptime_seconds);
    log_info_u64("  Total memory bytes: ", info.total_memory_bytes);
    log_info_u64("  Usable memory bytes: ", info.usable_memory_bytes);
    log_info_u64("  Free memory frames: ", info.free_memory_frames);
    log_info_u64("  Kernel tasks: ", info.kernel_task_count);
    log_info_u64("  Ready tasks: ", info.ready_task_count);
    log_info("  Process isolation: unavailable");
    log_info("  Scheduler: cooperative round-robin");
}

static void command_netinfo(const char *arguments) {
    (void)arguments;
    log_info("netinfo");
    log_info("  IPv4 UDP socket core: loopback and IPv4 external ready");
    log_info(net_e1000_detected()
        ? "  e1000 NIC: initialized (RX/TX DMA rings ready)"
        : "  e1000 NIC: no compatible adapter");
    struct net_interface_info info;
    if (net_interface_info(&info) && info.available) {
        if (info.configuration_source == NET_CONFIGURATION_DHCP) {
            log_info("  eth0: DHCP configured; lease active");
        }
        else {
            log_info("  eth0: static IPv4 configured; ARP/Ethernet ready");
        }
    }
    else {
        log_info("  eth0: unavailable");
    }
    log_info_u64("  UDP datagrams sent: ", net_udp_datagrams_sent());
    log_info_u64("  UDP datagrams received: ", net_udp_datagrams_received());
    log_info_u64("  UDP datagrams dropped: ", net_udp_datagrams_dropped());
    log_info_u64("  Ethernet frames transmitted: ", e1000_transmitted_frame_count());
    log_info_u64("  Ethernet frames received: ", e1000_received_frame_count());
    log_info_u64("  Ethernet frames dropped: ", net_ethernet_frames_dropped());
    log_info_u64("  e1000 TX timeouts: ", e1000_transmit_timeout_count());
    log_info_u64("  ARP requests sent: ", net_arp_requests_sent());
    log_info_u64("  ARP replies received: ", net_arp_replies_received());
    log_info_u64("  ICMP echo requests sent: ", net_icmp_echo_requests_sent());
    log_info_u64("  ICMP echo replies received: ", net_icmp_echo_replies_received());
    log_info_u64("  DHCP discovers sent: ", net_dhcp_discovers_sent());
    log_info_u64("  DHCP offers received: ", net_dhcp_offers_received());
    log_info_u64("  DHCP requests sent: ", net_dhcp_requests_sent());
    log_info_u64("  DHCP acks received: ", net_dhcp_acks_received());
    log_info_u64("  TCP segments sent: ", net_tcp_segments_sent());
    log_info_u64("  TCP segments received: ", net_tcp_segments_received());
    log_info_u64("  TCP retransmissions: ", net_tcp_retransmissions());
    log_info_u64("  DNS queries sent: ", net_dns_queries_sent());
    log_info_u64("  DNS responses received: ", net_dns_responses_received());
}

static void command_dhcp(const char *arguments) {
    (void)arguments;
    if (!net_has_external_interface()) {
        log_error("dhcp: eth0 is unavailable.");
        return;
    }
    if (!net_dhcp_acquire(timer_frequency_hz() * 5)) {
        log_error("dhcp: failed to acquire DHCP lease.");
        return;
    }
    struct net_interface_info info;
    if (net_interface_info(&info) && info.available) {
        char address[16];
        char gateway[16];
        char dns[16];
        char line[64];
        net_format_ipv4(info.ipv4_address, address);
        net_format_ipv4(info.gateway, gateway);
        net_format_ipv4(info.dns_server, dns);

        uint64_t pos = 0;
        const char *prefix_addr = "DHCP: address ";
        for (uint64_t i = 0; prefix_addr[i] != '\0'; ++i) {
            line[pos++] = prefix_addr[i];
        }
        for (uint64_t i = 0; address[i] != '\0'; ++i) {
            line[pos++] = address[i];
        }
        line[pos] = '\0';
        log_info(line);

        pos = 0;
        const char *prefix_gw = "DHCP: gateway ";
        for (uint64_t i = 0; prefix_gw[i] != '\0'; ++i) {
            line[pos++] = prefix_gw[i];
        }
        for (uint64_t i = 0; gateway[i] != '\0'; ++i) {
            line[pos++] = gateway[i];
        }
        line[pos] = '\0';
        log_info(line);

        pos = 0;
        const char *prefix_dns = "DHCP: DNS ";
        for (uint64_t i = 0; prefix_dns[i] != '\0'; ++i) {
            line[pos++] = prefix_dns[i];
        }
        for (uint64_t i = 0; dns[i] != '\0'; ++i) {
            line[pos++] = dns[i];
        }
        line[pos] = '\0';
        log_info(line);
    }
}

static void command_ifconfig(const char *arguments) {
    (void)arguments;
    struct net_interface_info info;
    if (!net_interface_info(&info) || !info.available) {
        log_error("ifconfig: eth0 is unavailable.");
        return;
    }
    char mac[18];
    char address[16];
    char mask[16];
    char gateway[16];
    char dns[16];
    net_format_mac(info.mac_address, mac);
    net_format_ipv4(info.ipv4_address, address);
    net_format_ipv4(info.subnet_mask, mask);
    net_format_ipv4(info.gateway, gateway);
    net_format_ipv4(info.dns_server, dns);
    log_info("eth0");
    log_info("  MAC:"); log_info(mac);
    log_info("  IPv4:"); log_info(address);
    log_info("  Netmask:"); log_info(mask);
    log_info("  Gateway:"); log_info(gateway);
    log_info("  DNS:"); log_info(dns);
    log_info_u64("  IPv4 packets received: ", info.ipv4_packets_received);
    log_info_u64("  IPv4 packets dropped: ", info.ipv4_packets_dropped);
}

static void command_arp(const char *arguments) {
    (void)arguments;
    log_info("ARP cache");
    uint64_t count = net_arp_cache_count();
    if (count == 0) {
        log_info("  (empty)");
        return;
    }
    for (uint64_t index = 0; index < count; ++index) {
        struct net_arp_cache_entry entry;
        char address[16];
        char mac[18];
        if (net_arp_cache_at(index, &entry)) {
            net_format_ipv4(entry.ipv4_address, address);
            net_format_mac(entry.mac_address, mac);
            log_info(address);
            log_info(mac);
        }
    }
}

static bool command_parse_ipv4(const char *text, uint32_t *address) {
    if (text == 0 || address == 0 || *text == '\0') {
        return false;
    }
    uint32_t result = 0;
    for (uint64_t octet = 0; octet < 4; ++octet) {
        uint32_t value = 0;
        uint64_t digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (uint32_t)(*text - '0');
            ++text;
            ++digits;
            if (value > 255 || digits > 3) {
                return false;
            }
        }
        if (digits == 0 || (octet < 3 && *text != '.') || (octet == 3 && *text != '\0')) {
            return false;
        }
        result = (result << 8) | value;
        if (octet < 3) {
            ++text;
        }
    }
    *address = result;
    return true;
}

static void command_arping(const char *arguments) {
    uint32_t address;
    uint8_t mac[NET_ETHERNET_ADDRESS_SIZE];
    if (!command_parse_ipv4(arguments, &address)) {
        log_error("arping: use an IPv4 address, for example arping 10.0.2.2.");
        return;
    }
    if (!net_arp_resolve(address, mac, NET_ARP_DEFAULT_TIMEOUT_TICKS)) {
        log_error("arping: no ARP reply before timeout.");
        return;
    }
    char address_text[16];
    char mac_text[18];
    net_format_ipv4(address, address_text);
    net_format_mac(mac, mac_text);
    log_info("ARP reply from:");
    log_info(address_text);
    log_info(mac_text);
}

static bool command_parse_ping_arguments(const char *arguments, uint32_t *address, uint64_t *count) {
    if (arguments == 0 || address == 0 || count == 0) {
        return false;
    }
    char address_text[16];
    uint64_t length = 0;
    while (arguments[length] != '\0' && arguments[length] != ' ' && length + 1 < sizeof(address_text)) {
        address_text[length] = arguments[length];
        ++length;
    }
    address_text[length] = '\0';
    if (!command_parse_ipv4(address_text, address)) {
        return false;
    }
    while (arguments[length] == ' ') {
        ++length;
    }
    if (arguments[length] == '\0') {
        *count = 4;
        return true;
    }
    uint64_t value = 0;
    uint64_t digits = 0;
    while (arguments[length] >= '0' && arguments[length] <= '9') {
        value = value * 10 + (uint64_t)(arguments[length] - '0');
        ++length;
        ++digits;
        if (value > 16) {
            return false;
        }
    }
    if (digits == 0 || arguments[length] != '\0' || value == 0) {
        return false;
    }
    *count = value;
    return true;
}

static void command_ping(const char *arguments) {
    uint32_t address;
    uint64_t count;
    if (!command_parse_ping_arguments(arguments, &address, &count)) {
        log_error("ping: use ping <IPv4> [count], with count from 1 to 16.");
        return;
    }
    char address_text[16];
    net_format_ipv4(address, address_text);
    log_info("PING");
    log_info(address_text);
    uint64_t received = 0;
    for (uint64_t sequence = 1; sequence <= count; ++sequence) {
        uint64_t round_trip_ticks;
        if (net_icmp_ping(address, (uint16_t)sequence, timer_frequency_hz(), &round_trip_ticks)) {
            ++received;
            log_info("64 bytes from:");
            log_info(address_text);
            log_info_u64("  seq: ", sequence);
            log_info_u64("  time ms: ", round_trip_ticks * 1000 / timer_frequency_hz());
        }
        else {
            log_info_u64("Request timed out, seq: ", sequence);
        }
    }
    log_info_u64("Packets transmitted: ", count);
    log_info_u64("Packets received: ", received);
    log_info_u64("Packet loss percent: ", (count - received) * 100 / count);
}

static void command_udptest(const char *arguments) {
    (void)arguments;
    if (net_udp_loopback_self_test()) {
        log_info("udptest passed: UDP payload crossed the 127.0.0.1 socket path.");
    }
    else {
        log_error("udptest failed: UDP loopback socket path is unhealthy.");
    }
}

static bool command_parse_tcp_arguments(const char *arguments, uint32_t *address, uint16_t *port) {
    if (arguments == 0 || address == 0 || port == 0) {
        return false;
    }
    char ip_text[16];
    uint64_t length = 0;
    while (arguments[length] != '\0' && arguments[length] != ' ' && length + 1 < sizeof(ip_text)) {
        ip_text[length] = arguments[length];
        ++length;
    }
    ip_text[length] = '\0';
    if (!command_parse_ipv4(ip_text, address)) {
        return false;
    }
    while (arguments[length] == ' ') {
        ++length;
    }
    if (arguments[length] == '\0') {
        *port = 80;
        return true;
    }
    uint32_t val = 0;
    while (arguments[length] >= '0' && arguments[length] <= '9') {
        val = val * 10 + (uint32_t)(arguments[length] - '0');
        if (val > 65535) {
            return false;
        }
        ++length;
    }
    if (val == 0 || arguments[length] != '\0') {
        return false;
    }
    *port = (uint16_t)val;
    return true;
}

static void command_tcptest(const char *arguments) {
    uint32_t address;
    uint16_t port;
    if (!command_parse_tcp_arguments(arguments, &address, &port)) {
        log_error("tcptest: use tcptest <IPv4> [port], default port 80.");
        return;
    }
    char address_text[16];
    net_format_ipv4(address, address_text);
    log_info("tcptest: connecting to TCP host...");
    int sock = tcp_connect(address, port);
    if (sock < 0) {
        log_error("tcptest: TCP connection failed or timed out.");
        return;
    }
    log_info("tcptest: TCP connection ESTABLISHED!");
    log_info("tcptest: sending HTTP GET request...");
    const char *req = "GET / HTTP/1.0\r\nHost: 1.1.1.1\r\nUser-Agent: KOS\r\n\r\n";
    uint64_t req_len = 0;
    while (req[req_len] != '\0') {
        ++req_len;
    }
    int sent = tcp_send(sock, (const uint8_t *)req, req_len);
    if (sent <= 0) {
        log_error("tcptest: failed to send HTTP request.");
        tcp_close(sock);
        return;
    }
    log_info("tcptest: waiting for HTTP response...");
    uint8_t buffer[512];
    int received = tcp_receive(sock, buffer, sizeof(buffer) - 1);
    if (received > 0) {
        buffer[received] = '\0';
        log_info("tcptest: HTTP response received:");
        log_info((const char *)buffer);
    }
    else {
        log_error("tcptest: no response received or connection closed.");
    }
    tcp_close(sock);
    log_info("tcptest: TCP connection closed cleanly.");
}

static void command_nslookup(const char *arguments) {
    if (arguments == 0 || *arguments == '\0') {
        log_error("nslookup: use nslookup <hostname>");
        return;
    }
    while (*arguments == ' ') {
        ++arguments;
    }
    char hostname[64];
    uint64_t len = 0;
    while (arguments[len] != '\0' && arguments[len] != ' ' && len + 1 < sizeof(hostname)) {
        hostname[len] = arguments[len];
        ++len;
    }
    hostname[len] = '\0';
    if (len == 0) {
        log_error("nslookup: use nslookup <hostname>");
        return;
    }

    struct net_interface_info info;
    if (net_interface_info(&info) && info.available && info.dns_server != 0) {
        char dns_text[16];
        char server_line[48];
        net_format_ipv4(info.dns_server, dns_text);
        uint64_t pos = 0;
        const char *p1 = "Server: ";
        for (uint64_t i = 0; p1[i] != '\0'; ++i) server_line[pos++] = p1[i];
        for (uint64_t i = 0; dns_text[i] != '\0'; ++i) server_line[pos++] = dns_text[i];
        server_line[pos] = '\0';
        log_info(server_line);
    }

    uint32_t resolved_ip = 0;
    if (!net_dns_resolve(hostname, &resolved_ip, timer_frequency_hz() * 5)) {
        log_error("nslookup: non-existent domain or server timed out.");
        return;
    }

    char ip_text[16];
    net_format_ipv4(resolved_ip, ip_text);

    char name_line[80];
    uint64_t pos = 0;
    const char *p2 = "Name: ";
    for (uint64_t i = 0; p2[i] != '\0'; ++i) name_line[pos++] = p2[i];
    for (uint64_t i = 0; hostname[i] != '\0'; ++i) name_line[pos++] = hostname[i];
    name_line[pos] = '\0';
    log_info(name_line);

    char addr_line[48];
    pos = 0;
    const char *p3 = "Address: ";
    for (uint64_t i = 0; p3[i] != '\0'; ++i) addr_line[pos++] = p3[i];
    for (uint64_t i = 0; ip_text[i] != '\0'; ++i) addr_line[pos++] = ip_text[i];
    addr_line[pos] = '\0';
    log_info(addr_line);
}

static char curl_response_buffer[NET_HTTP_BUFFER_MAX];

static void command_curl(const char *arguments) {
    if (arguments == 0 || *arguments == '\0') {
        log_error("curl: use curl [-i] <url>");
        return;
    }
    while (*arguments == ' ') {
        ++arguments;
    }
    bool include_headers = false;
    if (arguments[0] == '-' && (arguments[1] == 'i' || arguments[1] == 'I')
        && (arguments[2] == ' ' || arguments[2] == '\0')) {
        include_headers = true;
        arguments += 2;
        while (*arguments == ' ') {
            ++arguments;
        }
    }
    if (*arguments == '\0') {
        log_error("curl: missing URL. Use curl [-i] <url>");
        return;
    }

    char url[256];
    uint64_t url_len = 0;
    while (arguments[url_len] != '\0' && arguments[url_len] != ' ' && url_len + 1 < sizeof(url)) {
        url[url_len] = arguments[url_len];
        ++url_len;
    }
    url[url_len] = '\0';

    uint64_t response_size = 0;
    if (!net_http_get(url, curl_response_buffer, sizeof(curl_response_buffer), &response_size)) {
        log_error("curl: failed to fetch URL (connection, DNS, or HTTP error).");
        return;
    }

    const char *output = curl_response_buffer;
    if (!include_headers) {
        const char *body = 0;
        for (uint64_t i = 0; curl_response_buffer[i] != '\0'; ++i) {
            if (curl_response_buffer[i] == '\r' && curl_response_buffer[i + 1] == '\n'
                && curl_response_buffer[i + 2] == '\r' && curl_response_buffer[i + 3] == '\n') {
                body = &curl_response_buffer[i + 4];
                break;
            }
            if (curl_response_buffer[i] == '\n' && curl_response_buffer[i + 1] == '\n') {
                body = &curl_response_buffer[i + 2];
                break;
            }
        }
        if (body != 0) {
            output = body;
        }
    }

    for (uint64_t index = 0; output[index] != '\0'; ++index) {
        char character = output[index];
        if (character == '\r') {
            continue;
        }
        char printable = (character >= ' ' && character <= '~') || character == '\n' || character == '\t'
            ? character : '?';
        serial_write_char(printable);
        console_write_char(printable);
    }
    uint64_t out_len = 0;
    while (output[out_len] != '\0') {
        ++out_len;
    }
    if (out_len == 0 || output[out_len - 1] != '\n') {
        serial_write_char('\n');
        console_write_char('\n');
    }
}

static void command_httpd(const char *arguments) {
    while (*arguments == ' ') {
        ++arguments;
    }
    if (*arguments == '\0' || (arguments[0] == 's' && arguments[1] == 't' && arguments[2] == 'a' && arguments[3] == 't' && arguments[4] == 'u' && arguments[5] == 's')) {
        if (httpd_is_running()) {
            log_info_u64("httpd: running on port ", httpd_listen_port());
            log_info_u64("  Requests served: ", httpd_requests_served());
        }
        else {
            log_info("httpd: stopped. Use httpd start [port] to launch.");
        }
        return;
    }
    if (arguments[0] == 's' && arguments[1] == 't' && arguments[2] == 'o' && arguments[3] == 'p') {
        httpd_stop();
        return;
    }
    if (arguments[0] == 's' && arguments[1] == 't' && arguments[2] == 'a' && arguments[3] == 'r' && arguments[4] == 't') {
        arguments += 5;
        while (*arguments == ' ') {
            ++arguments;
        }
        uint16_t port = NET_HTTP_PORT;
        if (*arguments >= '0' && *arguments <= '9') {
            uint32_t val = 0;
            while (*arguments >= '0' && *arguments <= '9') {
                val = val * 10 + (uint32_t)(*arguments++ - '0');
                if (val > 65535) {
                    log_error("httpd: invalid port.");
                    return;
                }
            }
            if (val > 0) {
                port = (uint16_t)val;
            }
        }
        if (!httpd_start(port)) {
            log_error("httpd: failed to start web server.");
        }
        return;
    }
    log_error("httpd: use httpd start [port], httpd stop, or httpd status.");
}

static const char *tcp_state_name(enum tcp_state state) {
    switch (state) {
        case TCP_STATE_LISTEN: return "LISTEN";
        case TCP_STATE_SYN_SENT: return "SYN_SENT";
        case TCP_STATE_SYN_RECEIVED: return "SYN_RECV";
        case TCP_STATE_ESTABLISHED: return "ESTABLISHED";
        case TCP_STATE_FIN_WAIT_1: return "FIN_WAIT1";
        case TCP_STATE_FIN_WAIT_2: return "FIN_WAIT2";
        case TCP_STATE_CLOSE_WAIT: return "CLOSE_WAIT";
        case TCP_STATE_LAST_ACK: return "LAST_ACK";
        case TCP_STATE_TIME_WAIT: return "TIME_WAIT";
        case TCP_STATE_CLOSED: return "CLOSED";
        default: return "UNKNOWN";
    }
}

static void format_endpoint(uint32_t ip, uint16_t port, char *buf, uint64_t max_len) {
    char ip_str[16];
    if (ip == 0) {
        ip_str[0] = '*';
        ip_str[1] = '\0';
    }
    else {
        net_format_ipv4(ip, ip_str);
    }
    uint64_t pos = 0;
    for (uint64_t i = 0; ip_str[i] != '\0' && pos + 1 < max_len; ++i) {
        buf[pos++] = ip_str[i];
    }
    if (pos + 1 < max_len) {
        buf[pos++] = ':';
    }
    if (port == 0) {
        if (pos + 1 < max_len) buf[pos++] = '*';
    }
    else {
        uint32_t p = port;
        uint32_t digits = 0;
        char tmp[8];
        do {
            tmp[digits++] = (char)('0' + (p % 10));
            p /= 10;
        } while (p > 0);
        for (uint32_t d = 0; d < digits && pos + 1 < max_len; ++d) {
            buf[pos++] = tmp[digits - 1 - d];
        }
    }
    buf[pos] = '\0';
}

static void command_netstat(const char *arguments) {
    (void)arguments;
    log_info("Active Internet connections");
    log_info("Proto  Local Address          Foreign Address        State");

    struct net_tcp_socket_info sockets[NET_TCP_SOCKET_MAX];
    uint64_t count = net_tcp_get_sockets(sockets, NET_TCP_SOCKET_MAX);
    for (uint64_t i = 0; i < count; ++i) {
        char line[80];
        char local_ep[24];
        char remote_ep[24];
        format_endpoint(sockets[i].local_ip, sockets[i].local_port, local_ep, sizeof(local_ep));
        format_endpoint(sockets[i].remote_ip, sockets[i].remote_port, remote_ep, sizeof(remote_ep));

        uint64_t pos = 0;
        const char *proto = "tcp    ";
        for (uint64_t c = 0; proto[c] != '\0'; ++c) line[pos++] = proto[c];

        for (uint64_t c = 0; local_ep[c] != '\0'; ++c) line[pos++] = local_ep[c];
        while (pos < 30) line[pos++] = ' ';

        for (uint64_t c = 0; remote_ep[c] != '\0'; ++c) line[pos++] = remote_ep[c];
        while (pos < 53) line[pos++] = ' ';

        const char *st = tcp_state_name(sockets[i].state);
        for (uint64_t c = 0; st[c] != '\0'; ++c) line[pos++] = st[c];
        line[pos] = '\0';

        log_info(line);
    }
    if (count == 0) {
        log_info("  (no active TCP sockets)");
    }
}

static void command_echo(const char *arguments) {
    log_info(arguments);
}

static void command_reboot(const char *arguments) {
    (void)arguments;
    log_info("Rebooting through the PS/2 controller.");
    cpu_reboot();
}

static void command_irqinfo(const char *arguments) {
    (void)arguments;
    log_info("irqinfo");
    log_info_u64("  IRQ0 timer dispatches: ", irq_dispatch_count(KOS_PIC_IRQ_TIMER));
    log_info_u64("  IRQ0 timer unhandled: ", irq_unhandled_count(KOS_PIC_IRQ_TIMER));
    log_info_u64("  IRQ1 keyboard dispatches: ", irq_dispatch_count(KOS_PIC_IRQ_KEYBOARD));
    log_info_u64("  IRQ1 keyboard unhandled: ", irq_unhandled_count(KOS_PIC_IRQ_KEYBOARD));
}

static void command_kbdinfo(const char *arguments) {
    (void)arguments;
    log_info("kbdinfo");
    log_info_u64("  Pending characters: ", keyboard_pending_count());
    log_info_u64("  Dropped characters: ", keyboard_dropped_char_count());
}

static void command_drivers(const char *arguments) {
    (void)arguments;
    log_info("drivers");
    for (uint64_t index = 0; index < driver_count(); ++index) {
        const struct driver *driver = driver_at(index);
        if (driver != 0) {
            log_info(driver->name);
        }
    }
}

static void command_lspci(const char *arguments) {
    (void)arguments;
    log_info("lspci");
    for (uint64_t index = 0; index < pci_device_count(); ++index) {
        const struct pci_device *device = pci_device_at(index);
        if (device == 0) {
            continue;
        }
        uint64_t bdf = ((uint64_t)device->bus << 16) | ((uint64_t)device->device << 8)
            | device->function;
        uint64_t class_info = ((uint64_t)device->class_code << 16)
            | ((uint64_t)device->subclass << 8) | device->programming_interface;
        log_info_hex("  BDF: ", bdf);
        log_info_hex("    Vendor ID: ", device->vendor_id);
        log_info_hex("    Device ID: ", device->device_id);
        log_info_hex("    Class/Subclass/ProgIF: ", class_info);
        log_info_u64("    IRQ line: ", device->interrupt_line);
    }
}

static void command_taskinfo(const char *arguments) {
    (void)arguments;
    log_info("taskinfo");
    log_info_u64("  Known tasks: ", task_count());
    log_info_u64("  Ready tasks: ", task_ready_count());
    log_info("  Scheduler: cooperative round-robin; timer requests reschedule.");
}

static void command_tasktest(const char *arguments) {
    (void)arguments;
    if (task_run_self_test()) {
        log_info("tasktest passed: two kernel tasks each yielded 32 times.");
    }
    else {
        log_error("tasktest failed: scheduler is busy or context switch failed.");
    }
}

static void command_taskdemo(const char *arguments) {
    (void)arguments;
    if (task_run_log_demo()) {
        log_info("taskdemo passed: keyboard IRQ remained enabled while tasks alternated.");
    }
    else {
        log_error("taskdemo failed: scheduler is busy or task creation failed.");
    }
}

static bool command_parse_volume(const char *arguments, char *letter) {
    if (arguments == 0 || letter == 0) {
        return false;
    }
    if (*arguments == '\0') {
        *letter = 'C';
        return true;
    }
    if (arguments[1] != ':' || arguments[2] != '\0') {
        return false;
    }
    *letter = arguments[0] >= 'a' && arguments[0] <= 'z'
        ? (char)(arguments[0] - ('a' - 'A')) : arguments[0];
    return vfs_volume_is_mounted(*letter);
}

static void command_vol(const char *arguments) {
    (void)arguments;
    log_info("vol");
    for (uint64_t index = 0; index < vfs_volume_count(); ++index) {
        struct vfs_volume_info info;
        if (!vfs_volume_info_at(index, &info)) {
            continue;
        }
        char name[] = { info.letter, ':', '\0' };
        log_info(name);
        log_info("  Label:");
        log_info(info.label);
        log_info_u64("  Files: ", info.file_count);
    }
}

static void command_ls(const char *arguments) {
    char letter;
    if (!command_parse_volume(arguments, &letter)) {
        log_error("ls: use a mounted volume, for example C: or D:.");
        return;
    }
    char name[] = { letter, ':', '\\', '\0' };
    log_info(name);
    for (uint64_t index = 0; index < vfs_file_count_on_volume(letter); ++index) {
        const struct vfs_file *file = vfs_file_at_on_volume(letter, index);
        if (file != 0) {
            log_info(file->path);
        }
    }
}

static void command_cat(const char *arguments) {
    const uint8_t *data;
    uint64_t size;
    if (*arguments == '\0' || !vfs_read_file(arguments, &data, &size)) {
        log_error("cat: file not found.");
        return;
    }
    for (uint64_t index = 0; index < size; ++index) {
        char character = data[index] >= ' ' && data[index] <= '~' ? (char)data[index]
            : data[index] == '\n' ? '\n' : '?';
        serial_write_char(character);
        console_write_char(character);
    }
    if (size == 0 || data[size - 1] != '\n') {
        serial_write_char('\n');
        console_write_char('\n');
    }
}

static void command_elfinfo(const char *arguments) {
    const uint8_t *data;
    uint64_t size;
    struct elf_image_info info;
    if (*arguments == '\0' || !vfs_read_file(arguments, &data, &size)) {
        log_error("elfinfo: file not found.");
        return;
    }
    if (!elf_inspect_image(data, size, &info)) {
        log_error("elfinfo: not a supported x86_64 ELF image.");
        return;
    }
    log_info_hex("ELF entry: ", info.entry_point);
    log_info_u64("ELF program headers: ", info.program_header_count);
    log_info_u64("ELF loadable segments: ", info.loadable_segment_count);
}

/* ========================================================================= */
/* Diagnostic Command: synctest (K12.17 - K12.21)                            */
/* ========================================================================= */

struct pmm_stress_worker_arg {
    uint32_t worker_id;
    uint32_t iterations;
    volatile uint32_t completed_iters;
    volatile uint32_t errors;
    volatile bool finished;
};

static void pmm_stress_worker(void *arg) {
    struct pmm_stress_worker_arg *w = (struct pmm_stress_worker_arg *)arg;
    for (uint32_t iter = 0; iter < w->iterations; ++iter) {
        uint64_t p1 = pmm_alloc_page();
        uint64_t p2 = pmm_alloc_page();
        uint64_t pmulti = pmm_alloc_pages(2);

        if (p1 == 0 || p2 == 0 || pmulti == 0 || p1 == p2 || p1 == pmulti || p2 == pmulti) {
            w->errors++;
        }
        if ((p1 & 0xfff) != 0 || (p2 & 0xfff) != 0 || (pmulti & 0xfff) != 0) {
            w->errors++;
        }

        task_yield();

        if (p1 != 0 && !pmm_free_page(p1)) {
            w->errors++;
        }
        if (p2 != 0 && !pmm_free_page(p2)) {
            w->errors++;
        }
        if (pmulti != 0 && !pmm_free_pages(pmulti, 2)) {
            w->errors++;
        }

        w->completed_iters++;
    }
    w->finished = true;
}

struct heap_stress_worker_arg {
    uint32_t worker_id;
    uint32_t iterations;
    volatile uint32_t completed_iters;
    volatile uint32_t errors;
    volatile bool finished;
};

static void heap_stress_worker(void *arg) {
    struct heap_stress_worker_arg *w = (struct heap_stress_worker_arg *)arg;
    for (uint32_t iter = 0; iter < w->iterations; ++iter) {
        uint8_t *b32 = (uint8_t *)kmalloc(32);
        uint8_t *b64 = (uint8_t *)kmalloc(64);
        uint8_t *b128 = (uint8_t *)kmalloc(128);
        uint32_t *bzero = (uint32_t *)kcalloc(16, sizeof(uint32_t));

        if (b32 == 0 || b64 == 0 || b128 == 0 || bzero == 0) {
            w->errors++;
        } else {
            for (uint32_t i = 0; i < 16; ++i) {
                if (bzero[i] != 0) {
                    w->errors++;
                    break;
                }
            }

            uint8_t tag = (uint8_t)((w->worker_id << 4) | (iter & 0x0f));
            for (int i = 0; i < 32; ++i) b32[i] = tag;
            for (int i = 0; i < 64; ++i) b64[i] = (uint8_t)(tag ^ 0xaa);

            uint8_t *b_realloc = (uint8_t *)krealloc(b64, 128);
            if (b_realloc == 0) {
                w->errors++;
            } else {
                for (int i = 0; i < 64; ++i) {
                    if (b_realloc[i] != (uint8_t)(tag ^ 0xaa)) {
                        w->errors++;
                        break;
                    }
                }
                b64 = b_realloc;
            }

            task_yield();

            for (int i = 0; i < 32; ++i) {
                if (b32[i] != tag) {
                    w->errors++;
                    break;
                }
            }
        }

        if (b32 != 0) (void)kfree(b32);
        if (b64 != 0) (void)kfree(b64);
        if (b128 != 0) (void)kfree(b128);
        if (bzero != 0) (void)kfree(bzero);

        w->completed_iters++;
    }
    w->finished = true;
}

struct vfs_stress_worker_arg {
    char filename[32];
    char pattern[32];
    uint32_t iterations;
    volatile uint32_t completed_iters;
    volatile uint32_t errors;
    volatile bool finished;
};

static void vfs_stress_worker(void *arg) {
    struct vfs_stress_worker_arg *w = (struct vfs_stress_worker_arg *)arg;
    uint64_t pat_len = 0;
    while (w->pattern[pat_len] != '\0') pat_len++;

    for (uint32_t iter = 0; iter < w->iterations; ++iter) {
        if (!vfs_write_file(w->filename, (const uint8_t *)w->pattern, pat_len)) {
            w->errors++;
        }

        task_yield();

        const uint8_t *data = 0;
        uint64_t size = 0;
        if (!vfs_read_file(w->filename, &data, &size) || size != pat_len || data == 0) {
            w->errors++;
        } else {
            for (uint64_t i = 0; i < pat_len; ++i) {
                if (data[i] != (uint8_t)w->pattern[i]) {
                    w->errors++;
                    break;
                }
            }
        }

        task_yield();

        if (!vfs_unlink_file(w->filename)) {
            w->errors++;
        }

        w->completed_iters++;
    }
    w->finished = true;
}

struct vfs_reader_worker_arg {
    uint32_t iterations;
    volatile uint32_t completed_iters;
    volatile uint32_t errors;
    volatile bool finished;
};

static void vfs_reader_worker(void *arg) {
    struct vfs_reader_worker_arg *w = (struct vfs_reader_worker_arg *)arg;
    for (uint32_t iter = 0; iter < w->iterations; ++iter) {
        uint64_t vcount = vfs_volume_count();
        if (vcount == 0) {
            w->errors++;
        }
        struct vfs_volume_info vinfo;
        if (!vfs_volume_info_at(0, &vinfo)) {
            w->errors++;
        }
        (void)vfs_file_count_on_volume('D');
        task_yield();
        w->completed_iters++;
    }
    w->finished = true;
}

struct socket_sync_sender_arg {
    uint16_t port;
    uint64_t delay_ms;
    volatile bool finished;
};

static void socket_sync_sender_worker(void *arg) {
    struct socket_sync_sender_arg *sarg = (struct socket_sync_sender_arg *)arg;
    task_sleep(sarg->delay_ms);
    uint16_t sender_id = UINT16_MAX;
    if (net_udp_open(&sender_id)) {
        struct net_ipv4_endpoint target = {
            .address = NET_IPV4_LOOPBACK,
            .port = sarg->port,
        };
        static const uint8_t msg[] = "SYNC-WAITQ-OK";
        (void)net_udp_sendto(sender_id, target, msg, sizeof(msg) - 1);
        (void)net_udp_close(sender_id);
    }
    sarg->finished = true;
}

static void command_synctest(const char *arguments) {
    (void)arguments;
    log_info("==================================================");
    log_info("=== Milestone K12 Step 12 Subsystem Sync Tests ===");
    log_info("==================================================");

    bool all_passed = true;
    struct process *cur_proc = process_get_kernel();

    /* ------------------------------------------------------------- */
    /* Test 1: PMM Concurrent Stress (K12.17)                        */
    /* ------------------------------------------------------------- */
    log_info("[synctest] 1. PMM Concurrent Allocation Stress...");
    {
        uint64_t initial_frames = pmm_allocated_frame_count();
        enum { WORKER_COUNT = 4, ITERS = 25 };
        struct pmm_stress_worker_arg args[WORKER_COUNT];

        for (int i = 0; i < WORKER_COUNT; ++i) {
            args[i].worker_id = (uint32_t)i;
            args[i].iterations = ITERS;
            args[i].completed_iters = 0;
            args[i].errors = 0;
            args[i].finished = false;
            char name[16] = "pmm_sync_0";
            name[9] = (char)('0' + i);
            struct thread *t = thread_create(cur_proc, pmm_stress_worker, &args[i], name);
            if (t == 0) {
                log_error("  [FAIL] Failed to create PMM worker thread!");
                all_passed = false;
            }
        }

        uint64_t start_time = timer_get_ticks();
        bool done = false;
        while (!done && (timer_get_ticks() - start_time) < timer_ms_to_ticks(3000)) {
            done = true;
            for (int i = 0; i < WORKER_COUNT; ++i) {
                if (!args[i].finished) {
                    done = false;
                    break;
                }
            }
            if (!done) {
                task_yield();
            }
        }

        uint32_t total_errors = 0;
        for (int i = 0; i < WORKER_COUNT; ++i) {
            if (!args[i].finished) {
                log_error("  [FAIL] PMM worker timed out!");
                all_passed = false;
            }
            total_errors += args[i].errors;
        }

        uint64_t final_frames = pmm_allocated_frame_count();
        if (total_errors > 0 || final_frames != initial_frames) {
            log_errorf("  [FAIL] PMM stress errors=%u, initial=%llu, final=%llu", total_errors, initial_frames, final_frames);
            all_passed = false;
        } else {
            log_infof("  4 threads completed %d iterations each with 0 leaks.", ITERS);
            log_info("  [PASS] PMM concurrent allocation/free stress verified!");
        }
    }

    /* ------------------------------------------------------------- */
    /* Test 2: Heap Concurrent Stress (K12.18)                       */
    /* ------------------------------------------------------------- */
    log_info("[synctest] 2. Heap Concurrent Allocation & Realloc Stress...");
    {
        uint64_t initial_allocs = heap_active_allocation_count();
        enum { WORKER_COUNT = 4, ITERS = 20 };
        struct heap_stress_worker_arg args[WORKER_COUNT];

        for (int i = 0; i < WORKER_COUNT; ++i) {
            args[i].worker_id = (uint32_t)i;
            args[i].iterations = ITERS;
            args[i].completed_iters = 0;
            args[i].errors = 0;
            args[i].finished = false;
            char name[16] = "heap_sync_0";
            name[10] = (char)('0' + i);
            struct thread *t = thread_create(cur_proc, heap_stress_worker, &args[i], name);
            if (t == 0) {
                log_error("  [FAIL] Failed to create heap worker thread!");
                all_passed = false;
            }
        }

        uint64_t start_time = timer_get_ticks();
        bool done = false;
        while (!done && (timer_get_ticks() - start_time) < timer_ms_to_ticks(3000)) {
            done = true;
            for (int i = 0; i < WORKER_COUNT; ++i) {
                if (!args[i].finished) {
                    done = false;
                    break;
                }
            }
            if (!done) {
                task_yield();
            }
        }

        uint32_t total_errors = 0;
        for (int i = 0; i < WORKER_COUNT; ++i) {
            if (!args[i].finished) {
                log_error("  [FAIL] Heap worker timed out!");
                all_passed = false;
            }
            total_errors += args[i].errors;
        }

        uint64_t final_allocs = heap_active_allocation_count();
        if (total_errors > 0 || final_allocs != initial_allocs) {
            log_errorf("  [FAIL] Heap stress errors=%u, initial=%llu, final=%llu", total_errors, initial_allocs, final_allocs);
            all_passed = false;
        } else {
            log_infof("  4 threads completed %d iterations with malloc/calloc/realloc/free.", ITERS);
            log_info("  [PASS] Heap concurrent allocation/realloc/free stress verified!");
        }
    }

    /* ------------------------------------------------------------- */
    /* Test 3: VFS / RAMFS Concurrent Access & Unlink (K12.20)       */
    /* ------------------------------------------------------------- */
    log_info("[synctest] 3. VFS / RAMFS Concurrent Create/Read/Write/Unlink...");
    {
        uint64_t initial_files = vfs_file_count_on_volume('D');
        struct vfs_stress_worker_arg w_a = {
            .filename = "D:/sync_file_a.txt",
            .pattern = "KOS_SYNC_PAYLOAD_AAA",
            .iterations = 10,
            .completed_iters = 0,
            .errors = 0,
            .finished = false,
        };
        struct vfs_stress_worker_arg w_b = {
            .filename = "D:/sync_file_b.txt",
            .pattern = "KOS_SYNC_PAYLOAD_BBB",
            .iterations = 10,
            .completed_iters = 0,
            .errors = 0,
            .finished = false,
        };
        struct vfs_reader_worker_arg w_r = {
            .iterations = 20,
            .completed_iters = 0,
            .errors = 0,
            .finished = false,
        };

        struct thread *ta = thread_create(cur_proc, vfs_stress_worker, &w_a, "vfs_worker_a");
        struct thread *tb = thread_create(cur_proc, vfs_stress_worker, &w_b, "vfs_worker_b");
        struct thread *tr = thread_create(cur_proc, vfs_reader_worker, &w_r, "vfs_reader");

        if (ta == 0 || tb == 0 || tr == 0) {
            log_error("  [FAIL] Failed to create VFS worker threads!");
            all_passed = false;
        }

        uint64_t start_time = timer_get_ticks();
        while ((!w_a.finished || !w_b.finished || !w_r.finished) && (timer_get_ticks() - start_time) < timer_ms_to_ticks(3000)) {
            task_yield();
        }

        uint64_t final_files = vfs_file_count_on_volume('D');
        uint32_t errs = w_a.errors + w_b.errors + w_r.errors;
        if (!w_a.finished || !w_b.finished || !w_r.finished || errs > 0 || final_files != initial_files) {
            log_errorf("  [FAIL] VFS concurrent stress errors=%u, files initial=%llu, final=%llu", errs, initial_files, final_files);
            all_passed = false;
        } else {
            log_info("  Concurrent writers and readers completed with clean unlinks.");
            log_info("  [PASS] VFS/RAMFS concurrent create/read/write/unlink verified!");
        }
    }

    /* ------------------------------------------------------------- */
    /* Test 4: Socket Wait Queue Synchronization (K12.21)           */
    /* ------------------------------------------------------------- */
    log_info("[synctest] 4. Socket Wait Queue Non-Busy Synchronization...");
    {
        uint16_t recv_sock = UINT16_MAX;
        if (!net_udp_open(&recv_sock)) {
            log_error("  [FAIL] Failed to open UDP receiver socket!");
            all_passed = false;
        } else {
            struct net_ipv4_endpoint recv_ep = {
                .address = NET_IPV4_LOOPBACK,
                .port = 43210,
            };
            if (!net_udp_bind(recv_sock, recv_ep)) {
                log_error("  [FAIL] Failed to bind receiver socket!");
                all_passed = false;
            } else {
                struct socket_sync_sender_arg sarg = {
                    .port = 43210,
                    .delay_ms = 20,
                    .finished = false,
                };
                struct thread *tsend = thread_create(cur_proc, socket_sync_sender_worker, &sarg, "sock_sender");
                if (tsend == 0) {
                    log_error("  [FAIL] Failed to create socket sender thread!");
                    all_passed = false;
                } else {
                    uint8_t buffer[64];
                    struct net_ipv4_endpoint sender_ep;
                    uint64_t recv_size = 0;

                    /* Wait queue timed receive (200ms timeout) */
                    uint64_t t0 = timer_get_ticks();
                    bool ok = net_udp_recvfrom_timeout(recv_sock, &sender_ep, buffer, sizeof(buffer), &recv_size, 200);
                    uint64_t elapsed_ms = timer_ticks_to_ms(timer_get_ticks() - t0);

                    if (!ok || recv_size != 13) {
                        log_errorf("  [FAIL] Timed UDP receive failed! ok=%d, size=%llu", (int)ok, recv_size);
                        all_passed = false;
                    } else {
                        buffer[recv_size] = '\0';
                        log_infof("  Received message '%s' in %llu ms via socket wait queue.", (const char *)buffer, elapsed_ms);

                        /* Test empty socket timeout (30ms timeout) */
                        uint64_t t_to0 = timer_get_ticks();
                        bool to_res = net_udp_recvfrom_timeout(recv_sock, &sender_ep, buffer, sizeof(buffer), &recv_size, 30);
                        uint64_t to_elapsed = timer_ticks_to_ms(timer_get_ticks() - t_to0);

                        if (to_res) {
                            log_error("  [FAIL] Expected timeout on empty socket but received data!");
                            all_passed = false;
                        } else {
                            log_infof("  Empty socket cleanly timed out in %llu ms (expected ~30ms).", to_elapsed);
                            log_info("  [PASS] Socket wait queue non-busy synchronization verified!");
                        }
                    }
                }
                (void)net_udp_close(recv_sock);
            }
        }
    }

    log_info("--------------------------------------------------");
    if (all_passed) {
        log_info(">>> [PASS] ALL SUBSYSTEM SYNCHRONIZATION TESTS PASSED! <<<");
    } else {
        log_error(">>> [FAIL] ONE OR MORE SUBSYSTEM SYNCHRONIZATION TESTS FAILED! <<<");
    }
    log_info("==================================================");
}

/*
 * ============================================================================
 * Diagnostic Command: handletest (K12.14, K12.15)
 * ============================================================================
 */

struct handletest_obj {
    struct kos_object_header header;
    uint32_t magic;
    bool destructor_called;
};

static void handletest_destructor(void *object) {
    struct handletest_obj *obj = (struct handletest_obj *)object;
    if (obj != NULL) {
        obj->destructor_called = true;
    }
}

static void command_handletest(const char *arguments) {
    (void)arguments;
    log_info("==================================================");
    log_info("   KOS Kernel Object & Handle Table Diagnostics   ");
    log_info("==================================================");

    bool all_passed = true;

    /* ------------------------------------------------------------
     * Test 1: Object refcounting & custom destructor callback
     * ------------------------------------------------------------ */
    log_info("[handletest] 1. Kernel Object Refcounting & Destructor...");
    {
        struct handletest_obj obj1;
        obj1.magic = 0xABCD1234;
        obj1.destructor_called = false;

        kos_object_init(&obj1.header, KOS_OBJ_MUTEX, handletest_destructor);
        int32_t ref = kos_object_get_refcount(&obj1.header);
        if (ref != 1) {
            log_errorf("  [FAIL] Expected initial refcount 1, got %d", ref);
            all_passed = false;
        } else if (obj1.header.type != KOS_OBJ_MUTEX) {
            log_errorf("  [FAIL] Expected object type MUTEX (%d), got %d",
                       (int)KOS_OBJ_MUTEX, (int)obj1.header.type);
            all_passed = false;
        } else if (obj1.destructor_called) {
            log_error("  [FAIL] Destructor called prematurely on init!");
            all_passed = false;
        } else {
            kos_object_ref(&obj1.header);
            ref = kos_object_get_refcount(&obj1.header);
            if (ref != 2) {
                log_errorf("  [FAIL] Expected refcount 2 after kos_object_ref, got %d", ref);
                all_passed = false;
            } else {
                kos_object_unref(&obj1.header);
                ref = kos_object_get_refcount(&obj1.header);
                if (ref != 1 || obj1.destructor_called) {
                    log_errorf("  [FAIL] Expected refcount 1 and no destructor, got ref=%d called=%d",
                               ref, (int)obj1.destructor_called);
                    all_passed = false;
                } else {
                    kos_object_unref(&obj1.header);
                    ref = kos_object_get_refcount(&obj1.header);
                    if (ref != 0) {
                        log_errorf("  [FAIL] Expected refcount 0 after final unref, got %d", ref);
                        all_passed = false;
                    } else if (!obj1.destructor_called) {
                        log_error("  [FAIL] Custom destructor was not invoked upon reaching 0 refcount!");
                        all_passed = false;
                    } else {
                        log_info("  Object refcount transitions (1 -> 2 -> 1 -> 0) verified.");
                        log_info("  Custom destructor callback successfully executed at refcount 0.");
                        log_info("  [PASS] Kernel object refcounting and destructor verified!");
                    }
                }
            }
        }
    }

    /* ------------------------------------------------------------
     * Test 2: Handle creation, rights validation, generation tagging
     * ------------------------------------------------------------ */
    log_info("[handletest] 2. Handle Creation, Rights & Generation Tagging...");
    {
        struct process *proc = process_create("htest-proc2");
        if (proc == NULL || proc->handles == NULL) {
            log_error("  [FAIL] Failed to create test process with handle table!");
            all_passed = false;
        } else {
            struct handletest_obj obj2;
            obj2.magic = 0x5678EF01;
            obj2.destructor_called = false;
            kos_object_init(&obj2.header, KOS_OBJ_SEMAPHORE, NULL);

            /* Create handle with READ and WAIT rights */
            handle_t h = handle_create(proc, &obj2.header, KOS_RIGHT_READ | KOS_RIGHT_WAIT);
            if (h == KOS_INVALID_HANDLE) {
                log_error("  [FAIL] Failed to create handle!");
                all_passed = false;
            } else {
                uint16_t idx = KOS_HANDLE_INDEX(h);
                uint16_t gen = KOS_HANDLE_GEN(h);
                int32_t ref = kos_object_get_refcount(&obj2.header);

                log_infof("  Created handle 0x%08x (Index=%u, Gen=%u), object refcount=%d",
                          h, (unsigned)idx, (unsigned)gen, ref);

                if (ref != 2) {
                    log_errorf("  [FAIL] Expected object refcount 2, got %d", ref);
                    all_passed = false;
                } else if (gen == 0) {
                    log_error("  [FAIL] Handle generation must be non-zero!");
                    all_passed = false;
                } else {
                    /* Lookup with matching rights */
                    void *p_read = handle_lookup(proc, h, KOS_RIGHT_READ);
                    void *p_wait = handle_lookup(proc, h, KOS_RIGHT_WAIT);
                    void *p_both = handle_lookup(proc, h, KOS_RIGHT_READ | KOS_RIGHT_WAIT);

                    /* Lookup with forbidden / unheld rights */
                    void *p_write = handle_lookup(proc, h, KOS_RIGHT_WRITE);
                    void *p_exec = handle_lookup(proc, h, KOS_RIGHT_EXECUTE);

                    /* Lookup with type checking */
                    void *p_type_ok = handle_lookup_type(proc, h, KOS_OBJ_SEMAPHORE, KOS_RIGHT_READ);
                    void *p_type_bad = handle_lookup_type(proc, h, KOS_OBJ_MUTEX, KOS_RIGHT_READ);

                    if (p_read != &obj2.header || p_wait != &obj2.header || p_both != &obj2.header) {
                        log_error("  [FAIL] Handle lookup failed with held rights!");
                        all_passed = false;
                    } else if (p_write != NULL || p_exec != NULL) {
                        log_error("  [FAIL] Handle lookup succeeded with unheld rights (security bypass)!");
                        all_passed = false;
                    } else if (p_type_ok != &obj2.header || p_type_bad != NULL) {
                        log_error("  [FAIL] Handle lookup type validation failed!");
                        all_passed = false;
                    } else {
                        log_info("  Handle rights correctly enforced (granted rights pass, unheld fail).");
                        log_info("  Handle object type validation confirmed (SEMAPHORE match, MUTEX reject).");

                        bool closed = handle_close(proc, h);
                        ref = kos_object_get_refcount(&obj2.header);
                        if (!closed || ref != 1) {
                            log_errorf("  [FAIL] handle_close failed or bad refcount (%d)", ref);
                            all_passed = false;
                        } else {
                            log_info("  Handle successfully closed and table reference dropped.");
                            log_info("  [PASS] Handle creation, rights validation, and type check verified!");
                        }
                    }
                }
            }

            process_destroy(proc);
            kos_object_unref(&obj2.header);
        }
    }

    /* ------------------------------------------------------------
     * Test 3: Stale handle prevention (Anti-ABA)
     * ------------------------------------------------------------ */
    log_info("[handletest] 3. Stale Handle Prevention (Anti-ABA Verification)...");
    {
        struct process *proc = process_create("htest-proc3");
        if (proc == NULL || proc->handles == NULL) {
            log_error("  [FAIL] Failed to create test process for ABA test!");
            all_passed = false;
        } else {
            struct handletest_obj objA;
            struct handletest_obj objB;
            kos_object_init(&objA.header, KOS_OBJ_EVENT, NULL);
            kos_object_init(&objB.header, KOS_OBJ_EVENT, NULL);

            /* Create handle A */
            handle_t handleA = handle_create(proc, &objA.header, KOS_RIGHT_ALL);
            uint16_t idxA = KOS_HANDLE_INDEX(handleA);
            uint16_t genA = KOS_HANDLE_GEN(handleA);

            log_infof("  Created handle A 0x%08x at slot %u with generation %u",
                      handleA, (unsigned)idxA, (unsigned)genA);

            /* Close handle A */
            if (!handle_close(proc, handleA)) {
                log_error("  [FAIL] Failed to close handle A!");
                all_passed = false;
            } else {
                /* Attempt to close handle A second time (must fail) */
                if (handle_close(proc, handleA)) {
                    log_error("  [FAIL] Second handle_close succeeded on already closed handle!");
                    all_passed = false;
                } else {
                    /* Create handle B: will reuse slot idxA, but with incremented generation */
                    handle_t handleB = handle_create(proc, &objB.header, KOS_RIGHT_ALL);
                    uint16_t idxB = KOS_HANDLE_INDEX(handleB);
                    uint16_t genB = KOS_HANDLE_GEN(handleB);

                    log_infof("  Created handle B 0x%08x at slot %u with generation %u",
                              handleB, (unsigned)idxB, (unsigned)genB);

                    if (idxB != idxA) {
                        log_errorf("  [FAIL] Expected slot reuse %u, got %u", (unsigned)idxA, (unsigned)idxB);
                        all_passed = false;
                    } else if (genB != genA + 1) {
                        log_errorf("  [FAIL] Expected generation increment (%u -> %u), got %u",
                                   (unsigned)genA, (unsigned)(genA + 1), (unsigned)genB);
                        all_passed = false;
                    } else {
                        /* Attempt lookup using old stale handle A: MUST return NULL */
                        void *stale_lookup = handle_lookup(proc, handleA, KOS_RIGHT_NONE);
                        void *valid_lookup = handle_lookup(proc, handleB, KOS_RIGHT_NONE);

                        if (stale_lookup != NULL) {
                            log_error("  [FAIL] ABA vulnerability! Stale handle A accessed newly allocated object!");
                            all_passed = false;
                        } else if (valid_lookup != &objB.header) {
                            log_error("  [FAIL] Valid handle B lookup failed!");
                            all_passed = false;
                        } else {
                            log_info("  Stale handle A successfully rejected via generation tag mismatch.");
                            log_info("  Reallocated slot correctly serves handle B with generation + 1.");
                            log_info("  [PASS] Stale handle prevention (anti-ABA) verified!");
                        }

                        handle_close(proc, handleB);
                    }
                }
            }

            process_destroy(proc);
            kos_object_unref(&objA.header);
            kos_object_unref(&objB.header);
        }
    }

    /* ------------------------------------------------------------
     * Test 4: Handle duplication across processes & exit cleanup
     * ------------------------------------------------------------ */
    log_info("[handletest] 4. Handle Duplication & Process Exit Table Cleanup...");
    {
        struct process *p_src = process_create("htest-src");
        struct process *p_dst = process_create("htest-dst");

        if (p_src == NULL || p_dst == NULL || p_src->handles == NULL || p_dst->handles == NULL) {
            log_error("  [FAIL] Failed to create source/destination test processes!");
            all_passed = false;
        } else {
            struct handletest_obj shared_obj;
            shared_obj.magic = 0xCAFE9999;
            shared_obj.destructor_called = false;
            kos_object_init(&shared_obj.header, KOS_OBJ_FILE, handletest_destructor);

            /* Create handle in source process with READ, WRITE, DUPLICATE rights */
            handle_t h_src = handle_create(p_src, &shared_obj.header,
                                           KOS_RIGHT_READ | KOS_RIGHT_WRITE | KOS_RIGHT_DUPLICATE);
            if (h_src == KOS_INVALID_HANDLE) {
                log_error("  [FAIL] Failed to create handle in source process!");
                all_passed = false;
            } else {
                int32_t ref = kos_object_get_refcount(&shared_obj.header);
                if (ref != 2) {
                    log_errorf("  [FAIL] Expected refcount 2, got %d", ref);
                    all_passed = false;
                } else {
                    /* Attempt duplication with rights amplification (EXECUTE not held by src) */
                    handle_t h_amplified = handle_duplicate(p_src, p_dst, h_src, KOS_RIGHT_EXECUTE);
                    if (h_amplified != KOS_INVALID_HANDLE) {
                        log_error("  [FAIL] Rights amplification permitted during duplication!");
                        all_passed = false;
                    } else {
                        /* Duplicate with attenuated rights (READ only) */
                        handle_t h_dst = handle_duplicate(p_src, p_dst, h_src, KOS_RIGHT_READ);
                        if (h_dst == KOS_INVALID_HANDLE) {
                            log_error("  [FAIL] Failed to duplicate handle with attenuated rights!");
                            all_passed = false;
                        } else {
                            ref = kos_object_get_refcount(&shared_obj.header);
                            log_infof("  Duplicated handle: src=0x%08x -> dst=0x%08x, object refcount=%d",
                                      h_src, h_dst, ref);

                            if (ref != 3) {
                                log_errorf("  [FAIL] Expected refcount 3 after duplication, got %d", ref);
                                all_passed = false;
                            } else {
                                /* In dst process: READ must succeed, WRITE must fail */
                                void *dst_read = handle_lookup(p_dst, h_dst, KOS_RIGHT_READ);
                                void *dst_write = handle_lookup(p_dst, h_dst, KOS_RIGHT_WRITE);

                                if (dst_read != &shared_obj.header || dst_write != NULL) {
                                    log_error("  [FAIL] Destination handle rights incorrect after attenuation!");
                                    all_passed = false;
                                } else {
                                    /* Test automatic cleanup on source process destruction */
                                    process_destroy(p_src);
                                    p_src = NULL;

                                    ref = kos_object_get_refcount(&shared_obj.header);
                                    log_infof("  After source process destroyed: object refcount=%d, destructor=%d",
                                              ref, (int)shared_obj.destructor_called);

                                    if (ref != 2 || shared_obj.destructor_called) {
                                        log_errorf("  [FAIL] Incorrect refcount (%d) after src cleanup!", ref);
                                        all_passed = false;
                                    } else {
                                        /* Destination process can still access object */
                                        void *dst_after = handle_lookup(p_dst, h_dst, KOS_RIGHT_READ);
                                        if (dst_after != &shared_obj.header) {
                                            log_error("  [FAIL] Destination handle broken after src destruction!");
                                            all_passed = false;
                                        } else {
                                            /* Destroy destination process */
                                            process_destroy(p_dst);
                                            p_dst = NULL;

                                            ref = kos_object_get_refcount(&shared_obj.header);
                                            log_infof("  After dst process destroyed: object refcount=%d, destructor=%d",
                                                      ref, (int)shared_obj.destructor_called);

                                            if (ref != 1 || shared_obj.destructor_called) {
                                                log_errorf("  [FAIL] Incorrect refcount (%d) after dst cleanup!", ref);
                                                all_passed = false;
                                            } else {
                                                /* Release final baseline reference */
                                                kos_object_unref(&shared_obj.header);
                                                ref = kos_object_get_refcount(&shared_obj.header);

                                                if (ref != 0 || !shared_obj.destructor_called) {
                                                    log_errorf("  [FAIL] Final unref failed: ref=%d, destructor=%d",
                                                               ref, (int)shared_obj.destructor_called);
                                                    all_passed = false;
                                                } else {
                                                    log_info("  Both process handle tables cleanly destroyed on exit.");
                                                    log_info("  Object references properly released across process life cycles.");
                                                    log_info("  Final destructor called when last reference released.");
                                                    log_info("  [PASS] Handle duplication and automatic cleanup verified!");
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (p_src != NULL) process_destroy(p_src);
        if (p_dst != NULL) process_destroy(p_dst);
    }

    log_info("--------------------------------------------------");
    if (all_passed) {
        log_info(">>> [PASS] ALL KERNEL OBJECT & HANDLE TESTS PASSED! <<<");
    } else {
        log_error(">>> [FAIL] KERNEL OBJECT & HANDLE TESTS FAILED! <<<");
    }
    log_info("==================================================");
}

/*
 * ============================================================================
 * Diagnostic Command: waittest (K12.13, K12.16)
 * ============================================================================
 */

/* Test 1 Waiter */
struct waittest1_state {
    wait_queue_t *wq;
    volatile bool *flag;
    volatile bool woken;
};

static bool waittest_flag_cond(void *arg) {
    volatile bool *flag = (volatile bool *)arg;
    return *flag;
}

static void waittest1_worker(void *arg) {
    struct waittest1_state *st = (struct waittest1_state *)arg;
    wait_event(st->wq, waittest_flag_cond, (void *)st->flag);
    st->woken = true;
    thread_exit(0);
}

/* Test 2 Waiters */
struct waittest2_state {
    wait_queue_t *wq;
    volatile bool *flag;
    atomic_t woken_count;
};

static void waittest2_worker(void *arg) {
    struct waittest2_state *st = (struct waittest2_state *)arg;
    wait_event(st->wq, waittest_flag_cond, (void *)st->flag);
    atomic_inc(&st->woken_count);
    thread_exit(0);
}

/* Test 3 Condition (always false) */
static bool waittest_never_cond(void *arg) {
    (void)arg;
    return false;
}

/* Test 4 Event Worker */
struct waittest4_state {
    struct process *proc;
    handle_t event_handle;
    volatile bool woken;
};

static void waittest4_worker(void *arg) {
    struct waittest4_state *st = (struct waittest4_state *)arg;
    if (event_wait(st->proc, st->event_handle, 1000)) {
        st->woken = true;
    }
    thread_exit(0);
}

/* Test 5 Cleanup Object & Child Process */
struct waittest_cleanup_obj {
    struct kos_object_header header;
    volatile bool destructor_called;
};

static void waittest_cleanup_destructor(void *obj) {
    struct waittest_cleanup_obj *cobj = (struct waittest_cleanup_obj *)obj;
    if (cobj != NULL) {
        cobj->destructor_called = true;
    }
}

static void waittest5_child_worker(void *arg) {
    struct process *proc = (struct process *)arg;
    task_sleep(20);
    process_exit(proc, 77);
}

static void command_waittest(const char *arguments) {
    (void)arguments;
    log_info("==================================================");
    log_info("    KOS Wait Queue & Resource Cleanup Diagnostics ");
    log_info("==================================================");

    bool all_passed = true;

    /* ------------------------------------------------------------
     * Test 1: Single wakeup (wait_queue_wake_one)
     * ------------------------------------------------------------ */
    log_info("[waittest] 1. Single Waiter Wakeup (wait_queue_wake_one)...");
    {
        wait_queue_t wq1;
        wait_queue_init(&wq1);
        volatile bool flag1 = false;
        struct waittest1_state st1;
        st1.wq = &wq1;
        st1.flag = &flag1;
        st1.woken = false;

        preempt_disable();
        struct thread *t1 = thread_create(process_get_kernel(), waittest1_worker, &st1, "wq_single");
        preempt_enable();

        if (t1 == NULL) {
            log_error("  [FAIL] Failed to create worker thread for Test 1!");
            all_passed = false;
        } else {
            /* Wait until thread blocks on wq1 */
            uint64_t wait_start = timer_get_ticks();
            while (wq1.waiter_count == 0 && (timer_get_ticks() - wait_start) < timer_ms_to_ticks(500)) {
                task_yield();
            }

            log_infof("  Waiter queued: count=%u, thread state=%s, woken=%d",
                      wq1.waiter_count, thread_state_name(t1->state), (int)st1.woken);

            if (wq1.waiter_count != 1 || st1.woken) {
                log_error("  [FAIL] Waiter was not properly queued in BLOCKED state!");
                all_passed = false;
            } else {
                /* Signal condition and wake single waiter */
                flag1 = true;
                wait_queue_wake_one(&wq1);

                /* Wait for worker to complete */
                uint64_t wake_start = timer_get_ticks();
                while (!st1.woken && (timer_get_ticks() - wake_start) < timer_ms_to_ticks(500)) {
                    task_yield();
                }

                log_infof("  After wake_one: woken=%d, queue count=%u", (int)st1.woken, wq1.waiter_count);

                if (!st1.woken || wq1.waiter_count != 0) {
                    log_error("  [FAIL] Single waiter did not awaken or queue not empty!");
                    all_passed = false;
                } else {
                    log_info("  Worker cleanly awakened and queue count reached 0.");
                    log_info("  [PASS] Single wakeup (wait_queue_wake_one) verified!");
                }
            }
        }
    }

    /* ------------------------------------------------------------
     * Test 2: Broadcast wakeup (wait_queue_wake_all) with 3 waiters
     * ------------------------------------------------------------ */
    log_info("[waittest] 2. Broadcast Wakeup (wait_queue_wake_all)...");
    {
        wait_queue_t wq2;
        wait_queue_init(&wq2);
        volatile bool flag2 = false;
        struct waittest2_state st2;
        st2.wq = &wq2;
        st2.flag = &flag2;
        atomic_set(&st2.woken_count, 0);

        preempt_disable();
        struct thread *tw1 = thread_create(process_get_kernel(), waittest2_worker, &st2, "wq_bcast1");
        struct thread *tw2 = thread_create(process_get_kernel(), waittest2_worker, &st2, "wq_bcast2");
        struct thread *tw3 = thread_create(process_get_kernel(), waittest2_worker, &st2, "wq_bcast3");
        preempt_enable();

        if (tw1 == NULL || tw2 == NULL || tw3 == NULL) {
            log_error("  [FAIL] Failed to create 3 broadcast worker threads!");
            all_passed = false;
        } else {
            /* Wait until all 3 threads queue on wq2 */
            uint64_t wait_start = timer_get_ticks();
            while (wq2.waiter_count < 3 && (timer_get_ticks() - wait_start) < timer_ms_to_ticks(500)) {
                task_yield();
            }

            log_infof("  Queued waiters: count=%u (expected 3), woken=%d",
                      wq2.waiter_count, atomic_read(&st2.woken_count));

            if (wq2.waiter_count != 3 || atomic_read(&st2.woken_count) != 0) {
                log_error("  [FAIL] Not all 3 waiters queued properly!");
                all_passed = false;
            } else {
                /* Signal condition and broadcast wake all */
                flag2 = true;
                wait_queue_wake_all(&wq2);

                uint64_t wake_start = timer_get_ticks();
                while (atomic_read(&st2.woken_count) < 3 &&
                       (timer_get_ticks() - wake_start) < timer_ms_to_ticks(500)) {
                    task_yield();
                }

                int32_t final_woken = atomic_read(&st2.woken_count);
                log_infof("  After wake_all: woken_count=%d, queue count=%u",
                          final_woken, wq2.waiter_count);

                if (final_woken != 3 || wq2.waiter_count != 0) {
                    log_error("  [FAIL] Broadcast failed to wake all 3 threads or queue not drained!");
                    all_passed = false;
                } else {
                    log_info("  All 3 concurrent waiting threads awakened simultaneously.");
                    log_info("  [PASS] Broadcast wakeup (wait_queue_wake_all) verified!");
                }
            }
        }
    }

    /* ------------------------------------------------------------
     * Test 3: Timed wait (wait_event_timeout)
     * ------------------------------------------------------------ */
    log_info("[waittest] 3. Timed Wait & Expiration (wait_event_timeout)...");
    {
        wait_queue_t wq3;
        wait_queue_init(&wq3);

        /* Part A: Condition never signaled -> must time out after ~50ms */
        uint64_t t0 = timer_get_ticks();
        bool res_timeout = wait_event_timeout(&wq3, waittest_never_cond, NULL, 50);
        uint64_t t1 = timer_get_ticks();
        uint64_t elapsed_ms = timer_ticks_to_ms(t1 - t0);

        log_infof("  Timed wait (50ms requested): result=%d (expect 0), elapsed=%llu ms, wq_waiters=%u",
                  (int)res_timeout, (unsigned long long)elapsed_ms, wq3.waiter_count);

        if (res_timeout || elapsed_ms < 35 || wq3.waiter_count != 0) {
            log_error("  [FAIL] wait_event_timeout did not properly expire or queue not clean!");
            all_passed = false;
        } else {
            /* Part B: Condition already true -> must return true immediately */
            volatile bool already_true = true;
            uint64_t t2 = timer_get_ticks();
            bool res_immediate = wait_event_timeout(&wq3, waittest_flag_cond, (void *)&already_true, 200);
            uint64_t t3 = timer_get_ticks();
            uint64_t imm_elapsed = timer_ticks_to_ms(t3 - t2);

            log_infof("  Immediate true check: result=%d (expect 1), elapsed=%llu ms",
                      (int)res_immediate, (unsigned long long)imm_elapsed);

            if (!res_immediate || imm_elapsed > 10) {
                log_error("  [FAIL] wait_event_timeout did not return true immediately for satisfied condition!");
                all_passed = false;
            } else {
                log_info("  Timeout correctly expired without signal; satisfied condition returned immediately.");
                log_info("  [PASS] Timed wait (wait_event_timeout) verified!");
            }
        }
    }

    /* ------------------------------------------------------------
     * Test 4: Wait Queue Kernel Object (Event handles)
     * ------------------------------------------------------------ */
    log_info("[waittest] 4. Wait Queue Kernel Object & Event Handles...");
    {
        struct process *cur_proc = process_get_current();
        handle_t hev = event_create(cur_proc, true /* manual_reset */, false /* initial_state */);
        if (hev == KOS_INVALID_HANDLE) {
            log_error("  [FAIL] Failed to create event kernel object handle!");
            all_passed = false;
        } else {
            struct waittest4_state st4;
            st4.proc = cur_proc;
            st4.event_handle = hev;
            st4.woken = false;

            preempt_disable();
            struct thread *tev = thread_create(cur_proc, waittest4_worker, &st4, "wq_event");
            preempt_enable();

            if (tev == NULL) {
                log_error("  [FAIL] Failed to create event waiter thread!");
                all_passed = false;
            } else {
                /* Let worker begin waiting on event */
                task_sleep(10);

                if (st4.woken) {
                    log_error("  [FAIL] Event waiter woke up before signal!");
                    all_passed = false;
                } else {
                    /* Signal event */
                    bool sig_ok = event_signal(cur_proc, hev);
                    if (!sig_ok) {
                        log_error("  [FAIL] Failed to signal event object!");
                        all_passed = false;
                    } else {
                        /* Wait for worker to awaken */
                        uint64_t ev_start = timer_get_ticks();
                        while (!st4.woken && (timer_get_ticks() - ev_start) < timer_ms_to_ticks(500)) {
                            task_yield();
                        }

                        log_infof("  After event_signal: woken=%d", (int)st4.woken);

                        if (!st4.woken) {
                            log_error("  [FAIL] Event waiter was not awakened by event_signal!");
                            all_passed = false;
                        } else {
                            /* Reset event */
                            bool rst_ok = event_reset(cur_proc, hev);
                            bool now_sig = event_wait(cur_proc, hev, 0);
                            log_infof("  After event_reset: reset_ok=%d, signaled=%d",
                                      (int)rst_ok, (int)now_sig);

                            if (!rst_ok || now_sig) {
                                log_error("  [FAIL] Event reset failed!");
                                all_passed = false;
                            } else {
                                log_info("  Event handle creation, waiting, signaling and reset verified.");
                                log_info("  [PASS] Kernel Object Event integration verified!");
                            }
                        }
                    }
                }
            }

            handle_close(cur_proc, hev);
        }
    }

    /* ------------------------------------------------------------
     * Test 5: Process Wait & Automatic Resource Cleanup
     * ------------------------------------------------------------ */
    log_info("[waittest] 5. Process Wait & Automatic Resource Cleanup (process_wait)...");
    {
        struct process *child = process_create("proc_child_cleanup");
        if (child == NULL) {
            log_error("  [FAIL] Failed to create child process!");
            all_passed = false;
        } else {
            /* Create a tracked kernel object and place a handle in child */
            struct waittest_cleanup_obj cleanup_obj;
            cleanup_obj.destructor_called = false;
            kos_object_init(&cleanup_obj.header, KOS_OBJ_MUTEX, waittest_cleanup_destructor);

            handle_t h_child = handle_create(child, &cleanup_obj.header, KOS_RIGHT_ALL);
            int32_t ref_with_handle = kos_object_get_refcount(&cleanup_obj.header);
            log_infof("  Child process PID %llu created, handle 0x%08x created, object refcount=%d",
                      (unsigned long long)child->pid, h_child, ref_with_handle);

            if (h_child == KOS_INVALID_HANDLE || ref_with_handle != 2) {
                log_error("  [FAIL] Failed to associate handle with child process!");
                all_passed = false;
            } else {
                /* Spawn child worker thread */
                preempt_disable();
                struct thread *tchild = thread_create(child, waittest5_child_worker, child, "child_worker");
                preempt_enable();

                if (tchild == NULL) {
                    log_error("  [FAIL] Failed to spawn child thread!");
                    all_passed = false;
                } else {
                    /* Parent waits on child exit via wait queue */
                    int exit_code = -1;
                    uint64_t wait_start = timer_get_ticks();
                    bool wait_success = process_wait(child, &exit_code, 2000);
                    uint64_t wait_time = timer_ticks_to_ms(timer_get_ticks() - wait_start);

                    log_infof("  process_wait returned %d in %llu ms: exit_code=%d (expect 77), child state=%s",
                              (int)wait_success, (unsigned long long)wait_time, exit_code,
                              process_state_name(child->state));

                    if (!wait_success || exit_code != 77) {
                        log_errorf("  [FAIL] process_wait failed or exit code mismatch (got %d)", exit_code);
                        all_passed = false;
                    } else if (child->state != PROCESS_STATE_ZOMBIE && child->state != PROCESS_STATE_DEAD) {
                        log_error("  [FAIL] Child process did not transition to ZOMBIE/DEAD!");
                        all_passed = false;
                    } else {
                        /* Check that child handle table was destroyed and object unreffed */
                        int32_t ref_after_exit = kos_object_get_refcount(&cleanup_obj.header);
                        log_infof("  After process exit: object refcount=%d, destructor_called=%d",
                                  ref_after_exit, (int)cleanup_obj.destructor_called);

                        if (ref_after_exit != 1 || cleanup_obj.destructor_called) {
                            log_error("  [FAIL] Child handle table was not automatically destroyed on exit!");
                            all_passed = false;
                        } else {
                            /* Release parent's baseline reference */
                            kos_object_unref(&cleanup_obj.header);
                            if (!cleanup_obj.destructor_called) {
                                log_error("  [FAIL] Destructor not called after parent unref!");
                                all_passed = false;
                            } else {
                                log_info("  Child handle table cleaned up, object refcount decremented.");
                                log_info("  Parent successfully synchronized with child exit via exit_wq.");
                                log_info("  Exit code 77 cleanly delivered to parent.");
                                log_info("  [PASS] Process termination resource cleanup verified!");
                            }
                        }
                    }
                }
            }

            /* Clean up child slot if not already dead */
            if (child->state != PROCESS_STATE_DEAD) {
                process_destroy(child);
            }
        }
    }

    log_info("--------------------------------------------------");
    if (all_passed) {
        log_info(">>> [PASS] ALL WAIT QUEUE & RESOURCE CLEANUP TESTS PASSED! <<<");
    } else {
        log_error(">>> [FAIL] WAIT QUEUE & RESOURCE CLEANUP TESTS FAILED! <<<");
    }
    log_info("==================================================");
}

static const struct command_entry command_registry[] = {
    { "help", command_help },
    { "clear", command_clear },
    { "meminfo", command_meminfo },
    { "uptime", command_uptime },
    { "sysinfo", command_sysinfo },
    { "netinfo", command_netinfo },
    { "ifconfig", command_ifconfig },
    { "arp", command_arp },
    { "arping", command_arping },
    { "ping", command_ping },
    { "dhcp", command_dhcp },
    { "udptest", command_udptest },
    { "tcptest", command_tcptest },
    { "nslookup", command_nslookup },
    { "curl", command_curl },
    { "httpd", command_httpd },
    { "netstat", command_netstat },
    { "echo", command_echo },
    { "reboot", command_reboot },
    { "irqinfo", command_irqinfo },
    { "kbdinfo", command_kbdinfo },
    { "drivers", command_drivers },
    { "lspci", command_lspci },
    { "taskinfo", command_taskinfo },
    { "tasktest", command_tasktest },
    { "taskdemo", command_taskdemo },
    { "vol", command_vol },
    { "ls", command_ls },
    { "cat", command_cat },
    { "elfinfo", command_elfinfo },
};

bool kernel_command_execute(const char *command) {
    if (command == 0) {
        return false;
    }
    while (*command == ' ') {
        ++command;
    }
    const char *name = command;
    while (*command != '\0' && *command != ' ') {
        ++command;
    }
    uint64_t name_length = (uint64_t)(command - name);
    while (*command == ' ') {
        ++command;
    }

    for (uint64_t index = 0; index < sizeof(command_registry) / sizeof(command_registry[0]); ++index) {
        const char *registered_name = command_registry[index].name;
        uint64_t registered_length = 0;
        while (registered_name[registered_length] != '\0') {
            ++registered_length;
        }
        if (registered_length == name_length) {
            bool matches = true;
            for (uint64_t character = 0; character < name_length; ++character) {
                if (name[character] != registered_name[character]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                command_registry[index].handler(command);
                return true;
            }
        }
    }
    return false;
}
