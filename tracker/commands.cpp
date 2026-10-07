#include "commands.h"
#include "password.h"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <sstream>
#include <fstream>
#include <mutex>
#include <iostream>
#include <cctype>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
using namespace std;

// Must match PIECE_SIZE in client/fileutils.h
static const long long PIECE_SIZE = 512 * 1024;

struct User {
    string pass;   // hash of the password (see password.h), never the password
    bool logged = false;

    User() {}
    User(const string &p, bool l) : pass(p), logged(l) {}
};

struct FileMeta {
    string owner;
    string group;
    string filename;
    long long filesize;
    string file_sha1;
    vector<string> pieces;
    set<string> seeders;
};

static map<string, User> users;
static map<string, string> groups;
static map<string, set<string>> members;
static map<string, vector<string>> requests;
static mutex state_lock;

static map<string, vector<FileMeta>> group_files;
static map<string, string> seeder_addr;

// Every state-changing operation gets a sequence number from the tracker
// that accepted it and is logged as "<tracker_no> <seq> <command>". The
// numbers let a tracker tell which of the peer's operations it has already
// applied, so operations can be resent after the two were disconnected.
static int my_no = 1;
static long long my_seq = 0;      // last sequence number this tracker issued
static long long peer_seq = 0;    // peer operations 1..peer_seq are all applied
static set<long long> peer_ahead; // applied peer operations beyond peer_seq

// Sessions of the clients connected to this tracker
static map<int, string> conn_user;
static map<string, int> user_conn;

static vector<string> split_words(const string &line) {
    vector<string> v;
    istringstream ss(line);
    string w;
    while (ss >> w) v.push_back(w);
    return v;
}

// Both trackers append to the same oplog.txt, so every op goes out in a single
// write() on an O_APPEND fd to keep lines from interleaving.
static void save_op(const string &op) {
    int fd = open("oplog.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { perror("oplog.txt"); return; }
    string line = op + "\n";
    if (write(fd, line.data(), line.size()) != (ssize_t)line.size()) perror("oplog.txt write");
    close(fd);
}

static bool is_sha1(const string &s) {
    if (s.size() != 40) return false;
    for (unsigned char ch : s) if (!isxdigit(ch)) return false;
    return true;
}

static bool is_sha256(const string &s) {
    if (s.size() != 64) return false;
    for (unsigned char ch : s) if (!isxdigit(ch)) return false;
    return true;
}

static bool valid_addr(const string &addr) {
    size_t pos = addr.rfind(':');
    if (pos == string::npos || pos == 0) return false;
    string port = addr.substr(pos + 1);
    if (port.empty() || port.size() > 5) return false;
    for (unsigned char ch : port) if (!isdigit(ch)) return false;
    int p = stoi(port);
    if (p < 1 || p > 65535) return false;
    in_addr ip;
    return inet_pton(AF_INET, addr.substr(0, pos).c_str(), &ip) == 1;
}

static void drop_session(const string &user) {
    auto it = user_conn.find(user);
    if (it == user_conn.end()) return;
    conn_user.erase(it->second);
    user_conn.erase(it);
}

static string denied(const string &me) {
    return "Permission denied: you are logged in as " + me;
}

static string run_command(const string &cmd, int conn, bool &changed) {
    vector<string> t = split_words(cmd);
    if (t.empty()) return "Empty command";
    string c = t[0];
    for (auto &x : c) x = toupper((unsigned char)x);

    // A client may only act as the user logged in on its own connection
    bool client = (conn != PEER_CONN);
    string me;
    if (client && conn_user.count(conn)) me = conn_user[conn];

    if (c == "CREATE_USER") {
        if (t.size() < 3) return "Usage: CREATE_USER <u> <p>";
        if (client && !me.empty()) return "Already logged in as " + me;
        // "END" terminates a reply, so it cannot appear alone on a reply line
        if (t[1] == "END") return "Invalid user name";
        if (users.count(t[1])) return "User exists";
        users[t[1]] = {t[2], false};
        changed = true;
        return "User created";
    }
    if (c == "LOGIN") {
        // From a client the third word is the hash of the password it sent
        // (process_command put it there). The other tracker and the oplog
        // only say that the login happened.
        if (t.size() < (client ? 3u : 2u)) return "Usage: LOGIN <u> <p>";
        if (client && !me.empty()) return "Already logged in as " + me;
        if (!users.count(t[1])) return "Login failed";
        if (client && !same_hash(users[t[1]].pass, t[2])) return "Login failed";
        // A new login replaces any older session of the same user (for example
        // one left behind by a tracker restart), which has to register again.
        drop_session(t[1]);
        seeder_addr.erase(t[1]);
        users[t[1]].logged = true;
        if (client) { conn_user[conn] = t[1]; user_conn[t[1]] = conn; }
        changed = true;
        return "Login successful";
    }

    if (client && me.empty()) return "Not logged in";

    if (c == "LOGOUT") {
        if (t.size() < 2) return "Usage: LOGOUT <u>";
        if (client && me != t[1]) return denied(me);
        if (!users.count(t[1])) return "No user";
        if (!users[t[1]].logged) return "Not logged";
        users[t[1]].logged = false;
        seeder_addr.erase(t[1]);
        drop_session(t[1]);
        changed = true;
        return "Logout successful";
    }
    if (c == "REGISTER_ADDR") {
        if (t.size() < 4) return "Usage: REGISTER_ADDR <u> <ip:port> <key_fingerprint>";
        if (client && me != t[1]) return denied(me);
        if (!users.count(t[1])) return "No user";
        if (!users[t[1]].logged) return "User not logged in";
        if (!valid_addr(t[2])) return "Invalid address, expected <ip:port>";
        if (!is_sha256(t[3])) return "Invalid key fingerprint";
        // The fingerprint goes out with the address, so that a downloader can
        // tell this client from anyone else answering on that address.
        seeder_addr[t[1]] = t[2] + " " + t[3];
        changed = true;
        return "Addr registered";
    }
    if (c == "CREATE_GROUP") {
        if (t.size() < 3) return "Usage: CREATE_GROUP <g> <o>";
        if (client && me != t[2]) return denied(me);
        if (t[1] == "END") return "Invalid group name";
        if (!users.count(t[2])) return "No user";
        if (groups.count(t[1])) return "Group exists";
        groups[t[1]] = t[2];
        members[t[1]].insert(t[2]);
        changed = true;
        return "Group created";
    }
    if (c == "JOIN_GROUP") {
        if (t.size() < 3) return "Usage: JOIN_GROUP <g> <u>";
        if (client && me != t[2]) return denied(me);
        if (!groups.count(t[1])) return "No group";
        if (!users.count(t[2])) return "No user";
        if (members[t[1]].count(t[2])) return "Already a member";
        for (auto &u : requests[t[1]]) if (u == t[2]) return "Already requested";
        requests[t[1]].push_back(t[2]);
        changed = true;
        return "Request sent";
    }
    if (c == "LEAVE_GROUP") {
        if (t.size() < 3) return "Usage: LEAVE_GROUP <g> <u>";
        string g = t[1], u = t[2];
        if (client && me != u) return denied(me);
        if (!groups.count(g)) return "No group";
        if (!members[g].count(u)) return "Not a member";
        members[g].erase(u);
        auto &vec = group_files[g];
        for (auto it = vec.begin(); it != vec.end();) {
            it->seeders.erase(u);
            if (it->seeders.empty()) it = vec.erase(it);
            else ++it;
        }
        if (members[g].empty()) {
            groups.erase(g);
            members.erase(g);
            requests.erase(g);
            group_files.erase(g);
        } else if (groups[g] == u) {
            // The owner left: hand the group over so requests can still be accepted
            groups[g] = *members[g].begin();
        }
        changed = true;
        return "Left group";
    }
    if (c == "LIST_GROUPS") {
        if (groups.empty()) return "No groups";
        string r;
        for (auto &p : groups) r += p.first + "\n";
        return r;
    }
    if (c == "LIST_REQUESTS") {
        if (t.size() < 2) return "Usage: LIST_REQUESTS <g>";
        if (!groups.count(t[1])) return "No group";
        if (client && groups[t[1]] != me) return "Only the group owner can list requests";
        if (requests[t[1]].empty()) return "No requests";
        string r;
        for (auto &u : requests[t[1]]) r += u + "\n";
        return r;
    }
    if (c == "ACCEPT_REQUEST") {
        if (t.size() < 3) return "Usage: ACCEPT_REQUEST <g> <u>";
        if (!groups.count(t[1])) return "No group";
        if (client && groups[t[1]] != me) return "Only the group owner can accept requests";
        auto &v = requests[t[1]];
        bool ok = false;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (*it == t[2]) { v.erase(it); ok = true; break; }
        }
        if (!ok) return "No such request";
        members[t[1]].insert(t[2]);
        changed = true;
        return "Request accepted";
    }

    if (c == "UPLOAD_FILE") {
        if (t.size() < 7) return "Usage: UPLOAD_FILE <g> <u> <filename> <filesize> <file_sha1> <num_pieces> <piecesha1...>";
        string g = t[1], u = t[2], fname = t[3];
        if (client && me != u) return denied(me);
        if (!groups.count(g)) return "No group";
        if (!members[g].count(u)) return "User not in group";
        if (fname.find('/') != string::npos) return "Invalid filename";
        long long fsize = 0;
        try { fsize = stoll(t[4]); } catch(...) { return "Invalid filesize"; }
        if (fsize < 0) return "Invalid filesize";
        string file_sha1 = t[5];
        if (!is_sha1(file_sha1)) return "Invalid file hash";
        long long num_pieces = 0;
        try { num_pieces = stoll(t[6]); } catch(...) { return "Invalid num_pieces"; }
        if (num_pieces != (fsize + PIECE_SIZE - 1) / PIECE_SIZE) return "Invalid num_pieces";
        if ((long long)t.size() != 7 + num_pieces) return "Piece hashes missing";
        for (long long i = 0; i < num_pieces; ++i) {
            if (!is_sha1(t[7 + i])) return "Invalid piece hash";
        }
        auto &vec = group_files[g];
        for (auto &old : vec) {
            if (old.filename == fname) {
                if (old.file_sha1 != file_sha1) return "Filename already exists with different hash";
                if (old.seeders.count(u)) return "File uploaded";
                old.seeders.insert(u);
                changed = true;
                return "File uploaded";
            }
        }
        FileMeta fm;
        fm.owner = u; fm.group = g; fm.filename = fname; fm.filesize = fsize;
        fm.file_sha1 = file_sha1;
        fm.pieces.assign(t.begin() + 7, t.end());
        fm.seeders.insert(u);
        vec.push_back(fm);
        changed = true;
        return "File uploaded";
    }

    if (c == "LIST_FILES") {
        if (t.size() < 2) return "Usage: LIST_FILES <g>";
        string g = t[1];
        if (!groups.count(g)) return "No group";
        if (client && !members[g].count(me)) return "User not in group";
        auto &vec = group_files[g];
        if (vec.empty()) return "No files";
        string r;
        for (auto &fm : vec) r += fm.filename + " " + to_string(fm.filesize) + " " + fm.owner + "\n";
        return r;
    }

    if (c == "STOP_SHARE") {
        if (t.size() < 4) return "Usage: STOP_SHARE <g> <filename> <u>";
        string g = t[1], fname = t[2], u = t[3];
        if (client && me != u) return denied(me);
        if (!groups.count(g)) return "No group";
        auto &vec = group_files[g];
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if (it->filename == fname) {
                if (!it->seeders.count(u)) return "Not sharing this file";
                it->seeders.erase(u);
                if (it->seeders.empty()) vec.erase(it);
                changed = true;
                return "Stopped sharing";
            }
        }
        return "No such file";
    }

    if (c == "CHECK_ACCESS") {
        // A seeder asks, before it serves a request: may the client that
        // proved to hold this key have the files of this group?
        if (t.size() < 3) return "Usage: CHECK_ACCESS <g> <key_fingerprint>";
        string g = t[1];
        if (!groups.count(g)) return "Denied";
        if (client && !members[g].count(me)) return "User not in group";
        if (!is_sha256(t[2])) return "Denied";
        for (auto &u : members[g]) {
            auto it = seeder_addr.find(u);
            if (it == seeder_addr.end() || it->second.size() < t[2].size()) continue;
            // "<ip:port> <fingerprint>" of a member that is logged in
            if (it->second.compare(it->second.size() - t[2].size(), t[2].size(), t[2]) == 0) return "Allowed";
        }
        return "Denied";
    }

    if (c == "DOWNLOAD_FILE") {
        if (t.size() < 3) return "Usage: DOWNLOAD_FILE <g> <filename>";
        string g = t[1], fname = t[2];
        if (!groups.count(g)) return "No group";
        if (client && !members[g].count(me)) return "User not in group";
        auto &vec = group_files[g];
        for (auto &fm : vec) {
            if (fm.filename == fname) {
                string r;
                r += fm.filename + " " + to_string(fm.filesize) + " " + fm.file_sha1 + " " + to_string((int)fm.pieces.size()) + "\n";
                for (auto &ph : fm.pieces) r += ph + "\n";
                for (auto &seeder_user : fm.seeders) {
                    if (seeder_addr.count(seeder_user)) r += seeder_addr[seeder_user] + "\n";
                }
                return r;
            }
        }
        return "No such file";
    }

    return "Unknown command";
}

// Records a peer operation as applied. Returns false if it already was.
static bool mark_peer_op(long long seq) {
    if (seq <= peer_seq || peer_ahead.count(seq)) return false;
    peer_ahead.insert(seq);
    while (peer_ahead.count(peer_seq + 1)) peer_ahead.erase(++peer_seq);
    return true;
}

// Caller holds state_lock. Logs an operation this tracker accepted and
// returns the message that carries it to the peer.
static string record_own_op(const string &cmd) {
    ++my_seq;
    save_op(to_string(my_no) + " " + to_string(my_seq) + " " + cmd);
    return "OP " + to_string(my_seq) + " " + cmd;
}

static string stored_hash(const string &user) {
    lock_guard<mutex> lock(state_lock);
    auto it = users.find(user);
    return it == users.end() ? "" : it->second.pass;
}

string printable_command(const string &cmd) {
    vector<string> t = split_words(cmd);
    if (t.size() < 3) return cmd;
    string c = t[0];
    for (auto &x : c) x = toupper((unsigned char)x);
    if (c != "CREATE_USER" && c != "LOGIN") return cmd;
    return t[0] + " " + t[1] + " ****";
}

string process_command(const string &cmd, int conn, string &sync_msg) {
    sync_msg.clear();
    // A password goes no further than this function. The command that is
    // run carries its hash; the one that is logged and sent to the other
    // tracker carries the hash (CREATE_USER) or nothing (LOGIN). Hashing is
    // slow on purpose, so it is done before state_lock is taken.
    string run = cmd, log = cmd;
    vector<string> t = split_words(cmd);
    if (t.size() >= 3) {
        string c = t[0];
        for (auto &x : c) x = toupper((unsigned char)x);
        if (c == "CREATE_USER") {
            run = log = "CREATE_USER " + t[1] + " " + hash_password(t[2]);
        } else if (c == "LOGIN") {
            run = "LOGIN " + t[1] + " " + hash_password_like(stored_hash(t[1]), t[2]);
            log = "LOGIN " + t[1];
        }
    }
    bool changed = false;
    lock_guard<mutex> lock(state_lock);
    string res = run_command(run, conn, changed);
    if (changed) sync_msg = record_own_op(log);
    return res;
}

string end_session(int conn, string &sync_msg) {
    sync_msg.clear();
    lock_guard<mutex> lock(state_lock);
    auto it = conn_user.find(conn);
    if (it == conn_user.end()) return "";
    string op = "LOGOUT " + it->second;
    bool changed = false;
    run_command(op, conn, changed);
    if (!changed) return "";
    sync_msg = record_own_op(op);
    return op;
}

string apply_peer_op(const string &msg) {
    istringstream iss(msg);
    string tag, cmd;
    long long seq = 0;
    if (!(iss >> tag >> seq) || tag != "OP" || seq <= 0) return "Malformed sync message";
    getline(iss, cmd);
    lock_guard<mutex> lock(state_lock);
    if (!mark_peer_op(seq)) return "Already applied";
    bool changed = false;
    return run_command(cmd, PEER_CONN, changed);
}

long long peer_ops_applied() {
    lock_guard<mutex> lock(state_lock);
    return peer_seq;
}

vector<string> own_ops_after(long long seq) {
    // Holding state_lock keeps new operations out of the log while it is read
    lock_guard<mutex> lock(state_lock);
    // The peer knows more of this tracker's operations than its log holds:
    // the log was removed. Skip ahead so new operations are not taken for old ones.
    if (seq > my_seq) my_seq = seq;
    vector<string> ops;
    ifstream f("oplog.txt");
    string line;
    while (getline(f, line)) {
        istringstream iss(line);
        int no = 0;
        long long s = 0;
        string cmd;
        if (!(iss >> no >> s) || no != my_no || s <= seq) continue;
        getline(iss, cmd);
        ops.push_back("OP " + to_string(s) + cmd);
    }
    return ops;
}

void init_oplog(int tracker_no) {
    lock_guard<mutex> lock(state_lock);
    my_no = tracker_no;
    ifstream f("oplog.txt");
    string line;
    while (getline(f, line)) {
        istringstream iss(line);
        int no = 0;
        long long seq = 0;
        string cmd;
        if (!(iss >> no >> seq) || seq <= 0) continue;
        getline(iss, cmd);
        if (no == my_no) { if (seq > my_seq) my_seq = seq; }
        else mark_peer_op(seq);
        bool changed = false;
        run_command(cmd, PEER_CONN, changed);
    }
}
