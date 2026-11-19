// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sauce::fs::parsers
{

/// One node of a Valve KeyValues (VDF) document.
///
/// Keys are compared case-insensitively.  Children keep file order and may
/// repeat, which matters for files such as `libraryfolders.vdf` and
/// `appmanifest_*.acf` that store several entries under numeric keys.
struct VdfNode
{
    std::string key;
    /// Scalar value; empty when the node is a block.
    std::string value;
    std::vector<VdfNode> children;

    bool is_block() const noexcept
    {
        return !children.empty();
    }

    /// First direct child with this key, or nullptr.
    const VdfNode* child(std::string_view child_key) const noexcept;

    /// Every direct child with this key, in file order.
    std::vector<const VdfNode*> children_named(std::string_view child_key) const;

    /// Scalar value of a direct child, or `fallback` when it is absent.
    std::string child_value(std::string_view child_key, std::string_view fallback = {}) const;
};

/// Parse a VDF/KeyValues document.  Handles quoted and unquoted tokens,
/// `//` comments, escaped characters, and `[$condition]` tokens.
bool parse_vdf(std::string_view text, VdfNode& root, std::string* error = nullptr);

/// Read and parse a VDF/KeyValues file.
bool load_vdf(const std::filesystem::path& path, VdfNode& root, std::string* error = nullptr);

} // namespace sauce::fs::parsers
