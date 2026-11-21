// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include <sauce/core/archive_index.hpp>

namespace sauce::fs::core::detail
{

/// \brief Thread-safe, bounded queue of semantic watch events.
///
/// One producer (the watch worker) appends events; any number of consumers may
/// poll, wait, or drain them. No watcher lock is held while consumers process
/// an event because events are returned by value.
class WatchEventQueue
{
public:
    /// \param capacity Maximum retained events; zero disables the bound.
    explicit WatchEventQueue(std::size_t capacity);

    /// \brief Discard queued events and mark the source active again.
    void open();

    /// \brief Mark the source finished and wake every waiter.
    ///
    /// Later push() calls are still accepted so a final Stopped event can be
    /// delivered even when the queue is full.
    void close() noexcept;

    /// \brief Wake every wait() call without changing the open/closed state.
    void wake() noexcept;

    /// \brief Append an event, coalescing or dropping it when the queue is full.
    void push(WatchEvent event);

    /// \brief Remove the oldest event, if any.
    std::optional<WatchEvent> poll();

    /// \brief Wait for the oldest event.
    ///
    /// Returns \c std::nullopt when the timeout expires, or when the source is
    /// closed and no events remain.
    std::optional<WatchEvent> wait(std::optional<std::chrono::milliseconds> timeout);

    /// \brief Remove every queued event in order.
    std::vector<WatchEvent> drain();

private:
    bool is_full() const noexcept;
    void pop_front_locked();

    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<WatchEvent> events_;
    std::uint64_t next_generation_ = 1;
    bool dropped_marker_ = false;
    bool closed_ = true;
};

} // namespace sauce::fs::core::detail
