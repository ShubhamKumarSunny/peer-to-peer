#include "tls.h"
#include <iostream>
#include <openssl/err.h>
#include <openssl/x509.h>
using namespace std;

static SSL_CTX *g_ctx = nullptr;

bool tls_init(const string &dir, int tracker_no) {
    string name = dir + "tracker" + to_string(tracker_no);
    string cert = name + "_cert.pem";
    string key = name + "_key.pem";
    string ca = dir + "ca_cert.pem";
    g_ctx = SSL_CTX_new(TLS_method());
    if (!g_ctx) return false;
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
    // The authority's certificate is the only thing this tracker trusts:
    // whoever presents a certificate it signed and passes the handshake is
    // the other tracker.
    if (SSL_CTX_use_certificate_file(g_ctx, cert.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(g_ctx, key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(g_ctx) != 1 ||
        SSL_CTX_load_verify_locations(g_ctx, ca.c_str(), nullptr) != 1) {
        cerr << "Cannot load " << cert << ", " << key << " and " << ca << " (create them with: make cert)\n";
        return false;
    }
    // Accepting: ask for a certificate. Clients have none and are let in; the
    // other tracker sends one, and a certificate the authority did not sign
    // fails the handshake. Connecting: the other tracker's certificate must
    // be signed by the authority.
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER, nullptr);
    // No session tickets, so that nothing arrives on a connection after the
    // handshake unless the other side sends data or closes (peer_connector
    // relies on this).
    SSL_CTX_set_num_tickets(g_ctx, 0);
    SSL_CTX_set_session_cache_mode(g_ctx, SSL_SESS_CACHE_OFF);
    return true;
}

static SSL *handshake(int fd, bool accepting) {
    SSL *ssl = SSL_new(g_ctx);
    if (!ssl) return nullptr;
    SSL_set_fd(ssl, fd);
    int r = accepting ? SSL_accept(ssl) : SSL_connect(ssl);
    if (r != 1) {
        ERR_clear_error();
        SSL_free(ssl);
        return nullptr;
    }
    return ssl;
}

SSL *tls_accept(int fd) { return handshake(fd, true); }
SSL *tls_connect(int fd) { return handshake(fd, false); }

bool tls_peer_is_tracker(SSL *ssl) {
    X509 *cert = SSL_get_peer_certificate(ssl);
    if (!cert) return false;
    X509_free(cert);
    return SSL_get_verify_result(ssl) == X509_V_OK;
}

void tls_close(SSL *ssl) {
    if (!ssl) return;
    SSL_shutdown(ssl);
    ERR_clear_error();
    SSL_free(ssl);
}
