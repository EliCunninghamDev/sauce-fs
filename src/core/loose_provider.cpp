// SPDX-License-Identifier: MIT

#include <sauce/core/archive_access.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sauce::fs::core
{

namespace
{

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

bool append_component(std::string& key, std::string_view component)
{
    if (component.empty() || component == ".")
    {
        return true;
    }
    if (component == "..")
    {
        return false;
    }
    if (!key.empty())
    {
        key.push_back('/');
    }
    key += lower_ascii(component);
    return true;
}

Result<std::string> normalized_key(std::string_view path, std::string_view prefix)
{
    std::string key(prefix);
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!append_component(key, part))
        {
            return std::unexpected(Error{ErrorCode::InvalidPath, "path traversal is not allowed"});
        }
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return key;
}

} // namespace

LooseFileProvider::LooseFileProvider(std::shared_ptr<const IndexDatabase> database,
                                     std::string root, std::string prefix)
    : database_(std::move(database)), root_(std::move(root))
{
    std::string key;
    std::size_t start = 0;
    while (start <= prefix.size())
    {
        const std::size_t slash = prefix.find('/', start);
        const std::string_view part =
            prefix.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == ".")
        {
            if (slash == std::string::npos)
            {
                break;
            }
            start = slash + 1;
            continue;
        }
        if (!append_component(key, part))
        {
            break;
        }
        if (slash == std::string::npos)
        {
            break;
        }
        start = slash + 1;
    }
    prefix_key_ = std::move(key);
}

Result<NodeInfo> LooseFileProvider::stat(std::string_view path) const
{
    auto key = normalized_key(path, prefix_key_);
    if (!key)
    {
        return std::unexpected(key.error());
    }
    return database_->loose_stat(root_, *key);
}

Result<std::vector<DirectoryEntry>> LooseFileProvider::list(std::string_view path) const
{
    auto key = normalized_key(path, prefix_key_);
    if (!key)
    {
        return std::unexpected(key.error());
    }
    return database_->loose_list(root_, *key);
}

Result<Blob> LooseFileProvider::get(std::string_view path) const
{
    auto key = normalized_key(path, prefix_key_);
    if (!key)
    {
        return std::unexpected(key.error());
    }
    return database_->loose_blob(root_, *key);
}

} // namespace sauce::fs::core
