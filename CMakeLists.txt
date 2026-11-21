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

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

# Diagnostics are callback-based; this switch removes the call sites and their
# formatting entirely. It is exported so consumers compile the inline
# LogOptions::enabled() the same way as the library.
option(SAUCE_FS_LOGGING "Compile diagnostics into the libraries" ON)

# Doxygen and the Sphinx requirements are not part of the C++ build, so the
# docs target is opt-in. docs/ is also a standalone CMake project for CI.
option(SAUCE_FS_BUILD_DOCS "Add the Doxygen/Sphinx documentation target" OFF)

# Indexing core: reusable discovery, scanning, SQLite storage, watch mode, and
# the runtime provider API. The archive format readers are an implementation
# detail, so their sources are compiled into the same target; that keeps the
# installed package to one library with no private headers on its interface.
add_library(sauce_fs_core STATIC
    src/core/archive_access.cpp
    src/core/archive_discovery.cpp
    src/core/archive_index.cpp
    src/core/archive_scan.cpp
    src/core/archive_store.cpp
    src/core/archive_watch.cpp
    src/core/sqlite_util.cpp
    src/core/thread_pool.cpp
    src/parsers/bsp.cpp
    src/parsers/bytes.cpp
    src/parsers/compression.cpp
    src/parsers/gameinfo.cpp
    src/parsers/gma.cpp
    src/parsers/steam.cpp
    src/parsers/vdf.cpp
    src/parsers/vpk.cpp
)
add_library(sauce::core ALIAS sauce_fs_core)
set_target_properties(sauce_fs_core PROPERTIES EXPORT_NAME core)
target_compile_features(sauce_fs_core PUBLIC cxx_std_23)
target_include_directories(sauce_fs_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(sauce_fs_core
    PRIVATE
        ${SAUCE_FS_SQLITE}
        Threads::Threads
        fmt::fmt
        LibLZMA::LibLZMA
        ZLIB::ZLIB
        xxHash::xxhash
        efsw::efsw
)
if(SAUCE_FS_LOGGING)
    target_compile_definitions(sauce_fs_core PUBLIC SAUCE_FS_LOGGING=1)
else()
    target_compile_definitions(sauce_fs_core PUBLIC SAUCE_FS_LOGGING=0)
endif()

add_executable(sauce-fs-index src/core/archive_index_cli.cpp)
target_include_directories(sauce-fs-index PRIVATE src)
target_link_libraries(sauce-fs-index PRIVATE sauce_fs_core fmt::fmt)

add_executable(sauce-fs-get src/core/archive_get_cli.cpp)
target_include_directories(sauce-fs-get PRIVATE src)
target_link_libraries(sauce-fs-get PRIVATE sauce_fs_core fmt::fmt)

if(NOT WIN32)
    # FUSE is part of the host system (it must match the running kernel),
    # so Linux takes FUSE3 from pkg-config instead of vcpkg.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3>=3.10)

    # Filesystem layer: read-only FUSE mount over the index providers.
    add_library(sauce_fs_fuse STATIC src/fuse/archive_mount.cpp)
    add_library(sauce::fuse ALIAS sauce_fs_fuse)
    target_include_directories(sauce_fs_fuse PRIVATE src)
    target_link_libraries(sauce_fs_fuse
        PRIVATE
            sauce_fs_core
            PkgConfig::FUSE3
            Threads::Threads
            xxHash::xxhash
    )

    add_executable(sauce-fs-mount src/fuse/archive_mount_cli.cpp)
    target_include_directories(sauce-fs-mount PRIVATE src)
    target_link_libraries(sauce-fs-mount PRIVATE sauce_fs_fuse sauce_fs_core fmt::fmt)
endif()

enable_testing()

# Installs a fake game tree and exercises a full build, a replace, an add, and
# a delete through ArchiveIndexWatcher.
add_executable(sauce-fs-index-watch-test tests/archive_index_watch_test.cpp)
target_include_directories(sauce-fs-index-watch-test PRIVATE src)
target_link_libraries(sauce-fs-index-watch-test PRIVATE sauce_fs_core fmt::fmt)
add_test(NAME index-watch COMMAND sauce-fs-index-watch-test)
set_tests_properties(index-watch PROPERTIES TIMEOUT 60)

# Builds GMAD fixtures and checks the archive_type lookup plus the addon
# metadata read back from the database.
add_executable(sauce-fs-gma-metadata-test tests/gma_metadata_test.cpp)
target_include_directories(sauce-fs-gma-metadata-test PRIVATE src)
target_link_libraries(sauce-fs-gma-metadata-test PRIVATE sauce_fs_core fmt::fmt)
add_test(NAME gma-metadata COMMAND sauce-fs-gma-metadata-test)
set_tests_properties(gma-metadata PROPERTIES TIMEOUT 60)

# Installs into a scratch prefix, then configures and builds a standalone
# project that consumes sauce::core through find_package(sauce-fs).
add_test(
    NAME install-consumer
    COMMAND
        ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer-test
        -DPROJECT_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DDEPENDENCY_DIR=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}
        -DGENERATOR=${CMAKE_GENERATOR}
        -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/consumer_test.cmake
)
set_tests_properties(install-consumer PROPERTIES TIMEOUT 180)

install(
    TARGETS sauce_fs_core
    EXPORT sauce-fs-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    EXPORT sauce-fs-targets
    FILE sauce-fs-targets.cmake
    NAMESPACE sauce::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sauce-fs
)
configure_package_config_file(
    cmake/sauce-fs-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/sauce-fs-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sauce-fs
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/sauce-fs-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/sauce-fs-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/sauce-fs-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sauce-fs
)

if(SAUCE_FS_BUILD_DOCS)
    add_subdirectory(docs)
endif()
