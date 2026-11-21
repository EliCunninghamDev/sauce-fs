// SPDX-License-Identifier: MIT

#include "core/detail/watch_refresh.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/detail/archive_constants.hpp"
#include "core/detail/archive_discovery.hpp"
#include "core/detail/archive_scan.hpp"
#include "core/detail/archive_store.hpp"
#include "core/detail/log.hpp"
#include "core/detail/thread_pool.hpp"
#include "parsers/bytes.hpp"
#include "parsers/vpk.hpp"

namespace sauce::fs::core::detail
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

bool refresh_watch_index(const ArchiveIndexOptions& options, bool verbose,
                         const std::set<std::string>& dirty, const std::set<std::string>& reparse,
                         bool force_all, WatchRefreshSummary& summary, std::string& error)
{
    summary = {};
    ScanPlan plan = collect_scan_plan(options);
    std::unordered_map<std::string, StoredArchive> existing;
    if (!read_archive_rows(options.database, existing, error))
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
    if (!read_loose_rows(options.database, existing_loose, error))
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

    const std::size_t hash_workers = std::max<std::size_t>(1, std::thread::hardware_concurrency());
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
                    if (force_all || reparse.find(record.path) != reparse.end() || !same_contents)
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
            if (found->second.hash_value != record.hash || found->second.size != candidate.size ||
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

    const bool has_changes = !delta.removed.empty() || !delta.replaced.empty() || !added.empty() ||
                             !changed.empty() || forced_any || force_all || !delta.loose.empty() ||
                             !delta.loose_removed.empty() || metadata_dirty;
    if (has_changes && !apply_database_delta(options.database, delta, error))
    {
        return false;
    }
    summary.committed = has_changes;

    if (verbose &&
        (!added.empty() || !changed.empty() || !delta.removed.empty() || !failed.empty() ||
         !parse_warnings.empty() || !delta.loose.empty() || !delta.loose_removed.empty()))
    {
        SAUCE_FS_LOG(options.log, Info,
                     "index updated: +{} added, -{} removed, ~{} changed, "
                     "loose +{} -{}",
                     added.size(), delta.removed.size(), changed.size(), delta.loose.size(),
                     delta.loose_removed.size());
        log_paths(options.log, "+", added);
        log_paths(options.log, "~", changed);
        log_paths(options.log, "-", delta.removed);
        for (const std::string& path : failed)
        {
            (void)path;
            SAUCE_FS_LOG(options.log, Warning, "{} (unreadable; kept previous index rows)", path);
        }
        const std::size_t warning_limit = std::min<std::size_t>(parse_warnings.size(), 5);
        for (std::size_t index = 0; index < warning_limit; ++index)
        {
            SAUCE_FS_LOG(options.log, Warning, "{}", parse_warnings[index]);
        }
    }

    summary.loose_added = delta.loose.size();
    summary.loose_removed = delta.loose_removed.size();
    summary.added = std::move(added);
    summary.changed = std::move(changed);
    summary.removed = std::move(delta.removed);
    summary.unreadable = std::move(failed);
    summary.warnings = std::move(parse_warnings);
    return true;
}

} // namespace sauce::fs::core::detail
