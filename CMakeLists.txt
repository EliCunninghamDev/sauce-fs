cmake_minimum_required(VERSION 3.24)

# Every dependency comes from the vcpkg manifest. Check the toolchain before
# project() so a missing VCPKG_ROOT reports that, instead of CMake's generic
# "Could not find toolchain file" error from inside the language check.
if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "sauce-fs takes its dependencies from vcpkg.\n"
        "Set VCPKG_ROOT to a vcpkg checkout, or pass "
        "-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake.")
endif()
if(IS_ABSOLUTE "${CMAKE_TOOLCHAIN_FILE}" AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR
        "CMAKE_TOOLCHAIN_FILE points at a missing vcpkg checkout:\n"
        "  ${CMAKE_TOOLCHAIN_FILE}\n"
        "Clone https://github.com/microsoft/vcpkg and point the VCPKG_ROOT "
        "environment variable at it (the presets read that variable).")
endif()

project(sauce-fs VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Ported libraries: LZMA, zlib (also provides crc32), SQLite, xxHash, efsw.
find_package(LibLZMA REQUIRED)
find_package(ZLIB REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(efsw CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)

# CMake 3.31+ renamed SQLite::SQLite3 to SQLite3::SQLite3.
if(TARGET SQLite3::SQLite3)
    set(SAUCE_FS_SQLITE SQLite3::SQLite3)
else()
    set(SAUCE_FS_SQLITE SQLite::SQLite3)
endif()

# Archive format readers: VPK, BSP pakfiles, GMAD, and Source install metadata.
# Implementation detail of the indexing core.
add_library(sauce_fs_parsers STATIC
    src/parsers/bsp.cpp
    src/parsers/bytes.cpp
    src/parsers/compression.cpp
    src/parsers/gameinfo.cpp
    src/parsers/gma.cpp
    src/parsers/steam.cpp
    src/parsers/vdf.cpp
    src/parsers/vpk.cpp
)
target_include_directories(sauce_fs_parsers PUBLIC src)
target_link_libraries(sauce_fs_parsers PUBLIC
    LibLZMA::LibLZMA
    ZLIB::ZLIB
    fmt::fmt
)

# Indexing core: reusable discovery, scanning, SQLite storage, watch mode, and
# the runtime provider API. Consumers do not need the FUSE layer.
add_library(sauce_fs_core STATIC
    src/core/archive_access.cpp
    src/core/archive_discovery.cpp
    src/core/archive_index.cpp
    src/core/archive_scan.cpp
    src/core/archive_store.cpp
    src/core/archive_watch.cpp
    src/core/sqlite_util.cpp
    src/core/thread_pool.cpp
)
target_include_directories(sauce_fs_core PUBLIC src)
add_library(sauce::core ALIAS sauce_fs_core)
target_link_libraries(sauce_fs_core
    PUBLIC
        ${SAUCE_FS_SQLITE}
        Threads::Threads
        fmt::fmt
    PRIVATE
        sauce_fs_parsers
        ZLIB::ZLIB
        xxHash::xxhash
        efsw::efsw
)

add_executable(sauce-fs-index src/core/archive_index_cli.cpp)
target_link_libraries(sauce-fs-index PRIVATE sauce_fs_core)

add_executable(sauce-fs-get src/core/archive_get_cli.cpp)
target_link_libraries(sauce-fs-get PRIVATE sauce_fs_core)

if(NOT WIN32)
    # FUSE is part of the host system (it must match the running kernel),
    # so Linux takes FUSE3 from pkg-config instead of vcpkg.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3>=3.10)

    # Filesystem layer: read-only FUSE mount over the index providers.
    add_library(sauce_fs_fuse STATIC src/fuse/archive_mount.cpp)
    add_library(sauce::fuse ALIAS sauce_fs_fuse)
    target_include_directories(sauce_fs_fuse PUBLIC src)
    target_link_libraries(sauce_fs_fuse
        PRIVATE
            sauce_fs_core
            PkgConfig::FUSE3
            Threads::Threads
            xxHash::xxhash
    )

    add_executable(sauce-fs-mount src/fuse/archive_mount_cli.cpp)
    target_link_libraries(sauce-fs-mount PRIVATE sauce_fs_fuse fmt::fmt)
endif()

enable_testing()

# Installs a fake game tree and exercises a full build, a replace, an add, and
# a delete through ArchiveIndexWatcher.
add_executable(sauce-fs-index-watch-test tests/archive_index_watch_test.cpp)
target_link_libraries(sauce-fs-index-watch-test PRIVATE sauce_fs_core)
add_test(NAME index-watch COMMAND sauce-fs-index-watch-test)
set_tests_properties(index-watch PROPERTIES TIMEOUT 60)

# Builds GMAD fixtures and checks the archive_type lookup plus the addon
# metadata read back from the database.
add_executable(sauce-fs-gma-metadata-test tests/gma_metadata_test.cpp)
target_link_libraries(sauce-fs-gma-metadata-test PRIVATE sauce_fs_core)
add_test(NAME gma-metadata COMMAND sauce-fs-gma-metadata-test)
set_tests_properties(gma-metadata PROPERTIES TIMEOUT 60)
