#ifndef PEER_H
#define PEER_H

#include <functional>
#include <string>
#include <vector>

// Starts the seeder on `port`. Returns false if the port cannot be bound.
// Before it serves a request the seeder calls `may_download(group, key)`:
// may the client that proved to hold the key with this fingerprint have the
// files of this group? (The tracker knows; see CHECK_ACCESS.)
bool start_seeder(int port, std::function<bool(const std::string &, const std::string &)> may_download);
// Stops accepting peer requests (called once, before the program exits).
void stop_seeder();

// The seeder only serves files in this table (name known to the tracker ->
// local path and the groups it is shared in) and the verified pieces of
// downloads that are still running. The table is kept per user in
// .shared_<user> so that seeding resumes after the client is restarted.
void load_shared_files(const std::string &username);
void clear_shared_files();
void share_file(const std::string &group, const std::string &name, const std::string &path);
void unshare_file(const std::string &group, const std::string &name);
bool is_shared(const std::string &name);

struct DownloadStatus {
    std::string group;
    std::string filename;
    char state;   // 'D' downloading, 'C' completed, 'F' failed
    int done;     // pieces verified so far
    int total;
};

// Starts downloading `filename` into `dest` in the background, fetching
// several pieces at a time from the given peers, each one
// "<ip>:<port> <key fingerprint>" as the tracker lists them. `on_finish` is
// called from a worker thread once the download has completed or failed.
// Returns false (and sets `err`) if the download could not be started.
bool start_download(const std::string &group,
                    const std::string &filename,
                    const std::string &dest,
                    long long filesize,
                    const std::string &file_sha1,
                    const std::vector<std::string> &piece_hashes,
                    const std::vector<std::string> &peers,
                    std::function<void(bool)> on_finish,
                    std::string &err);

std::vector<DownloadStatus> download_status();
bool is_downloading(const std::string &filename);

// Aborts the running downloads and waits until they have all finished.
void cancel_downloads();

// Per-piece logging of the seeder and downloader (off by default).
void set_verbose(bool on);

#endif
