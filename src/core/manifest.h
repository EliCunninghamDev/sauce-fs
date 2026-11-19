#pragma once

#include <string>
#include <vector>

namespace sauce::fs
{

// Result of probing one linked component.
struct Check
{
    std::string name;
    bool ok;
    std::string detail;
};

// Version report for every linked port, with integrity digests of the report
// itself (zlib's crc32 and XXH3).
std::string manifest();

// Round-trips data through each linked library. No mount required.
std::vector<Check> self_check();

} // namespace sauce::fs
