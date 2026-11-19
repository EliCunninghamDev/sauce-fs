cmake_minimum_required(VERSION 3.24)

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

if(NOT CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake")
    message(FATAL_ERROR
        "sauce-fs takes its dependencies from vcpkg.\n"
        "Configure with 'cmake --preset linux' (or pass -DCMAKE_TOOLCHAIN_FILE=$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake).")
endif()

# Ported libraries: LZMA, zlib (also provides crc32), SQLite, xxHash, efsw.
find_package(LibLZMA REQUIRED)
find_package(ZLIB REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(efsw CONFIG REQUIRED)
find_package(Threads REQUIRED)

# CMake 3.31+ renamed SQLite::SQLite3 to SQLite3::SQLite3.
if(TARGET SQLite3::SQLite3)
    set(SAUCE_FS_SQLITE SQLite3::SQLite3)
else()
    set(SAUCE_FS_SQLITE SQLite::SQLite3)
endif()

add_library(sauce_fs_core STATIC
    src/core/manifest.cpp
    src/core/volume.cpp
)
target_include_directories(sauce_fs_core PUBLIC src)
target_compile_definitions(sauce_fs_core PRIVATE SAUCE_FS_VERSION="${PROJECT_VERSION}")
target_link_libraries(sauce_fs_core PUBLIC
    LibLZMA::LibLZMA
    ZLIB::ZLIB
    ${SAUCE_FS_SQLITE}
    xxHash::xxhash
    efsw::efsw
)

add_executable(sauce-fs src/main.cpp)
target_link_libraries(sauce-fs PRIVATE sauce_fs_core)

# Archive parsers and indexer: standalone libraries, not linked into sauce-fs.
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
)

add_library(sauce_fs_index STATIC
    src/index/archive_index.cpp
    src/index/thread_pool.cpp
)
target_include_directories(sauce_fs_index PUBLIC src)
target_link_libraries(sauce_fs_index PUBLIC
    sauce_fs_parsers
    ${SAUCE_FS_SQLITE}
    Threads::Threads
    ZLIB::ZLIB
    xxHash::xxhash
)

add_executable(sauce-fs-index src/index/archive_index_cli.cpp)
target_link_libraries(sauce-fs-index PRIVATE sauce_fs_index)

# Runtime access to the index: providers, merged views, and raw file bytes.
add_library(sauce_fs_archive STATIC src/index/archive_access.cpp)
target_include_directories(sauce_fs_archive PUBLIC src)
target_link_libraries(sauce_fs_archive PUBLIC
    sauce_fs_parsers
    ${SAUCE_FS_SQLITE}
    xxHash::xxhash
    Threads::Threads
)

add_executable(sauce-fs-get src/index/archive_get_cli.cpp)
target_link_libraries(sauce-fs-get PRIVATE sauce_fs_archive)

if(WIN32)
    # WinFsp is not a vcpkg port: it is installed by its own MSI.
    # Point WINFSP_ROOT at the install tree (default:
    # "C:/Program Files (x86)/WinFsp") or set WINFSP_INCLUDE_DIR / WINFSP_LIBRARY.
    set(WINFSP_ROOT "$ENV{WINFSP_ROOT}" CACHE PATH "WinFsp installation root")
    if(WINFSP_ROOT)
        list(APPEND _winfsp_hints "${WINFSP_ROOT}")
    endif()
    list(APPEND _winfsp_hints "C:/Program Files (x86)/WinFsp" "C:/Program Files/WinFsp")

    find_path(WINFSP_INCLUDE_DIR winfsp/winfsp.h HINTS ${_winfsp_hints} PATH_SUFFIXES inc)
    find_library(WINFSP_LIBRARY NAMES winfsp-x64 winfsp-${CMAKE_SYSTEM_PROCESSOR} winfsp
        HINTS ${_winfsp_hints} PATH_SUFFIXES lib bin)
    if(NOT WINFSP_INCLUDE_DIR OR NOT WINFSP_LIBRARY)
        message(FATAL_ERROR
            "WinFsp not found. Install WinFsp and set WINFSP_ROOT (header: ${WINFSP_INCLUDE_DIR}, lib: ${WINFSP_LIBRARY}).")
    endif()

    add_library(winfsp INTERFACE)
    target_include_directories(winfsp INTERFACE "${WINFSP_INCLUDE_DIR}")
    target_link_libraries(winfsp INTERFACE "${WINFSP_LIBRARY}")
    add_library(WinFsp::WinFsp ALIAS winfsp)

    target_sources(sauce-fs PRIVATE src/backends/winfsp.cpp)
    target_link_libraries(sauce-fs PRIVATE WinFsp::WinFsp)
else()
    # FUSE is part of the host system (it must match the running kernel),
    # so Linux takes FUSE3 from pkg-config instead of vcpkg.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3>=3.10)

    target_sources(sauce-fs PRIVATE src/backends/fuse3.cpp)
    target_link_libraries(sauce-fs PRIVATE PkgConfig::FUSE3)

    # Read-only index mount. Kept separate from sauce-fs and its VFS.
    add_executable(sauce-fs-mount
        src/mount/archive_mount.cpp
        src/mount/archive_mount_cli.cpp
    )
    target_include_directories(sauce-fs-mount PRIVATE src)
    target_link_libraries(sauce-fs-mount PRIVATE
        sauce_fs_archive
        PkgConfig::FUSE3
        Threads::Threads
    )
endif()

enable_testing()
add_test(NAME self-check COMMAND sauce-fs --self-check)
