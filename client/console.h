#ifndef CONSOLE_H
#define CONSOLE_H
#include <iostream>
#include <mutex>

// The shell thread and the worker threads all print to the terminal.
//     Out() << "text" << value << std::endl;
// holds one lock for the whole statement, so two lines never mix and the
// stream is never used by two threads at once. Out(std::cerr) for errors.
std::mutex &console_mutex();

struct Out {
    std::lock_guard<std::mutex> lock;
    std::ostream &os;
    explicit Out(std::ostream &o = std::cout) : lock(console_mutex()), os(o) {}
    template <typename T> Out &operator<<(const T &v) { os << v; return *this; }
    Out &operator<<(std::ostream &(*f)(std::ostream &)) { os << f; return *this; }
};

#endif
