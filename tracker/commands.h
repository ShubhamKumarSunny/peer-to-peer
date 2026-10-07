#ifndef COMMANDS_H
#define COMMANDS_H
#include <string>
#include <vector>

// Connection id for commands that did not come from a client (peer tracker
// sync and oplog replay). They are trusted and skip the session checks.
const int PEER_CONN = -1;

// Sets this tracker's number (1 or 2) and rebuilds the state from oplog.txt.
void init_oplog(int tracker_no);

// Runs one client command. If it changed tracker state, `sync_msg` is set to
// the message to forward to the peer tracker ("OP <seq> <command>").
std::string process_command(const std::string &cmd, int conn, std::string &sync_msg);

// The command as it may be shown on the console: without the password.
std::string printable_command(const std::string &cmd);

// Called when a client connection closes. Logs out the user that was logged in
// on it; returns the LOGOUT command that was run ("" if none).
std::string end_session(int conn, std::string &sync_msg);

// Applies an "OP <seq> <command>" message from the peer tracker. Operations
// that were already applied are skipped, so the peer may safely resend.
std::string apply_peer_op(const std::string &msg);

// Highest sequence number up to which all of the peer's operations are applied.
long long peer_ops_applied();

// This tracker's own operations with a sequence number above `seq`, as sync
// messages, read back from oplog.txt. `seq` is what the peer says it has.
std::vector<std::string> own_ops_after(long long seq);
#endif
