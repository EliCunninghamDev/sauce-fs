// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sauce::fs::parsers
{

/// A Steam library discovered from `libraryfolders.vdf` or a Steam root.
struct SteamLibrary
{
    /// Library root (the directory containing `steamapps`).
    std::string root;
    /// `<root>/steamapps`.
    std::string steamapps;
    /// The `libraryfolders.vdf` that declared this library, if any.
    std::string libraryfolders;
    /// Optional Steam label (usually empty).
    std::string label;
};

/// One installed application that contains Source engine content.
struct InstalledTitle
{
    std::uint32_t appid = 0;
    /// Manifest display name, e.g. "Counter-Strike: Source".
    std::string name;
    /// `.../steamapps/common/<installdir>`.
    std::string install_dir;
    /// Directory containing `gameinfo.txt`, e.g. `.../cstrike`.
    std::string game_dir;
    /// `<game_dir>/gameinfo.txt`, or `gameinfo.gi` for Source 2, when present.
    std::string gameinfo;
    /// Library root that owns the manifest.
    std::string library;
    /// Path to the `appmanifest_*.acf`.
    std::string manifest;
    bool dedicated_server = false;
    /// Detected Source 2 content (`gameinfo.gi`, CS2, Deadlock, ...).
    bool source2 = false;
    /// True when a Source 1 `gameinfo.txt` was found.
    bool gameinfo_present = false;
    /// `SizeOnDisk` from the manifest, or 0 when unknown.
    std::uint64_t size_on_disk = 0;
};

/// Options for find_steam_libraries() and discover_source_titles().
struct DiscoveryOptions
{
    /// Steam roots to inspect.  Empty means auto-detect the usual Linux,
    /// macOS, and Windows locations plus `$STEAM_ROOT`.
    std::vector<std::filesystem::path> steam_roots;
    /// Include apps whose name contains "Dedicated Server".
    bool include_dedicated_servers = true;
    /// Include Source 2 installs (CS2, Deadlock, ...).  They have no
    /// Source 1 `gameinfo.txt` and cannot be passed to add_title().
    bool include_source2 = false;
    /// Include manifests whose install directory is missing.
    bool include_uninstalled = false;
    /// How many directory levels below `install_dir` to search for
    /// `gameinfo.txt`.  1 is enough for every standard Source install.
    int max_gameinfo_depth = 2;
};

/// Locate Steam libraries by reading `libraryfolders.vdf` from each Steam
/// root.  Both the modern nested format and the older numeric format are
/// accepted.  Missing or malformed files produce warnings, not failures.
std::vector<SteamLibrary> find_steam_libraries(const DiscoveryOptions& options = {},
                                               std::vector<std::string>* warnings = nullptr);

/// Find every installed Source engine title across all Steam libraries.
///
/// Each `appmanifest_*.acf` is parsed, its install directory is checked for a
/// `gameinfo.txt`, and the correct game directory is selected.  Results are
/// sorted by display name.  Source 2 installs are skipped unless
/// `include_source2` is set.
std::vector<InstalledTitle> discover_source_titles(const DiscoveryOptions& options = {},
                                                   std::vector<std::string>* warnings = nullptr);

} // namespace sauce::fs::parsers
