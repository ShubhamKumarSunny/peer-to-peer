#include "peer.h"
#include "fileutils.h"
#include "threadpool.h"
#include "tls.h"
#include "console.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <random>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
using namespace std;

static int g_listen_port = -1;
static const uint32_t ERROR_LEN = 0xFFFFFFFFu;
static const int IO_TIMEOUT_SEC = 5;
static const size_t MAX_REQUEST_LINE = 1024;
static const int ACCESS_ALLOWED_SEC = 30;    // how long a "yes" from the tracker is reused
static const int ACCESS_DENIED_SEC = 2;      // and a "no"
static const long long MAX_PIECES = 1 << 24;
static const size_t SEEDER_THREADS = 8;      // workers serving other peers
static const size_t DOWNLOAD_THREADS = 8;    // workers shared by all downloads
static const int PIECES_IN_PARALLEL = 4;     // pieces of one file fetched at a time
static const int PEER_MAX_FAILURES = 3;      // consecutive failures before a peer is dropped
static const int PIECE_EXTRA_ATTEMPTS = 5;   // retries per piece beyond one per peer
static atomic<bool> g_verbose(false);

struct Share {
    string path;
    set<string> groups;   // groups whose members may have this file
};

static mutex share_mtx;
static map<string, Share> g_shared;
static string g_share_store;

// Sockets here are blocking with SO_RCVTIMEO/SO_SNDTIMEO set, so a read or
// write that does not complete means the peer stalled past the timeout (or
// went away) and is treated as a failure.
static bool send_all(SSL *ssl, const char *buf, size_t total) {
    size_t sent = 0;
    while (sent < total) {
        int n = SSL_write(ssl, buf + sent, (int)(total - sent));
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool recv_all(SSL *ssl, char *buf, size_t total) {
    size_t got = 0;
    while (got < total) {
        int n = SSL_read(ssl, buf + got, (int)(total - got));
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

static string recv_line(SSL *ssl) {
    string s;
    char c;
    while (true) {
        if (SSL_read(ssl, &c, 1) <= 0) return "";
        if (c == '\n') break;
        if (c == '\r') continue;
        if (s.size() >= MAX_REQUEST_LINE) return "";
        s.push_back(c);
    }
    return s;
}

static void set_io_timeout(int sock) {
    struct timeval tv;
    tv.tv_sec = IO_TIMEOUT_SEC; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
}

static bool connect_with_timeout(int sock, const sockaddr_in &addr) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    if (connect(sock, (const sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) return false;
        struct pollfd pfd;
        pfd.fd = sock; pfd.events = POLLOUT; pfd.revents = 0;
        int r;
        do { r = poll(&pfd, 1, IO_TIMEOUT_SEC * 1000); } while (r < 0 && errno == EINTR);
        if (r == 0) { errno = ETIMEDOUT; return false; }
        if (r < 0) return false;
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return false;
        if (err != 0) { errno = err; return false; }
    }
    return fcntl(sock, F_SETFL, flags) == 0;
}

// Caller holds share_mtx
static void save_shared_files() {
    if (g_share_store.empty()) return;
    ofstream f(g_share_store, ios::trunc);
    // "<name> <group> <path>", one line per group the file is shared in
    for (auto &p : g_shared) {
        for (auto &group : p.second.groups) f << p.first << " " << group << " " << p.second.path << "\n";
    }
}

void load_shared_files(const string &username) {
    lock_guard<mutex> lg(share_mtx);
    string safe = username;
    for (auto &ch : safe) if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') ch = '_';
    g_share_store = ".shared_" + safe;
    g_shared.clear();
    ifstream f(g_share_store);
    string line;
    while (getline(f, line)) {
        size_t a = line.find(' ');
        size_t b = a == string::npos ? a : line.find(' ', a + 1);
        if (a == 0 || b == string::npos || b == a + 1 || b + 1 >= line.size()) continue;
        Share &share = g_shared[line.substr(0, a)];
        share.groups.insert(line.substr(a + 1, b - a - 1));
        share.path = line.substr(b + 1);
    }
}

void clear_shared_files() {
    lock_guard<mutex> lg(share_mtx);
    g_shared.clear();
    g_share_store.clear();
}

void share_file(const string &group, const string &name, const string &path) {
    lock_guard<mutex> lg(share_mtx);
    g_shared[name].path = path;
    g_shared[name].groups.insert(group);
    save_shared_files();
}

void unshare_file(const string &group, const string &name) {
    lock_guard<mutex> lg(share_mtx);
    auto it = g_shared.find(name);
    if (it == g_shared.end()) return;
    it->second.groups.erase(group);
    if (it->second.groups.empty()) g_shared.erase(it);
    save_shared_files();
}

bool is_shared(const string &name) {
    lock_guard<mutex> lg(share_mtx);
    return g_shared.count(name) > 0;
}

void set_verbose(bool on) { g_verbose = on; }

mutex &console_mutex() {
    static mutex m;
    return m;
}

static void say(const string &msg) {
    Out() << msg << endl;
}

// Per-piece chatter, only shown after VERBOSE ON
static void vlog(const string &msg) {
    if (g_verbose) say(msg);
}

// Destroyed (workers joined) when the program exits
static ThreadPool g_seeder_pool(SEEDER_THREADS);
static ThreadPool g_download_pool(DOWNLOAD_THREADS);

static ThreadPool &seeder_pool() { return g_seeder_pool; }
static ThreadPool &download_pool() { return g_download_pool; }

struct PeerState {
    string addr;
    string key;          // fingerprint of the peer's certificate, from the tracker
    vector<bool> have;   // pieces this peer said it has
    int active;          // requests of this download currently sent to it
    int failures;        // consecutive failures
    bool dead;
};

struct DownloadJob {
    string group, filename, dest, tmp, file_sha1;
    long long filesize;
    vector<string> hashes;
    function<void(bool)> on_finish;
    atomic<bool> active;   // still running: verified pieces are served from tmp
    atomic<bool> cancel;

    mutex mtx;             // guards everything below
    int fd;
    vector<PeerState> peers;
    deque<int> pending;    // pieces still to fetch, rarest first
    vector<char> have;     // pieces verified and written to tmp
    vector<int> attempts;
    int done;
    int chains;            // piece tasks still running or queued
    bool failed;
    char state;

    mutex refresh_mtx;
    chrono::steady_clock::time_point last_refresh;
};

static mutex g_jobs_mtx;
static condition_variable g_jobs_cv;
static vector<shared_ptr<DownloadJob>> g_jobs;
static int g_running = 0;

static size_t piece_len(long long filesize, long long index) {
    return (size_t)min<long long>(PIECE_SIZE, filesize - index * (long long)PIECE_SIZE);
}

static shared_ptr<DownloadJob> find_active(const string &name) {
    lock_guard<mutex> lg(g_jobs_mtx);
    for (auto &job : g_jobs) {
        if (job->active && job->filename == name) return job;
    }
    return nullptr;
}

// ---------------------------------------------------------------- seeder

// The local path of `name` if this client shares it in `group`
static bool shared_path(const string &name, const string &group, string &path) {
    lock_guard<mutex> lg(share_mtx);
    auto it = g_shared.find(name);
    if (it == g_shared.end() || !it->second.groups.count(group)) return false;
    path = it->second.path;
    return true;
}

// One '0'/'1' character per piece of `name` that this client can serve to
// the members of `group`
static bool local_bitfield(const string &name, const string &group, string &bits) {
    shared_ptr<DownloadJob> job = find_active(name);
    if (job) {
        lock_guard<mutex> lg(job->mtx);
        if (job->active) {
            if (job->group != group) return false;
            bits.clear();
            for (char h : job->have) bits.push_back(h ? '1' : '0');
            return true;
        }
    }
    string path;
    if (!shared_path(name, group, path)) return false;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    long long pieces = ((long long)st.st_size + (long long)PIECE_SIZE - 1) / (long long)PIECE_SIZE;
    bits.assign((size_t)pieces, '1');
    return true;
}

// Only files this client shares in `group` are served, never arbitrary paths
static bool local_piece(const string &name, const string &group, long long index, string &piece) {
    shared_ptr<DownloadJob> job = find_active(name);
    if (job) {
        lock_guard<mutex> lg(job->mtx);
        if (job->active) {
            if (job->group != group) return false;
            if (index >= (long long)job->have.size() || !job->have[index]) return false;
            piece.resize(piece_len(job->filesize, index));
            ssize_t n = pread(job->fd, &piece[0], piece.size(), index * (long long)PIECE_SIZE);
            return n == (ssize_t)piece.size();
        }
    }
    string path;
    if (!shared_path(name, group, path)) return false;
    ifstream f(path.c_str(), ios::binary);
    if (!f.is_open()) return false;
    f.seekg(index * (long long)PIECE_SIZE);
    piece.resize(PIECE_SIZE);
    f.read(&piece[0], PIECE_SIZE);
    piece.resize(f.gcount());
    return !piece.empty();
}

// Whether the tracker allows the holder of `key` the files of `group`. The
// answer is reused for a while, so a download does not ask for every piece.
static function<bool(const string &, const string &)> g_may_download;
static mutex access_mtx;
static map<pair<string, string>, pair<bool, chrono::steady_clock::time_point>> g_access;

static bool access_allowed(const string &group, const string &key) {
    if (key.empty()) return false;
    pair<string, string> who(group, key);
    auto now = chrono::steady_clock::now();
    {
        lock_guard<mutex> lg(access_mtx);
        // Forget old answers so the table cannot grow without limit
        for (auto it = g_access.begin(); it != g_access.end();) {
            int keep = it->second.first ? ACCESS_ALLOWED_SEC : ACCESS_DENIED_SEC;
            if (now - it->second.second > chrono::seconds(keep)) it = g_access.erase(it);
            else ++it;
        }
        auto it = g_access.find(who);
        if (it != g_access.end()) return it->second.first;
    }
    bool allowed = g_may_download && g_may_download(group, key);
    lock_guard<mutex> lg(access_mtx);
    g_access[who] = {allowed, chrono::steady_clock::now()};
    return allowed;
}

// Does this client have anything called `name` for the members of `group`?
static bool offers(const string &name, const string &group) {
    shared_ptr<DownloadJob> job = find_active(name);
    if (job && job->group == group) return true;
    string path;
    return shared_path(name, group, path);
}

static void handle_peer(int sock) {
    set_io_timeout(sock);
    SSL *ssl = tls_accept_peer(sock);
    if (!ssl) return;
    string req = recv_line(ssl);
    if (req.empty()) { tls_close(ssl); return; }

    // "GET_BITFIELD <file> <group>" or "GET_PIECE <file> <index> <group>".
    // The request is served only if the file is shared in that group and the
    // tracker says that the client on this connection, known by the key it
    // proved to hold in the handshake, is a member of the group.
    istringstream iss(req);
    string cmd, fname, group;
    long long index = -1;
    string body;
    bool ok = false;
    if (iss >> cmd >> fname) {
        bool bitfield = (cmd == "GET_BITFIELD");
        bool parsed = bitfield ? bool(iss >> group)
                               : (cmd == "GET_PIECE" && (iss >> index >> group) && index >= 0);
        if (parsed && offers(fname, group) && access_allowed(group, tls_peer_fingerprint(ssl))) {
            ok = bitfield ? local_bitfield(fname, group, body) : local_piece(fname, group, index, body);
        }
    }
    if (!ok) {
        uint32_t err = htonl(ERROR_LEN);
        send_all(ssl, (const char*)&err, sizeof(err));
        tls_close(ssl);
        vlog("[SEEDER] Refused request: " + req);
        return;
    }

    uint32_t netlen = htonl((uint32_t)body.size());
    bool sent = send_all(ssl, (const char*)&netlen, sizeof(netlen)) &&
                send_all(ssl, body.data(), body.size());
    tls_close(ssl);
    vlog(string("[SEEDER] ") + (sent ? "Served: " : "Failed to send: ") + req);
}

static atomic<bool> g_seeder_stop(false);
static thread g_seeder_thread;

static void seeder_thread(int s) {
    while (!g_seeder_stop) {
        // Wait with a timeout so that stop_seeder() is noticed
        struct pollfd pfd;
        pfd.fd = s; pfd.events = POLLIN; pfd.revents = 0;
        if (poll(&pfd, 1, 200) <= 0) continue;
        sockaddr_in cli; socklen_t len = sizeof(cli);
        int ns = accept(s, (sockaddr*)&cli, &len);
        if (ns >= 0) seeder_pool().submit([ns] { handle_peer(ns); });
        else if (errno != EINTR) usleep(100 * 1000);
    }
    close(s);
}

void stop_seeder() {
    g_seeder_stop = true;
    if (g_seeder_thread.joinable()) g_seeder_thread.join();
}

bool start_seeder(int port, function<bool(const string &, const string &)> may_download) {
    if (g_listen_port != -1) return g_listen_port == port;
    g_may_download = may_download;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("seeder socket"); return false; }
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    serv.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s, (sockaddr*)&serv, sizeof(serv)) < 0) { perror("bind seeder"); close(s); return false; }
    if (listen(s, 64) < 0) { perror("listen seeder"); close(s); return false; }
    g_listen_port = port;
    g_seeder_thread = thread(seeder_thread, s);
    Out() << "[SEEDER] Listening on port " << port << endl;
    return true;
}

// ------------------------------------------------------------ downloader

// Connects to "<ip>:<port>", checks that whoever answers holds the key with
// fingerprint `key`, and sends one request line. Returns the connection, or
// nullptr on failure.
static SSL *send_request(const string &peer, const string &key, const string &req) {
    auto pos = peer.rfind(':');
    if (pos == string::npos) return nullptr;
    string host = peer.substr(0, pos);
    int port = atoi(peer.substr(pos + 1).c_str());
    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, host.c_str(), &serv.sin_addr) <= 0) return nullptr;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return nullptr;
    set_io_timeout(s);
    if (!connect_with_timeout(s, serv)) {
        close(s);
        return nullptr;
    }
    SSL *ssl = tls_connect_peer(s, key);
    if (!ssl) {
        vlog("[DOWNLOADER] " + peer + " did not prove to be the peer the tracker named");
        return nullptr;
    }
    if (!send_all(ssl, req.c_str(), req.size())) {
        tls_close(ssl);
        return nullptr;
    }
    return ssl;
}

// Asks a peer which pieces of the file it has
static bool fetch_bitfield(const string &peer, const string &key, const string &group, const string &filename,
                           size_t num_pieces, vector<bool> &have) {
    SSL *s = send_request(peer, key, "GET_BITFIELD " + filename + " " + group + "\n");
    if (!s) return false;
    uint32_t netlen = 0;
    string bits;
    bool ok = recv_all(s, (char*)&netlen, sizeof(netlen)) && ntohl(netlen) == num_pieces;
    if (ok) {
        bits.resize(num_pieces);
        ok = num_pieces == 0 || recv_all(s, &bits[0], bits.size());
    }
    tls_close(s);
    if (!ok) return false;
    have.assign(num_pieces, false);
    for (size_t i = 0; i < num_pieces; ++i) have[i] = (bits[i] == '1');
    return true;
}

// Fetches one piece from one peer and checks its length and SHA-1
static bool fetch_piece(const string &peer, const string &key, const string &group, const string &filename,
                        int index, size_t expected_len, const string &expected_hash, string &piece) {
    string what = "piece " + to_string(index) + " of '" + filename + "' from " + peer;
    SSL *s = send_request(peer, key, "GET_PIECE " + filename + " " + to_string(index) + " " + group + "\n");
    if (!s) { vlog("[DOWNLOADER] Cannot reach peer for " + what); return false; }
    uint32_t netlen = 0;
    bool ok = recv_all(s, (char*)&netlen, sizeof(netlen)) && ntohl(netlen) == expected_len;
    if (ok) {
        piece.resize(expected_len);
        ok = recv_all(s, &piece[0], expected_len);
    }
    tls_close(s);
    if (!ok) { vlog("[DOWNLOADER] Failed to receive " + what); return false; }
    if (sha1_hex(piece.data(), piece.size()) != expected_hash) {
        vlog("[DOWNLOADER] Hash mismatch for " + what);
        return false;
    }
    vlog("[DOWNLOADER] Got " + what);
    return true;
}

static bool write_piece(int fd, const string &piece, long long offset) {
    size_t written = 0;
    while (written < piece.size()) {
        ssize_t n = pwrite(fd, piece.data() + written, piece.size() - written, offset + (long long)written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        written += n;
    }
    return true;
}

// Asks every live peer for its current bitfield. Peers that are themselves
// still downloading gain pieces over time, so this is repeated when needed.
static void query_bitfields(const shared_ptr<DownloadJob> &job) {
    lock_guard<mutex> rl(job->refresh_mtx);
    auto now = chrono::steady_clock::now();
    if (now - job->last_refresh < chrono::milliseconds(500)) return;

    // The addresses and keys of the peers never change after the download
    // has started, so they are read without the lock.
    vector<size_t> targets;
    {
        lock_guard<mutex> lg(job->mtx);
        for (size_t i = 0; i < job->peers.size(); ++i) {
            if (!job->peers[i].dead) targets.push_back(i);
        }
    }
    size_t n = job->hashes.size();
    for (size_t t : targets) {
        if (job->cancel) break;
        vector<bool> bits;
        bool ok = fetch_bitfield(job->peers[t].addr, job->peers[t].key, job->group, job->filename, n, bits);
        lock_guard<mutex> lg(job->mtx);
        PeerState &p = job->peers[t];
        if (ok) {
            p.have = bits;
        } else {
            p.have.assign(n, false);
            if (++p.failures >= PEER_MAX_FAILURES) p.dead = true;
        }
    }
    job->last_refresh = chrono::steady_clock::now();
}

// Caller holds job.mtx. Of the peers that have the piece, picks the one with
// the fewest requests in flight so the load is spread over all of them.
static int pick_peer(DownloadJob &job, int piece) {
    int best = -1;
    for (size_t i = 0; i < job.peers.size(); ++i) {
        PeerState &p = job.peers[i];
        if (p.dead || !p.have[piece]) continue;
        if (best < 0 || p.active < job.peers[best].active) best = (int)i;
    }
    return best;
}

static void finish_download(const shared_ptr<DownloadJob> &job) {
    bool ok;
    {
        lock_guard<mutex> lg(job->mtx);
        ok = !job->failed && !job->cancel && job->done == (int)job->hashes.size();
    }
    if (ok) {
        size_t got_size = 0;
        string got_hash;
        vector<string> unused;
        ok = compute_file_hashes(job->tmp, got_size, got_hash, unused) &&
             (long long)got_size == job->filesize && got_hash == job->file_sha1;
        if (!ok) say("[DOWNLOADER] File SHA1 mismatch after download of " + job->filename);
    }
    if (ok && rename(job->tmp.c_str(), job->dest.c_str()) != 0) {
        perror("[DOWNLOADER] rename");
        ok = false;
    }
    if (ok) {
        // From here on the file is seeded from its final place
        char buf[PATH_MAX];
        share_file(job->group, job->filename, realpath(job->dest.c_str(), buf) ? string(buf) : job->dest);
    }
    {
        lock_guard<mutex> lg(job->mtx);
        close(job->fd);
        job->fd = -1;
        job->state = ok ? 'C' : 'F';
        job->active = false;
    }
    if (!ok) remove(job->tmp.c_str());
    if (ok) say("[DOWNLOADER] Completed " + job->filename + " -> " + job->dest);
    else say("[DOWNLOADER] Download of " + job->filename + (job->cancel ? " cancelled" : " failed"));

    if (job->on_finish) job->on_finish(ok);
    {
        lock_guard<mutex> lg(g_jobs_mtx);
        --g_running;
    }
    g_jobs_cv.notify_all();
}

// Downloads one piece, then queues itself again. PIECES_IN_PARALLEL of these
// chains run per file; going back through the pool's queue after every piece
// lets several downloads share the worker threads fairly.
static void piece_task(shared_ptr<DownloadJob> job) {
    int piece = -1, p = -1;
    string addr, key;
    bool stop = false;
    {
        lock_guard<mutex> lg(job->mtx);
        if (job->failed || job->cancel || job->pending.empty()) {
            stop = true;
        } else {
            piece = job->pending.front();
            job->pending.pop_front();
            p = pick_peer(*job, piece);
            if (p >= 0) { job->peers[p].active++; addr = job->peers[p].addr; key = job->peers[p].key; }
        }
    }
    if (stop) {
        bool last;
        {
            lock_guard<mutex> lg(job->mtx);
            last = (--job->chains == 0);
        }
        if (last) finish_download(job);
        return;
    }

    bool ok = false;
    if (p >= 0) {
        string data;
        bool fetched = fetch_piece(addr, key, job->group, job->filename, piece,
                                   piece_len(job->filesize, piece), job->hashes[piece], data);
        bool written = fetched && write_piece(job->fd, data, piece * (long long)PIECE_SIZE);
        lock_guard<mutex> lg(job->mtx);
        PeerState &peer = job->peers[p];
        peer.active--;
        if (written) {
            peer.failures = 0;
            job->have[piece] = 1;
            job->done++;
            ok = true;
        } else if (fetched) {
            say("[DOWNLOADER] Cannot write to " + job->tmp);
            job->failed = true;
        } else {
            // Do not ask this peer for this piece again; drop it after repeated failures
            peer.have[piece] = false;
            if (++peer.failures >= PEER_MAX_FAILURES) peer.dead = true;
        }
    }

    if (!ok) {
        bool wait = false;
        {
            lock_guard<mutex> lg(job->mtx);
            bool any_alive = false;
            for (auto &peer : job->peers) if (!peer.dead) any_alive = true;
            if (!any_alive || ++job->attempts[piece] > (int)job->peers.size() + PIECE_EXTRA_ATTEMPTS) {
                job->failed = true;
            } else if (!job->failed) {
                job->pending.push_back(piece);
                wait = pick_peer(*job, piece) < 0;
            }
        }
        if (wait) {
            // Nobody has this piece right now: give the peers that are still
            // downloading a moment, then see what they have.
            for (int i = 0; i < 10 && !job->cancel; ++i) this_thread::sleep_for(chrono::milliseconds(100));
            query_bitfields(job);
        }
    }
    download_pool().submit([job] { piece_task(job); });
}

// First task of a download: learns who has what and orders the pieces
static void run_download(shared_ptr<DownloadJob> job) {
    query_bitfields(job);

    int num_pieces = (int)job->hashes.size();
    int chains;
    {
        lock_guard<mutex> lg(job->mtx);
        // Rarest first: pieces held by the fewest peers are fetched first so
        // they get replicated before their few holders can leave. Ties are
        // broken randomly so that downloaders do not all want the same piece.
        vector<int> avail(num_pieces, 0);
        for (auto &p : job->peers) {
            for (int i = 0; i < num_pieces; ++i) if (p.have[i]) avail[i]++;
        }
        vector<int> order(num_pieces);
        for (int i = 0; i < num_pieces; ++i) order[i] = i;
        mt19937 rng(random_device{}());
        shuffle(order.begin(), order.end(), rng);
        // Pieces nobody has yet go last; a peer may get them in the meantime
        stable_sort(order.begin(), order.end(), [&avail](int a, int b) {
            int ka = avail[a] == 0 ? INT_MAX : avail[a];
            int kb = avail[b] == 0 ? INT_MAX : avail[b];
            return ka < kb;
        });
        job->pending.assign(order.begin(), order.end());
        chains = min(PIECES_IN_PARALLEL, num_pieces);
        job->chains = chains;
    }
    if (chains == 0) { finish_download(job); return; }
    for (int i = 0; i < chains; ++i) download_pool().submit([job] { piece_task(job); });
}

bool start_download(const string &group,
                    const string &filename,
                    const string &dest,
                    long long filesize,
                    const string &file_sha1,
                    const vector<string> &piece_hashes,
                    const vector<string> &peers,
                    function<void(bool)> on_finish,
                    string &err) {
    long long num_pieces = (long long)piece_hashes.size();
    if (filesize < 0 || num_pieces > MAX_PIECES ||
        num_pieces != (filesize + (long long)PIECE_SIZE - 1) / (long long)PIECE_SIZE) {
        err = "Piece count does not match file size";
        return false;
    }
    // Each peer is "<ip>:<port> <key fingerprint>". One without a fingerprint
    // could not be told from an impostor and is left out.
    vector<PeerState> known;
    for (auto &line : peers) {
        PeerState p;
        istringstream iss(line);
        if (!(iss >> p.addr >> p.key)) continue;
        p.have.assign((size_t)num_pieces, false);
        p.active = 0; p.failures = 0; p.dead = false;
        known.push_back(p);
    }
    if (known.empty()) { err = "No peers available"; return false; }
    if (is_shared(filename)) { err = "You already have " + filename; return false; }
    if (find_active(filename)) { err = filename + " is already being downloaded"; return false; }

    auto job = make_shared<DownloadJob>();
    job->group = group;
    job->filename = filename;
    job->dest = dest;
    // Assemble in a temporary file so a failed download never leaves a
    // half-written file behind.
    job->tmp = dest + ".part";
    job->file_sha1 = file_sha1;
    job->filesize = filesize;
    job->hashes = piece_hashes;
    job->on_finish = on_finish;
    job->active = true;
    job->cancel = false;
    job->peers = known;
    job->have.assign((size_t)num_pieces, 0);
    job->attempts.assign((size_t)num_pieces, 0);
    job->done = 0;
    job->chains = 0;
    job->failed = false;
    job->state = 'D';
    job->fd = open(job->tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (job->fd < 0 || ftruncate(job->fd, filesize) != 0) {
        err = "Cannot create " + job->tmp + ": " + strerror(errno);
        if (job->fd >= 0) { close(job->fd); remove(job->tmp.c_str()); }
        return false;
    }
    {
        lock_guard<mutex> lg(g_jobs_mtx);
        g_jobs.push_back(job);
        ++g_running;
    }
    download_pool().submit([job] { run_download(job); });
    return true;
}

bool is_downloading(const string &filename) {
    return find_active(filename) != nullptr;
}

vector<DownloadStatus> download_status() {
    vector<shared_ptr<DownloadJob>> jobs;
    {
        lock_guard<mutex> lg(g_jobs_mtx);
        jobs = g_jobs;
    }
    vector<DownloadStatus> out;
    for (auto &job : jobs) {
        lock_guard<mutex> lg(job->mtx);
        DownloadStatus st;
        st.group = job->group;
        st.filename = job->filename;
        st.state = job->state;
        st.done = job->done;
        st.total = (int)job->hashes.size();
        out.push_back(st);
    }
    return out;
}

void cancel_downloads() {
    unique_lock<mutex> lg(g_jobs_mtx);
    for (auto &job : g_jobs) if (job->active) job->cancel = true;
    g_jobs_cv.wait(lg, [] { return g_running == 0; });
}
