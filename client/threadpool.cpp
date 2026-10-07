#include "threadpool.h"
using namespace std;

ThreadPool::ThreadPool(size_t num_threads) : stopping(false) {
    for (size_t i = 0; i < num_threads; ++i)
        workers.emplace_back(&ThreadPool::worker_loop, this);
}

ThreadPool::~ThreadPool() {
    {
        lock_guard<mutex> lock(mtx);
        stopping = true;
    }
    cv.notify_all();
    for (auto &w : workers) w.join();
}

void ThreadPool::submit(function<void()> task) {
    {
        lock_guard<mutex> lock(mtx);
        tasks.push(std::move(task));
    }
    cv.notify_one();
}

void ThreadPool::worker_loop() {
    while (true) {
        function<void()> task;
        {
            unique_lock<mutex> lock(mtx);
            cv.wait(lock, [this] { return stopping || !tasks.empty(); });
            if (tasks.empty()) return;
            task = std::move(tasks.front());
            tasks.pop();
        }
        task();
    }
}
