// SPDX-License-Identifier: MIT

// Exercises the installed public headers through sauce::core only. Built by
// the install-consumer CTest from a scratch prefix, without access to src/.

#include <sauce/core/core.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

int main()
{
    using namespace sauce::fs::core;

    LogOptions log;
    log.level = LogLevel::Debug;
    log.callback = [](LogLevel level, std::string_view message)
    {
        std::fprintf(stderr, "[%d] %.*s\n", static_cast<int>(level),
                     static_cast<int>(message.size()), message.data());
    };

    std::vector<TreeProvider::Child> children;
    children.push_back(TreeProvider::Child{
        "empty", std::make_shared<TreeProvider>(std::vector<TreeProvider::Child>{})});
    const auto root = std::make_shared<TreeProvider>(std::move(children));

    const auto root_info = root->stat("/");
    if (!root_info || root_info->kind != NodeInfo::Kind::Directory)
    {
        return 1;
    }

    const auto listing = root->list("/");
    if (!listing || listing->size() != 1 || listing->front().name != "empty")
    {
        return 2;
    }

    const auto missing = root->get("/missing");
    if (missing || missing.error().code != ErrorCode::NotFound)
    {
        return 3;
    }

    std::puts("sauce::core consumer ok");
    return 0;
}
