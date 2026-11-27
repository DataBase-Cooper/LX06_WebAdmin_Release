#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include "ws.h"
#include "ws_frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * LX06 ships the OpenSSL 1.0 ABI; builds may use target 1.0 or newer headers.
 * Keep this ABI bridge target-only: the host test build must exercise its native
 * OpenSSL API, but the armhf artifact must not import 1.1+/3.x symbols.
 */
#ifdef XZ_OPENSSL_1_0_COMPAT
#undef SSLv23_client_method
#undef SSL_get_peer_certificate
#undef SSL_library_init
#undef SSL_load_error_strings
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
/* Old target headers already provide _STACK and typed SAN accessors. */
#undef sk_num
#undef sk_value
#undef sk_GENERAL_NAME_num
#undef sk_GENERAL_NAME_value
extern const SSL_METHOD *SSLv23_client_method(void);
extern X509 *SSL_get_peer_certificate(const SSL *ssl);
extern int sk_num(const OPENSSL_STACK *stack);
extern void *sk_value(const OPENSSL_STACK *stack, int index);
#define sk_GENERAL_NAME_num(stack) sk_num((const OPENSSL_STACK *)(stack))
#define sk_GENERAL_NAME_value(stack, index) \
    ((GENERAL_NAME *)sk_value((const OPENSSL_STACK *)(stack), (index)))
#endif

extern unsigned char *xz_openssl10_asn1_string_data(ASN1_STRING *value)
    __asm__("ASN1_STRING_data");
static const unsigned char *xz_asn1_data(const ASN1_STRING *value) {
    return xz_openssl10_asn1_string_data((ASN1_STRING *)value);
}

static void tls_library_init(void) {
    extern int SSL_library_init(void);
    extern void SSL_load_error_strings(void);
    (void)SSL_library_init();
    SSL_load_error_strings();
}
#else
static const unsigned char *xz_asn1_data(const ASN1_STRING *value) {
    return ASN1_STRING_get0_data(value);
}

static void tls_library_init(void) {}
#endif

#define WS_HEADER_CAP 8192
#define WS_FRAME_CAP (DC_XZ_WS_MAX_MESSAGE + 14u)

static int has_line_break(const char *s) {
    return s && (strchr(s, '\r') || strchr(s, '\n'));
}

static int parse_url(dc_xz_ws *ws, const char *url) {
    const char *authority;
    int tls;
    if (strncmp(url, "wss://", 6) == 0) { tls = 1; authority = url + 6; }
    else if (strncmp(url, "ws://", 5) == 0) { tls = 0; authority = url + 5; }
    else return -1;

    const char *end = strpbrk(authority, "/?#");
    if (!end) end = authority + strlen(authority);
    if (end == authority || (size_t)(end - authority) >= 300) return -1;
    char auth[300];
    memcpy(auth, authority, (size_t)(end - authority));
    auth[end - authority] = '\0';
    if (strchr(auth, '@')) return -1;

    char *port = NULL;
    if (auth[0] == '[') {
        char *close = strchr(auth, ']');
        if (!close) return -1;
        *close = '\0';
        if (strlen(auth + 1) >= sizeof(ws->host)) return -1;
        strcpy(ws->host, auth + 1);
        if (close[1] == ':') port = close + 2;
        else if (close[1] != '\0') return -1;
    } else {
        char *colon = strrchr(auth, ':');
        if (colon && strchr(auth, ':') == colon) { *colon = '\0'; port = colon + 1; }
        if (strlen(auth) >= sizeof(ws->host)) return -1;
        strcpy(ws->host, auth);
    }
    if (!ws->host[0]) return -1;
    const char *default_port = tls ? "443" : "80";
    if (port) {
        if (!*port || strlen(port) >= sizeof(ws->port)) return -1;
        for (const char *p = port; *p; ++p) if (*p < '0' || *p > '9') return -1;
        strcpy(ws->port, port);
    } else strcpy(ws->port, default_port);

    if (*end == '#') return -1;
    if (*end == '?') {
        if (snprintf(ws->path, sizeof(ws->path), "/%s", end) >= (int)sizeof(ws->path)) return -1;
    } else {
        if (strlen(end) >= sizeof(ws->path)) return -1;
        strcpy(ws->path, *end ? end : "/");
    }
    ws->tls = tls;
    return 0;
}

static int socket_connect(const char *host, const char *port, int timeout_ms) {
    struct addrinfo hints, *list = NULL, *it;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, port, &hints, &list) != 0) return -1;
    int fd = -1;
    for (it = list; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}

static int write_all(dc_xz_ws *ws, const unsigned char *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        int n;
        if (ws->tls) n = SSL_write((SSL *)ws->ssl, data + offset,
                                   (int)((length - offset) > 16384 ? 16384 : length - offset));
        else n = (int)send(ws->fd, data + offset, length - offset, 0);
        if (n <= 0) return -1;
        offset += (size_t)n;
    }
    return 0;
}

static int read_all(dc_xz_ws *ws, unsigned char *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        int n;
        if (ws->tls) n = SSL_read((SSL *)ws->ssl, data + offset,
                                  (int)((length - offset) > 16384 ? 16384 : length - offset));
        else n = (int)recv(ws->fd, data + offset, length - offset, 0);
        if (n <= 0) return -1;
        offset += (size_t)n;
    }
    return 0;
}

static int dns_match(const char *pattern, const char *host) {
    if (!pattern || !host) return 0;
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *dot = strchr(host, '.');
        return dot && dot != host && strcasecmp(dot + 1, pattern + 2) == 0;
    }
    return strcasecmp(pattern, host) == 0;
}

static int verify_cert_host(X509 *cert, const char *host) {
    unsigned char ip[16];
    int ip_len = inet_pton(AF_INET, host, ip) == 1 ? 4 :
                 inet_pton(AF_INET6, host, ip) == 1 ? 16 : 0;
    int found_san = 0, matched = 0;
    GENERAL_NAMES *sans = (GENERAL_NAMES *)X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
    if (sans) {
        int count = sk_GENERAL_NAME_num(sans);
        for (int i = 0; i < count; ++i) {
            const GENERAL_NAME *gn = sk_GENERAL_NAME_value(sans, i);
            if (ip_len && gn->type == GEN_IPADD) {
                const ASN1_OCTET_STRING *s = gn->d.iPAddress;
                if (ASN1_STRING_length(s) == ip_len &&
                    memcmp(xz_asn1_data(s), ip, (size_t)ip_len) == 0) matched = 1;
            } else if (!ip_len && gn->type == GEN_DNS) {
                const ASN1_STRING *s = gn->d.dNSName;
                int n = ASN1_STRING_length(s);
                const unsigned char *data = xz_asn1_data(s);
                if (n > 0 && n < 256 && !memchr(data, 0, (size_t)n)) {
                    char name[256];
                    memcpy(name, data, (size_t)n);
                    name[n] = '\0';
                    found_san = 1;
                    if (dns_match(name, host)) matched = 1;
                }
            }
        }
        GENERAL_NAMES_free(sans);
    }
    if (matched) return 1;
    if (found_san || ip_len) return 0;
    X509_NAME *subject = X509_get_subject_name(cert);
    char cn[256];
    int n = X509_NAME_get_text_by_NID(subject, NID_commonName, cn, sizeof(cn));
    if (n <= 0 || n >= (int)sizeof(cn)) return 0;
    cn[n] = '\0';
    return dns_match(cn, host);
}

static int tls_start(dc_xz_ws *ws, const char *ca_file) {
    tls_library_init();
    SSL_CTX *ctx = SSL_CTX_new(SSLv23_client_method());
    if (!ctx) return -1;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    int loaded = ca_file && *ca_file ?
        SSL_CTX_load_verify_locations(ctx, ca_file, NULL) : SSL_CTX_set_default_verify_paths(ctx);
    if (loaded != 1) { SSL_CTX_free(ctx); return -1; }
    SSL *ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); return -1; }
    SSL_set_fd(ssl, ws->fd);
    SSL_set_tlsext_host_name(ssl, ws->host);
    if (SSL_connect(ssl) != 1 || SSL_get_verify_result(ssl) != X509_V_OK) {
        SSL_free(ssl); SSL_CTX_free(ctx); return -1;
    }
    X509 *cert = SSL_get_peer_certificate(ssl);
    int host_ok = cert && verify_cert_host(cert, ws->host);
    if (cert) X509_free(cert);
    if (!host_ok) { SSL_free(ssl); SSL_CTX_free(ctx); return -1; }
    ws->ssl_ctx = ctx;
    ws->ssl = ssl;
    return 0;
}

static int read_http_response(dc_xz_ws *ws, char *out, size_t cap) {
    size_t used = 0;
    while (used + 1 < cap) {
        if (read_all(ws, (unsigned char *)out + used, 1) != 0) return -1;
        used++;
        out[used] = '\0';
        if (used >= 4 && memcmp(out + used - 4, "\r\n\r\n", 4) == 0) return 0;
    }
    return -1;
}

static int header_value(const char *headers, const char *name, char *out, size_t cap) {
    size_t name_len = strlen(name);
    const char *line = strstr(headers, "\r\n");
    if (!line) return -1;
    line += 2;
    while (*line && !(line[0] == '\r' && line[1] == '\n')) {
        const char *end = strstr(line, "\r\n");
        if (!end) return -1;
        if ((size_t)(end - line) > name_len && strncasecmp(line, name, name_len) == 0 &&
            line[name_len] == ':') {
            const char *v = line + name_len + 1;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            size_t n = (size_t)(end - v);
            if (n + 1 > cap) return -1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        line = end + 2;
    }
    return -1;
}

static int websocket_upgrade(dc_xz_ws *ws, const char *device_id,
                             const char *client_id, const char *token) {
    if (!device_id || !*device_id || !client_id || !*client_id ||
        has_line_break(device_id) || has_line_break(client_id) || has_line_break(token)) return -1;
    unsigned char nonce[16], digest[EVP_MAX_MD_SIZE];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) return -1;
    char key[32], expected[64];
    int key_len = EVP_EncodeBlock((unsigned char *)key, nonce, sizeof(nonce));
    key[key_len] = '\0';
    char source[128];
    if (snprintf(source, sizeof(source), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key) >= (int)sizeof(source)) return -1;
    unsigned int digest_len = 0;
    if (EVP_Digest(source, strlen(source), digest, &digest_len, EVP_sha1(), NULL) != 1) return -1;
    int expected_len = EVP_EncodeBlock((unsigned char *)expected, digest, (int)digest_len);
    expected[expected_len] = '\0';

    char host_header[320];
    const char *default_port = ws->tls ? "443" : "80";
    if (strcmp(ws->port, default_port) == 0) snprintf(host_header, sizeof(host_header), "%s", ws->host);
    else snprintf(host_header, sizeof(host_header), "%s:%s", ws->host, ws->port);

    char request[4096];
    int n = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
        "Protocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n%s%s%s\r\n",
        ws->path, host_header, key, device_id, client_id,
        token && *token ? "Authorization: Bearer " : "",
        token && *token ? token : "",
        token && *token ? "\r\n" : "");
    if (n < 0 || n >= (int)sizeof(request) || write_all(ws, (unsigned char *)request, (size_t)n) != 0) return -1;

    char response[WS_HEADER_CAP];
    if (read_http_response(ws, response, sizeof(response)) != 0) return -1;
    if (strncmp(response, "HTTP/1.1 101 ", 13) != 0 && strncmp(response, "HTTP/1.0 101 ", 13) != 0) return -1;
    char accept[128];
    if (header_value(response, "Sec-WebSocket-Accept", accept, sizeof(accept)) != 0 ||
        strcmp(accept, expected) != 0) return -1;
    return 0;
}

int dc_xz_ws_connect(dc_xz_ws *ws, const char *url, const char *device_id,
                  const char *client_id, const char *access_token, int timeout_ms) {
    if (!ws || !url || timeout_ms <= 0) return -1;
    memset(ws, 0, sizeof(*ws));
    ws->fd = -1;
    if (parse_url(ws, url) != 0) return -1;
    ws->fd = socket_connect(ws->host, ws->port, timeout_ms);
    if (ws->fd < 0) return -1;
    if (ws->tls && tls_start(ws, getenv("XZ_CA_FILE")) != 0) { dc_xz_ws_close(ws); return -1; }
    if (websocket_upgrade(ws, device_id, client_id, access_token) != 0) { dc_xz_ws_close(ws); return -1; }
    ws->connected = 1;
    return 0;
}

static int send_frame(dc_xz_ws *ws, int opcode, const unsigned char *data, size_t length) {
    if (!ws || !ws->connected || length > DC_XZ_WS_MAX_MESSAGE) return -1;
    unsigned char mask[4], *frame;
    if (RAND_bytes(mask, sizeof(mask)) != 1) return -1;
    size_t cap = length + 14, frame_len = 0;
    frame = (unsigned char *)malloc(cap);
    if (!frame) return -1;
    int rc = dc_xz_ws_build_client_frame(opcode, data, length, mask, frame, cap, &frame_len);
    if (rc == 0) rc = write_all(ws, frame, frame_len);
    free(frame);
    return rc;
}

int dc_xz_ws_send_text(dc_xz_ws *ws, const char *text) {
    return text ? send_frame(ws, 1, (const unsigned char *)text, strlen(text)) : -1;
}

int dc_xz_ws_send_binary(dc_xz_ws *ws, const unsigned char *data, size_t length) {
    return send_frame(ws, 2, data, length);
}

int dc_xz_ws_send_control(dc_xz_ws *ws, int opcode, const unsigned char *data, size_t length) {
    if (opcode != 8 && opcode != 9 && opcode != 10) return -1;
    if (length > 125) return -1;
    return send_frame(ws, opcode, data, length);
}

static int receive_frame(dc_xz_ws *ws, int *fin, int *opcode, unsigned char *payload,
                         size_t cap, size_t *length) {
    unsigned char head[2], ext[8];
    if (read_all(ws, head, 2) != 0) return -1;
    if ((head[0] & 0x70) || (head[1] & 0x80)) return -1;
    *fin = (head[0] & 0x80) != 0;
    *opcode = head[0] & 0x0f;
    unsigned code = head[1] & 0x7f;
    uint64_t n = code;
    if (code == 126) {
        if (read_all(ws, ext, 2) != 0) return -1;
        n = ((uint64_t)ext[0] << 8) | ext[1];
        if (n < 126) return -1;
    } else if (code == 127) {
        if (read_all(ws, ext, 8) != 0 || (ext[0] & 0x80)) return -1;
        n = 0;
        for (int i = 0; i < 8; ++i) n = (n << 8) | ext[i];
        if (n <= 65535) return -1;
    }
    if ((*opcode & 8) && (!*fin || n > 125)) return -1;
    if (n > cap || n > DC_XZ_WS_MAX_MESSAGE) return -1;
    *length = (size_t)n;
    return n ? read_all(ws, payload, (size_t)n) : 0;
}

int dc_xz_ws_receive(dc_xz_ws *ws, int *type, unsigned char *out, size_t capacity,
                  size_t *length, int timeout_ms) {
    if (!ws || !ws->connected || !type || !out || !length) return -1;
    int ready = dc_xz_ws_readable(ws, timeout_ms);
    if (ready <= 0) return ready;
    unsigned char *part = (unsigned char *)malloc(DC_XZ_WS_MAX_MESSAGE);
    if (!part) return -1;
    size_t used = 0, part_len = 0;
    int message_opcode = 0, rc = -1;
    for (;;) {
        int fin, opcode;
        if (receive_frame(ws, &fin, &opcode, part, DC_XZ_WS_MAX_MESSAGE, &part_len) != 0) break;
        if (opcode == 9) {
            if (dc_xz_ws_send_control(ws, 10, part, part_len) != 0) break;
            /* Yield between complete control messages so microphone reads,
             * termination signals and idle deadlines cannot be starved by
             * a peer sending only keepalives. Preserve partial messages. */
            if (!message_opcode) { rc = 0; break; }
            continue;
        }
        if (opcode == 10) {
            if(part_len==8){uint64_t token=0;for(unsigned i=0;i<8;i++)token=(token<<8)|part[i];ws->pong_token=token;}
            if (!message_opcode) { rc = 0; break; }
            continue;
        }
        if (opcode == 8) {
            (void)dc_xz_ws_send_control(ws, 8, part, part_len > 125 ? 0 : part_len);
            *type = DC_XZ_WS_CLOSED; *length = 0; rc = 1; break;
        }
        if (opcode == 1 || opcode == 2) {
            if (message_opcode != 0) break;
            message_opcode = opcode;
        } else if (opcode != 0 || message_opcode == 0) break;
        if (part_len > capacity - used) break;
        memcpy(out + used, part, part_len);
        used += part_len;
        if (fin) {
            *type = message_opcode == 1 ? DC_XZ_WS_TEXT : DC_XZ_WS_BINARY;
            *length = used;
            rc = 1;
            break;
        }
    }
    free(part);
    return rc;
}

int dc_xz_ws_readable(dc_xz_ws *ws, int timeout_ms) {
    if (!ws || !ws->connected) return -1;
    if (ws->tls && SSL_pending((SSL *)ws->ssl) > 0) return 1;
    struct pollfd pfd = {ws->fd, POLLIN, 0};
    int rc;
    do { rc = poll(&pfd, 1, timeout_ms); } while (rc < 0 && errno == EINTR);
    if (rc <= 0) return rc;
    return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0;
}

void dc_xz_ws_close(dc_xz_ws *ws) {
    if (!ws) return;
    if (ws->connected) (void)send_frame(ws, 8, NULL, 0);
    ws->connected = 0;
    if (ws->ssl) { SSL_shutdown((SSL *)ws->ssl); SSL_free((SSL *)ws->ssl); ws->ssl = NULL; }
    if (ws->ssl_ctx) { SSL_CTX_free((SSL_CTX *)ws->ssl_ctx); ws->ssl_ctx = NULL; }
    if (ws->fd >= 0) { close(ws->fd); ws->fd = -1; }
}
