#include <stdbool.h>
#include <stdint.h>

#include <kos/cpu.h>
#include <kos/log.h>
#include <kos/net.h>
#include <kos/tls.h>
#include <kos/timer.h>

enum {
    HTTP_DEFAULT_PORT = 80,
    HTTPS_DEFAULT_PORT = 443,
    HTTP_MAX_HOST = 64,
    HTTP_MAX_PATH = 256,
};

struct parsed_url {
    char host[HTTP_MAX_HOST];
    uint16_t port;
    char path[HTTP_MAX_PATH];
    bool secure;
};

static enum net_http_error last_error;
static enum net_tls_error last_tls_error;

static uint64_t str_length(const char *s) {
    if (s == 0) {
        return 0;
    }
    uint64_t len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    return len;
}

static char to_lower_char(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static bool str_starts_with_case_insensitive(const char *str, const char *prefix) {
    if (str == 0 || prefix == 0) {
        return false;
    }
    while (*prefix != '\0') {
        if (to_lower_char(*str) != to_lower_char(*prefix)) {
            return false;
        }
        ++str;
        ++prefix;
    }
    return true;
}

static bool parse_ipv4_literal(const char *text, uint32_t *address) {
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

static bool parse_http_url(const char *url, struct parsed_url *parsed) {
    if (url == 0 || parsed == 0 || *url == '\0') {
        return false;
    }

    const char *p = url;
    if (str_starts_with_case_insensitive(p, "http://")) {
        p += 7;
    }
    else if (str_starts_with_case_insensitive(p, "https://")) {
        parsed->secure = true;
        p += 8;
    }

    uint64_t h_idx = 0;
    while (*p != '\0' && *p != ':' && *p != '/' && h_idx + 1 < sizeof(parsed->host)) {
        parsed->host[h_idx++] = *p++;
    }
    parsed->host[h_idx] = '\0';
    if (h_idx == 0) {
        return false;
    }

    parsed->port = parsed->secure ? HTTPS_DEFAULT_PORT : HTTP_DEFAULT_PORT;
    if (*p == ':') {
        ++p;
        uint32_t port_val = 0;
        while (*p >= '0' && *p <= '9') {
            port_val = port_val * 10 + (uint32_t)(*p++ - '0');
            if (port_val > 65535) {
                return false;
            }
        }
        if (port_val == 0) {
            return false;
        }
        parsed->port = (uint16_t)port_val;
    }

    if (*p == '/') {
        uint64_t path_idx = 0;
        while (*p != '\0' && path_idx + 1 < sizeof(parsed->path)) {
            parsed->path[path_idx++] = *p++;
        }
        parsed->path[path_idx] = '\0';
    }
    else {
        parsed->path[0] = '/';
        parsed->path[1] = '\0';
    }

    return true;
}

static const char *find_substring_case_insensitive(const char *haystack, const char *needle) {
    if (haystack == 0 || needle == 0 || *needle == '\0') {
        return 0;
    }
    uint64_t needle_len = str_length(needle);
    for (uint64_t i = 0; haystack[i] != '\0'; ++i) {
        bool match = true;
        for (uint64_t j = 0; j < needle_len; ++j) {
            if (haystack[i + j] == '\0' || to_lower_char(haystack[i + j]) != to_lower_char(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return &haystack[i];
        }
    }
    return 0;
}

static bool decode_chunk_size(const char *text, uint64_t available, uint64_t *size,
        uint64_t *consumed) {
    if (text == 0 || size == 0 || consumed == 0 || available == 0) return false;
    uint64_t value = 0;
    uint64_t index = 0;
    bool has_digit = false;
    while (index < available && text[index] != '\r' && text[index] != '\n'
        && text[index] != ';') {
        char character = text[index];
        uint8_t digit;
        if (character >= '0' && character <= '9') digit = (uint8_t)(character - '0');
        else if (character >= 'a' && character <= 'f') digit = (uint8_t)(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F') digit = (uint8_t)(character - 'A' + 10);
        else return false;
        if (value > (UINT64_MAX - digit) / 16u) return false;
        value = value * 16u + digit;
        has_digit = true;
        ++index;
    }
    if (!has_digit) return false;
    /* A chunk extension is permitted, but must end in a complete CRLF line. */
    while (index < available && text[index] != '\n') ++index;
    if (index == available || index == 0 || text[index - 1] != '\r') return false;
    *size = value;
    *consumed = index + 1;
    return true;
}

static bool unchunk_http_body(char *body_start, uint64_t body_len, uint64_t *out_body_size,
        bool allow_truncated, bool *out_complete) {
    if (body_start == 0 || out_body_size == 0 || out_complete == 0) return false;
    char *read_ptr = body_start;
    char *write_ptr = body_start;
    uint64_t remaining = body_len;
    *out_body_size = 0;
    *out_complete = false;

    while (remaining > 0) {
        uint64_t header_consumed = 0;
        uint64_t chunk_size = 0;
        if (!decode_chunk_size(read_ptr, remaining, &chunk_size, &header_consumed)
            || header_consumed > remaining) {
            if (allow_truncated) {
                *write_ptr = '\0';
                *out_body_size = (uint64_t)(write_ptr - body_start);
            }
            return allow_truncated;
        }
        read_ptr += header_consumed;
        remaining -= header_consumed;

        if (chunk_size == 0) {
            *write_ptr = '\0';
            *out_body_size = (uint64_t)(write_ptr - body_start);
            *out_complete = true;
            return true;
        }
        if (chunk_size > remaining) {
            if (!allow_truncated) return false;
            for (uint64_t i = 0; i < remaining; ++i) *write_ptr++ = *read_ptr++;
            *write_ptr = '\0';
            *out_body_size = (uint64_t)(write_ptr - body_start);
            return true;
        }
        for (uint64_t i = 0; i < chunk_size; ++i) {
            *write_ptr++ = *read_ptr++;
        }
        remaining -= chunk_size;
        if (remaining < 2) {
            if (!allow_truncated) return false;
            *write_ptr = '\0';
            *out_body_size = (uint64_t)(write_ptr - body_start);
            return true;
        }
        if (read_ptr[0] != '\r' || read_ptr[1] != '\n') return false;
        read_ptr += 2;
        remaining -= 2;
    }
    *write_ptr = '\0';
    *out_body_size = (uint64_t)(write_ptr - body_start);
    return allow_truncated;
}

static bool execute_http_request(const struct parsed_url *parsed, char *response_buffer,
        uint64_t capacity, uint64_t *out_size, char *out_redirect_location,
        uint64_t redirect_capacity, uint16_t *out_status_code) {
    uint32_t ip_address = 0;
    if (!parse_ipv4_literal(parsed->host, &ip_address)) {
        if (!net_dns_resolve(parsed->host, &ip_address, timer_frequency_hz() * 5)) {
            last_error = NET_HTTP_ERROR_DNS;
            return false;
        }
    }

    int sock = tcp_connect(ip_address, parsed->port);
    if (sock < 0) {
        last_error = NET_HTTP_ERROR_TCP;
        return false;
    }

    if (parsed->secure) {
        struct net_tls_session tls_session;
        net_tls_session_initialize(&tls_session, parsed->host, false);
        last_tls_error = net_tls_client_handshake(&tls_session, sock, timer_frequency_hz() * 10);
        if (last_tls_error != NET_TLS_ERROR_NONE) {
            tcp_close(sock);
            last_error = NET_HTTP_ERROR_TLS_UNSUPPORTED;
            return false;
        }
    }

    /* Build HTTP/1.1 request */
    char req[512];
    uint64_t r_pos = 0;
    const char *part1 = "GET ";
    for (uint64_t i = 0; part1[i] != '\0'; ++i) req[r_pos++] = part1[i];
    for (uint64_t i = 0; parsed->path[i] != '\0'; ++i) req[r_pos++] = parsed->path[i];
    const char *part2 = " HTTP/1.1\r\nHost: ";
    for (uint64_t i = 0; part2[i] != '\0'; ++i) req[r_pos++] = part2[i];
    for (uint64_t i = 0; parsed->host[i] != '\0'; ++i) req[r_pos++] = parsed->host[i];
    uint16_t default_port = parsed->secure ? HTTPS_DEFAULT_PORT : HTTP_DEFAULT_PORT;
    if (parsed->port != default_port) {
        req[r_pos++] = ':';
        uint32_t p_val = parsed->port;
        uint32_t p_digits = 0;
        char tmp[8];
        do {
            tmp[p_digits++] = (char)('0' + (p_val % 10));
            p_val /= 10;
        } while (p_val > 0);
        for (uint32_t d = 0; d < p_digits; ++d) {
            req[r_pos++] = tmp[p_digits - 1 - d];
        }
    }
    const char *part3 = "\r\nUser-Agent: KOS-curl/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    for (uint64_t i = 0; part3[i] != '\0'; ++i) req[r_pos++] = part3[i];
    req[r_pos] = '\0';

    int sent = tcp_send(sock, (const uint8_t *)req, r_pos);
    if (sent <= 0) {
        tcp_close(sock);
        last_error = NET_HTTP_ERROR_TCP;
        return false;
    }

    /* Receive full response */
    uint64_t total_read = 0;
    while (total_read + 1 < capacity) {
        int r = tcp_receive(sock, (uint8_t *)&response_buffer[total_read], capacity - total_read - 1);
        if (r <= 0) {
            break;
        }
        total_read += (uint64_t)r;
        response_buffer[total_read] = '\0';

        /* Early completion check if full payload has arrived */
        const char *hdr_end = find_substring_case_insensitive(response_buffer, "\r\n\r\n");
        if (hdr_end != 0) {
            uint64_t body_offset = (uint64_t)(hdr_end + 4 - response_buffer);
            uint64_t cur_body_len = total_read >= body_offset ? total_read - body_offset : 0;

            const char *cl = find_substring_case_insensitive(response_buffer, "Content-Length:");
            if (cl != 0 && cl < hdr_end) {
                while (*cl != ':') ++cl;
                ++cl;
                while (*cl == ' ' || *cl == '\t') ++cl;
                uint64_t cl_val = 0;
                while (*cl >= '0' && *cl <= '9') {
                    cl_val = cl_val * 10 + (uint64_t)(*cl++ - '0');
                }
                if (cur_body_len >= cl_val) {
                    break;
                }
            }

            const char *te = find_substring_case_insensitive(response_buffer, "Transfer-Encoding:");
            if (te != 0 && te < hdr_end) {
                if (find_substring_case_insensitive(hdr_end, "\r\n0\r\n\r\n") != 0
                    || find_substring_case_insensitive(hdr_end, "0\r\n\r\n") == hdr_end + 4) {
                    break;
                }
            }
        }
    }
    response_buffer[total_read] = '\0';
    tcp_close(sock);

    if (total_read == 0) {
        last_error = NET_HTTP_ERROR_TCP;
        return false;
    }
    *out_size = total_read;

    /* Parse Status Code */
    uint16_t status_code = 0;
    if (str_starts_with_case_insensitive(response_buffer, "HTTP/")) {
        const char *sp = response_buffer;
        while (*sp != '\0' && *sp != ' ') {
            ++sp;
        }
        if (*sp == ' ') {
            ++sp;
            while (*sp >= '0' && *sp <= '9') {
                status_code = status_code * 10 + (uint16_t)(*sp++ - '0');
            }
        }
    }
    *out_status_code = status_code;
    if (status_code == 0) {
        last_error = NET_HTTP_ERROR_RESPONSE_MALFORMED;
        return false;
    }

    /* Check Location header on redirect status */
    if (status_code == 301 || status_code == 302 || status_code == 303
        || status_code == 307 || status_code == 308) {
        const char *loc = find_substring_case_insensitive(response_buffer, "\r\nLocation:");
        if (loc == 0) {
            loc = find_substring_case_insensitive(response_buffer, "\nLocation:");
        }
        if (loc != 0) {
            while (*loc != ':') {
                ++loc;
            }
            ++loc;
            while (*loc == ' ' || *loc == '\t') {
                ++loc;
            }
            uint64_t l_idx = 0;
            while (*loc != '\r' && *loc != '\n' && *loc != '\0' && l_idx + 1 < redirect_capacity) {
                out_redirect_location[l_idx++] = *loc++;
            }
            out_redirect_location[l_idx] = '\0';
        }
    }

    bool response_limited = total_read + 1 == capacity;

    /* If chunked transfer encoding, decode body. A response that fills the
       bounded buffer may end halfway through a chunk; preserve all complete
       bytes and report a controlled truncation instead of misdiagnosing it as
       a malformed HTTP response. */
    const char *te = find_substring_case_insensitive(response_buffer, "Transfer-Encoding:");
    if (te != 0) {
        const char *chk = find_substring_case_insensitive(te, "chunked");
        const char *hdr_end = find_substring_case_insensitive(response_buffer, "\r\n\r\n");
        if (chk != 0 && hdr_end != 0 && chk < hdr_end) {
            char *body_start = (char *)(hdr_end + 4);
            uint64_t body_len = (uint64_t)(&response_buffer[total_read] - body_start);
            uint64_t decoded_body_size;
            bool chunk_complete = false;
            if (!unchunk_http_body(body_start, body_len, &decoded_body_size,
                    response_limited, &chunk_complete)) {
                last_error = NET_HTTP_ERROR_RESPONSE_MALFORMED;
                return false;
            }
            *out_size = (uint64_t)(body_start - response_buffer) + decoded_body_size;
            if (!chunk_complete) response_limited = true;
        }
    }

    last_error = response_limited ? NET_HTTP_ERROR_RESPONSE_TOO_LARGE : NET_HTTP_ERROR_NONE;
    return true;
}

static bool url_has_scheme(const char *url) {
    return str_starts_with_case_insensitive(url, "http://")
        || str_starts_with_case_insensitive(url, "https://");
}

static bool copy_url_with_scheme(char destination[512], const char *scheme, const char *url) {
    uint64_t position = 0;
    for (uint64_t index = 0; scheme[index] != '\0'; ++index) {
        if (position + 1 >= 512) return false;
        destination[position++] = scheme[index];
    }
    for (uint64_t index = 0; url[index] != '\0'; ++index) {
        if (position + 1 >= 512) return false;
        destination[position++] = url[index];
    }
    destination[position] = '\0';
    return true;
}

static bool perform_http_get(const char *url, bool follow_redirects, char *response_buffer,
        uint64_t capacity, uint64_t *out_size, struct net_http_response_info *info) {
    char current_url[512];
    uint64_t u_len = str_length(url);
    if (u_len >= sizeof(current_url)) {
        last_error = NET_HTTP_ERROR_URL_INVALID;
        return false;
    }
    for (uint64_t i = 0; i <= u_len; ++i) current_url[i] = url[i];

    for (uint32_t redirect = 0; redirect < NET_HTTP_MAX_REDIRECTS; ++redirect) {
        struct parsed_url parsed = {0};
        if (!parse_http_url(current_url, &parsed)) {
            last_error = NET_HTTP_ERROR_URL_INVALID;
            return false;
        }
        char redirect_location[512] = { 0 };
        uint16_t status_code = 0;
        if (!execute_http_request(&parsed, response_buffer, capacity, out_size,
                redirect_location, sizeof(redirect_location), &status_code)) return false;
        if (info != 0) {
            *info = (struct net_http_response_info){ .status_code = status_code,
                .chunked = find_substring_case_insensitive(response_buffer,
                    "Transfer-Encoding: chunked") != 0,
                .body_truncated = last_error == NET_HTTP_ERROR_RESPONSE_TOO_LARGE };
        }
        bool is_redirect = status_code == 301 || status_code == 302 || status_code == 303
            || status_code == 307 || status_code == 308;
        if (!is_redirect || !follow_redirects || redirect_location[0] == '\0') return true;
        if (redirect_location[0] == '/') {
            uint64_t position = 0;
            const char *scheme = parsed.secure ? "https://" : "http://";
            for (uint64_t index = 0; scheme[index] != '\0'; ++index) current_url[position++] = scheme[index];
            for (uint64_t index = 0; parsed.host[index] != '\0'; ++index) current_url[position++] = parsed.host[index];
            uint16_t default_port = parsed.secure ? HTTPS_DEFAULT_PORT : HTTP_DEFAULT_PORT;
            if (parsed.port != default_port) return false;
            for (uint64_t index = 0; redirect_location[index] != '\0'; ++index) {
                if (position + 1 >= sizeof(current_url)) { last_error = NET_HTTP_ERROR_URL_INVALID; return false; }
                current_url[position++] = redirect_location[index];
            }
            current_url[position] = '\0';
        }
        else {
            uint64_t length = str_length(redirect_location);
            if (length >= sizeof(current_url)) { last_error = NET_HTTP_ERROR_URL_INVALID; return false; }
            for (uint64_t index = 0; index <= length; ++index) current_url[index] = redirect_location[index];
        }
    }
    last_error = NET_HTTP_ERROR_REDIRECT_LIMIT;
    return false;
}

bool net_http_get_ex(const char *url, bool follow_redirects, char *response_buffer,
        uint64_t capacity, uint64_t *out_size, struct net_http_response_info *info) {
    if (url == 0 || response_buffer == 0 || capacity == 0 || out_size == 0) {
        last_error = NET_HTTP_ERROR_URL_INVALID;
        return false;
    }
    last_error = NET_HTTP_ERROR_NONE;
    last_tls_error = NET_TLS_ERROR_NONE;
    if (url_has_scheme(url)) return perform_http_get(url, follow_redirects, response_buffer,
        capacity, out_size, info);
    /* Bare host names intentionally use plain HTTP for predictable behavior.
     * This avoids silently downgrading a user-requested HTTPS URL while still
     * making `curl example.com` useful before the TLS client is complete. Use
     * an explicit https:// URL (or the shell's --https flag) to require TLS. */
    char http_url[512];
    if (!copy_url_with_scheme(http_url, "http://", url)) {
        last_error = NET_HTTP_ERROR_URL_INVALID;
        return false;
    }
    return perform_http_get(http_url, follow_redirects, response_buffer, capacity, out_size, info);
}

bool net_http_get(const char *url, char *response_buffer, uint64_t capacity, uint64_t *out_size) {
    return net_http_get_ex(url, true, response_buffer, capacity, out_size, 0);
}

enum net_http_error net_http_last_error(void) { return last_error; }
enum net_tls_error net_http_last_tls_error(void) { return last_tls_error; }
