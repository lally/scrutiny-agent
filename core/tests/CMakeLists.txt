add_executable(indexer_tests indexer_tests.cpp)

target_link_libraries(indexer_tests
    PRIVATE
        GitReviewCore
        nlohmann_json::nlohmann_json
        fmt::fmt
        spdlog::spdlog
        Boost::filesystem
        Boost::system
        SQLite::SQLite3
)

target_include_directories(indexer_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

add_test(NAME IndexerTests COMMAND indexer_tests)

# indexer_tests resolves fixtures as tests/fixtures relative to CWD.
set_tests_properties(IndexerTests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_executable(lsp_workspace_tests lsp_workspace_tests.cpp)
target_link_libraries(lsp_workspace_tests
    PRIVATE
        GitReviewCore
        nlohmann_json::nlohmann_json
        fmt::fmt
        spdlog::spdlog
)
target_include_directories(lsp_workspace_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME LSPWorkspaceTests COMMAND lsp_workspace_tests)
