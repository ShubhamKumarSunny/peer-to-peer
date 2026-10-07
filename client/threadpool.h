#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Fixed set of worker threads that run queued tasks in FIFO order.
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads);
    // Runs the tasks that are still queued, then joins the workers.
    ~ThreadPool();

    void submit(std::function<void()> task);

private:
    void worker_loop();

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    bool stopping;
};

#endif
