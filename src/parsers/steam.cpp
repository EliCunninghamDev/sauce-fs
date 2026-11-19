// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#include "steam.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "bytes.hpp"
#include "vdf.hpp"

namespace sauce::fs::parsers
{
namespace
{

std::string trim_copy(std::string_view text)
{
    return trim(text);
}

std::uint64_t vdf_uint(std::string_view text)
{
    std::uint64_t value = 0;
    bool started = false;
    for (char c : text)
    {
        if (c < '0' || c > '9')
        {
            if (started)
            {
                break;
            }
            continue;
        }
        started = true;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

bool contains_ci(std::string_view text, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i)
    {
        if (ascii_iequals(text.substr(i, needle.size()), needle))
        {
            return true;
        }
    }
    return false;
}

std::string ascii_lower_copy(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
    {
        c = ascii_lower(c);
    }
    return result;
}

std::string canonical_path(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    if (ec)
    {
        result = std::filesystem::absolute(path, ec);
    }
    if (ec)
    {
        result = path;
    }
    return result.lexically_normal().string();
}

std::string environment_value(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) == 0 && value)
    {
        std::string result(value);
        std::free(value);
        return result;
    }
    return {};
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

std::vector<std::filesystem::path> default_steam_roots()
{
    std::vector<std::filesystem::path> roots;
    std::error_code ec;
    auto add = [&](const std::filesystem::path& path)
    {
        if (!path.empty() && std::filesystem::is_directory(path, ec))
        {
            roots.push_back(path);
        }
    };
    for (const char* variable : {"STEAM_ROOT", "STEAM_PATH", "STEAM_DIR"})
    {
        const std::string value = environment_value(variable);
        if (!value.empty())
        {
            add(value);
        }
    }
    const std::string home = environment_value(
#if defined(_WIN32)
        "USERPROFILE"
#else
        "HOME"
#endif
    );
#if defined(_WIN32)
    const std::string program_files = environment_value("ProgramFiles(x86)");
    const std::string program_files64 = environment_value("ProgramFiles");
    if (!program_files.empty())
    {
        add(std::filesystem::path(program_files) / "Steam");
    }
    if (!program_files64.empty())
    {
        add(std::filesystem::path(program_files64) / "Steam");
    }
    add("C:/Program Files (x86)/Steam");
#elif defined(__APPLE__)
    if (!home.empty())
    {
        add(std::filesystem::path(home) / "Library/Application Support/Steam");
    }
#else
    if (!home.empty())
    {
        add(std::filesystem::path(home) / ".steam/steam");
        add(std::filesystem::path(home) / ".steam/root");
        add(std::filesystem::path(home) / ".local/share/Steam");
    }
    const std::string xdg = environment_value("XDG_DATA_HOME");
    if (!xdg.empty())
    {
        add(std::filesystem::path(xdg) / "Steam");
    }
#endif
    return roots;
}

struct KnownTitle
{
    std::uint32_t appid;
    const char* game_dir;
    bool source2;
};

const KnownTitle* known_title(std::uint32_t appid)
{
    static const KnownTitle titles[] = {
        {220, "hl2", false},
        {240, "cstrike", false},
        {340, "lostcoast", false},
        {380, "episodic", false},
        {400, "portal", false},
        {420, "ep2", false},
        {440, "tf", false},
        {550, "left4dead2", false},
        {4000, "garrysmod", false},
        {232250, "tf", false},
        {869480, "jbep3", false},
        {869800, "jbep3", false},
        {1802710, "momentum", false},
        {3545060, "tf2classified", false},
        {730, "game/csgo", true},
        {570, "game/dota", true},
        {546560, "game/hlvr", true},
        {590830, "game/sbox", true},
        {1422450, "game/citadel", true},
    };
    for (const KnownTitle& title : titles)
    {
        if (title.appid == appid)
        {
            return &title;
        }
    }
    return nullptr;
}

std::vector<std::filesystem::path> find_named_files(const std::filesystem::path& root,
                                                    std::string_view name, int max_depth,
                                                    std::vector<std::string>* warnings)
{
    std::vector<std::filesystem::path> results;
    if (!std::filesystem::is_directory(root))
    {
        return results;
    }
    struct Pending
    {
        std::filesystem::path directory;
        int depth;
    };
    std::vector<Pending> pending{{root, 0}};
    while (!pending.empty())
    {
        const Pending current = pending.back();
        pending.pop_back();
        std::error_code ec;
        std::vector<std::filesystem::path> children;
        for (std::filesystem::directory_iterator it(
                 current.directory, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            const std::filesystem::path path = it->path();
            if (std::filesystem::is_directory(path, ec))
            {
                children.push_back(path);
            }
            else if (ascii_iequals(path.filename().string(), name))
            {
                results.push_back(path);
            }
        }
        if (ec && warnings)
        {
            warnings->push_back("cannot scan " + current.directory.string() + ": " + ec.message());
        }
        if (current.depth >= max_depth)
        {
            continue;
        }
        for (auto& child : children)
        {
            pending.push_back(Pending{std::move(child), current.depth + 1});
        }
    }
    std::sort(results.begin(), results.end());
    return results;
}

int gameinfo_score(const std::filesystem::path& gameinfo)
{
    const std::filesystem::path directory = gameinfo.parent_path();
    const std::string name = ascii_lower_copy(directory.filename().string());
    const std::string full = ascii_lower_copy(directory.generic_string());
    int score = static_cast<int>(name.size());
    if (name == "update" || name == "bin" || name == "platform")
    {
        score += 10000;
    }
    if (name.find("_dlc") != std::string::npos || name.find("_complete") != std::string::npos)
    {
        score += 5000;
    }
    if (full.find("/custom/") != std::string::npos || full.find("/addons/") != std::string::npos ||
        full.find("/download/") != std::string::npos)
    {
        score += 2000;
    }
    return score;
}

std::filesystem::path choose_gameinfo(const std::vector<std::filesystem::path>& candidates)
{
    std::filesystem::path best;
    int best_score = 0;
    for (const std::filesystem::path& candidate : candidates)
    {
        const int score = gameinfo_score(candidate);
        if (best.empty() || score < best_score ||
            (score == best_score && candidate.generic_string() < best.generic_string()))
        {
            best = candidate;
            best_score = score;
        }
    }
    return best;
}

bool library_from_vdf_entry(const VdfNode& entry, const std::string& vdf_path,
                            std::vector<SteamLibrary>& libraries,
                            std::unordered_set<std::string>& seen,
                            std::vector<std::string>* warnings)
{
    std::string path = entry.child_value("path");
    std::string label = entry.child_value("label");
    if (path.empty() && !entry.is_block())
    {
        path = entry.value;
    }
    path = trim_copy(path);
    if (path.empty())
    {
        return false;
    }
    std::filesystem::path root(path);
    std::error_code ec;
    if (!std::filesystem::is_directory(root / "steamapps", ec))
    {
        if (warnings)
        {
            warnings->push_back("Steam library has no steamapps directory: " + path);
        }
        return false;
    }
    const std::string canonical = canonical_path(root);
    if (!seen.insert(canonical).second)
    {
        return false;
    }
    SteamLibrary library;
    library.root = canonical;
    library.steamapps = (std::filesystem::path(canonical) / "steamapps").string();
    library.libraryfolders = vdf_path;
    library.label = trim_copy(label);
    libraries.push_back(std::move(library));
    return true;
}

} // namespace

std::vector<SteamLibrary> find_steam_libraries(const DiscoveryOptions& options,
                                               std::vector<std::string>* warnings)
{
    std::vector<SteamLibrary> libraries;
    std::unordered_set<std::string> seen;
    std::vector<std::filesystem::path> roots = options.steam_roots;
    if (roots.empty())
    {
        roots = default_steam_roots();
    }
    for (const std::filesystem::path& root : roots)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
        {
            if (warnings)
            {
                warnings->push_back("Steam root does not exist: " + root.string());
            }
            continue;
        }
        const std::string canonical = canonical_path(root);
        const std::filesystem::path base(canonical);
        const std::string steamapps = (base / "steamapps").string();
        if (std::filesystem::is_directory(base / "steamapps", ec) && seen.insert(canonical).second)
        {
            SteamLibrary library;
            library.root = canonical;
            library.steamapps = steamapps;
            libraries.push_back(std::move(library));
        }
        for (const std::filesystem::path& relative :
             {std::filesystem::path("steamapps/libraryfolders.vdf"),
              std::filesystem::path("config/libraryfolders.vdf")})
        {
            const std::filesystem::path vdf_path = base / relative;
            if (!std::filesystem::is_regular_file(vdf_path, ec))
            {
                if (warnings)
                {
                    warnings->push_back("no libraryfolders.vdf at " + vdf_path.string());
                }
                continue;
            }
            VdfNode document;
            std::string parse_error;
            if (!load_vdf(vdf_path, document, &parse_error))
            {
                if (warnings)
                {
                    warnings->push_back(parse_error);
                }
                continue;
            }
            for (SteamLibrary& library : libraries)
            {
                if (library.root == canonical && library.libraryfolders.empty())
                {
                    library.libraryfolders = vdf_path.string();
                }
            }
            const VdfNode* table = &document;
            const VdfNode* nested = document.child("libraryfolders");
            if (nested && nested->is_block())
            {
                table = nested;
            }
            for (const VdfNode& entry : table->children)
            {
                if (entry.is_block())
                {
                    library_from_vdf_entry(entry, vdf_path.string(), libraries, seen, warnings);
                }
                else if (!entry.key.empty() &&
                         entry.key.find_first_not_of("0123456789") == std::string::npos)
                {
                    library_from_vdf_entry(entry, vdf_path.string(), libraries, seen, warnings);
                }
            }
        }
    }
    return libraries;
}

std::vector<InstalledTitle> discover_source_titles(const DiscoveryOptions& options,
                                                   std::vector<std::string>* warnings)
{
    const int max_depth = options.max_gameinfo_depth < 0 ? 0 : options.max_gameinfo_depth;
    std::vector<InstalledTitle> titles;
    const std::vector<SteamLibrary> libraries = find_steam_libraries(options, warnings);
    for (const SteamLibrary& library : libraries)
    {
        const std::filesystem::path steamapps(library.steamapps);
        std::error_code ec;
        std::vector<std::filesystem::path> manifests;
        for (std::filesystem::directory_iterator it(
                 steamapps, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file(ec))
            {
                continue;
            }
            const std::string name = it->path().filename().string();
            const std::string lower = ascii_lower_copy(name);
            if (lower.rfind("appmanifest_", 0) == 0 && lower.size() > 4 &&
                lower.compare(lower.size() - 4, 4, ".acf") == 0)
            {
                manifests.push_back(it->path());
            }
        }
        if (ec && warnings)
        {
            warnings->push_back("cannot scan " + steamapps.string() + ": " + ec.message());
        }
        std::sort(manifests.begin(), manifests.end());
        for (const std::filesystem::path& manifest : manifests)
        {
            VdfNode document;
            std::string parse_error;
            if (!load_vdf(manifest, document, &parse_error))
            {
                if (warnings)
                {
                    warnings->push_back(parse_error);
                }
                continue;
            }
            const VdfNode* state = document.child("AppState");
            if (!state)
            {
                state = &document;
            }
            InstalledTitle title;
            title.appid = static_cast<std::uint32_t>(vdf_uint(state->child_value("appid")));
            title.name = state->child_value("name");
            const std::string install_name = state->child_value("installdir");
            const std::uint64_t flags = vdf_uint(state->child_value("StateFlags"));
            title.size_on_disk = vdf_uint(state->child_value("SizeOnDisk"));
            if (title.appid == 0 || install_name.empty())
            {
                continue;
            }
            const bool installed_flag = flags == 0 || (flags & 4u) != 0;
            if (!installed_flag && !options.include_uninstalled)
            {
                continue;
            }
            title.library = library.root;
            title.manifest = manifest.string();
            title.install_dir =
                (std::filesystem::path(library.steamapps) / "common" / install_name).string();
            const bool install_exists = std::filesystem::is_directory(title.install_dir, ec);
            if (!install_exists && !options.include_uninstalled)
            {
                continue;
            }
            title.dedicated_server = contains_ci(title.name, "dedicated server");
            if (title.dedicated_server && !options.include_dedicated_servers)
            {
                continue;
            }

            const KnownTitle* known = known_title(title.appid);
            bool source1 = false;
            bool source2 = false;
            if (known && !known->source2)
            {
                const std::filesystem::path game_dir =
                    std::filesystem::path(title.install_dir) / known->game_dir;
                const std::filesystem::path gameinfo = game_dir / "gameinfo.txt";
                if (std::filesystem::is_regular_file(gameinfo, ec))
                {
                    title.game_dir = game_dir.string();
                    title.gameinfo = gameinfo.string();
                    title.gameinfo_present = true;
                    source1 = true;
                }
                else if (!install_exists && options.include_uninstalled)
                {
                    title.game_dir = game_dir.string();
                    title.gameinfo = gameinfo.string();
                    source1 = true;
                }
            }
            if (!source1 && install_exists)
            {
                const std::vector<std::filesystem::path> found =
                    find_named_files(title.install_dir, "gameinfo.txt", max_depth, warnings);
                if (!found.empty())
                {
                    const std::filesystem::path chosen = choose_gameinfo(found);
                    title.game_dir = chosen.parent_path().string();
                    title.gameinfo = chosen.string();
                    title.gameinfo_present = true;
                    source1 = true;
                }
            }
            if (!source1)
            {
                if (known && known->source2)
                {
                    source2 = true;
                    const std::filesystem::path game_dir =
                        std::filesystem::path(title.install_dir) / known->game_dir;
                    title.game_dir = game_dir.string();
                    const std::filesystem::path gameinfo = game_dir / "gameinfo.gi";
                    if (std::filesystem::is_regular_file(gameinfo, ec))
                    {
                        title.gameinfo = gameinfo.string();
                    }
                }
                else if (install_exists)
                {
                    const std::vector<std::filesystem::path> found =
                        find_named_files(title.install_dir, "gameinfo.gi", max_depth, warnings);
                    if (!found.empty())
                    {
                        const std::filesystem::path chosen = found.front();
                        title.game_dir = chosen.parent_path().string();
                        title.gameinfo = chosen.string();
                        source2 = true;
                    }
                }
            }
            if (!source1 && !source2)
            {
                continue;
            }
            if (source2)
            {
                if (!options.include_source2)
                {
                    continue;
                }
                title.source2 = true;
            }
            titles.push_back(std::move(title));
        }
    }
    std::sort(titles.begin(), titles.end(),
              [](const InstalledTitle& a, const InstalledTitle& b)
              {
                  const std::string left = ascii_lower_copy(a.name);
                  const std::string right = ascii_lower_copy(b.name);
                  if (left != right)
                  {
                      return left < right;
                  }
                  if (a.name != b.name)
                  {
                      return a.name < b.name;
                  }
                  return a.appid < b.appid;
              });
    return titles;
}

} // namespace sauce::fs::parsers
