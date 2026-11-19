// Ported from seeass-index's seeass_index.h.
// SPDX-License-Identifier: MIT

#include "gameinfo.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "bytes.hpp"

namespace sauce::fs::parsers
{

static std::vector<std::string> tokenize_gameinfo_line(std::string_view line)
{
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() &&
               (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n'))
        {
            ++i;
        }
        if (i >= line.size())
        {
            break;
        }
        if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/')
        {
            break;
        }
        std::string token;
        if (line[i] == '"')
        {
            ++i;
            while (i < line.size() && line[i] != '"')
            {
                if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '"')
                {
                    ++i;
                }
                token.push_back(line[i++]);
            }
            if (i < line.size())
            {
                ++i;
            }
        }
        else if (line[i] == '{' || line[i] == '}')
        {
            token.push_back(line[i++]);
        }
        else
        {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' &&
                   line[i] != '\n' && line[i] != '{' && line[i] != '}')
            {
                token.push_back(line[i++]);
            }
        }
        if (!token.empty())
        {
            tokens.push_back(std::move(token));
        }
    }
    return tokens;
}

std::vector<GameInfoEntry> parse_gameinfo_search_paths(const std::filesystem::path& file,
                                                       std::vector<std::string>& warnings)
{
    std::vector<GameInfoEntry> result;
    std::ifstream input(file);
    if (!input)
    {
        warnings.push_back("cannot read " + file.string());
        return result;
    }
    std::string line;
    bool found_keyword = false;
    bool in_block = false;
    int depth = 0;
    while (std::getline(input, line))
    {
        std::vector<std::string> tokens = tokenize_gameinfo_line(line);
        if (!found_keyword)
        {
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (ascii_iequals(tokens[i], "SearchPaths"))
                {
                    found_keyword = true;
                    for (std::size_t j = i + 1; j < tokens.size(); ++j)
                    {
                        if (tokens[j] == "{")
                        {
                            in_block = true;
                            ++depth;
                        }
                        else if (tokens[j] == "}")
                        {
                            --depth;
                        }
                    }
                    break;
                }
            }
            continue;
        }
        if (!in_block)
        {
            for (const std::string& token : tokens)
            {
                if (token == "{")
                {
                    in_block = true;
                    depth = 1;
                    break;
                }
            }
            continue;
        }
        // Strip braces, count nesting, and keep the remaining tokens for one
        // line-oriented search-path entry.
        std::vector<std::string> words;
        bool closed = false;
        for (const std::string& token : tokens)
        {
            if (token == "{")
            {
                ++depth;
            }
            else if (token == "}")
            {
                --depth;
                if (depth <= 0)
                {
                    closed = true;
                    break;
                }
            }
            else
            {
                words.push_back(token);
            }
        }
        if (words.size() >= 2)
        {
            GameInfoEntry entry;
            entry.types = words.front();
            entry.path = words.back();
            if (!entry.path.empty() && entry.path != "{" && entry.path != "}")
            {
                result.push_back(std::move(entry));
            }
        }
        if (closed)
        {
            break;
        }
    }
    if (!found_keyword)
    {
        warnings.push_back("no SearchPaths block in " + file.string());
    }
    return result;
}

bool glob_match(std::string_view pattern, std::string_view text)
{
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t retry = 0;
    while (t < text.size())
    {
        if (p < pattern.size() &&
            (pattern[p] == '?' || ascii_lower(pattern[p]) == ascii_lower(text[t])))
        {
            ++p;
            ++t;
        }
        else if (p < pattern.size() && pattern[p] == '*')
        {
            star = p++;
            retry = t;
        }
        else if (star != std::string_view::npos)
        {
            p = star + 1;
            t = ++retry;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
    {
        ++p;
    }
    return p == pattern.size();
}

std::vector<std::filesystem::path> expand_wildcard(const std::filesystem::path& pattern)
{
    std::vector<std::filesystem::path> result;
    const std::string text = pattern.string();
    if (text.find('*') == std::string::npos && text.find('?') == std::string::npos)
    {
        result.push_back(pattern);
        return result;
    }
    const std::filesystem::path parent = pattern.parent_path();
    const std::string name_pattern = pattern.filename().string();
    std::error_code ec;
    std::vector<std::filesystem::path> matches;
    for (std::filesystem::directory_iterator it(
             parent.empty() ? "." : parent,
             std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (glob_match(name_pattern, name))
        {
            matches.push_back(it->path());
        }
    }
    std::sort(matches.begin(), matches.end(),
              [](const auto& a, const auto& b)
              {
                  std::string left = a.filename().string();
                  std::string right = b.filename().string();
                  std::transform(left.begin(), left.end(), left.begin(),
                                 [](unsigned char c) { return ascii_lower(static_cast<char>(c)); });
                  std::transform(right.begin(), right.end(), right.begin(),
                                 [](unsigned char c) { return ascii_lower(static_cast<char>(c)); });
                  return left < right;
              });
    for (const auto& match : matches)
    {
        result.push_back(match);
    }
    return result;
}

} // namespace sauce::fs::parsers
