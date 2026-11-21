// SPDX-License-Identifier: MIT

#include "core/detail/archive_discovery.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/detail/archive_constants.hpp"
#include "core/detail/thread_pool.hpp"
#include "parsers/bytes.hpp"
#include "parsers/gma.hpp"
#include "parsers/steam.hpp"
#include "parsers/vpk.hpp"

namespace sauce::fs::core
{

namespace
{

namespace fs = std::filesystem;

struct GameFolder
{
    fs::path path;
    std::string title;
    std::string slug;
    bool garrys_mod = false;
};

struct ScanJob
{
    fs::path root;
    /// Content root the mount paths are relative to.
    fs::path game_path;
    bool gmod = false;
    std::vector<fs::path> extra_roots;
};

struct JobResult
{
    std::vector<Candidate> candidates;
    std::vector<std::string> warnings;
};

void fill_signature(const fs::path& path, std::uint64_t& size, std::int64_t& modified)
{
    std::error_code ec;
    const std::uintmax_t length = fs::file_size(path, ec);
    size = ec ? 0 : static_cast<std::uint64_t>(length);

    ec.clear();
    const fs::file_time_type stamp = fs::last_write_time(path, ec);
    modified = ec ? 0 : static_cast<std::int64_t>(stamp.time_since_epoch().count());
}

/// Lowercase ASCII slug used as the mount directory name under `sourcegames`.
/// Apostrophes are dropped so "Garry's Mod" becomes "garrys-mod".
std::string slugify(std::string_view text)
{
    std::string slug;
    slug.reserve(text.size());
    bool separator = false;
    for (const char character : text)
    {
        if (character >= 'a' && character <= 'z')
        {
            if (separator && !slug.empty())
            {
                slug.push_back('-');
            }
            separator = false;
            slug.push_back(character);
        }
        else if (character >= 'A' && character <= 'Z')
        {
            if (separator && !slug.empty())
            {
                slug.push_back('-');
            }
            separator = false;
            slug.push_back(static_cast<char>(character - 'A' + 'a'));
        }
        else if (character >= '0' && character <= '9')
        {
            if (separator && !slug.empty())
            {
                slug.push_back('-');
            }
            separator = false;
            slug.push_back(character);
        }
        else if (character != '\'')
        {
            separator = true;
        }
    }
    return slug.empty() ? std::string(constants::kFallbackGameSlug) : slug;
}

std::string unique_slug(std::string base, std::set<std::string>& used)
{
    if (used.insert(base).second)
    {
        return base;
    }
    for (std::size_t suffix = 2;; ++suffix)
    {
        std::string candidate = base + '-' + std::to_string(suffix);
        if (used.insert(candidate).second)
        {
            return candidate;
        }
    }
}

bool is_gmod_folder(const fs::path& folder)
{
    if (parsers::ascii_iequals(folder.filename().string(), constants::kGarrysModFolder))
    {
        return true;
    }
    std::error_code ec;
    return fs::is_directory(folder / constants::kGarrysModFolder, ec);
}

/// The game directory that owns Garry's Mod addons. Accepts either the game
/// directory itself or an install root containing `garrysmod`.
fs::path gmod_content_folder(const fs::path& folder)
{
    std::error_code ec;
    const fs::path nested = folder / constants::kGarrysModFolder;
    if (fs::is_directory(nested, ec))
    {
        return nested;
    }
    return folder;
}

bool contains_ci(std::string_view text, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    for (std::size_t index = 0; index + needle.size() <= text.size(); ++index)
    {
        if (parsers::ascii_iequals(text.substr(index, needle.size()), needle))
        {
            return true;
        }
    }
    return false;
}

/// Walk up from a game directory until a Steam library's workshop content for
/// Garry's Mod (appid 4000) is found.
std::vector<fs::path> workshop_roots_for(const fs::path& game_dir)
{
    std::vector<fs::path> roots;
    std::error_code ec;
    for (fs::path ancestor = game_dir; !ancestor.empty(); ancestor = ancestor.parent_path())
    {
        const fs::path candidate = ancestor / constants::kSteamAppsFolder /
                                   constants::kWorkshopFolder / constants::kWorkshopContentFolder /
                                   constants::kGarrysModAppId;
        if (fs::is_directory(candidate, ec))
        {
            roots.push_back(candidate);
            break;
        }
        if (ancestor == ancestor.root_path())
        {
            break;
        }
    }
    return roots;
}

std::vector<GameFolder> collect_game_folders(const ArchiveIndexOptions& options,
                                             std::vector<std::string>& warnings)
{
    std::vector<GameFolder> folders;
    if (!options.game_folders.empty())
    {
        for (const fs::path& folder : options.game_folders)
        {
            if (folder.empty())
            {
                warnings.push_back("game folder path is empty");
                continue;
            }
            const fs::path path = absolute_normalized(folder);
            std::error_code ec;
            if (!fs::is_directory(path, ec))
            {
                warnings.push_back("game folder does not exist: " + path.string());
                continue;
            }
            std::string title = path.filename().string();
            if (title.empty())
            {
                title = path.generic_string();
            }
            folders.push_back({path, std::move(title), {}, is_gmod_folder(path)});
        }
    }
    else
    {
        parsers::DiscoveryOptions discovery;
        discovery.steam_roots = options.steam_roots;
        discovery.include_dedicated_servers = false;
        std::vector<std::string> discovery_warnings;
        const std::vector<parsers::InstalledTitle> titles =
            parsers::discover_source_titles(discovery, &discovery_warnings);
        warnings.insert(warnings.end(), discovery_warnings.begin(), discovery_warnings.end());

        for (const parsers::InstalledTitle& title : titles)
        {
            if (title.game_dir.empty())
            {
                continue;
            }
            const fs::path path = absolute_normalized(title.game_dir);
            const bool gmod =
                is_gmod_folder(path) || contains_ci(title.name, constants::kGarrysModTitleHint);
            std::string display = title.name;
            if (display.empty())
            {
                display = path.filename().string();
            }
            folders.push_back({path, std::move(display), {}, gmod});
        }
    }

    std::vector<GameFolder> unique;
    std::set<std::string> seen;
    for (GameFolder& folder : folders)
    {
        const std::string key = folder.path.generic_string();
        if (!seen.insert(key).second)
        {
            continue;
        }
        unique.push_back(std::move(folder));
    }

    // Stable order also makes the generated slug suffixes reproducible.
    std::sort(unique.begin(), unique.end(), [](const GameFolder& left, const GameFolder& right)
              { return left.path.generic_string() < right.path.generic_string(); });
    std::set<std::string> slugs;
    for (GameFolder& folder : unique)
    {
        folder.slug = unique_slug(slugify(folder.title), slugs);
    }
    return unique;
}
std::string make_mount_path(const fs::path& path, const fs::path& root, const std::string& prefix,
                            ArchiveVariant variant)
{
    fs::path relative = path.lexically_relative(root);
    if (relative.empty() || relative.begin()->string() == "..")
    {
        relative = path.filename();
    }

    fs::path mount = fs::path(prefix) / relative;
    mount = mount.parent_path() / mount.stem();
    if (variant == ArchiveVariant::Vpk)
    {
        const std::string name = mount.filename().string();
        if (parsers::ends_with_ci(name, constants::kVpkDirectoryStemSuffix) && name.size() > 4)
        {
            mount = mount.parent_path() /
                    name.substr(0, name.size() - constants::kVpkDirectoryStemSuffix.size());
        }
    }
    return mount.generic_string();
}

/// Prefix of `root` below `game_path`, or empty when they are the same or
/// unrelated.
std::string relative_mount_prefix(const fs::path& root, const fs::path& game_path)
{
    if (root == game_path)
    {
        return {};
    }
    const fs::path relative = root.lexically_relative(game_path);
    if (relative.empty() || relative == "." || relative.begin()->string() == "..")
    {
        return {};
    }
    return relative.generic_string();
}

/// Mount prefix for a Workshop content root:
/// `.../steamapps/workshop/content/4000` -> `workshop/content/4000`.
std::string workshop_mount_prefix(const fs::path& root)
{
    std::string prefix;
    bool in_workshop = false;
    for (const fs::path& part : root)
    {
        if (part == constants::kWorkshopFolder)
        {
            in_workshop = true;
        }
        if (in_workshop)
        {
            if (!prefix.empty())
            {
                prefix.push_back('/');
            }
            prefix += part.string();
        }
    }
    return prefix.empty() ? root.filename().string() : prefix;
}

void add_candidate(const fs::path& path, ArchiveVariant variant, const fs::path& game_path,
                   const fs::path& mount_root, const std::string& mount_prefix, JobResult& result)
{
    Candidate candidate;
    candidate.path = absolute_normalized(path);
    candidate.variant = variant;
    candidate.game_path = game_path.generic_string();
    candidate.mount_path = make_mount_path(candidate.path, mount_root, mount_prefix, variant);
    fill_signature(candidate.path, candidate.size, candidate.modified);
    result.candidates.push_back(std::move(candidate));
}

bool is_vpk_directory_file(const fs::path& path, const std::string& name)
{
    if (!parsers::ends_with_ci(name, constants::kVpkExtension) || parsers::is_vpk_chunk_name(name))
    {
        return false;
    }
    if (parsers::ends_with_ci(name, constants::kVpkDirectorySuffix))
    {
        return true;
    }

    // foo.vpk is the directory file only when foo_dir.vpk does not exist.
    const fs::path directory_file =
        path.parent_path() / (path.stem().string() + std::string(constants::kVpkDirectorySuffix));
    std::error_code ec;
    return !fs::is_regular_file(directory_file, ec);
}

void scan_root(const fs::path& root, const fs::path& game_path, const std::string& mount_prefix,
               bool gmod, JobResult& result)
{
    using directory_options = fs::directory_options;
    std::error_code ec;
    fs::recursive_directory_iterator iterator(root, directory_options::skip_permission_denied, ec);
    if (ec)
    {
        result.warnings.push_back("cannot walk " + root.string() + ": " + ec.message());
        return;
    }

    const fs::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(ec))
    {
        if (ec)
        {
            result.warnings.push_back("directory walk stopped in " + root.string() + ": " +
                                      ec.message());
            break;
        }

        const fs::path path = iterator->path();
        const std::string name = path.filename().string();
        if (!name.empty() && name[0] == '.')
        {
            std::error_code type_error;
            if (iterator->is_directory(type_error))
            {
                iterator.disable_recursion_pending();
            }
            continue;
        }

        std::error_code type_error;
        if (!iterator->is_regular_file(type_error) || type_error)
        {
            continue;
        }

        if (gmod)
        {
            if (parsers::ends_with_ci(name, constants::kGmaExtension))
            {
                add_candidate(path, ArchiveVariant::Gma, game_path, root, mount_prefix, result);
            }
            else if (parsers::is_legacy_bin_name(name))
            {
                add_candidate(path, ArchiveVariant::GmaBin, game_path, root, mount_prefix, result);
            }
            continue;
        }

        if (is_vpk_directory_file(path, name))
        {
            add_candidate(path, ArchiveVariant::Vpk, game_path, root, mount_prefix, result);
        }
        else if (parsers::ends_with_ci(name, constants::kBspExtension))
        {
            add_candidate(path, ArchiveVariant::Bsp, game_path, root, mount_prefix, result);
        }
    }
}

void scan_job(const ScanJob& job, JobResult& result)
{
    scan_root(job.root, job.game_path, relative_mount_prefix(job.root, job.game_path), job.gmod,
              result);
    for (const fs::path& extra : job.extra_roots)
    {
        if (extra == job.root)
        {
            continue;
        }
        scan_root(extra, job.game_path, workshop_mount_prefix(extra), job.gmod, result);
    }
}
} // namespace

fs::path absolute_normalized(const fs::path& path)
{
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec)
    {
        absolute = path;
    }
    return absolute.lexically_normal();
}

/// Cheap change marker stored beside the hash. The value is only compared
/// against rows written by this same platform, so file_clock's epoch does not
/// need to be converted to a wall-clock time.
ScanPlan collect_scan_plan(const ArchiveIndexOptions& options)
{
    ScanPlan plan;
    const std::vector<GameFolder> folders = collect_game_folders(options, plan.warnings);
    for (const GameFolder& folder : folders)
    {
        plan.games.push_back(GameRecord{folder.slug, folder.title, folder.path.generic_string()});
        plan.watch_roots.push_back(folder.path);
    }
    std::sort(plan.games.begin(), plan.games.end(),
              [](const GameRecord& left, const GameRecord& right)
              { return left.slug < right.slug; });

    std::vector<ScanJob> jobs;
    jobs.reserve(folders.size() + 1);
    for (const GameFolder& folder : folders)
    {
        jobs.push_back(ScanJob{folder.path, folder.path, false, {}});
    }
    for (const GameFolder& folder : folders)
    {
        if (!folder.garrys_mod)
        {
            continue;
        }
        ScanJob job;
        job.root = gmod_content_folder(folder.path);
        job.game_path = folder.path;
        job.gmod = true;
        job.extra_roots = workshop_roots_for(folder.path);
        plan.watch_roots.insert(plan.watch_roots.end(), job.extra_roots.begin(),
                                job.extra_roots.end());
        jobs.push_back(std::move(job));
        ++plan.gmod_crawlers;
    }

    if (jobs.empty())
    {
        return plan;
    }

    std::vector<JobResult> job_results(jobs.size());
    ThreadPool pool(std::max<std::size_t>(1, jobs.size()));
    for (std::size_t index = 0; index < jobs.size(); ++index)
    {
        pool.submit([&jobs, &job_results, index] { scan_job(jobs[index], job_results[index]); });
    }
    try
    {
        pool.wait();
    }
    catch (const std::exception& exception)
    {
        plan.warnings.push_back(std::string("worker failed: ") + exception.what());
    }

    for (JobResult& job_result : job_results)
    {
        plan.candidates.insert(plan.candidates.end(),
                               std::make_move_iterator(job_result.candidates.begin()),
                               std::make_move_iterator(job_result.candidates.end()));
        plan.warnings.insert(plan.warnings.end(),
                             std::make_move_iterator(job_result.warnings.begin()),
                             std::make_move_iterator(job_result.warnings.end()));
    }

    // Deduplicate before hashing: overlapping roots (an explicit game folder
    // plus the Garry's Mod crawler) must not read the same file twice. Sorting
    // first keeps which copy survives reproducible.
    std::sort(plan.candidates.begin(), plan.candidates.end(),
              [](const Candidate& left, const Candidate& right)
              { return left.path.generic_string() < right.path.generic_string(); });
    std::vector<Candidate> unique;
    unique.reserve(plan.candidates.size());
    std::set<std::string> seen;
    for (Candidate& candidate : plan.candidates)
    {
        if (seen.insert(candidate.path.generic_string()).second)
        {
            unique.push_back(std::move(candidate));
        }
    }
    plan.candidates = std::move(unique);

    // Mount directory names must be unique per game. Two archives can share a
    // stem (`foo_dir.vpk` and `foo.bsp`), so disambiguate rather than fail the
    // UNIQUE(game_id, mount_path) constraint.
    std::set<std::string> mounts;
    for (Candidate& candidate : plan.candidates)
    {
        const std::string base = candidate.mount_path;
        std::string mount = base;
        for (std::size_t suffix = 2;; ++suffix)
        {
            if (mounts.insert(candidate.game_path + '\n' + mount).second)
            {
                break;
            }
            mount = base + '-' + std::to_string(suffix);
        }
        candidate.mount_path = std::move(mount);
    }

    // Distinct roots: game folders are recursive watches, and a workshop root
    // can be nested below one of them.
    std::sort(plan.watch_roots.begin(), plan.watch_roots.end(),
              [](const fs::path& left, const fs::path& right)
              { return left.generic_string() < right.generic_string(); });
    plan.watch_roots.erase(std::unique(plan.watch_roots.begin(), plan.watch_roots.end(),
                                       [](const fs::path& left, const fs::path& right)
                                       { return left.generic_string() == right.generic_string(); }),
                           plan.watch_roots.end());
    std::vector<fs::path> roots;
    for (const fs::path& root : plan.watch_roots)
    {
        bool nested = false;
        for (const fs::path& parent : roots)
        {
            const fs::path relative = root.lexically_relative(parent);
            if (!relative.empty() && relative.begin()->string() != "..")
            {
                nested = true;
                break;
            }
        }
        if (!nested)
        {
            roots.push_back(root);
        }
    }
    plan.watch_roots = std::move(roots);
    return plan;
}

} // namespace sauce::fs::core
