/* Synthetic in-memory certificates and TLS only; includes actual consumers. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if TLS_CONSUMER == 0
#include "../phone.c"
#elif TLS_CONSUMER == 1
#include "../support/xiaozhi/ws.c"
#elif TLS_CONSUMER == 2
#include "../support/doubao/ws.c"
#else
#error Select an actual TLS consumer
#endif
#include <openssl/err.h>
#include <openssl/rsa.h>

#define MUST(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); ERR_print_errors_fp(stderr); return 1; } } while (0)

int main(void) {
    SSL_library_init();
    SSL_load_error_strings();
    RSA *rsa = RSA_new();
    BIGNUM *exponent = BN_new();
    EVP_PKEY *key = EVP_PKEY_new();
    MUST(rsa && exponent && key && BN_set_word(exponent, RSA_F4));
    MUST(RSA_generate_key_ex(rsa, 2048, exponent, NULL) == 1);
    MUST(EVP_PKEY_assign_RSA(key, rsa) == 1);
    BN_free(exponent);
    X509 *cert = X509_new();
    MUST(cert && X509_set_version(cert, 2));
    MUST(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1));
    MUST(X509_gmtime_adj(X509_get_notBefore(cert), -60));
    MUST(X509_gmtime_adj(X509_get_notAfter(cert), 86400));
    MUST(X509_set_pubkey(cert, key));
    X509_NAME *subject = X509_get_subject_name(cert);
    MUST(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                  (const unsigned char *)"ignored-cn.invalid", -1, -1, 0));
    MUST(X509_set_issuer_name(cert, subject));
    X509_EXTENSION *san = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name,
        "DNS:probe.invalid,DNS:*.wild.invalid,IP:127.0.0.1,IP:2001:db8::1");
    MUST(san && X509_add_ext(cert, san, -1));
    X509_EXTENSION_free(san);
    MUST(X509_sign(cert, key, EVP_sha256()) > 0);

#if TLS_CONSUMER == 0
    SSL_CTX *client_ctx = SSL_CTX_new(TLSv1_2_client_method());
    SSL_CTX *server_ctx = SSL_CTX_new(TLSv1_2_server_method());
    MUST(client_ctx && server_ctx);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_PEER, NULL);
    MUST(X509_STORE_add_cert(SSL_CTX_get_cert_store(client_ctx), cert) == 1);
    MUST(SSL_CTX_use_certificate(server_ctx, cert) == 1);
    MUST(SSL_CTX_use_PrivateKey(server_ctx, key) == 1);
    SSL *client = SSL_new(client_ctx), *server = SSL_new(server_ctx);
    BIO *cb = NULL, *sb = NULL;
    MUST(client && server && BIO_new_bio_pair(&cb, 0, &sb, 0) == 1);
    SSL_set_bio(client, cb, cb); SSL_set_bio(server, sb, sb);
    SSL_set_connect_state(client); SSL_set_accept_state(server);
    int cdone = 0, sdone = 0;
    for (int i = 0; i < 100 && (!cdone || !sdone); ++i) {
        if (!cdone) { int r = SSL_connect(client); if (r == 1) cdone = 1;
            else { int e = SSL_get_error(client, r); MUST(e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE); } }
        if (!sdone) { int r = SSL_accept(server); if (r == 1) sdone = 1;
            else { int e = SSL_get_error(server, r); MUST(e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE); } }
    }
    MUST(cdone && sdone && SSL_get_verify_result(client) == X509_V_OK);
    static struct phone worker;
    worker.ssl = client;
    strcpy(worker.config.host, "127.0.0.1");
    unsigned char der[1024], *cursor = der;
    int size = i2d_PUBKEY(key, &cursor);
    MUST(size > 0 && size < (int)sizeof(der));
    MUST(SHA256(der, (size_t)size, worker.config.pin));
    MUST(peer_verified(&worker) == 1);
    strcpy(worker.config.host, "127.0.0.2");
    MUST(peer_verified(&worker) == 0);
    strcpy(worker.config.host, "127.0.0.1");
    worker.config.pin[0] ^= 1;
    MUST(peer_verified(&worker) == 0);
    worker.config.pin[0] ^= 1;
    SSL_set_verify_result(client, X509_V_ERR_CERT_REJECTED);
    MUST(peer_verified(&worker) == 0);
    SSL_set_verify_result(client, X509_V_OK);
    MUST(peer_verified(&worker) == 1);
    SSL_free(client); SSL_free(server);
    SSL_CTX_free(client_ctx); SSL_CTX_free(server_ctx);
    puts("actual phone peer_verified: TLS1.2 + SAN IP + SPKI; wrong IP/pin/chain rejected");
#else
    MUST(verify_cert_host(cert, "probe.invalid") == 1);
    MUST(verify_cert_host(cert, "PROBE.INVALID") == 1);
    MUST(verify_cert_host(cert, "a.wild.invalid") == 1);
    MUST(verify_cert_host(cert, "a.b.wild.invalid") == 0);
    MUST(verify_cert_host(cert, "wild.invalid") == 0);
    MUST(verify_cert_host(cert, "other.invalid") == 0);
    MUST(verify_cert_host(cert, "ignored-cn.invalid") == 0);
    MUST(verify_cert_host(cert, "127.0.0.1") == 1);
    MUST(verify_cert_host(cert, "127.0.0.2") == 0);
    MUST(verify_cert_host(cert, "2001:db8::1") == 1);
    MUST(verify_cert_host(cert, "2001:db8::2") == 0);
    puts("actual ws verify_cert_host: SAN DNS/wildcard/IPv4/IPv6; mismatches and CN fallback rejected");
#endif
    X509_free(cert); EVP_PKEY_free(key);
    return 0;
}
