#ifndef FILEUTILS_H
#define FILEUTILS_H
#include <string>
#include <vector>

// Must match PIECE_SIZE in tracker/commands.cpp
const size_t PIECE_SIZE = 512 * 1024;

std::string sha1_hex(const char *data, size_t len);
bool compute_file_hashes(const std::string &fname,
                         size_t &fsize,
                         std::string &full_hash,
                         std::vector<std::string> &piece_hashes);

#endif
