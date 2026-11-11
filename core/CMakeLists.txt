# GitReviewCore -- the shared C++ core (git bridge, LSP client,
# indexer, caches, filesystem, HEAD watcher) with a C API for Swift
# interop. The agent links it whole; the Scrutiny Mac app links the
# same library in-process for its local backend.

add_library(GitReviewCore STATIC
    src/GitReviewCore.cpp
    src/lsp/LSPClient.cpp
    src/lsp/LSPWorkspace.cpp
    src/lsp/DeclarationSearch.cpp
    src/lsp/Process.cpp
    src/db/Database.cpp
    src/db/LSPCache.cpp
    src/indexer/Indexer.cpp
    src/fs/FileSystem.cpp
    src/fs/HeadWatcher.cpp
    src/fs/TreeWatcher.cpp
    src/GitManipBridge.cpp
)

target_include_directories(GitReviewCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(GitReviewCore
    PUBLIC
        gitmanip
    PRIVATE
        nlohmann_json::nlohmann_json
        fmt::fmt
        spdlog::spdlog
        Boost::filesystem
        Boost::system
        SQLite::SQLite3
        libgit2::libgit2
)

if(NOT APPLE AND UNIX)
    target_link_libraries(GitReviewCore PRIVATE pthread)
endif()

# The working-tree watch is FSEvents on Darwin. PUBLIC so the agent,
# the tests and the Mac app's link line all pick the framework up.
if(APPLE)
    target_link_libraries(GitReviewCore PUBLIC "-framework CoreServices")
endif()

set_target_properties(GitReviewCore PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    POSITION_INDEPENDENT_CODE ON
)

if(SCRUTINY_BUILD_TOOLS)
    add_executable(grc-index tools/grc-index.cpp)
    target_link_libraries(grc-index PRIVATE GitReviewCore)
    install(TARGETS grc-index RUNTIME DESTINATION bin)
endif()

if(SCRUTINY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
