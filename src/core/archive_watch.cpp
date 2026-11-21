// SPDX-License-Identifier: MIT

#include <sauce/core/archive_index.hpp>

#include <algorithm>
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
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <efsw/efsw.hpp>

#include "core/detail/archive_build.hpp"
#include "core/detail/archive_constants.hpp"
#include "core/detail/archive_discovery.hpp"
#include "core/detail/archive_scan.hpp"
#include "core/detail/archive_store.hpp"
#include "core/detail/index_summary.hpp"
#include "core/detail/log.hpp"
#include "core/detail/thread_pool.hpp"
#include "parsers/bytes.hpp"
#include "parsers/vpk.hpp"

namespace sauce::fs::core
{

namespace fs = std::filesystem;

namespace
{

constexpr std::size_t kListedPaths = 5;

std::string folded(std::string_view text)
{
    std::string key;
    key.reserve(text.size());
    for (const char character : text)
    {
        key.push_back(parsers::ascii_lower(character));
    }
    return key;
}

std::string loose_key(const LooseEntry& entry)
{
    return entry.root + '\n' + folded(entry.path);
}

void log_paths(const LogOptions& log, std::string_view label, const std::vector<std::string>& paths)
{
    (void)log;
    (void)label;

    const std::size_t limit = std::min<std::size_t>(paths.size(), kListedPaths);
    for (std::size_t index = 0; index < limit; ++index)
    {
        SAUCE_FS_LOG(log, Info, "  {} {}", label, paths[index]);
    }
    if (paths.size() > limit)
    {
        SAUCE_FS_LOG(log, Info, "  {} ... {} more", label, paths.size() - limit);
    }
}

} // namespace

struct ArchiveIndexWatcher::Impl final : private efsw::FileWatchListener
{
    Impl(ArchiveIndexOptions options, ArchiveWatchOptions watch)
        : options_(std::move(options)), watch_(watch)
    {
    }

    ~Impl() override
    {
        stop();
        for (const efsw::WatchID id : watch_ids_)
        {
            watcher_.removeWatch(id);
        }
    }

    bool run(ArchiveIndexResult& result)
    {
        ScanPlan plan = collect_scan_plan(options_);
        const std::vector<fs::path> roots = plan.watch_roots;
        const bool built = build_from_plan(options_, plan, result);
        detail::log_index_summary(options_.log, result, options_.database);
        if (!built)
        {
            return false;
        }

        for (const fs::path& root : roots)
        {
            std::error_code ec;
            if (!fs::is_directory(root, ec))
            {
                continue;
            }
            const efsw::WatchID id = watcher_.addWatch(root.string(), this, true);
            if (id < 0)
            {
                SAUCE_FS_LOG(options_.log, Warning, "cannot watch {}", root.string());
                continue;
            }
            watch_ids_.push_back(id);
        }
        watcher_.watch();

        // Reconcile once more: a game update can land between the initial walk
        // and the point where efsw has installed every recursive watch.
        {
            std::string error;
            if (!refresh(result, {}, {}, false, error))
            {
                SAUCE_FS_LOG(options_.log, Error, "{}", error);
                return false;
            }
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

            std::string error;
            if (!refresh(result, dirty, reparse, missed, error))
            {
                SAUCE_FS_LOG(options_.log, Error, "{}", error);
                return false;
            }
        }
        return true;
    }

    void stop() noexcept
    {
        stopping_.store(true);
        event_ready_.notify_all();
    }

private:
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

    bool refresh(ArchiveIndexResult&, const std::set<std::string>& dirty,
                 const std::set<std::string>& reparse, bool force_all, std::string& error)
    {
        ScanPlan plan = collect_scan_plan(options_);
        std::unordered_map<std::string, StoredArchive> existing;
        if (!read_archive_rows(options_.database, existing, error))
        {
            return false;
        }

        const std::vector<Candidate>& candidates = plan.candidates;
        std::vector<ArchiveRecord> records(candidates.size());
        std::vector<std::uint8_t> need_hash(candidates.size(), 0);
        std::vector<std::uint8_t> hashed(candidates.size(), 0);
        std::vector<std::uint8_t> replace(candidates.size(), 0);
        std::vector<std::string> hashing_errors(candidates.size());
        std::vector<std::vector<ArchiveContent>> content_lists(candidates.size());
        std::vector<std::vector<std::string>> content_warnings(candidates.size());
        std::vector<std::uint8_t> existed(candidates.size(), 0);

        bool forced_any = false;
        std::size_t changed_count = 0;
        std::size_t added_count = 0;
        for (std::size_t index = 0; index < candidates.size(); ++index)
        {
            const Candidate& candidate = candidates[index];
            ArchiveRecord& record = records[index];
            record.path = candidate.path.generic_string();
            record.game_path = candidate.game_path;
            record.mount_path = candidate.mount_path;
            record.stem = candidate.path.stem().string();
            record.variant = candidate.variant;
            record.size = candidate.size;
            record.modified = candidate.modified;

            const auto found = existing.find(record.path);
            if (found == existing.end())
            {
                need_hash[index] = 1;
                ++added_count;
                continue;
            }
            existed[index] = 1;
            record.hash = found->second.hash_value;
            // Metadata is only reparsed when the file changed, so keep the
            // stored values for unchanged archives.
            record.addon_name = found->second.addon_name;
            record.addon_description = found->second.addon_description;
            record.addon_author = found->second.addon_author;
            record.addon_version = found->second.addon_version;
            record.steamid64 = found->second.steamid64;
            record.timestamp = found->second.timestamp;
            const bool forced = force_all || dirty.find(record.path) != dirty.end();
            if (forced)
            {
                forced_any = true;
            }
            if (!forced && found->second.size == candidate.size &&
                found->second.modified == candidate.modified &&
                found->second.archive_type_id == archive_type_id(candidate.variant))
            {
                continue;
            }
            need_hash[index] = 1;
            ++changed_count;
        }

        std::unordered_map<std::string, LooseEntry> existing_loose;
        if (!read_loose_rows(options_.database, existing_loose, error))
        {
            return false;
        }

        DatabaseDelta loose_delta;
        std::unordered_set<std::string> desired_loose;
        desired_loose.reserve(plan.loose.size());
        for (const LooseEntry& entry : plan.loose)
        {
            const std::string key = loose_key(entry);
            desired_loose.insert(key);
            const auto found = existing_loose.find(key);
            if (found == existing_loose.end() || found->second.directory != entry.directory ||
                found->second.size != entry.size || found->second.modified != entry.modified)
            {
                loose_delta.loose.push_back(entry);
            }
        }
        for (const auto& [key, entry] : existing_loose)
        {
            (void)entry;
            if (desired_loose.find(key) == desired_loose.end())
            {
                loose_delta.loose_removed.push_back(key);
            }
        }

        bool metadata_dirty = force_all;
        for (const std::string& path : dirty)
        {
            const std::string name = fs::path(path).filename().string();
            if (parsers::ascii_iequals(name, "gameinfo.txt") ||
                parsers::ascii_iequals(name, "gameinfo.gi") ||
                parsers::ascii_iequals(name, "mount.cfg"))
            {
                metadata_dirty = true;
                break;
            }
        }

        const bool loose_changed = !loose_delta.loose.empty() || !loose_delta.loose_removed.empty();
        if (!forced_any && added_count == 0 && changed_count == 0 &&
            existing.size() == candidates.size() && !loose_changed && !metadata_dirty)
        {
            return true;
        }

        const std::size_t hash_workers =
            std::max<std::size_t>(1, std::thread::hardware_concurrency());
        if (std::find(need_hash.begin(), need_hash.end(), std::uint8_t{1}) != need_hash.end())
        {
            ThreadPool pool(hash_workers);
            for (std::size_t index = 0; index < candidates.size(); ++index)
            {
                if (need_hash[index] == 0)
                {
                    continue;
                }
                pool.submit(
                    [&candidates, &records, &hashed, &replace, &hashing_errors, &content_lists,
                     &content_warnings, &existing, &reparse, force_all, index]
                    {
                        const Candidate& candidate = candidates[index];
                        ArchiveRecord& record = records[index];
                        if (!compute_hash(candidate.path, record.hash, hashing_errors[index]))
                        {
                            return;
                        }
                        const auto found = existing.find(record.path);
                        const bool same_contents =
                            found != existing.end() && found->second.hash_value == record.hash &&
                            found->second.archive_type_id == archive_type_id(candidate.variant);
                        // A side archive event forces a parse even when the
                        // directory file itself is byte-identical: entries can
                        // have appeared or disappeared with the side file.
                        if (force_all || reparse.find(record.path) != reparse.end() ||
                            !same_contents)
                        {
                            scan_contents(candidate.path, record, content_lists[index],
                                          content_warnings[index]);
                            replace[index] = 1;
                        }
                        hashed[index] = 1;
                    });
            }
            try
            {
                pool.wait();
            }
            catch (const std::exception& exception)
            {
                error = std::string("worker failed: ") + exception.what();
                return false;
            }
        }

        DatabaseDelta delta;
        delta.games = plan.games;
        delta.loose = std::move(loose_delta.loose);
        delta.loose_removed = std::move(loose_delta.loose_removed);
        std::unordered_set<std::string> desired;
        desired.reserve(records.size());
        std::vector<std::string> added;
        std::vector<std::string> changed;
        std::vector<std::string> failed;

        for (std::size_t index = 0; index < candidates.size(); ++index)
        {
            const Candidate& candidate = candidates[index];
            ArchiveRecord& record = records[index];
            if (need_hash[index] != 0 && hashed[index] == 0)
            {
                failed.push_back(record.path);
                if (existed[index] != 0)
                {
                    // Keep the old rows: a transient read failure (a game
                    // updater writing the archive) must not delete a mount.
                    // Preserve the old signature so the next rescan retries.
                    const auto found = existing.find(record.path);
                    record.hash = found->second.hash_value;
                    record.size = found->second.size;
                    record.modified = found->second.modified;
                    delta.archives.push_back(std::move(record));
                    desired.insert(candidate.path.generic_string());
                }
                continue;
            }

            desired.insert(record.path);
            if (existed[index] == 0)
            {
                added.push_back(record.path);
            }
            else if (need_hash[index] != 0)
            {
                const auto found = existing.find(record.path);
                if (found->second.hash_value != record.hash ||
                    found->second.size != candidate.size ||
                    found->second.modified != candidate.modified ||
                    found->second.archive_type_id != archive_type_id(candidate.variant))
                {
                    changed.push_back(record.path);
                }
            }
            if (replace[index] != 0)
            {
                delta.replaced.emplace(record.path, std::move(content_lists[index]));
            }
            delta.archives.push_back(std::move(record));
        }

        for (const auto& [path, row] : existing)
        {
            (void)row;
            if (desired.find(path) == desired.end())
            {
                delta.removed.push_back(path);
            }
        }

        std::vector<std::string> parse_warnings;
        for (std::vector<std::string>& warnings : content_warnings)
        {
            parse_warnings.insert(parse_warnings.end(), std::make_move_iterator(warnings.begin()),
                                  std::make_move_iterator(warnings.end()));
        }

        const bool has_changes = !delta.removed.empty() || !delta.replaced.empty() ||
                                 !added.empty() || !changed.empty() || forced_any || force_all ||
                                 !delta.loose.empty() || !delta.loose_removed.empty() ||
                                 metadata_dirty;
        if (has_changes && !apply_database_delta(options_.database, delta, error))
        {
            return false;
        }

        if (watch_.verbose &&
            (!added.empty() || !changed.empty() || !delta.removed.empty() || !failed.empty() ||
             !parse_warnings.empty() || !delta.loose.empty() || !delta.loose_removed.empty()))
        {
            SAUCE_FS_LOG(options_.log, Info,
                         "index updated: +{} added, -{} removed, ~{} changed, "
                         "loose +{} -{}",
                         added.size(), delta.removed.size(), changed.size(), delta.loose.size(),
                         delta.loose_removed.size());
            log_paths(options_.log, "+", added);
            log_paths(options_.log, "~", changed);
            log_paths(options_.log, "-", delta.removed);
            for (const std::string& path : failed)
            {
                (void)path;
                SAUCE_FS_LOG(options_.log, Warning, "{} (unreadable; kept previous index rows)",
                             path);
            }
            const std::size_t warning_limit = std::min<std::size_t>(parse_warnings.size(), 5);
            for (std::size_t index = 0; index < warning_limit; ++index)
            {
                SAUCE_FS_LOG(options_.log, Warning, "{}", parse_warnings[index]);
            }
        }
        return true;
    }

    ArchiveIndexOptions options_;
    ArchiveWatchOptions watch_;
    efsw::FileWatcher watcher_;
    std::vector<efsw::WatchID> watch_ids_;
    std::mutex event_mutex_;
    std::condition_variable event_ready_;
    std::set<std::string> dirty_paths_;
    std::set<std::string> reparse_paths_;
    std::uint64_t event_generation_ = 0;
    bool dirty_ = false;
    bool missed_events_ = false;
    std::atomic<bool> stopping_{false};
};
ArchiveIndexWatcher::ArchiveIndexWatcher(ArchiveIndexOptions options, ArchiveWatchOptions watch)
    : impl_(std::make_unique<Impl>(std::move(options), watch))
{
}

ArchiveIndexWatcher::~ArchiveIndexWatcher() = default;

bool ArchiveIndexWatcher::run(ArchiveIndexResult& result)
{
    return impl_->run(result);
}

void ArchiveIndexWatcher::stop() noexcept
{
    impl_->stop();
}
} // namespace sauce::fs::core
