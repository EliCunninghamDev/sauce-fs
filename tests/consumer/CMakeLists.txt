cmake_minimum_required(VERSION 3.24)

project(sauce-fs-consumer LANGUAGES CXX)

find_package(sauce-fs REQUIRED)

add_executable(sauce-fs-consumer main.cpp)
target_link_libraries(sauce-fs-consumer PRIVATE sauce::core)

# Running the check as part of the build keeps the script free of executable
# path and multi-config generator assumptions.
add_custom_target(
    check-consumer ALL
    COMMAND $<TARGET_FILE:sauce-fs-consumer>
    DEPENDS sauce-fs-consumer
)
