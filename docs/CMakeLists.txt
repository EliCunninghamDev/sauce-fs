cmake_minimum_required(VERSION 3.24)

# Builds the Sphinx site together with the Doxygen XML that Breathe consumes.
#
# This directory is also a standalone CMake project so CI can generate the
# documentation from a plain checkout without configuring the vcpkg toolchain
# for the C++ targets:
#
#   cmake -S docs -B build/docs
#   cmake --build build/docs --target docs
#
# The root project can include it with -DSAUCE_FS_BUILD_DOCS=ON, which makes
# `cmake --build build/linux --target docs` work from the normal build tree.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(sauce-fs-docs LANGUAGES NONE)

    set(SAUCE_FS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.."
        CACHE PATH "sauce-fs checkout to document")
    set(SAUCE_FS_DOCS_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
        CACHE PATH "Root directory for the generated documentation")

    # Keep the site version in step with the project version without forcing a
    # full configure (and therefore a vcpkg bootstrap) just to build docs.
    file(READ "${SAUCE_FS_SOURCE_DIR}/CMakeLists.txt" sauce_fs_root_cmake)
    if(sauce_fs_root_cmake MATCHES "project\\(sauce-fs VERSION ([0-9.]+)")
        set(SAUCE_FS_VERSION "${CMAKE_MATCH_1}")
    else()
        set(SAUCE_FS_VERSION "0.0.0")
    endif()
else()
    # Included by the root project: inherit its source tree, version, and
    # binary directory.
    set(SAUCE_FS_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
    set(SAUCE_FS_VERSION "${PROJECT_VERSION}")
    set(SAUCE_FS_DOCS_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()

find_package(Doxygen REQUIRED)

# An explicit -DSPHINX_EXECUTABLE path lets a caller use a virtualenv that is
# not activated; otherwise sphinx-build is taken from PATH. The variable is a
# normal cache entry so reconfiguring keeps the discovered path.
if(NOT SPHINX_EXECUTABLE)
    find_program(SPHINX_EXECUTABLE NAMES sphinx-build)
endif()
if(NOT SPHINX_EXECUTABLE)
    message(FATAL_ERROR
        "sphinx-build was not found.\n"
        "Install the Python requirements and point CMake at the environment:\n"
        "  python3 -m venv build/docs-venv\n"
        "  build/docs-venv/bin/pip install -r docs/requirements.txt\n"
        "  cmake -S docs -B build/docs "
        "-DSPHINX_EXECUTABLE=\"$PWD/build/docs-venv/bin/sphinx-build\"")
endif()

set(SAUCE_FS_DOXYGEN_OUTPUT "${SAUCE_FS_DOCS_OUTPUT_DIR}/doxygen")
set(SAUCE_FS_DOXYGEN_XML "${SAUCE_FS_DOXYGEN_OUTPUT}/xml")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
    "${SAUCE_FS_DOCS_OUTPUT_DIR}/Doxyfile"
    @ONLY
)

add_custom_target(docs-doxygen
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAUCE_FS_DOXYGEN_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAUCE_FS_DOXYGEN_OUTPUT}"
    COMMAND "${DOXYGEN_EXECUTABLE}" "${SAUCE_FS_DOCS_OUTPUT_DIR}/Doxyfile"
    COMMENT "Generating Doxygen XML in ${SAUCE_FS_DOXYGEN_XML}"
    VERBATIM
)

# -W turns Sphinx/Breathe warnings into build failures, so a directive that no
# longer matches the headers breaks the docs build instead of silently dropping
# an API section.
add_custom_target(docs
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "SAUCE_FS_DOXYGEN_XML=${SAUCE_FS_DOXYGEN_XML}"
        "SAUCE_FS_VERSION=${SAUCE_FS_VERSION}"
        "${SPHINX_EXECUTABLE}" -b html
        -d "${SAUCE_FS_DOCS_OUTPUT_DIR}/doctrees"
        -W --keep-going -q
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${SAUCE_FS_DOCS_OUTPUT_DIR}/html"
    DEPENDS docs-doxygen
    COMMENT "Building Sphinx HTML in ${SAUCE_FS_DOCS_OUTPUT_DIR}/html"
    VERBATIM
)
