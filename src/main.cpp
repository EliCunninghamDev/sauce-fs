#include <string>
#include <vector>

#include "backends/backend.h"
#include "core/manifest.h"
#include "util/format.hpp"

namespace
{

void usage(const char* program)
{
    sauce::fs::print_error("usage: {} <mountpoint> [options]\n"
                           "       {} --self-check\n",
                           program, program);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        usage(argv[0]);
        return 2;
    }

    if (std::string(argv[1]) == "--self-check")
    {
        std::vector<sauce::fs::Check> checks = sauce::fs::self_check();
        checks.push_back(sauce::fs::backend_check());

        int failures = 0;
        for (const auto& check : checks)
        {
            if (!check.ok)
            {
                ++failures;
            }
            sauce::fs::print("{:<4} {:<8} {}\n", check.ok ? "ok" : "FAIL", check.name,
                             check.detail);
        }
        sauce::fs::print("{} checks, {} failed\n", checks.size(), failures);
        return failures == 0 ? 0 : 1;
    }

    return sauce::fs::mount(argc - 1, argv + 1);
}
