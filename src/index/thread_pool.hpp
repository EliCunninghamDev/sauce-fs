// SPDX-License-Identifier: MIT

#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace sauce::fs
{

/// Fixed-size cross-platform worker pool.
///
/// Jobs can be submitted from any thread. wait() blocks until every queued and
/// running job finishes. If a job throws, wait() rethrows the first exception
/// once the pool is idle. The destructor joins every worker.
class ThreadPool
{
public:
    explicit ThreadPool(std::size_t workers);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    void submit(std::function<void()> job);

    /// Block until the queue is empty and all workers are idle.
    void wait();

private:
    void run();

    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable idle_;
    std::queue<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    std::exception_ptr failure_;
    std::size_t active_ = 0;
    bool stopping_ = false;
};

} // namespace sauce::fs
