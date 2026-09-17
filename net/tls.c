#include <stdbool.h>
#include <stdint.h>

#include <kos/crypto.h>
#include <kos/tls.h>

static enum net_tls_error last_error;

static uint64_t string_length(const char *text) {
    uint64_t length = 0;
    if (text == 0) return 0;
    while (text[length] != '\0') ++length;
    return length;
}

void net_tls_session_initialize(struct net_tls_session *session, const char *server_name, bool insecure) {
    if (session == 0) return;
    *session = (struct net_tls_session){ .state = NET_TLS_STATE_IDLE, .insecure = insecure };
    if (server_name != 0) {
        uint64_t length = string_length(server_name);
        if (length >= sizeof(session->server_name)) length = sizeof(session->server_name) - 1;
        for (uint64_t i = 0; i < length; ++i) session->server_name[i] = server_name[i];
        session->server_name[length] = '\0';
    }
    last_error = NET_TLS_ERROR_NONE;
}

enum net_tls_error net_tls_client_handshake(struct net_tls_session *session, int tcp_socket,
        uint64_t timeout_ticks) {
    (void)tcp_socket;
    (void)timeout_ticks;
    if (session == 0) { last_error = NET_TLS_ERROR_UNSUPPORTED_PROTOCOL; return last_error; }
    session->state = NET_TLS_STATE_CLIENT_HELLO;
    session->error = NET_TLS_ERROR_NONE;
    uint8_t random_seed[32];
    if (!kos_crypto_self_test()) {
        session->state = NET_TLS_STATE_FAILED; session->error = NET_TLS_ERROR_UNSUPPORTED_CIPHER;
        last_error = session->error; return last_error;
    }
    if (!kos_entropy_fill(random_seed, sizeof(random_seed))) {
        session->state = NET_TLS_STATE_FAILED; session->error = NET_TLS_ERROR_NO_ENTROPY;
        last_error = session->error; return last_error;
    }
    /* The record encoder, X.509 parser and certificate verifier are intentionally
     * not bypassed here. Returning a distinct status prevents curl from sending
     * plaintext on a socket that the caller believed to be TLS. */
    session->state = NET_TLS_STATE_FAILED;
    session->error = NET_TLS_ERROR_NOT_IMPLEMENTED;
    last_error = session->error;
    return last_error;
}

enum net_tls_error net_tls_last_error(void) { return last_error; }

const char *net_tls_error_string(enum net_tls_error error) {
    switch (error) {
        case NET_TLS_ERROR_NONE: return "none";
        case NET_TLS_ERROR_UNSUPPORTED_CIPHER: return "unsupported cipher suite";
        case NET_TLS_ERROR_INVALID_CERTIFICATE_CHAIN: return "invalid certificate chain";
        case NET_TLS_ERROR_HOSTNAME_MISMATCH: return "hostname mismatch";
        case NET_TLS_ERROR_HANDSHAKE_TIMEOUT: return "handshake timed out";
        case NET_TLS_ERROR_UNSUPPORTED_PROTOCOL: return "server requires unsupported protocol";
        case NET_TLS_ERROR_NO_ENTROPY: return "secure entropy unavailable";
        case NET_TLS_ERROR_NOT_IMPLEMENTED: return "TLS 1.3 handshake not implemented";
        default: return "unknown TLS error";
    }
}

const char *net_tls_state_string(enum net_tls_state state) {
    switch (state) {
        case NET_TLS_STATE_IDLE: return "IDLE";
        case NET_TLS_STATE_CLIENT_HELLO: return "CLIENT_HELLO";
        case NET_TLS_STATE_SERVER_HELLO: return "SERVER_HELLO";
        case NET_TLS_STATE_CERTIFICATE: return "CERTIFICATE";
        case NET_TLS_STATE_FINISHED: return "FINISHED";
        case NET_TLS_STATE_ESTABLISHED: return "ESTABLISHED";
        case NET_TLS_STATE_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}
