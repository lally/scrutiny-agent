# scrutiny-agent: the remote backend process (JSON-RPC 2.0 over
# LSP-style Content-Length framing on stdio). The shipped deliverable.

add_executable(scrutiny-agent main.cpp)

target_link_libraries(scrutiny-agent
    PRIVATE
        GitReviewCore
        nlohmann_json::nlohmann_json
)

target_compile_definitions(scrutiny-agent PRIVATE
    SCRUTINY_AGENT_VERSION="${SCRUTINY_AGENT_VERSION}"
)

install(TARGETS scrutiny-agent RUNTIME DESTINATION bin)
