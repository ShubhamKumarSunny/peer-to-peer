#include "password.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
using namespace std;

static const char *SCHEME = "pbkdf2-sha256";
// Each guess at a password costs this many rounds of HMAC-SHA256
static const int ITERATIONS = 600000;
static const int MAX_ITERATIONS = 10000000;
static const size_t SALT_LEN = 16;
static const size_t HASH_LEN = 32;

static string to_hex(const unsigned char *data, size_t len) {
    string out;
    char buf[3];
    for (size_t i = 0; i < len; i++) {
        snprintf(buf, sizeof(buf), "%02x", data[i]);
        out += buf;
    }
    return out;
}

static bool from_hex(const string &hex, vector<unsigned char> &out) {
    if (hex.empty() || hex.size() % 2 != 0) return false;
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        char *end = nullptr;
        string byte = hex.substr(i, 2);
        long v = strtol(byte.c_str(), &end, 16);
        if (*end != '\0') return false;
        out.push_back((unsigned char)v);
    }
    return true;
}

static string derive(const string &password, const vector<unsigned char> &salt, int iterations) {
    unsigned char hash[HASH_LEN];
    if (PKCS5_PBKDF2_HMAC(password.data(), (int)password.size(), salt.data(), (int)salt.size(),
                          iterations, EVP_sha256(), HASH_LEN, hash) != 1) return "";
    return string(SCHEME) + "$" + to_string(iterations) + "$" + to_hex(salt.data(), salt.size()) +
           "$" + to_hex(hash, HASH_LEN);
}

static vector<unsigned char> random_salt() {
    vector<unsigned char> salt(SALT_LEN);
    if (RAND_bytes(salt.data(), (int)salt.size()) != 1) abort();
    return salt;
}

string hash_password(const string &password) {
    return derive(password, random_salt(), ITERATIONS);
}

string hash_password_like(const string &stored, const string &password) {
    // "<scheme>$<iterations>$<salt>$<hash>"
    size_t a = stored.find('$');
    size_t b = a == string::npos ? a : stored.find('$', a + 1);
    size_t c = b == string::npos ? b : stored.find('$', b + 1);
    vector<unsigned char> salt;
    int iterations = 0;
    if (c != string::npos && stored.substr(0, a) == SCHEME) {
        iterations = atoi(stored.substr(a + 1, b - a - 1).c_str());
        if (iterations < 1 || iterations > MAX_ITERATIONS || !from_hex(stored.substr(b + 1, c - b - 1), salt)) {
            iterations = 0;
        }
    }
    if (iterations == 0) {
        // No such user, or nothing usable is stored. Take as long as a real
        // check would, so the reply time does not tell which names exist.
        static const vector<unsigned char> own_salt = random_salt();
        return derive(password, own_salt, ITERATIONS);
    }
    return derive(password, salt, iterations);
}

bool same_hash(const string &a, const string &b) {
    return !a.empty() && a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
