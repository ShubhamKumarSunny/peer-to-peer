#include "fileutils.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <fstream>
#include <cstdio>
#include <sys/stat.h>

using namespace std;

static string to_hex(const unsigned char *hash, size_t len) {
    string out;
    char buf[3];
    for (size_t i = 0; i < len; i++) {
        snprintf(buf, sizeof(buf), "%02x", hash[i]);
        out += buf;
    }
    return out;
}

string sha1_hex(const char *data, size_t len) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(data), len, hash);
    return to_hex(hash, SHA_DIGEST_LENGTH);
}

bool compute_file_hashes(const string &fname,
                         size_t &fsize,
                         string &full_hash,
                         vector<string> &piece_hashes) {
    struct stat st;
    if (stat(fname.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    ifstream f(fname, ios::binary);
    if (!f.is_open())
        return false;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    piece_hashes.clear();
    fsize = 0;
    vector<char> buf(PIECE_SIZE);
    while (f) {
        f.read(buf.data(), buf.size());
        streamsize n = f.gcount();
        if (n <= 0) break;
        piece_hashes.push_back(sha1_hex(buf.data(), n));
        EVP_DigestUpdate(ctx, buf.data(), n);
        fsize += n;
    }
    unsigned char final_hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_DigestFinal_ex(ctx, final_hash, &hash_len);
    EVP_MD_CTX_free(ctx);
    full_hash = to_hex(final_hash, hash_len);
    // A read error part way through would leave hashes of a truncated file
    return !f.bad() && fsize == (size_t)st.st_size;
}
