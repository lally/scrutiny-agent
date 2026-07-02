// Process.cpp
// Process management using Boost.Process

#include "lsp/Process.hpp"

#include <spdlog/spdlog.h>
#include <boost/process.hpp>
#include <boost/asio.hpp>

#include <iostream>
#include <thread>
#include <filesystem>

namespace bp = boost::process;
namespace asio = boost::asio;

namespace gitreview {
namespace lsp {

struct Process::Impl {
    bp::child child;
    bp::opstream stdinStream;
    bp::ipstream stdoutStream;
    bp::ipstream stderrStream;
    std::unique_ptr<std::thread> readThread;
    std::atomic<bool> running{false};
    OutputCallback outputCallback;

    void readLoop() {
        spdlog::debug("[Process] Read loop started");
        char buffer[4096];

        while (running && child.running()) {
            // Check if data is available on stdout
            auto avail = stdoutStream.rdbuf()->in_avail();
            if (avail > 0) {
                // Read available data
                auto bytesRead = stdoutStream.readsome(buffer, sizeof(buffer));
                if (bytesRead > 0) {
                    spdlog::debug("[Process] Read {} bytes from stdout", bytesRead);
                    if (outputCallback) {
                        outputCallback(std::string(buffer, bytesRead));
                    }
                }
            } else {
                // No data in buffer, try a non-blocking peek to check for more
                // Use a short timeout approach - check if stream is good and try to get one char
                stdoutStream.clear(); // Clear any error flags

                // Try to read - this may block briefly but will return when data arrives
                if (stdoutStream.good() && !stdoutStream.eof()) {
                    int ch = stdoutStream.peek();
                    if (ch != EOF) {
                        // Data is available, read it
                        auto bytesRead = stdoutStream.readsome(buffer, sizeof(buffer));
                        if (bytesRead > 0) {
                            spdlog::debug("[Process] Read {} bytes from stdout (after peek)", bytesRead);
                            if (outputCallback) {
                                outputCallback(std::string(buffer, bytesRead));
                            }
                        }
                    }
                }
            }

            // Read from stderr for debugging (non-blocking check)
            if (stderrStream.rdbuf()->in_avail() > 0) {
                std::string errLine;
                if (std::getline(stderrStream, errLine)) {
                    spdlog::warn("[Process] STDERR: {}", errLine);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        spdlog::debug("[Process] Read loop exiting");
    }
};

Process::Process(
    std::string executable,
    std::vector<std::string> arguments,
    std::string workingDirectory
)
    : impl_(std::make_unique<Impl>())
    , executable_(std::move(executable))
    , arguments_(std::move(arguments))
    , workingDirectory_(std::move(workingDirectory)) {
}

Process::~Process() {
    stop();
}

bool Process::start() {
    spdlog::info("[Process] Starting: {}", executable_);
    spdlog::debug("[Process] Working directory: {}", workingDirectory_);
    for (size_t i = 0; i < arguments_.size(); ++i) {
        spdlog::debug("[Process]   arg[{}]: {}", i, arguments_[i]);
    }

    // Check if executable exists
    if (!std::filesystem::exists(executable_)) {
        spdlog::error("[Process] Executable does not exist: {}", executable_);
        return false;
    }

    try {
        // Build command line
        std::vector<std::string> args = {executable_};
        args.insert(args.end(), arguments_.begin(), arguments_.end());

        // Start the process
        impl_->child = bp::child(
            bp::exe = executable_,
            bp::args = arguments_,
            bp::start_dir = workingDirectory_,
            bp::std_in < impl_->stdinStream,
            bp::std_out > impl_->stdoutStream,
            bp::std_err > impl_->stderrStream
        );

        if (!impl_->child.running()) {
            spdlog::error("[Process] Failed to start process");
            return false;
        }

        spdlog::info("[Process] Started with PID: {}", impl_->child.id());
        impl_->running = true;

        // Start read thread
        impl_->readThread = std::make_unique<std::thread>(&Impl::readLoop, impl_.get());

        return true;
    } catch (const std::exception& e) {
        spdlog::error("[Process] Exception starting process: {}", e.what());
        return false;
    }
}

void Process::stop() {
    if (!impl_->running) return;

    spdlog::debug("[Process] Stopping process");
    impl_->running = false;

    if (impl_->child.running()) {
        impl_->child.terminate();
        impl_->child.wait();
    }

    if (impl_->readThread && impl_->readThread->joinable()) {
        impl_->readThread->join();
    }

    spdlog::debug("[Process] Process stopped");
}

bool Process::isRunning() const {
    return impl_->running && impl_->child.running();
}

void Process::setOutputCallback(OutputCallback callback) {
    impl_->outputCallback = std::move(callback);
}

bool Process::write(const std::string& data) {
    if (!impl_->running) return false;

    try {
        impl_->stdinStream.write(data.data(), data.size());
        impl_->stdinStream.flush();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[Process] Write failed: {}", e.what());
        return false;
    }
}

int Process::pid() const {
    return impl_->child.id();
}

} // namespace lsp
} // namespace gitreview
