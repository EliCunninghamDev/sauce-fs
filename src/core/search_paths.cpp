// SPDX-License-Identifier: MIT

#include "core/detail/search_paths.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "parsers/bytes.hpp"
#include "parsers/gameinfo.hpp"
#include "parsers/vdf.hpp"
#include "parsers/vpk.hpp"

namespace sauce::fs::core
{

namespace
{

namespace fs = std::filesystem;

constexpr std::string_view kGameInfoToken = "|gameinfo_path|";
constexpr std::string_view kEngineToken = "|all_source_engine_paths|";
constexpr std::string_view kMountConfig = "mount.cfg";

struct ResolvedPath
{
    fs::path path;
    std::string kind;
};

fs::path normalized_absolute(const fs::path& path)
{
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec)
    {
        absolute = path;
    }
    return absolute.lexically_normal();
}

/// Resolve `remainder` below the closest ancestor of `start` that contains it.
fs::path engine_relative_root(const fs::path& start, const fs::path& remainder)
{
    std::error_code ec;
    for (fs::path ancestor = start; !ancestor.empty(); ancestor = ancestor.parent_path())
    {
        const fs::path candidate = ancestor / remainder;
        if (fs::exists(candidate, ec) ||
            (parsers::ends_with_ci(remainder.filename().string(), ".vpk") &&
             fs::is_regular_file(parsers::resolve_vpk_path(candidate), ec)))
        {
            return ancestor;
        }
        ec.clear();
        if (ancestor == ancestor.root_path())
        {
            break;
        }
    }
    return {};
}

fs::path apply_tokens(std::string_view raw, const fs::path& game_path, const fs::path& base)
{
    if (parsers::starts_with(raw, kGameInfoToken))
    {
        const std::string_view remainder = raw.substr(kGameInfoToken.size());
        if (remainder.empty() || remainder == ".")
        {
            return game_path;
        }
        return game_path / fs::path(remainder);
    }
    if (parsers::starts_with(raw, kEngineToken))
    {
        const fs::path remainder(raw.substr(kEngineToken.size()));
        const fs::path root = engine_relative_root(game_path.parent_path(), remainder);
        return root.empty() ? fs::path{} : root / remainder;
    }
    return base / fs::path(raw);
}

std::string kind_of(const fs::path& path)
{
    std::error_code ec;
    if (fs::is_directory(path, ec))
    {
        return "dir";
    }
    ec.clear();
    if (!fs::is_regular_file(path, ec))
    {
        return "missing";
    }
    return parsers::ends_with_ci(path.filename().string(), ".vpk") ? "vpk" : "file";
}

std::vector<ResolvedPath> resolve_entry(std::string_view raw, const fs::path& game_path,
                                        const fs::path& base)
{
    const fs::path pattern = apply_tokens(raw, game_path, base);
    std::vector<ResolvedPath> resolved;
    if (pattern.empty())
    {
        return resolved;
    }

    std::vector<fs::path> matches;
    if (pattern.generic_string().find_first_of("*?") != std::string::npos)
    {
        matches = parsers::expand_wildcard(pattern);
    }
    else
    {
        matches.push_back(pattern);
    }

    for (fs::path& match : matches)
    {
        if (parsers::ends_with_ci(match.filename().string(), ".vpk"))
        {
            match = parsers::resolve_vpk_path(match);
        }
        const std::string kind = kind_of(match);
        if (kind != "missing")
        {
            resolved.push_back(ResolvedPath{normalized_absolute(match), kind});
        }
    }
    return resolved;
}

} // namespace

fs::path find_gameinfo(const fs::path& game_path)
{
    std::error_code ec;
    const fs::path text = game_path / "gameinfo.txt";
    if (fs::is_regular_file(text, ec))
    {
        return text;
    }
    ec.clear();
    const fs::path source2 = game_path / "gameinfo.gi";
    if (fs::is_regular_file(source2, ec))
    {
        return source2;
    }
    return {};
}

std::vector<GameSearchPath> load_search_paths(const fs::path& game_path, const fs::path& gameinfo,
                                              std::vector<std::string>& warnings)
{
    std::vector<GameSearchPath> result;
    if (gameinfo.empty())
    {
        return result;
    }

    const std::vector<parsers::GameInfoEntry> entries =
        parsers::parse_gameinfo_search_paths(gameinfo, warnings);
    const fs::path base = game_path.parent_path();
    for (const parsers::GameInfoEntry& entry : entries)
    {
        std::vector<ResolvedPath> resolved = resolve_entry(entry.path, game_path, base);
        if (resolved.empty())
        {
            result.push_back(GameSearchPath{entry.types, entry.path, {}, "missing"});
            continue;
        }
        for (ResolvedPath& path : resolved)
        {
            result.push_back(GameSearchPath{entry.types, entry.path, path.path.generic_string(),
                                            std::move(path.kind)});
        }
    }
    return result;
}

std::vector<GameMount> load_game_mounts(const fs::path& game_path,
                                        std::vector<std::string>& warnings)
{
    std::vector<GameMount> result;
    const fs::path config = game_path / "cfg" / kMountConfig;
    std::error_code ec;
    if (!fs::is_regular_file(config, ec))
    {
        return result;
    }

    parsers::VdfNode root;
    std::string error;
    if (!parsers::load_vdf(config, root, &error))
    {
        warnings.push_back("cannot read " + config.string() + ": " + error);
        return result;
    }

    const parsers::VdfNode* mountcfg = &root;
    if (!parsers::ascii_iequals(root.key, "mountcfg"))
    {
        mountcfg = root.child("mountcfg");
    }
    if (mountcfg == nullptr)
    {
        return result;
    }

    const fs::path base = game_path.parent_path();
    for (const parsers::VdfNode& node : mountcfg->children)
    {
        if (node.key.empty() || node.value.empty())
        {
            continue;
        }
        fs::path path(node.value);
        if (path.is_relative())
        {
            path = base / path;
        }
        result.push_back(GameMount{node.key, normalized_absolute(path).generic_string(),
                                   std::string(kMountConfig)});
    }
    return result;
}

} // namespace sauce::fs::core
