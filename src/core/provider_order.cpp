// SPDX-License-Identifier: MIT

#include <sauce/core/archive_access.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sauce::fs::core
{

namespace
{

namespace fs = std::filesystem;

std::string lower_ascii(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (const char character : text)
    {
        result.push_back(character >= 'A' && character <= 'Z'
                             ? static_cast<char>(character - 'A' + 'a')
                             : character);
    }
    return result;
}

const IndexDatabase::GameInfo* find_game(const std::vector<IndexDatabase::GameInfo>& games,
                                         std::string_view slug)
{
    const std::string key = lower_ascii(slug);
    for (const IndexDatabase::GameInfo& game : games)
    {
        if (lower_ascii(game.slug) == key)
        {
            return &game;
        }
    }
    return nullptr;
}

/// Content root that owns gameinfo and Garry's Mod's addons/config. This is
/// usually `game_path`, but an explicit install-root scan stores the nested
/// `garrysmod` directory as the gameinfo parent.
std::string content_path(const IndexDatabase::GameInfo& game)
{
    if (!game.gameinfo.empty())
    {
        return fs::path(game.gameinfo).parent_path().generic_string();
    }
    return game.game_path;
}

const IndexDatabase::ArchiveInfo*
find_archive(const std::vector<IndexDatabase::ArchiveInfo>& archives, std::string_view path)
{
    for (const IndexDatabase::ArchiveInfo& archive : archives)
    {
        if (archive.path == path)
        {
            return &archive;
        }
    }
    return nullptr;
}

/// Split an absolute path into the indexed root that contains it and the
/// relative prefix below that root.
struct LooseScope
{
    std::string root;
    std::string prefix;
};

LooseScope find_loose_scope(const std::string& path,
                            const std::vector<IndexDatabase::GameInfo>& games)
{
    LooseScope best;
    std::size_t best_length = 0;
    const std::string wanted = fs::path(path).lexically_normal().generic_string();
    for (const IndexDatabase::GameInfo& game : games)
    {
        if (game.game_path.empty())
        {
            continue;
        }
        const std::string root = fs::path(game.game_path).lexically_normal().generic_string();
        if (wanted.size() < root.size() || wanted.compare(0, root.size(), root) != 0)
        {
            continue;
        }
        if (wanted.size() > root.size() && wanted[root.size()] != '/')
        {
            continue;
        }
        if (root.size() <= best_length)
        {
            continue;
        }
        best_length = root.size();
        best.root = root;
        best.prefix.clear();
        if (wanted.size() > root.size() + 1)
        {
            best.prefix = wanted.substr(root.size() + 1);
        }
    }
    return best;
}

bool contains_path(std::string_view path, std::string_view prefix)
{
    if (path.size() < prefix.size() || path.compare(0, prefix.size(), prefix) != 0)
    {
        return false;
    }
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}

bool starts_with_ci(std::string_view text, std::string_view prefix)
{
    if (text.size() < prefix.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index)
    {
        const char left = text[index] >= 'A' && text[index] <= 'Z'
                              ? static_cast<char>(text[index] - 'A' + 'a')
                              : text[index];
        const char right = prefix[index] >= 'A' && prefix[index] <= 'Z'
                               ? static_cast<char>(prefix[index] - 'A' + 'a')
                               : prefix[index];
        if (left != right)
        {
            return false;
        }
    }
    return true;
}

class LayerBuilder
{
public:
    LayerBuilder(std::shared_ptr<const IndexDatabase> database,
                 const std::vector<IndexDatabase::GameInfo>& games,
                 const std::vector<IndexDatabase::ArchiveInfo>& archives)
        : database_(std::move(database)), games_(games), archives_(archives)
    {
    }

    void add_archive(const IndexDatabase::ArchiveInfo& archive)
    {
        if (!archive_ids_.insert(archive.id).second)
        {
            return;
        }
        auto provider = database_->archive_provider(archive.id);
        if (provider)
        {
            layers_.push_back(std::move(*provider));
        }
    }

    void add_loose(std::string_view path)
    {
        const LooseScope scope = find_loose_scope(std::string(path), games_);
        if (scope.root.empty())
        {
            return;
        }
        const std::string key = scope.root + '\n' + scope.prefix;
        if (!loose_scopes_.insert(key).second)
        {
            return;
        }
        auto provider = database_->loose_provider(scope.root, scope.prefix);
        if (provider)
        {
            layers_.push_back(std::move(*provider));
        }
    }

    void add_loose_addons(const std::string& game_path)
    {
        const LooseScope addon_scope = find_loose_scope(game_path + "/addons", games_);
        if (addon_scope.root.empty())
        {
            return;
        }
        auto addons = database_->loose_provider(addon_scope.root, addon_scope.prefix);
        if (!addons)
        {
            return;
        }
        auto entries = (*addons)->list("");
        if (!entries)
        {
            return;
        }
        std::vector<std::string> names;
        for (const DirectoryEntry& entry : *entries)
        {
            if (entry.info.kind == NodeInfo::Kind::Directory)
            {
                names.push_back(entry.name);
            }
        }
        std::sort(names.begin(), names.end(), std::greater<>{});
        for (const std::string& name : names)
        {
            add_loose(game_path + "/addons/" + name);
        }
    }

    void add_search_paths(const IndexDatabase::GameInfo& game)
    {
        for (const IndexDatabase::GameInfo::SearchPath& path : game.search_paths)
        {
            if (path.kind == "vpk")
            {
                if (const auto* archive = find_archive(archives_, path.resolved))
                {
                    add_archive(*archive);
                }
            }
            else if (path.kind == "dir" && !path.resolved.empty())
            {
                add_loose(path.resolved);
            }
        }
    }

    void add_bsp_layers(std::int64_t game_id)
    {
        std::vector<const IndexDatabase::ArchiveInfo*> maps;
        for (const IndexDatabase::ArchiveInfo& archive : archives_)
        {
            if (archive.game_id == game_id && archive.variant == "bsp-pak")
            {
                maps.push_back(&archive);
            }
        }
        std::sort(maps.begin(), maps.end(), [](const auto* left, const auto* right)
                  { return left->mount_path < right->mount_path; });
        for (const IndexDatabase::ArchiveInfo* archive : maps)
        {
            add_archive(*archive);
        }
    }

    Result<ProviderPtr> finish() const
    {
        if (layers_.empty())
        {
            return ProviderPtr(std::make_shared<TreeProvider>(std::vector<TreeProvider::Child>{}));
        }
        return ProviderPtr(std::make_shared<LayeredProvider>(layers_));
    }

    const std::vector<IndexDatabase::GameInfo>& games() const
    {
        return games_;
    }

    const std::vector<IndexDatabase::ArchiveInfo>& archives() const
    {
        return archives_;
    }

private:
    std::shared_ptr<const IndexDatabase> database_;
    const std::vector<IndexDatabase::GameInfo>& games_;
    const std::vector<IndexDatabase::ArchiveInfo>& archives_;
    std::vector<ProviderPtr> layers_;
    std::set<std::int64_t> archive_ids_;
    std::set<std::string> loose_scopes_;
};

std::vector<const IndexDatabase::ArchiveInfo*>
addon_archives(const std::vector<IndexDatabase::ArchiveInfo>& archives, std::int64_t game_id,
               const std::string& game_path, bool workshop)
{
    std::vector<const IndexDatabase::ArchiveInfo*> selected;
    const std::string addon_root = game_path + "/addons";
    for (const IndexDatabase::ArchiveInfo& archive : archives)
    {
        if (archive.game_id != game_id ||
            (archive.variant != "gma" && archive.variant != "gma-bin"))
        {
            continue;
        }
        const std::string path = archive.path;
        const bool under_workshop = path.find("/workshop/content/4000/") != std::string::npos ||
                                    path.find("/cache/workshop/") != std::string::npos ||
                                    starts_with_ci(archive.mount_path, "workshop/") ||
                                    starts_with_ci(archive.mount_path, "cache/workshop/");
        const bool under_addons =
            contains_path(path, addon_root) || starts_with_ci(archive.mount_path, "addons/");
        if ((workshop && under_workshop) || (!workshop && under_addons))
        {
            selected.push_back(&archive);
        }
    }
    // Later names win in Garry's Mod, so put the end of the alphabetical list
    // first in the layer stack.
    std::sort(selected.begin(), selected.end(), [](const auto* left, const auto* right)
              { return left->mount_path > right->mount_path; });
    return selected;
}

} // namespace

Result<ProviderPtr> IndexDatabase::engine_game_provider(std::string_view slug) const
{
    auto games_result = games();
    if (!games_result)
    {
        return std::unexpected(games_result.error());
    }
    auto archives_result = archives();
    if (!archives_result)
    {
        return std::unexpected(archives_result.error());
    }
    const GameInfo* game = find_game(*games_result, slug);
    if (game == nullptr)
    {
        return std::unexpected(Error{ErrorCode::NotFound, "game is not indexed"});
    }

    LayerBuilder builder(shared_from_this(), *games_result, *archives_result);
    builder.add_search_paths(*game);
    builder.add_bsp_layers(game->id);
    return builder.finish();
}

Result<ProviderPtr> IndexDatabase::garrysmod_game_provider(std::string_view slug) const
{
    auto games_result = games();
    if (!games_result)
    {
        return std::unexpected(games_result.error());
    }
    auto archives_result = archives();
    if (!archives_result)
    {
        return std::unexpected(archives_result.error());
    }
    const GameInfo* game = find_game(*games_result, slug);
    if (game == nullptr)
    {
        return std::unexpected(Error{ErrorCode::NotFound, "game is not indexed"});
    }

    LayerBuilder builder(shared_from_this(), *games_result, *archives_result);

    const std::string content = content_path(*game);

    // Loose addon directories are the easiest place to iterate on content, so
    // they stay above workshop and local archives.
    builder.add_loose_addons(content);

    for (const IndexDatabase::ArchiveInfo* archive :
         addon_archives(builder.archives(), game->id, content, true))
    {
        builder.add_archive(*archive);
    }
    for (const IndexDatabase::ArchiveInfo* archive :
         addon_archives(builder.archives(), game->id, content, false))
    {
        builder.add_archive(*archive);
    }

    builder.add_search_paths(*game);

    for (const GameInfo::Mount& mount : game->mounts)
    {
        builder.add_loose(mount.path);
        std::vector<const IndexDatabase::ArchiveInfo*> mounted;
        for (const IndexDatabase::ArchiveInfo& archive : builder.archives())
        {
            if (archive.variant == "vpk" && contains_path(archive.path, mount.path))
            {
                mounted.push_back(&archive);
            }
        }
        std::sort(mounted.begin(), mounted.end(), [](const auto* left, const auto* right)
                  { return left->mount_path > right->mount_path; });
        for (const IndexDatabase::ArchiveInfo* archive : mounted)
        {
            builder.add_archive(*archive);
        }
    }

    builder.add_bsp_layers(game->id);
    return builder.finish();
}

Result<ProviderPtr> IndexDatabase::ordered_game_provider(std::string_view slug,
                                                         GameOrder order) const
{
    if (order == GameOrder::Engine)
    {
        return engine_game_provider(slug);
    }
    if (order == GameOrder::GarrysMod)
    {
        return garrysmod_game_provider(slug);
    }

    auto games_result = games();
    if (!games_result)
    {
        return std::unexpected(games_result.error());
    }
    const GameInfo* game = find_game(*games_result, slug);
    if (game == nullptr)
    {
        return std::unexpected(Error{ErrorCode::NotFound, "game is not indexed"});
    }

    if (order == GameOrder::Auto)
    {
        if (game->garrys_mod)
        {
            return garrysmod_game_provider(slug);
        }
        if (!game->search_paths.empty())
        {
            return engine_game_provider(slug);
        }
    }

    auto archives_result = game_provider(slug);
    if (!archives_result)
    {
        return std::unexpected(archives_result.error());
    }
    return archives_result;
}

} // namespace sauce::fs::core
