#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sauce::fs
{

// A file served at the root of the mounted volume.
struct Entry
{
    std::string name;
    std::string data;
};

// Current contents of the volume root. Backends call this per request, so the
// volume always reflects the running binary.
std::vector<Entry> snapshot();

const Entry* find_entry(const std::vector<Entry>& entries, std::string_view name);

} // namespace sauce::fs
