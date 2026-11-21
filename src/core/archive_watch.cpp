// SPDX-License-Identifier: MIT

#include <sauce/core/archive_index.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <efsw/efsw.hpp>

#include "core/detail/archive_build.hpp"
#include "core/detail/archive_constants.hpp"
#include "core/detail/archive_discovery.hpp"
#include "core/detail/index_summary.hpp"
#include "core/detail/log.hpp"
#include "core/detail/watch_events.hpp"
#include "core/detail/watch_refresh.hpp"
#include "parsers/bytes.hpp"
#include "parsers/vpk.hpp"

namespace sauce::fs::core
{

namespace fs = std::filesystem;

struct ArchiveIndexWatcher::Impl final : private efsw::FileWatchListener
{
    Impl(ArchiveIndexOptions options, ArchiveWatchOptions watch)
        : options_(std::move(options)), watch_(watch), events_(watch.max_queued_events)
    {
    }

    ~Impl() override
    {
        stop();
        remove_watches();
    }

    bool start()
    {
        std::unique_lock lock(lifecycle_mutex_);
        if (worker_.joinable())
        {
            if (running_.load())
            {
                return false;
            }
            worker_.join();
        }
        begin_run();
        try
        {
            worker_ = std::thread([this] { (void)run_worker(nullptr); });
        }
        catch (const std::exception& exception)
        {
            running_.store(false);
            events_.push(failure_event(std::string("cannot start watcher: ") + exception.what()));
            events_.push(stop_event());
            events_.close();
            return false;
        }
        return true;
    }

    bool run(ArchiveIndexResult& result)
    {
        std::unique_lock lock(lifecycle_mutex_);
        if (worker_.joinable())
        {
            if (running_.load())
            {
                return false;
            }
            worker_.join();
        }
        begin_run();
        lock.unlock();
        return run_worker(&result);
    }

    void stop() noexcept
    {
        std::lock_guard lock(lifecycle_mutex_);
        // Set the flag under the lifecycle lock so a concurrent start() cannot
        // reset it after begin_run() and leave the worker running.
        stopping_.store(true);
        event_ready_.notify_all();
        events_.wake();

        // A synchronous run() has no worker; it observes stopping_ instead.
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
        {
            // run_worker() never takes lifecycle_mutex_, so joining here cannot
            // deadlock and a concurrent start() waits until the join finishes.
            worker_.join();
        }
    }

    std::optional<WatchEvent> poll_event()
    {
        return events_.poll();
    }

    std::optional<WatchEvent> wait_event(std::optional<std::chrono::milliseconds> timeout)
    {
        return events_.wait(timeout);
    }

    std::vector<WatchEvent> drain_events()
    {
        return events_.drain();
    }

    bool running() const noexcept
    {
        return running_.load();
    }

private:
    // Requires lifecycle_mutex_ and a finished or absent worker.
    void begin_run()
    {
        stopping_.store(false);
        {
            std::lock_guard lock(event_mutex_);
            dirty_paths_.clear();
            reparse_paths_.clear();
            event_generation_ = 0;
            dirty_ = false;
            missed_events_ = false;
        }
        remove_watches();
        events_.open();
        running_.store(true);
    }

    bool run_worker(ArchiveIndexResult* external) noexcept
    {
        bool ok = false;
        std::string failure;
        try
        {
            ok = run_worker_body(external, failure);
        }
        catch (const std::exception& exception)
        {
            failure = exception.what();
        }
        catch (...)
        {
            failure = "unknown exception";
        }

        if (!ok)
        {
            events_.push(failure_event(failure.empty() ? "index watcher failed" : failure));
        }
        events_.push(stop_event());

        remove_watches();
        running_.store(false);
        events_.close();
        return ok;
    }

    bool run_worker_body(ArchiveIndexResult* external, std::string& failure)
    {
        ScanPlan plan = collect_scan_plan(options_);
        const std::vector<fs::path> roots = plan.watch_roots;

        ArchiveIndexResult local;
        ArchiveIndexResult& result = external != nullptr ? *external : local;
        const bool built = build_from_plan(options_, plan, result);
        detail::log_index_summary(options_.log, result, options_.database);
        if (!built)
        {
            failure = result.error.empty() ? "initial index build failed" : result.error;
            return false;
        }

        watcher_ = std::make_unique<efsw::FileWatcher>();
        for (const fs::path& root : roots)
        {
            std::error_code ec;
            if (!fs::is_directory(root, ec))
            {
                continue;
            }
            const efsw::WatchID id = watcher_->addWatch(root.string(), this, true);
            if (id < 0)
            {
                SAUCE_FS_LOG(options_.log, Warning, "cannot watch {}", root.string());
                events_.push(warning_event("cannot watch " + root.string()));
                continue;
            }
            watch_ids_.push_back(id);
        }
        watcher_->watch();

        // Reconcile once more: a game update can land between the initial walk
        // and the point where efsw has installed every recursive watch. This is
        // still part of initial readiness, so it produces no incremental
        // events.
        {
            detail::WatchRefreshSummary reconcile;
            std::string error;
            if (!detail::refresh_watch_index(options_, watch_.verbose, {}, {}, false, reconcile,
                                             error))
            {
                SAUCE_FS_LOG(options_.log, Error, "{}", error);
                failure = std::move(error);
                return false;
            }
        }

        events_.push(ready_event());
        for (const std::string& warning : result.summary.warnings)
        {
            events_.push(warning_event(warning));
        }
        if (external == nullptr)
        {
            // The one-shot snapshot is not needed while watching; release it
            // before parking on the queue so large libraries do not stay
            // resident for the watcher's lifetime.
            local = {};
        }

        SAUCE_FS_LOG(options_.log, Info, "watching {} root(s); press Ctrl+C to stop",
                     watch_ids_.size());

        for (;;)
        {
            std::set<std::string> dirty;
            std::set<std::string> reparse;
            bool missed = false;
            {
                std::unique_lock lock(event_mutex_);
                event_ready_.wait(lock, [this] { return stopping_.load() || dirty_; });
                if (stopping_.load())
                {
                    break;
                }

                std::uint64_t generation = event_generation_;
                while (!stopping_.load())
                {
                    if (event_ready_.wait_for(
                            lock, watch_.debounce, [this, generation]
                            { return stopping_.load() || event_generation_ != generation; }))
                    {
                        if (stopping_.load())
                        {
                            break;
                        }
                        generation = event_generation_;
                        continue;
                    }
                    break;
                }
                if (stopping_.load())
                {
                    break;
                }
                dirty = std::move(dirty_paths_);
                dirty_paths_.clear();
                reparse = std::move(reparse_paths_);
                reparse_paths_.clear();
                dirty_ = false;
                missed = missed_events_;
                missed_events_ = false;
            }

            detail::WatchRefreshSummary summary;
            std::string error;
            if (!detail::refresh_watch_index(options_, watch_.verbose, dirty, reparse, missed,
                                             summary, error))
            {
                SAUCE_FS_LOG(options_.log, Error, "{}", error);
                failure = std::move(error);
                return false;
            }
            publish(summary);
        }
        return true;
    }

    void publish(const detail::WatchRefreshSummary& summary)
    {
        if (!summary.committed)
        {
            return;
        }

        for (const std::string& path : summary.added)
        {
            events_.push(archive_event(WatchEventKind::ArchiveAdded, path));
        }
        for (const std::string& path : summary.changed)
        {
            events_.push(archive_event(WatchEventKind::ArchiveChanged, path));
        }
        for (const std::string& path : summary.removed)
        {
            events_.push(archive_event(WatchEventKind::ArchiveRemoved, path));
        }

        WatchEvent updated;
        updated.kind = WatchEventKind::IndexUpdated;
        updated.added = summary.added.size();
        updated.changed = summary.changed.size();
        updated.removed = summary.removed.size();
        updated.loose_added = summary.loose_added;
        updated.loose_removed = summary.loose_removed;
        events_.push(std::move(updated));

        for (const std::string& path : summary.unreadable)
        {
            events_.push(warning_event(path + " (unreadable; kept previous index rows)"));
        }
        for (const std::string& warning : summary.warnings)
        {
            events_.push(warning_event(warning));
        }
    }

    static WatchEvent archive_event(WatchEventKind kind, std::string path)
    {
        WatchEvent event;
        event.kind = kind;
        event.path = std::move(path);
        return event;
    }

    static WatchEvent warning_event(std::string message)
    {
        WatchEvent event;
        event.kind = WatchEventKind::Warning;
        event.message = std::move(message);
        return event;
    }

    static WatchEvent failure_event(std::string message)
    {
        WatchEvent event;
        event.kind = WatchEventKind::Failed;
        event.message = std::move(message);
        return event;
    }

    static WatchEvent ready_event()
    {
        WatchEvent event;
        event.kind = WatchEventKind::IndexReady;
        return event;
    }

    static WatchEvent stop_event()
    {
        WatchEvent event;
        event.kind = WatchEventKind::Stopped;
        return event;
    }

    void remove_watches()
    {
        if (!watcher_)
        {
            return;
        }
        for (const efsw::WatchID id : watch_ids_)
        {
            watcher_->removeWatch(id);
        }
        watch_ids_.clear();
        watcher_.reset();
    }

    void mark_dirty(const fs::path& path)
    {
        const fs::path absolute = absolute_normalized(path);
        dirty_paths_.insert(absolute.generic_string());

        // A VPK side archive (foo_000.vpk) carries entry payloads for its
        // parent directory file. Even though the directory file's own bytes
        // did not change, the database must commit so readers drop cached
        // entry blobs.
        if (!parsers::is_vpk_chunk_name(absolute.filename().string()))
        {
            return;
        }
        const std::string name = absolute.filename().string();
        const std::size_t underscore = name.find_last_of('_');
        if (underscore == std::string::npos)
        {
            return;
        }
        const std::string stem = name.substr(0, underscore);
        std::error_code ec;
        const fs::path directory_file =
            absolute.parent_path() / (stem + std::string(constants::kVpkDirectorySuffix));
        if (fs::is_regular_file(directory_file, ec))
        {
            const std::string parent = directory_file.generic_string();
            dirty_paths_.insert(parent);
            reparse_paths_.insert(parent);
            return;
        }
        ec.clear();
        const fs::path plain_file =
            absolute.parent_path() / (stem + std::string(constants::kVpkExtension));
        if (fs::is_regular_file(plain_file, ec))
        {
            const std::string parent = plain_file.generic_string();
            dirty_paths_.insert(parent);
            reparse_paths_.insert(parent);
        }
    }

    void handleFileAction(efsw::WatchID, const std::string& dir, const std::string& filename,
                          efsw::Action, const std::string& oldFilename) override
    {
        std::lock_guard lock(event_mutex_);
        mark_dirty(fs::path(dir) / filename);
        if (!oldFilename.empty())
        {
            fs::path old = fs::path(oldFilename);
            if (!old.is_absolute())
            {
                old = fs::path(dir) / old;
            }
            mark_dirty(old);
        }
        ++event_generation_;
        dirty_ = true;
        event_ready_.notify_one();
    }

    void handleMissedFileActions(efsw::WatchID, const std::string&) override
    {
        std::lock_guard lock(event_mutex_);
        missed_events_ = true;
        ++event_generation_;
        dirty_ = true;
        event_ready_.notify_one();
    }

    ArchiveIndexOptions options_;
    ArchiveWatchOptions watch_;
    std::unique_ptr<efsw::FileWatcher> watcher_;
    std::vector<efsw::WatchID> watch_ids_;

    std::mutex lifecycle_mutex_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    std::mutex event_mutex_;
    std::condition_variable event_ready_;
    std::set<std::string> dirty_paths_;
    std::set<std::string> reparse_paths_;
    std::uint64_t event_generation_ = 0;
    bool dirty_ = false;
    bool missed_events_ = false;
    std::atomic<bool> stopping_{false};

    detail::WatchEventQueue events_;
};

ArchiveIndexWatcher::ArchiveIndexWatcher(ArchiveIndexOptions options, ArchiveWatchOptions watch)
    : impl_(std::make_unique<Impl>(std::move(options), watch))
{
}

ArchiveIndexWatcher::~ArchiveIndexWatcher() = default;

bool ArchiveIndexWatcher::start()
{
    return impl_->start();
}

std::optional<WatchEvent> ArchiveIndexWatcher::pollEvent()
{
    return impl_->poll_event();
}

std::optional<WatchEvent>
ArchiveIndexWatcher::waitEvent(std::optional<std::chrono::milliseconds> timeout)
{
    return impl_->wait_event(timeout);
}

std::optional<WatchEvent> ArchiveIndexWatcher::waitEvent()
{
    return impl_->wait_event(std::nullopt);
}

std::vector<WatchEvent> ArchiveIndexWatcher::drainEvents()
{
    return impl_->drain_events();
}

bool ArchiveIndexWatcher::running() const noexcept
{
    return impl_->running();
}

bool ArchiveIndexWatcher::run(ArchiveIndexResult& result)
{
    return impl_->run(result);
}

void ArchiveIndexWatcher::stop() noexcept
{
    impl_->stop();
}

} // namespace sauce::fs::core
