// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#include "vdf.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bytes.hpp"

namespace sauce::fs::parsers
{
namespace
{

bool vdf_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

class VdfReader
{
public:
    explicit VdfReader(std::string_view text) : text_(text)
    {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF)
        {
            position_ = 3;
        }
    }

    std::size_t line() const noexcept
    {
        return line_;
    }

    /// Read one token.  Returns false at end of input or on a syntax error;
    /// `error` is set for errors and left empty at a clean end.
    bool next(std::string& out, std::string* error)
    {
        out.clear();
        skip();
        if (position_ >= text_.size())
        {
            return false;
        }
        const char c = text_[position_];
        if (c == '{' || c == '}')
        {
            out.push_back(c);
            advance();
            return true;
        }
        if (c == '"')
        {
            advance();
            return quoted(out, error);
        }
        if (c == '[')
        {
            const std::size_t close = text_.find(']', position_ + 1);
            if (close == std::string_view::npos)
            {
                return fail(error, "unterminated condition token");
            }
            out.assign(text_.substr(position_, close - position_ + 1));
            while (position_ <= close)
            {
                advance();
            }
            return true;
        }
        while (position_ < text_.size())
        {
            const char value = text_[position_];
            if (vdf_space(value) || value == '{' || value == '}' || value == '"')
            {
                break;
            }
            if (value == '/' && position_ + 1 < text_.size() && text_[position_ + 1] == '/')
            {
                break;
            }
            out.push_back(value);
            advance();
        }
        if (out.empty())
        {
            return fail(error, "unexpected character");
        }
        return true;
    }

private:
    void advance()
    {
        if (text_[position_] == '\n')
        {
            ++line_;
        }
        ++position_;
    }

    void skip()
    {
        for (;;)
        {
            while (position_ < text_.size() && vdf_space(text_[position_]))
            {
                advance();
            }
            if (position_ + 1 < text_.size() && text_[position_] == '/' &&
                text_[position_ + 1] == '/')
            {
                while (position_ < text_.size() && text_[position_] != '\n')
                {
                    advance();
                }
                continue;
            }
            break;
        }
    }

    bool quoted(std::string& out, std::string* error)
    {
        while (position_ < text_.size())
        {
            const char c = text_[position_];
            if (c == '"')
            {
                advance();
                return true;
            }
            if (c == '\\' && position_ + 1 < text_.size())
            {
                advance();
                const char escaped = text_[position_];
                switch (escaped)
                {
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'v':
                    out.push_back('\v');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                default:
                    out.push_back(escaped);
                    break;
                }
                advance();
                continue;
            }
            out.push_back(c);
            advance();
        }
        return fail(error, "unterminated quoted string");
    }

    bool fail(std::string* error, const char* message)
    {
        if (error)
        {
            *error = std::string(message) + " at VDF line " + std::to_string(line_);
        }
        return false;
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::size_t line_ = 1;
};

bool vdf_parse_block(VdfReader& reader, VdfNode& node, int depth, std::string* error)
{
    if (depth > 64)
    {
        if (error)
        {
            *error = "VDF nesting is too deep";
        }
        return false;
    }
    for (;;)
    {
        std::string key;
        if (!reader.next(key, error))
        {
            if (error && error->empty())
            {
                *error = "unexpected end of VDF while reading a key";
            }
            return false;
        }
        if (key == "}")
        {
            return true;
        }
        if (key.front() == '[' && key.back() == ']')
        {
            // Conditional token such as [$WIN32]; it qualifies the next key.
            continue;
        }
        if (key == "{")
        {
            if (error)
            {
                *error = "unexpected '{' in VDF";
            }
            return false;
        }
        std::string value;
        if (!reader.next(value, error))
        {
            if (error && error->empty())
            {
                *error = "unexpected end of VDF while reading a value";
            }
            return false;
        }
        if (value == "{")
        {
            VdfNode child;
            child.key = std::move(key);
            if (!vdf_parse_block(reader, child, depth + 1, error))
            {
                return false;
            }
            node.children.push_back(std::move(child));
        }
        else if (value == "}")
        {
            if (error)
            {
                *error = "missing value for VDF key '" + key + "'";
            }
            return false;
        }
        else
        {
            VdfNode child;
            child.key = std::move(key);
            child.value = std::move(value);
            node.children.push_back(std::move(child));
        }
    }
}

} // namespace

const VdfNode* VdfNode::child(std::string_view child_key) const noexcept
{
    for (const VdfNode& candidate : children)
    {
        if (ascii_iequals(candidate.key, child_key))
        {
            return &candidate;
        }
    }
    return nullptr;
}

std::vector<const VdfNode*> VdfNode::children_named(std::string_view child_key) const
{
    std::vector<const VdfNode*> result;
    for (const VdfNode& candidate : children)
    {
        if (ascii_iequals(candidate.key, child_key))
        {
            result.push_back(&candidate);
        }
    }
    return result;
}

std::string VdfNode::child_value(std::string_view child_key, std::string_view fallback) const
{
    const VdfNode* found = child(child_key);
    return found ? found->value : std::string(fallback);
}

bool parse_vdf(std::string_view text, VdfNode& root, std::string* error)
{
    if (error)
    {
        error->clear();
    }
    root = VdfNode{};
    VdfReader reader(text);
    std::string key;
    if (!reader.next(key, error))
    {
        if (error && error->empty())
        {
            *error = "empty VDF document";
        }
        return false;
    }
    if (key != "{")
    {
        std::string brace;
        if (!reader.next(brace, error))
        {
            if (error && error->empty())
            {
                *error = "VDF ended before its root block";
            }
            return false;
        }
        if (brace != "{")
        {
            if (error)
            {
                *error = "expected '{' after VDF root key '" + key + "'";
            }
            return false;
        }
        root.key = std::move(key);
    }
    if (!vdf_parse_block(reader, root, 0, error))
    {
        return false;
    }
    std::string trailing;
    if (reader.next(trailing, error))
    {
        if (error)
        {
            *error = "unexpected data after the VDF root block";
        }
        return false;
    }
    if (error && !error->empty())
    {
        return false;
    }
    return true;
}

bool load_vdf(const std::filesystem::path& path, VdfNode& root, std::string* error)
{
    FileHandle file;
    if (!file.open(path, error))
    {
        return false;
    }
    if (file.size() > static_cast<std::uint64_t>(SIZE_MAX))
    {
        if (error)
        {
            *error = "VDF is too large: " + path.string();
        }
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!read_file_range(path, 0, file.size(), bytes, error))
    {
        return false;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!parse_vdf(text, root, error))
    {
        if (error && !error->empty())
        {
            *error = path.string() + ": " + *error;
        }
        return false;
    }
    return true;
}

} // namespace sauce::fs::parsers
