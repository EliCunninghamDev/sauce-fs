// SPDX-License-Identifier: MIT

#include "index/thread_pool.hpp"

#include <utility>

namespace sauce::fs
{

ThreadPool::ThreadPool(std::size_t workers)
{
    workers_.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index)
    {
        workers_.emplace_back([this] { run(); });
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (std::thread& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

void ThreadPool::submit(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push(std::move(job));
    }
    ready_.notify_one();
}

void ThreadPool::wait()
{
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && active_ == 0; });
    if (failure_)
    {
        std::exception_ptr failure = failure_;
        failure_ = nullptr;
        lock.unlock();
        std::rethrow_exception(failure);
    }
}

void ThreadPool::run()
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty())
            {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop();
            ++active_;
        }

        try
        {
            job();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!failure_)
            {
                failure_ = std::current_exception();
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_;
            if (jobs_.empty() && active_ == 0)
            {
                idle_.notify_all();
            }
        }
    }
}

} // namespace sauce::fs
