#ifndef KOS_TLS_H
#define KOS_TLS_H

#include <stdbool.h>
#include <stdint.h>

enum net_tls_error {
    NET_TLS_ERROR_NONE,
    NET_TLS_ERROR_UNSUPPORTED_CIPHER,
    NET_TLS_ERROR_INVALID_CERTIFICATE_CHAIN,
    NET_TLS_ERROR_HOSTNAME_MISMATCH,
    NET_TLS_ERROR_HANDSHAKE_TIMEOUT,
    NET_TLS_ERROR_UNSUPPORTED_PROTOCOL,
    NET_TLS_ERROR_NO_ENTROPY,
    NET_TLS_ERROR_NOT_IMPLEMENTED,
};

enum net_tls_state {
    NET_TLS_STATE_IDLE,
    NET_TLS_STATE_CLIENT_HELLO,
    NET_TLS_STATE_SERVER_HELLO,
    NET_TLS_STATE_CERTIFICATE,
    NET_TLS_STATE_FINISHED,
    NET_TLS_STATE_ESTABLISHED,
    NET_TLS_STATE_FAILED,
};

struct net_tls_session {
    enum net_tls_state state;
    enum net_tls_error error;
    bool insecure;
    char server_name[64];
    uint16_t cipher_suite;
};

void net_tls_session_initialize(struct net_tls_session *session, const char *server_name, bool insecure);
enum net_tls_error net_tls_client_handshake(struct net_tls_session *session, int tcp_socket,
    uint64_t timeout_ticks);
enum net_tls_error net_tls_last_error(void);
const char *net_tls_error_string(enum net_tls_error error);
const char *net_tls_state_string(enum net_tls_state state);

#endif
