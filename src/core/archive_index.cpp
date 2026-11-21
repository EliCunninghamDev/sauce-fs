// SPDX-License-Identifier: MIT

#include <sauce/core/archive_index.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/detail/archive_build.hpp"
#include "core/detail/archive_constants.hpp"
#include "core/detail/archive_discovery.hpp"
#include "core/detail/archive_scan.hpp"
#include "core/detail/archive_store.hpp"
#include "core/detail/index_summary.hpp"
#include "core/detail/log.hpp"
#include "core/detail/thread_pool.hpp"

namespace sauce::fs::core
{

namespace
{

constexpr std::size_t kPrintedWarnings = 20;

} // namespace

bool build_from_plan(const ArchiveIndexOptions& options, ScanPlan& plan, ArchiveIndexResult& result)
{
    result = ArchiveIndexResult{};
    result.games = std::move(plan.games);
    result.summary.warnings = std::move(plan.warnings);
    result.summary.game_folders = result.games.size();
    result.summary.gmod_crawlers = plan.gmod_crawlers;
    result.loose = std::move(plan.loose);
    for (const LooseEntry& entry : result.loose)
    {
        if (entry.directory)
        {
            ++result.summary.loose_directories;
        }
        else
        {
            ++result.summary.loose_files;
        }
    }

    std::vector<Candidate> candidates = std::move(plan.candidates);
    if (!candidates.empty())
    {
        const std::size_t hash_workers =
            std::max<std::size_t>(1, std::thread::hardware_concurrency());
        ThreadPool pool(hash_workers);
        std::vector<ArchiveRecord> records(candidates.size());
        std::vector<std::string> errors(candidates.size());
        std::vector<std::uint8_t> hashed(candidates.size(), 0);
        std::vector<std::vector<ArchiveContent>> content_lists(candidates.size());
        std::vector<std::vector<std::string>> content_warnings(candidates.size());
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
        }

        for (std::size_t index = 0; index < candidates.size(); ++index)
        {
            pool.submit(
                [&candidates, &records, &errors, &hashed, &content_lists, &content_warnings, index]
                {
                    if (!compute_hash(candidates[index].path, records[index].hash, errors[index]))
                    {
                        return;
                    }
                    scan_contents(candidates[index].path, records[index], content_lists[index],
                                  content_warnings[index]);
                    hashed[index] = 1;
                });
        }
        try
        {
            pool.wait();
        }
        catch (const std::exception& exception)
        {
            result.summary.warnings.push_back(std::string("worker failed: ") + exception.what());
        }

        for (std::size_t index = 0; index < candidates.size(); ++index)
        {
            if (hashed[index] != 0)
            {
                result.archives.push_back(std::move(records[index]));
                for (const ArchiveContent& content : content_lists[index])
                {
                    if (content.directory)
                    {
                        ++result.summary.directories;
                    }
                }
                result.contents.insert(result.contents.end(),
                                       std::make_move_iterator(content_lists[index].begin()),
                                       std::make_move_iterator(content_lists[index].end()));
                result.summary.warnings.insert(
                    result.summary.warnings.end(),
                    std::make_move_iterator(content_warnings[index].begin()),
                    std::make_move_iterator(content_warnings[index].end()));
                continue;
            }
            ++result.summary.unreadable;
            if (!errors[index].empty())
            {
                result.summary.warnings.push_back(std::move(errors[index]));
            }
        }
    }

    std::sort(result.archives.begin(), result.archives.end(),
              [](const ArchiveRecord& left, const ArchiveRecord& right)
              { return left.path < right.path; });

    for (const ArchiveRecord& record : result.archives)
    {
        switch (record.variant)
        {
        case ArchiveVariant::Vpk:
            ++result.summary.vpk;
            break;
        case ArchiveVariant::Bsp:
            ++result.summary.bsp;
            break;
        case ArchiveVariant::Gma:
            ++result.summary.gma;
            break;
        case ArchiveVariant::GmaBin:
            ++result.summary.gma_bin;
            break;
        }
    }

    return write_database(options.database, result.games, result.archives, result.contents,
                          result.loose, result.error);
} // namespace

std::string_view archive_variant_name(ArchiveVariant variant) noexcept
{
    switch (variant)
    {
    case ArchiveVariant::Vpk:
        return constants::kVpk;
    case ArchiveVariant::Bsp:
        return constants::kBspPak;
    case ArchiveVariant::Gma:
        return constants::kGma;
    case ArchiveVariant::GmaBin:
        return constants::kGmaBin;
    }
    return constants::kUnknownVariant;
}

std::int64_t archive_type_id(ArchiveVariant variant) noexcept
{
    switch (variant)
    {
    case ArchiveVariant::Vpk:
        return constants::kArchiveTypeVpkId;
    case ArchiveVariant::Bsp:
        return constants::kArchiveTypeBspPakId;
    case ArchiveVariant::Gma:
        return constants::kArchiveTypeGmaId;
    case ArchiveVariant::GmaBin:
        return constants::kArchiveTypeGmaBinId;
    }
    return constants::kArchiveTypeVpkId;
}

std::string_view asset_type_name(AssetType type) noexcept
{
    switch (type)
    {
    case AssetType::Unknown:
        return constants::kAssetUnknown;
    case AssetType::Model:
        return constants::kAssetModel;
    case AssetType::Material:
        return constants::kAssetMaterial;
    case AssetType::Texture:
        return constants::kAssetTexture;
    case AssetType::Sound:
        return constants::kAssetSound;
    case AssetType::Map:
        return constants::kAssetMap;
    case AssetType::Script:
        return constants::kAssetScript;
    }
    return constants::kAssetUnknown;
}

bool build_archive_index(const ArchiveIndexOptions& options, ArchiveIndexResult& result)
{
    ScanPlan plan = collect_scan_plan(options);
    const bool written = build_from_plan(options, plan, result);
    detail::log_index_summary(options.log, result, options.database);
    return written;
}

namespace detail
{

void log_index_summary(const LogOptions& log, const ArchiveIndexResult& result,
                       const std::filesystem::path& database)
{
    (void)log;
    (void)database;

    const ArchiveIndexSummary& summary = result.summary;
    SAUCE_FS_LOG(log, Info, "archives: {} (vpk {}, bsp-pak {}, gma {}, gma-bin {})",
                 result.archives.size(), summary.vpk, summary.bsp, summary.gma, summary.gma_bin);
    SAUCE_FS_LOG(log, Info, "contents: {} files, {} directories",
                 result.contents.size() - summary.directories, summary.directories);
    SAUCE_FS_LOG(log, Info, "loose: {} files, {} directories", summary.loose_files,
                 summary.loose_directories);
    SAUCE_FS_LOG(log, Info, "games: {}, gmod crawlers: {}, unreadable: {}", result.games.size(),
                 summary.gmod_crawlers, summary.unreadable);
    SAUCE_FS_LOG(log, Info, "database: {}", database.string());

    const std::size_t limit = std::min(summary.warnings.size(), kPrintedWarnings);
    for (std::size_t index = 0; index < limit; ++index)
    {
        SAUCE_FS_LOG(log, Warning, "{}", summary.warnings[index]);
    }
    if (summary.warnings.size() > limit)
    {
        SAUCE_FS_LOG(log, Warning, "... {} more warning(s)", summary.warnings.size() - limit);
    }

    if (!result.error.empty())
    {
        SAUCE_FS_LOG(log, Error, "{}", result.error);
    }
}

} // namespace detail

} // namespace sauce::fs::core
