# gitmanip -- modern C++23 wrapper over libgit2.
# Built as part of the scrutiny-agent tree; the parent CMakeLists
# provides the toolchain, C++ standard, and find_package results.

add_library(gitmanip
    src/repository.cpp
    src/commit.cpp
    src/diff.cpp
    src/tree_builder.cpp
    src/rebase_plan.cpp
    src/operations.cpp
    src/error.cpp
    src/blame.cpp
    src/history.cpp
    src/refs.cpp
    src/signing.cpp
    src/status.cpp
)

target_include_directories(gitmanip
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(gitmanip
    PUBLIC
        libgit2::libgit2
        fmt::fmt
    PRIVATE
        spdlog::spdlog
)

target_compile_features(gitmanip PUBLIC cxx_std_23)
set_target_properties(gitmanip PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(gitmanip PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wno-unused-parameter
        -Wno-missing-field-initializers  # libgit2 macros trigger this
    )
endif()

if(SCRUTINY_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(SCRUTINY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
