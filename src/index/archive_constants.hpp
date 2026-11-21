// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>

/// String values shared by the index writer, the runtime reader, and the
/// mount. Keeping them here avoids scattering database values and archive
/// naming rules across translation units.
namespace sauce::fs::constants
{

// Steam library layout used for Garry's Mod workshop discovery.
inline constexpr std::string_view kSteamAppsFolder = "steamapps";
inline constexpr std::string_view kWorkshopFolder = "workshop";
inline constexpr std::string_view kWorkshopContentFolder = "content";
inline constexpr std::string_view kGarrysModAppId = "4000";

// Garry's Mod discovery.
inline constexpr std::string_view kGarrysModFolder = "garrysmod";
inline constexpr std::string_view kGarrysModTitleHint = "garry";
inline constexpr std::string_view kFallbackGameSlug = "game";

// `archive_type` values. Keep in sync with the seed rows in archive_sql.hpp.
inline constexpr std::int64_t kArchiveTypeVpkId = 1;
inline constexpr std::int64_t kArchiveTypeBspPakId = 2;
inline constexpr std::int64_t kArchiveTypeGmaId = 3;
inline constexpr std::int64_t kArchiveTypeGmaBinId = 4;

// `archive_type.name` values, also used by the reader.
inline constexpr std::string_view kVpk = "vpk";
inline constexpr std::string_view kBspPak = "bsp-pak";
inline constexpr std::string_view kGma = "gma";
inline constexpr std::string_view kGmaBin = "gma-bin";
inline constexpr std::string_view kUnknownVariant = "unknown";

// `archive_entries.kind` values.
inline constexpr std::string_view kDirectoryKind = "dir";
inline constexpr std::string_view kFileKind = "file";

// `archive_entries.assettype` values.
inline constexpr std::string_view kAssetUnknown = "Unknown";
inline constexpr std::string_view kAssetModel = "Model";
inline constexpr std::string_view kAssetMaterial = "Material";
inline constexpr std::string_view kAssetTexture = "Texture";
inline constexpr std::string_view kAssetSound = "Sound";
inline constexpr std::string_view kAssetMap = "Map";
inline constexpr std::string_view kAssetScript = "Script";

// Archive file names and suffixes.
inline constexpr std::string_view kVpkExtension = ".vpk";
inline constexpr std::string_view kVpkDirectorySuffix = "_dir.vpk";
inline constexpr std::string_view kVpkDirectoryStemSuffix = "_dir";
inline constexpr std::string_view kBspExtension = ".bsp";
inline constexpr std::string_view kGmaExtension = ".gma";

} // namespace sauce::fs::constants
