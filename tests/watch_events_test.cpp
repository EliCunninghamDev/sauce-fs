// SPDX-License-Identifier: MIT

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/detail/watch_events.hpp"
#include "util/format.hpp"

using sauce::fs::core::WatchEvent;
using sauce::fs::core::WatchEventKind;
using sauce::fs::core::detail::WatchEventQueue;
using namespace std::chrono_literals;

namespace
{

WatchEvent make_event(WatchEventKind kind, std::string path = {}, std::string message = {})
{
    WatchEvent event;
    event.kind = kind;
    event.path = std::move(path);
    event.message = std::move(message);
    return event;
}

int fail(const char* message)
{
    sauce::fs::print_error("watch events test: {}\n", message);
    return 1;
}

} // namespace

int main()
{
    // Ordered delivery and monotonic generations.
    {
        WatchEventQueue queue(8);
        queue.open();
        queue.push(make_event(WatchEventKind::ArchiveAdded, "a.vpk"));
        queue.push(make_event(WatchEventKind::ArchiveAdded, "b.vpk"));

        const std::vector<WatchEvent> events = queue.drain();
        if (events.size() != 2)
        {
            return fail("drain() did not return every queued event");
        }
        if (events[0].path != "a.vpk" || events[1].path != "b.vpk")
        {
            return fail("events were not returned in queue order");
        }
        if (events[0].generation == 0 || events[0].generation >= events[1].generation)
        {
            return fail("generation is not monotonic");
        }
        if (queue.poll())
        {
            return fail("drain() left an event behind");
        }
    }

    // Overflow coalesces repeated events, drops the rest, and appends one
    // marker.
    {
        WatchEventQueue queue(2);
        queue.open();
        queue.push(make_event(WatchEventKind::ArchiveAdded, "a.vpk"));
        queue.push(make_event(WatchEventKind::ArchiveAdded, "b.vpk"));
        queue.push(make_event(WatchEventKind::ArchiveAdded, "a.vpk"));
        queue.push(make_event(WatchEventKind::ArchiveChanged, "c.vpk"));
        queue.push(make_event(WatchEventKind::ArchiveChanged, "d.vpk"));

        const std::vector<WatchEvent> events = queue.drain();
        if (events.size() != 3)
        {
            return fail("overflow did not leave the queue bounded plus one marker");
        }
        if (events[0].path != "a.vpk" || events[1].path != "b.vpk" ||
            events[2].kind != WatchEventKind::EventsDropped)
        {
            return fail("overflow produced unexpected events");
        }
        if (events[2].generation <= events[1].generation)
        {
            return fail("dropped marker lost its generation");
        }
    }

    // IndexUpdated counts merge when a repeated event is coalesced.
    {
        WatchEventQueue queue(1);
        queue.open();

        WatchEvent first;
        first.kind = WatchEventKind::IndexUpdated;
        first.added = 1;
        queue.push(first);

        WatchEvent second;
        second.kind = WatchEventKind::IndexUpdated;
        second.added = 2;
        second.loose_removed = 3;
        queue.push(second);

        const std::vector<WatchEvent> events = queue.drain();
        if (events.size() != 1)
        {
            return fail("coalesced update was not merged");
        }
        if (events[0].added != 3 || events[0].loose_removed != 3)
        {
            return fail("coalesced update counts were not summed");
        }
    }

    // Terminal events are always delivered, even when the queue is full.
    {
        WatchEventQueue queue(1);
        queue.open();
        queue.push(make_event(WatchEventKind::Warning, {}, "w"));
        queue.push(make_event(WatchEventKind::Failed, {}, "boom"));
        queue.push(make_event(WatchEventKind::Stopped));

        const std::vector<WatchEvent> events = queue.drain();
        if (events.size() != 3 || events[0].kind != WatchEventKind::Warning ||
            events[1].kind != WatchEventKind::Failed || events[2].kind != WatchEventKind::Stopped)
        {
            return fail("terminal events were dropped");
        }
    }

    // wait() returns queued events, times out on an open empty queue, and
    // returns immediately once the source is closed.
    {
        WatchEventQueue queue(4);
        if (queue.wait(10ms))
        {
            return fail("closed empty queue returned an event");
        }

        queue.open();
        if (queue.wait(10ms))
        {
            return fail("open empty queue returned an event");
        }

        queue.push(make_event(WatchEventKind::ArchiveRemoved, "gone.vpk"));
        const auto event = queue.wait(10ms);
        if (!event || event->kind != WatchEventKind::ArchiveRemoved)
        {
            return fail("wait() did not return the queued event");
        }

        queue.close();
        if (queue.wait(10ms))
        {
            return fail("closed empty queue did not return nullopt");
        }
    }

    // A blocked waiter wakes on push, and on close.
    {
        WatchEventQueue queue(4);
        queue.open();
        std::optional<WatchEvent> received;
        std::thread waiter([&] { received = queue.wait(std::nullopt); });
        queue.push(make_event(WatchEventKind::IndexReady));
        waiter.join();
        if (!received || received->kind != WatchEventKind::IndexReady)
        {
            return fail("blocked waiter was not woken by push()");
        }

        std::thread blocked([&] { (void)queue.wait(std::nullopt); });
        std::this_thread::sleep_for(50ms);
        queue.close();
        blocked.join();
    }

    return 0;
}
