cmake_minimum_required(VERSION 3.25)
project(scrutiny-agent VERSION 0.2.0 LANGUAGES CXX)

# gitmanip requires C++23; the whole tree builds with one standard so
# every target and every Conan dependency share a single ABI.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SCRUTINY_BUILD_TESTS "Build unit tests (gitmanip + core)" ON)
option(SCRUTINY_BUILD_TOOLS "Build command-line tools (grc-index)" ON)
option(SCRUTINY_BUILD_EXAMPLES "Build gitmanip example programs" OFF)

# The version the agent reports in meta.hello and that names release
# artifacts (scrutiny-agent-<version>-<arch>). Release builds pass the
# git tag; dev builds default to the project version.
set(SCRUTINY_AGENT_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Version reported by meta.hello and used in artifact names")

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# fmt 10.2.1's `basic_format_string` constructor is `consteval`. Apple
# Clang 21 (Xcode 26+) regressed on evaluating fmt's own internal
# format-string calls as constant expressions, so translation units
# that instantiate fmt/spdlog format templates fail with "call to
# consteval function ... is not a constant expression". Pre-defining
# FMT_CONSTEVAL to empty downgrades those checks to runtime -- exactly
# fmt's behaviour on pre-consteval compilers -- with no ABI or runtime
# impact. Apple-only: Linux clang/gcc build fmt's checks fine at full
# compile-time strength. Remove once fmt is bumped to a version that
# builds cleanly under current Xcode.
if(APPLE)
    add_compile_definitions(FMT_CONSTEVAL=)
endif()

find_package(nlohmann_json REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem system)
find_package(SQLite3 REQUIRED)
find_package(libgit2 REQUIRED)

if(SCRUTINY_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
endif()

add_subdirectory(gitmanip)
add_subdirectory(core)
add_subdirectory(agent)

message(STATUS "")
message(STATUS "scrutiny-agent configuration:")
message(STATUS "  Agent version: ${SCRUTINY_AGENT_VERSION}")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "  Tests:         ${SCRUTINY_BUILD_TESTS}")
message(STATUS "  Tools:         ${SCRUTINY_BUILD_TOOLS}")
message(STATUS "")
