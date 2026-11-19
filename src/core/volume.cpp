#include "core/volume.h"

#include <algorithm>

#include "core/manifest.h"

namespace sauce::fs
{

std::vector<Entry> snapshot()
{
    return {{"manifest.txt", manifest()}};
}

const Entry* find_entry(const std::vector<Entry>& entries, std::string_view name)
{
    const auto match = std::find_if(entries.begin(), entries.end(),
                                    [name](const Entry& entry) { return entry.name == name; });
    return match == entries.end() ? nullptr : &*match;
}

} // namespace sauce::fs
