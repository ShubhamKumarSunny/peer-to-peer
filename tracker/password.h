#ifndef PASSWORD_H
#define PASSWORD_H
#include <string>

// Passwords are never stored, logged or sent to the other tracker. What is
// kept is "pbkdf2-sha256$<iterations>$<salt>$<hash>": the result of running
// the password and a random salt through a deliberately slow hash.

// Hashes a new password with a fresh random salt.
std::string hash_password(const std::string &password);

// Hashes `password` with the salt and iteration count found in `stored`, so
// that the result equals `stored` exactly when the password is the right one.
// If `stored` is not a hash made by hash_password, the work is still done
// (with a salt of this tracker's own) and the result matches nothing.
std::string hash_password_like(const std::string &stored, const std::string &password);

// Compares two hashes without stopping at the first difference.
bool same_hash(const std::string &a, const std::string &b);

#endif
