// SPDX-License-Identifier: MIT

#include "core/detail/watch_events.hpp"

#include <utility>

namespace sauce::fs::core::detail
{

namespace
{

bool is_terminal(WatchEventKind kind) noexcept
{
    return kind == WatchEventKind::IndexReady || kind == WatchEventKind::Failed ||
           kind == WatchEventKind::Stopped;
}

} // namespace

WatchEventQueue::WatchEventQueue(std::size_t capacity) : capacity_(capacity) {}

void WatchEventQueue::open()
{
    std::lock_guard lock(mutex_);
    events_.clear();
    dropped_marker_ = false;
    closed_ = false;
}

void WatchEventQueue::close() noexcept
{
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    ready_.notify_all();
}

void WatchEventQueue::wake() noexcept
{
    ready_.notify_all();
}

void WatchEventQueue::push(WatchEvent event)
{
    std::lock_guard lock(mutex_);
    event.generation = next_generation_++;

    if (!is_terminal(event.kind) && is_full())
    {
        // Keep the queue bounded by merging repeated events. A changed archive
        // reported twice before the consumer runs is one event, not two.
        for (WatchEvent& queued : events_)
        {
            if (queued.kind != event.kind || queued.path != event.path)
            {
                continue;
            }
            queued.added += event.added;
            queued.changed += event.changed;
            queued.removed += event.removed;
            queued.loose_added += event.loose_added;
            queued.loose_removed += event.loose_removed;
            return;
        }

        // One marker is enough; it resets when the consumer pops it.
        if (!dropped_marker_)
        {
            WatchEvent marker;
            marker.kind = WatchEventKind::EventsDropped;
            marker.generation = next_generation_++;
            marker.message = "event queue overflow; some events were coalesced or dropped";
            events_.push_back(std::move(marker));
            dropped_marker_ = true;
        }
        return;
    }

    events_.push_back(std::move(event));
    ready_.notify_one();
}

std::optional<WatchEvent> WatchEventQueue::poll()
{
    std::lock_guard lock(mutex_);
    if (events_.empty())
    {
        return std::nullopt;
    }
    WatchEvent event = std::move(events_.front());
    pop_front_locked();
    return event;
}

std::optional<WatchEvent> WatchEventQueue::wait(std::optional<std::chrono::milliseconds> timeout)
{
    std::unique_lock lock(mutex_);
    const auto has_event = [this] { return !events_.empty() || closed_; };
    if (timeout)
    {
        if (!ready_.wait_for(lock, *timeout, has_event))
        {
            return std::nullopt;
        }
    }
    else
    {
        ready_.wait(lock, has_event);
    }

    if (events_.empty())
    {
        return std::nullopt;
    }
    WatchEvent event = std::move(events_.front());
    pop_front_locked();
    return event;
}

std::vector<WatchEvent> WatchEventQueue::drain()
{
    std::lock_guard lock(mutex_);
    std::vector<WatchEvent> events;
    events.reserve(events_.size());
    while (!events_.empty())
    {
        events.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    dropped_marker_ = false;
    return events;
}

bool WatchEventQueue::is_full() const noexcept
{
    return capacity_ != 0 && events_.size() >= capacity_;
}

void WatchEventQueue::pop_front_locked()
{
    if (events_.front().kind == WatchEventKind::EventsDropped)
    {
        dropped_marker_ = false;
    }
    events_.pop_front();
}

} // namespace sauce::fs::core::detail
