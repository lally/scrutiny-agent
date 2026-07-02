// Process.hpp
// Cross-platform process management for spawning LSP servers

#ifndef PROCESS_HPP
#define PROCESS_HPP

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace gitreview {
namespace lsp {

class Process {
public:
    using OutputCallback = std::function<void(const std::string&)>;

    Process(std::string executable,
            std::vector<std::string> arguments,
            std::string workingDirectory);
    ~Process();

    // Non-copyable, non-movable (due to thread management)
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) = delete;
    Process& operator=(Process&&) = delete;

    // Lifecycle
    bool start();
    void stop();
    bool isRunning() const;

    // I/O
    void setOutputCallback(OutputCallback callback);
    bool write(const std::string& data);

    // Process info
    int pid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string executable_;
    std::vector<std::string> arguments_;
    std::string workingDirectory_;
};

} // namespace lsp
} // namespace gitreview

#endif // PROCESS_HPP
