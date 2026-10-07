#ifndef TLS_H
#define TLS_H
#include <string>
#include <openssl/ssl.h>

// Every connection of the client is TLS.
// - A tracker must present a certificate signed by the system's certificate
//   authority. The client is given the authority's certificate (ca_cert.pem)
//   together with tracker_info.txt.
// - Peers have no certificate file. Each client makes a new key pair when it
//   starts and registers the fingerprint of its certificate with the tracker,
//   which hands it out together with the client's address. A downloader only
//   talks to a peer that proves it holds the key with that fingerprint, and
//   proves its own key to the seeder in the same handshake.

// Loads the authority's certificate and creates this client's key pair.
bool tls_init(const std::string &ca_cert);
void tls_cleanup();

// SHA-256 of this client's certificate, 64 hex digits.
const std::string &tls_my_fingerprint();

// Run the handshake on a connected socket. They return nullptr (and close the
// socket) if the handshake fails or the other side is not who it should be.
SSL *tls_connect_tracker(int fd);
SSL *tls_accept_peer(int fd);
SSL *tls_connect_peer(int fd, const std::string &fingerprint);

// SHA-256 of the certificate the other side proved to hold the key of
// (64 hex digits), or "" if it presented none.
std::string tls_peer_fingerprint(SSL *ssl);

// Ends the TLS session and closes its socket.
void tls_close(SSL *ssl);

#endif
