add_executable(gitmanip_tests
    test_main.cpp
    test_repository.cpp
    test_commit.cpp
    test_rebase.cpp
    test_diff.cpp
    test_blame.cpp
    test_history.cpp
    test_refs.cpp
    test_merge.cpp
    test_signing.cpp
    test_status.cpp
    test_tree.cpp
)

target_link_libraries(gitmanip_tests
    PRIVATE
        gitmanip
        GTest::gtest
        GTest::gtest_main
)

# Include the test fixture header
target_include_directories(gitmanip_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

include(GoogleTest)
gtest_discover_tests(gitmanip_tests)
