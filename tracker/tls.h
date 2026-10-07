#ifndef TLS_H
#define TLS_H
#include <string>
#include <openssl/ssl.h>

// Every connection of the tracker is TLS. Each tracker has a key pair of its
// own and a certificate for it signed by the system's certificate authority
// (tracker<n>_cert.pem and tracker<n>_key.pem, next to tracker_info.txt).
// Clients and trackers trust what that authority signed (ca_cert.pem), so a
// tracker's key can be replaced without touching the clients.

// Loads this tracker's certificate and key and the authority's certificate
// from `dir` ("" or a path ending in '/').
bool tls_init(const std::string &dir, int tracker_no);

// Run the handshake on a connected socket, as the accepting side or as the
// side that connected (to the other tracker, whose certificate is checked).
// Return nullptr if the handshake fails; the socket is left open.
SSL *tls_accept(int fd);
SSL *tls_connect(int fd);

// True if the other side proved during the handshake that it holds the key
// of a certificate signed by the authority, which only trackers have.
bool tls_peer_is_tracker(SSL *ssl);

// Ends the TLS session. The socket itself is not closed.
void tls_close(SSL *ssl);

#endif
