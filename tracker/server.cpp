#include "commands.h"
#include "tls.h"
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <set>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sstream>
using namespace std;

// The console, the accept thread, the connector and one thread per client
// all print.  Out() << ... << endl;  holds one lock for the whole statement,
// so two lines never mix and the stream is never used by two threads at once.
static mutex out_mtx;
struct Out {
    lock_guard<mutex> lock;
    ostream &os;
    explicit Out(ostream &o = cout) : lock(out_mtx), os(o) {}
    template <typename T> Out &operator<<(const T &v) { os << v; return *this; }
    Out &operator<<(ostream &(*f)(ostream &)) { os << f; return *this; }
};

// Longest accepted line (an UPLOAD_FILE carries 41 bytes per 512KB piece)
static const size_t MAX_LINE = 16 * 1024 * 1024;

// First line the other tracker sends on a connection, which turns it into
// the sync link instead of a client session.
static const string TRACKER_HELLO = "TRACKER_HELLO";

static int peer_fd_in = -1;
static int peer_fd_out = -1;
static SSL *peer_ssl_out = nullptr;   // TLS session on peer_fd_out
static mutex peer_mtx;
static string peer_host;
static int peer_port = 0;

// Shutdown bookkeeping: "quit" wakes every thread by shutting down its socket
// and waits for all of them to finish before the process exits.
static atomic<bool> stopping(false);
static mutex threads_mtx;
static condition_variable threads_cv;
static int live_threads = 0;
static set<int> live_fds;

struct ThreadGuard {
    ThreadGuard() { lock_guard<mutex> lock(threads_mtx); ++live_threads; }
    ~ThreadGuard() {
        { lock_guard<mutex> lock(threads_mtx); --live_threads; }
        threads_cv.notify_all();
    }
};

// Registers a socket a thread may block on. Returns false during shutdown.
static bool track_fd(int fd) {
    lock_guard<mutex> lock(threads_mtx);
    if (stopping) return false;
    live_fds.insert(fd);
    return true;
}

static void close_fd(int fd) {
    {
        lock_guard<mutex> lock(threads_mtx);
        live_fds.erase(fd);
    }
    close(fd);
}

static void sleep_unless_stopping(int ms) {
    for (int t = 0; t < ms && !stopping; t += 100) this_thread::sleep_for(chrono::milliseconds(100));
}

// Reads one line; returns false when the connection is closed or broken
bool read_from_socket(SSL *ssl, string &s) {
    s.clear();
    char c;
    while (true) {
        int n = SSL_read(ssl, &c, 1);
        if (n <= 0) {
            // WANT_READ on a blocking socket: the read was interrupted
            if (SSL_get_error(ssl, n) == SSL_ERROR_WANT_READ) continue;
            return false;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (s.size() >= MAX_LINE) return false;
        s.push_back(c);
    }
    return true;
}

bool write_to_socket(SSL *ssl, string line) {
    line += "\n";
    const char *buf = line.c_str();
    size_t total = line.size();
    size_t sent = 0;
    while (sent < total) {
        int n = SSL_write(ssl, buf + sent, (int)(total - sent));
        if (n <= 0) {
            if (SSL_get_error(ssl, n) == SSL_ERROR_WANT_WRITE) continue;
            return false;
        }
        sent += n;
    }
    return true;
}

static bool resolve_host(const string &host, int port, sockaddr_in &out) {
    addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    memcpy(&out, res->ai_addr, sizeof(out));
    out.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

void send_to_peer(const string &msg) {
    if (msg.empty()) return;
    lock_guard<mutex> lock(peer_mtx);
    // While the link is down nothing is sent: the operation is in the oplog
    // and is resent when the link comes back.
    if (peer_fd_out == -1) return;
    if (!write_to_socket(peer_ssl_out, msg)) {
        // peer_connector owns the fd: wake it up so it closes and reconnects
        shutdown(peer_fd_out, SHUT_RDWR);
        peer_fd_out = -1;
        peer_ssl_out = nullptr;
        Out(cerr) << "Warning: lost outgoing peer connection while forwarding\n";
    }
}

// Applies the operations the other tracker forwards on its sync link
void peer_reader(SSL *ssl, int sock) {
    {
        lock_guard<mutex> lock(peer_mtx);
        // The old reader thread closes its own socket once this wakes it up
        if (peer_fd_in != -1) shutdown(peer_fd_in, SHUT_RDWR);
        peer_fd_in = sock;
    }
    Out() << "Peer tracker connected (incoming)" << endl;
    string line;
    while (read_from_socket(ssl, line)) {
        if (line.empty()) continue;
        string res = apply_peer_op(line);
        Out() << "[SYNC] " << line << " -> " << res << endl;
    }
    {
        lock_guard<mutex> lock(peer_mtx);
        if (peer_fd_in == sock) peer_fd_in = -1;
    }
    Out(cerr) << "Peer tracker disconnected\n";
}

// Keeps the outgoing connection to the peer tracker alive, reconnecting
// whenever the peer goes away. Every time the link comes up, the peer says
// how many of this tracker's operations it has and the rest are resent, so
// nothing is lost while the trackers cannot reach each other.
void peer_connector(string host, int port) {
    ThreadGuard guard;
    while (!stopping) {
        sockaddr_in serv;
        int s = -1;
        if (resolve_host(host, port, serv)) s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) { sleep_unless_stopping(1000); continue; }
        if (!track_fd(s)) { close(s); break; }

        string reply, tag;
        long long have = -1;
        // The handshake fails unless the other end holds the tracker key
        SSL *ssl = nullptr;
        if (connect(s, (sockaddr*)&serv, sizeof(serv)) == 0) ssl = tls_connect(s);
        bool ok = ssl &&
                  write_to_socket(ssl, TRACKER_HELLO) &&
                  read_from_socket(ssl, reply);
        if (ok) {
            istringstream iss(reply);
            ok = (iss >> tag >> have) && tag == "HAVE" && have >= 0;
        }
        if (ok) {
            // peer_mtx is held from reading the log until the link is usable,
            // so every operation is either resent here or forwarded afterwards.
            lock_guard<mutex> lock(peer_mtx);
            vector<string> missed = own_ops_after(have);
            for (auto &op : missed) {
                if (!write_to_socket(ssl, op)) { ok = false; break; }
            }
            if (ok) {
                peer_fd_out = s;
                peer_ssl_out = ssl;
                Out() << "Connected to peer tracker at " << host << ":" << port
                     << " (resent " << missed.size() << " operations)" << endl;
            }
        }
        if (!ok) {
            tls_close(ssl);
            close_fd(s);
            sleep_unless_stopping(1000);
            continue;
        }

        // The peer sends nothing more on this socket, so anything that
        // arrives means the connection is going away. The socket is watched
        // directly: other threads write on the TLS session at the same time,
        // and a session must not be used by two threads at once.
        char c;
        while (recv(s, &c, 1, MSG_PEEK) < 0 && errno == EINTR) {}
        {
            lock_guard<mutex> lock(peer_mtx);
            if (peer_fd_out == s) { peer_fd_out = -1; peer_ssl_out = nullptr; }
            tls_close(ssl);
            close_fd(s);
        }
        if (!stopping) Out(cerr) << "Lost connection to peer tracker, reconnecting\n";
    }
}

bool send_response_to_client(SSL *ssl, const string &res) {
    istringstream iss(res);
    string out;
    while (getline(iss, out)) {
        if (!write_to_socket(ssl, out)) return false;
    }
    return write_to_socket(ssl, "END");
}

// The sync link is only accepted from the address of the other tracker
// (or from this machine, where both trackers usually run). An address can be
// faked, so the caller also checks the certificate (tls_peer_is_tracker).
static bool is_peer_tracker(const sockaddr_in &from) {
    sockaddr_in peer;
    if ((ntohl(from.sin_addr.s_addr) >> 24) == 127) return true;
    if (!resolve_host(peer_host, peer_port, peer)) return false;
    return from.sin_addr.s_addr == peer.sin_addr.s_addr;
}

void client_handler(int sock, sockaddr_in from) {
    ThreadGuard guard;
    if (!track_fd(sock)) { close(sock); return; }
    SSL *ssl = tls_accept(sock);
    if (!ssl) {
        Out(cerr) << "TLS handshake failed, connection dropped\n";
        close_fd(sock);
        return;
    }
    string line, sync_msg;
    while (read_from_socket(ssl, line)) {
        if (line == TRACKER_HELLO) {
            if (!tls_peer_is_tracker(ssl) || !is_peer_tracker(from)) {
                Out(cerr) << "Rejected tracker sync link: not the other tracker\n";
                break;
            }
            // Tell the peer which of its operations are already applied here
            if (write_to_socket(ssl, "HAVE " + to_string(peer_ops_applied()))) peer_reader(ssl, sock);
            tls_close(ssl);
            close_fd(sock);
            return;
        }
        Out() << "[SERVER] " << printable_command(line) << endl;
        string res = process_command(line, sock, sync_msg);
        send_to_peer(sync_msg);
        if (!send_response_to_client(ssl, res)) break;
    }
    // A client that goes away without LOGOUT must not stay listed as a seeder.
    // Not when the tracker itself is shutting down: its clients move to the
    // other tracker and log in there, and a LOGOUT recorded now would reach
    // that tracker later and end the session they have just started.
    if (!stopping) {
        string op = end_session(sock, sync_msg);
        if (!op.empty()) {
            Out() << "[SERVER] " << op << " (client disconnected)" << endl;
            send_to_peer(sync_msg);
        }
    }
    tls_close(ssl);
    close_fd(sock);
    Out() << "Client disconnected" << endl;
}

// Parses "<host>:<port>" (or "<host> <port>")
static bool parse_endpoint(const string &text, string &host, int &port) {
    string s = text;
    for (auto &ch : s) if (ch == ':') ch = ' ';
    istringstream iss(s);
    string p, extra;
    if (!(iss >> host >> p) || (iss >> extra)) return false;
    if (p.size() > 5) return false;
    for (unsigned char ch : p) if (!isdigit(ch)) return false;
    port = atoi(p.c_str());
    return port >= 1 && port <= 65535;
}

void accept_clients(int s) {
    ThreadGuard guard;
    while (!stopping) {
        // Wait with a timeout so that "quit" is noticed
        struct pollfd pfd;
        pfd.fd = s; pfd.events = POLLIN; pfd.revents = 0;
        if (poll(&pfd, 1, 200) <= 0) continue;
        sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int ns = accept(s, (sockaddr*)&cli, &len);
        if (ns < 0) {
            if (errno != EINTR) { perror("accept"); this_thread::sleep_for(chrono::milliseconds(100)); }
            continue;
        }
        thread(client_handler, ns, cli).detach();
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        Out() << "Usage: " << argv[0] << " tracker_info.txt <tracker_no>\n";
        return 1;
    }
    // tracker_info.txt lists the two trackers, one <ip>:<port> per line
    vector<pair<string, int>> trackers;
    ifstream info(argv[1]);
    string info_line;
    while (getline(info, info_line)) {
        string host;
        int port;
        if (parse_endpoint(info_line, host, port)) trackers.push_back({host, port});
    }
    int tracker_no = atoi(argv[2]);
    if (trackers.size() != 2) {
        Out(cerr) << argv[1] << " must list exactly two trackers, one <ip>:<port> per line\n";
        return 1;
    }
    if (tracker_no != 1 && tracker_no != 2) {
        Out(cerr) << "tracker_no must be 1 or 2\n";
        return 1;
    }
    int my_port = trackers[tracker_no - 1].second;
    peer_host = trackers[2 - tracker_no].first;
    peer_port = trackers[2 - tracker_no].second;

    // Writing to a socket the other side already closed must not kill the tracker
    signal(SIGPIPE, SIG_IGN);

    // The certificates and this tracker's key are kept next to tracker_info.txt
    string info_path = argv[1];
    size_t slash = info_path.rfind('/');
    if (!tls_init(slash == string::npos ? "" : info_path.substr(0, slash + 1), tracker_no)) return 1;

    init_oplog(tracker_no);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(my_port);
    if (::bind(s, (sockaddr*)&serv, sizeof(serv)) < 0) { perror("bind"); close(s); return 1; }
    if (listen(s, 64) < 0) { perror("listen"); close(s); return 1; }
    Out() << "Tracker " << tracker_no << " listening on " << my_port << " (type quit to stop)" << endl;

    thread(accept_clients, s).detach();
    thread(peer_connector, peer_host, peer_port).detach();

    // Console: "quit" shuts the tracker down. Without a console (stdin closed)
    // the tracker keeps running until it is killed.
    string cmd;
    while (getline(cin, cmd)) {
        if (cmd == "quit") {
            Out() << "Tracker shutting down" << endl;
            unique_lock<mutex> lock(threads_mtx);
            stopping = true;
            for (int fd : live_fds) shutdown(fd, SHUT_RDWR);
            threads_cv.wait_for(lock, chrono::seconds(3), [] { return live_threads == 0; });
            close(s);
            return 0;
        }
        if (!cmd.empty()) Out() << "Unknown command (type quit to stop)" << endl;
    }
    while (true) pause();
}
