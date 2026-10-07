#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <mutex>
#include <cctype>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "peer.h"
#include "fileutils.h"
#include "tls.h"
#include "console.h"
using namespace std;

struct Endpoint {
    string host;
    int port;
};

// The tracker connection is shared with the download threads, which report
// failed downloads. g_tracker_mtx keeps each request/reply exchange together
// and guards everything below.
static mutex g_tracker_mtx;
static vector<Endpoint> g_trackers;
static size_t g_current = 0;      // tracker in use (index into g_trackers)
static SSL *g_conn = nullptr;     // TLS connection to that tracker
static bool g_closed = false;     // the program is exiting: no more requests
static string g_user, g_pass;     // session to restore after switching trackers
static string g_my_addr;          // <ip>:<port> other peers reach this client on

vector<string> split_words(const string &s) {
    vector<string> v; string w; istringstream ss(s);
    while (ss >> w) v.push_back(w);
    return v;
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
        s.push_back(c);
    }
    return true;
}

bool write_to_socket(SSL *ssl, const string &line) {
    string msg = line + "\n";
    const char *buf = msg.c_str();
    size_t total = msg.size();
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

// Sends one command and collects the reply lines up to the "END" terminator
static bool exchange(SSL *conn, const string &line, vector<string> &resp_lines) {
    resp_lines.clear();
    if (!write_to_socket(conn, line)) return false;
    string r;
    while (true) {
        if (!read_from_socket(conn, r)) return false;
        if (r == "END") break;
        resp_lines.push_back(r);
    }
    return true;
}

static bool has_reply(const vector<string> &resp_lines, const string &text) {
    for (auto &l : resp_lines) if (l == text) return true;
    return false;
}

static int connect_to(const Endpoint &ep) {
    addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(ep.host.c_str(), nullptr, &hints, &res) != 0 || !res) return -1;
    sockaddr_in serv;
    memcpy(&serv, res->ai_addr, sizeof(serv));
    serv.sin_port = htons(ep.port);
    freeaddrinfo(res);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    if (connect(sock, (sockaddr*)&serv, sizeof(serv)) < 0) { close(sock); return -1; }
    return sock;
}

// Tells the tracker where other peers reach this client and which key the
// client proves itself with.
static string register_addr(const string &user) {
    return "REGISTER_ADDR " + user + " " + g_my_addr + " " + tls_my_fingerprint();
}

// Caller holds g_tracker_mtx. Connects to the first tracker that answers,
// starting with the one used last, and logs the user back in on it.
static bool connect_any() {
    for (size_t i = 0; i < g_trackers.size(); ++i) {
        size_t idx = (g_current + i) % g_trackers.size();
        int sock = connect_to(g_trackers[idx]);
        if (sock < 0) continue;
        // Whatever answers on the tracker's address without the tracker's
        // key is not the tracker: nothing is sent to it.
        SSL *conn = tls_connect_tracker(sock);
        if (!conn) {
            Out() << "TLS handshake with " << g_trackers[idx].host << ":" << g_trackers[idx].port
                 << " failed (it did not prove to be a tracker), skipping it\n";
            continue;
        }
        if (!g_user.empty()) {
            // Both trackers hold the same state, so the session carries over
            vector<string> resp;
            if (!exchange(conn, "LOGIN " + g_user + " " + g_pass, resp)) { tls_close(conn); continue; }
            if (has_reply(resp, "Login successful")) {
                if (!exchange(conn, register_addr(g_user), resp)) { tls_close(conn); continue; }
            } else {
                Out() << "Session could not be restored, please login again\n";
                g_user.clear();
                g_pass.clear();
            }
        }
        g_current = idx;
        g_conn = conn;
        Out() << "Connected to tracker " << g_trackers[idx].host << ":" << g_trackers[idx].port << endl;
        return true;
    }
    return false;
}

// Sends a command to the tracker. If the tracker is gone it switches to the
// other one and sends the command again. Returns false if no tracker answers.
static bool tracker_request(const string &line, vector<string> &resp_lines) {
    lock_guard<mutex> lock(g_tracker_mtx);
    if (g_closed) { resp_lines.clear(); return false; }
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!g_conn && !connect_any()) break;
        if (exchange(g_conn, line, resp_lines)) return true;
        tls_close(g_conn);
        g_conn = nullptr;
        Out() << "Lost connection to tracker, switching...\n";
    }
    resp_lines.clear();
    Out() << "No tracker reachable; try again later\n";
    return false;
}

// The seeder's question before it serves a request: is the client that
// holds this key a member of this group? Only the tracker can tell.
static bool may_download(const string &group, const string &key) {
    vector<string> resp;
    return tracker_request("CHECK_ACCESS " + group + " " + key, resp) && has_reply(resp, "Allowed");
}

static string session_user() {
    lock_guard<mutex> lock(g_tracker_mtx);
    return g_user;
}

static void set_session(const string &user, const string &pass) {
    lock_guard<mutex> lock(g_tracker_mtx);
    g_user = user;
    g_pass = pass;
}

static string base_name(const string &path) {
    size_t pos = path.rfind('/');
    return pos == string::npos ? path : path.substr(pos + 1);
}

static string absolute_path(const string &path) {
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf)) return buf;
    return path;
}

static bool is_sha1(const string &s) {
    if (s.size() != 40) return false;
    for (unsigned char ch : s) if (!isxdigit(ch)) return false;
    return true;
}

// Parses "<host>:<port>" (or "<host> <port>")
static bool parse_endpoint(const string &text, Endpoint &ep) {
    string s = text;
    for (auto &ch : s) if (ch == ':') ch = ' ';
    istringstream iss(s);
    string port, extra;
    if (!(iss >> ep.host >> port) || (iss >> extra)) return false;
    if (port.size() > 5) return false;
    for (unsigned char ch : port) if (!isdigit(ch)) return false;
    ep.port = atoi(port.c_str());
    return ep.port >= 1 && ep.port <= 65535;
}

static bool load_trackers(const string &path) {
    ifstream f(path);
    if (!f) return false;
    string line;
    while (getline(f, line)) {
        Endpoint ep;
        if (parse_endpoint(line, ep)) g_trackers.push_back(ep);
    }
    return !g_trackers.empty();
}

static void print_help() {
    Out() <<
        "Commands:\n"
        "  create_user <user_id> <password>\n"
        "  login <user_id> <password>\n"
        "  logout\n"
        "  create_group <group_id>\n"
        "  join_group <group_id>\n"
        "  leave_group <group_id>\n"
        "  list_groups\n"
        "  list_requests <group_id>                 (group owner)\n"
        "  accept_request <group_id> <user_id>      (group owner)\n"
        "  upload_file <group_id> <file_path>\n"
        "  list_files <group_id>\n"
        "  download_file <group_id> <file_name> <destination_path>\n"
        "  show_downloads\n"
        "  stop_share <group_id> <file_name>\n"
        "  verbose on|off                           (log every piece transfer)\n"
        "  help, quit\n";
}

static void show_downloads() {
    vector<DownloadStatus> downloads = download_status();
    if (downloads.empty()) Out() << "No downloads yet\n";
    for (auto &d : downloads) {
        ostringstream line;
        line << "[" << d.state << "] [" << d.group << "] " << d.filename;
        if (d.state == 'D') {
            int percent = d.total > 0 ? (int)(100LL * d.done / d.total) : 0;
            line << "  " << d.done << "/" << d.total << " pieces (" << percent << "%)";
        }
        Out() << line.str() << endl;
    }
}

// Handles the reply to DOWNLOAD_FILE: registers as a seeder and starts the
// download in the background.
static void begin_download(const string &group, const string &user, const string &dest_arg,
                           const vector<string> &resp_lines) {
    // Anything that is not "<name> <size> <sha1> <num_pieces>" is an error
    // message from the tracker.
    if (resp_lines.empty()) return;
    vector<string> hdr = split_words(resp_lines[0]);
    if (hdr.size() != 4 || !is_sha1(hdr[2])) {
        for (auto &l : resp_lines) Out() << l << endl;
        return;
    }
    string filename = hdr[0];
    string file_sha1 = hdr[2];
    long long filesize = 0;
    int num_pieces = 0;
    try { filesize = stoll(hdr[1]); num_pieces = stoi(hdr[3]); }
    catch (...) { Out(cerr) << "Malformed reply from tracker\n"; return; }
    if (filename.find('/') != string::npos || num_pieces < 0 || (int)resp_lines.size() < 1 + num_pieces) {
        Out(cerr) << "Malformed reply from tracker\n";
        return;
    }
    vector<string> piece_hashes(resp_lines.begin() + 1, resp_lines.begin() + 1 + num_pieces);
    vector<string> peers(resp_lines.begin() + 1 + num_pieces, resp_lines.end());
    if (peers.empty()) { Out() << "No peers are sharing " << filename << " right now\n"; return; }
    if (is_shared(filename)) { Out() << "You already have " << filename << "\n"; return; }
    if (is_downloading(filename)) { Out() << filename << " is already being downloaded\n"; return; }

    // The destination may be a directory or the full path of the new file
    string dest = dest_arg;
    struct stat st;
    if (stat(dest.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        if (dest.back() != '/') dest += "/";
        dest += filename;
    }

    // Register as a seeder right away: pieces are shared with other peers as
    // soon as they are verified, not only once the file is complete.
    ostringstream ss;
    ss << "UPLOAD_FILE " << group << " " << user << " " << filename
       << " " << filesize << " " << file_sha1 << " " << piece_hashes.size();
    for (auto &h : piece_hashes) ss << " " << h;
    vector<string> resp;
    if (!tracker_request(ss.str(), resp)) return;
    if (!has_reply(resp, "File uploaded")) {
        for (auto &l : resp) Out() << l << endl;
        return;
    }

    string stop_share = "STOP_SHARE " + group + " " + filename + " " + user;
    auto on_finish = [stop_share](bool ok) {
        // A download that did not complete has nothing to seed
        vector<string> ignored;
        if (!ok) tracker_request(stop_share, ignored);
    };
    string err;
    if (!start_download(group, filename, dest, filesize, file_sha1, piece_hashes, peers, on_finish, err)) {
        Out() << "Cannot start download: " << err << "\n";
        tracker_request(stop_share, resp);
        return;
    }
    Out() << "Downloading " << filename << " (" << filesize << " bytes, " << num_pieces << " pieces) from "
         << peers.size() << " peer(s) -> " << dest << "\n"
         << "Use show_downloads to see the progress\n";
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        Out(cerr) << "Usage: " << argv[0] << " <IP>:<PORT> tracker_info.txt\n";
        return 1;
    }
    Endpoint me;
    in_addr ip;
    if (!parse_endpoint(argv[1], me) || inet_pton(AF_INET, me.host.c_str(), &ip) != 1) {
        Out(cerr) << "Bad client address, expected <IP>:<PORT>\n";
        return 1;
    }
    g_my_addr = me.host + ":" + to_string(me.port);
    if (!load_trackers(argv[2])) {
        Out(cerr) << "Cannot read tracker addresses from " << argv[2] << "\n";
        return 1;
    }
    // A peer or tracker closing its socket mid-write must not kill the client
    signal(SIGPIPE, SIG_IGN);

    // OpenSSL sets itself up on first use; do that here rather than in a
    // worker thread, whose per-thread OpenSSL state would outlive the cleanup.
    sha1_hex("", 0);

    // The certificate of the authority that signs the trackers' certificates
    // is kept next to tracker_info.txt
    string info_path = argv[2];
    size_t slash = info_path.rfind('/');
    string cert_dir = slash == string::npos ? "" : info_path.substr(0, slash + 1);
    if (!tls_init(cert_dir + "ca_cert.pem")) { tls_cleanup(); return 1; }

    // Other peers fetch pieces from this address
    if (!start_seeder(me.port, may_download)) { tls_cleanup(); return 1; }
    {
        lock_guard<mutex> lock(g_tracker_mtx);
        if (!connect_any()) { Out(cerr) << "No tracker reachable\n"; stop_seeder(); tls_cleanup(); return 1; }
    }
    Out() << "Type help for the list of commands\n";

    string line;
    while (true) {
        string user = session_user();
        Out() << (user.empty() ? "> " : user + "> ") << flush;
        if (!getline(cin, line)) break;

        vector<string> parts = split_words(line);
        if (parts.empty()) continue;
        string cmd = parts[0];
        for (auto &x : cmd) x = tolower((unsigned char)x);
        size_t args = parts.size() - 1;

        if (cmd == "quit" || cmd == "exit") break;
        if (cmd == "help") { print_help(); continue; }
        if (cmd == "show_downloads") { show_downloads(); continue; }
        if (cmd == "verbose") {
            string arg = args == 1 ? parts[1] : "";
            for (auto &x : arg) x = tolower((unsigned char)x);
            if (arg != "on" && arg != "off") { Out() << "Usage: verbose on|off\n"; continue; }
            set_verbose(arg == "on");
            continue;
        }

        // Translate the command into the tracker's protocol, which names the
        // acting user explicitly (the trackers also exchange and log it that way).
        string request;
        string upload_name, upload_path;
        vector<string> resp_lines;

        if (cmd == "create_user") {
            if (args != 2) { Out() << "Usage: create_user <user_id> <password>\n"; continue; }
            if (!user.empty()) { Out() << "Logout first\n"; continue; }
            request = "CREATE_USER " + parts[1] + " " + parts[2];
        } else if (cmd == "login") {
            if (args != 2) { Out() << "Usage: login <user_id> <password>\n"; continue; }
            if (!user.empty()) { Out() << "Already logged in as " << user << "; logout first\n"; continue; }
            request = "LOGIN " + parts[1] + " " + parts[2];
        } else if (user.empty()) {
            Out() << "Please login first (type help for the list of commands)\n";
            continue;
        } else if (cmd == "logout") {
            if (args != 0) { Out() << "Usage: logout\n"; continue; }
            // Downloads belong to the session; unfinished ones are withdrawn
            // from the tracker while this user is still logged in.
            cancel_downloads();
            request = "LOGOUT " + user;
        } else if (cmd == "create_group") {
            if (args != 1) { Out() << "Usage: create_group <group_id>\n"; continue; }
            request = "CREATE_GROUP " + parts[1] + " " + user;
        } else if (cmd == "join_group") {
            if (args != 1) { Out() << "Usage: join_group <group_id>\n"; continue; }
            request = "JOIN_GROUP " + parts[1] + " " + user;
        } else if (cmd == "leave_group") {
            if (args != 1) { Out() << "Usage: leave_group <group_id>\n"; continue; }
            request = "LEAVE_GROUP " + parts[1] + " " + user;
        } else if (cmd == "list_groups") {
            request = "LIST_GROUPS";
        } else if (cmd == "list_requests") {
            if (args != 1) { Out() << "Usage: list_requests <group_id>\n"; continue; }
            request = "LIST_REQUESTS " + parts[1];
        } else if (cmd == "accept_request") {
            if (args != 2) { Out() << "Usage: accept_request <group_id> <user_id>\n"; continue; }
            request = "ACCEPT_REQUEST " + parts[1] + " " + parts[2];
        } else if (cmd == "list_files") {
            if (args != 1) { Out() << "Usage: list_files <group_id>\n"; continue; }
            request = "LIST_FILES " + parts[1];
        } else if (cmd == "stop_share") {
            if (args != 2) { Out() << "Usage: stop_share <group_id> <file_name>\n"; continue; }
            request = "STOP_SHARE " + parts[1] + " " + parts[2] + " " + user;
        } else if (cmd == "download_file") {
            if (args != 3) { Out() << "Usage: download_file <group_id> <file_name> <destination_path>\n"; continue; }
            request = "DOWNLOAD_FILE " + parts[1] + " " + parts[2];
        } else if (cmd == "upload_file") {
            if (args != 2) { Out() << "Usage: upload_file <group_id> <file_path>\n"; continue; }
            size_t fsize = 0;
            string full_hash;
            vector<string> piece_hashes;
            if (!compute_file_hashes(parts[2], fsize, full_hash, piece_hashes)) {
                Out() << "Cannot read file: " << parts[2] << endl;
                continue;
            }
            // The tracker and other peers only ever see the file's base name
            upload_name = base_name(parts[2]);
            upload_path = absolute_path(parts[2]);
            ostringstream ss;
            ss << "UPLOAD_FILE " << parts[1] << " " << user << " " << upload_name
               << " " << fsize << " " << full_hash << " " << piece_hashes.size();
            for (auto &h : piece_hashes) ss << " " << h;
            request = ss.str();
        } else {
            Out() << "Unknown command: " << parts[0] << " (type help for the list of commands)\n";
            continue;
        }

        if (!tracker_request(request, resp_lines)) continue;

        if (has_reply(resp_lines, "Not logged in")) {
            // The tracker dropped this session (the user logged in elsewhere)
            Out() << "Session ended on the tracker, please login again\n";
            cancel_downloads();
            set_session("", "");
            clear_shared_files();
            continue;
        }

        if (cmd == "download_file") {
            begin_download(parts[1], user, parts[3], resp_lines);
            continue;
        }
        for (auto &l : resp_lines) Out() << l << endl;

        if (cmd == "login" && has_reply(resp_lines, "Login successful")) {
            set_session(parts[1], parts[2]);
            load_shared_files(parts[1]);
            // Tell the tracker where other peers can reach this client
            if (tracker_request(register_addr(parts[1]), resp_lines) &&
                !has_reply(resp_lines, "Addr registered")) {
                for (auto &l : resp_lines) Out() << l << endl;
            }
        } else if (cmd == "logout" && has_reply(resp_lines, "Logout successful")) {
            set_session("", "");
            clear_shared_files();
        } else if (cmd == "upload_file" && has_reply(resp_lines, "File uploaded")) {
            share_file(parts[1], upload_name, upload_path);
        } else if (cmd == "stop_share" && has_reply(resp_lines, "Stopped sharing")) {
            unshare_file(parts[1], parts[2]);
        }
    }

    cancel_downloads();
    stop_seeder();
    {
        lock_guard<mutex> lock(g_tracker_mtx);
        tls_close(g_conn);
        g_conn = nullptr;
        // Seeder workers may still be finishing a request
        g_closed = true;
    }
    tls_cleanup();
    return 0;
}
