#include "gitmanip/signing.hpp"
#include "gitmanip/commit.hpp"
#include "gitmanip/error.hpp"
#include "gitmanip/repository.hpp"
#include "gitmanip/tree.hpp"

#include <git2.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define popen _popen
#define pclose _pclose
#define mkstemp _mkstemp
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace gitmanip {

namespace {

// Helper to run a command and capture output
struct ProcessResult {
    int exit_code = -1;
    std::string stdout_data;
    std::string stderr_data;
};

ProcessResult run_command(const std::vector<std::string>& args,
                          const std::string& stdin_data = "") {
    ProcessResult result;

#ifdef _WIN32
    // Windows implementation using _popen
    // Note: This is simplified and doesn't handle stdin well
    std::string cmd;
    for (const auto& arg : args) {
        if (!cmd.empty()) cmd += " ";
        // Basic quoting
        if (arg.find(' ') != std::string::npos || arg.find('"') != std::string::npos) {
            cmd += "\"";
            for (char c : arg) {
                if (c == '"') cmd += "\\\"";
                else cmd += c;
            }
            cmd += "\"";
        } else {
            cmd += arg;
        }
    }
    cmd += " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        result.stderr_data = "Failed to run command";
        return result;
    }

    std::array<char, 4096> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result.stdout_data += buffer.data();
    }

    result.exit_code = pclose(pipe);
#else
    // Unix implementation using fork/exec with pipes
    int stdin_pipe[2];
    int stdout_pipe[2];
    int stderr_pipe[2];

    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
        result.stderr_data = "Failed to create pipes";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.stderr_data = "Failed to fork";
        return result;
    }

    if (pid == 0) {
        // Child process
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Convert args to char**
        std::vector<char*> argv;
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    // Parent process
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Write stdin data
    if (!stdin_data.empty()) {
        ssize_t written = write(stdin_pipe[1], stdin_data.data(), stdin_data.size());
        (void)written;  // Ignore result for now
    }
    close(stdin_pipe[1]);

    // Read stdout
    std::array<char, 4096> buffer;
    ssize_t n;
    while ((n = read(stdout_pipe[0], buffer.data(), buffer.size())) > 0) {
        result.stdout_data.append(buffer.data(), n);
    }
    close(stdout_pipe[0]);

    // Read stderr
    while ((n = read(stderr_pipe[0], buffer.data(), buffer.size())) > 0) {
        result.stderr_data.append(buffer.data(), n);
    }
    close(stderr_pipe[0]);

    // Wait for child
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }
#endif

    return result;
}

// Create a temporary file with given content, returns path
std::filesystem::path create_temp_file(std::string_view content, const std::string& suffix = "") {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::string template_str = (temp_dir / ("gitmanip_XXXXXX" + suffix)).string();

#ifdef _WIN32
    // Windows: use _mktemp_s
    char* temp_name = _tempnam(temp_dir.string().c_str(), "gitmanip_");
    if (!temp_name) {
        throw GitError(ErrorCode::IoError, "Failed to create temporary file");
    }
    std::filesystem::path temp_path = temp_name;
    if (!suffix.empty()) {
        temp_path += suffix;
    }
    free(temp_name);
#else
    // Unix: use mkstemp for security
    std::vector<char> temp_name(template_str.begin(), template_str.end());
    temp_name.push_back('\0');

    int fd;
    if (suffix.empty()) {
        fd = mkstemp(temp_name.data());
    } else {
        fd = mkstemps(temp_name.data(), suffix.length());
    }
    if (fd < 0) {
        throw GitError(ErrorCode::IoError, "Failed to create temporary file");
    }
    close(fd);
    std::filesystem::path temp_path = temp_name.data();
#endif

    // Write content
    std::ofstream ofs(temp_path, std::ios::binary);
    ofs.write(content.data(), content.size());
    ofs.close();

    return temp_path;
}

// Read file content
std::string read_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        throw GitError(ErrorCode::IoError, fmt::format("Failed to read file: {}", path.string()));
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// Detect signature format from signature content
SigningFormat detect_signature_format(std::string_view signature) {
    if (signature.find("-----BEGIN PGP SIGNATURE-----") != std::string_view::npos ||
        signature.find("-----BEGIN PGP MESSAGE-----") != std::string_view::npos) {
        return SigningFormat::OpenPGP;
    }
    if (signature.find("-----BEGIN SSH SIGNATURE-----") != std::string_view::npos) {
        return SigningFormat::SSH;
    }
    if (signature.find("-----BEGIN SIGNED MESSAGE-----") != std::string_view::npos ||
        signature.find("-----BEGIN CERTIFICATE-----") != std::string_view::npos) {
        return SigningFormat::X509;
    }
    // Default to OpenPGP
    return SigningFormat::OpenPGP;
}

}  // namespace

// ============================================================================
// SigningConfig
// ============================================================================

SigningConfig SigningConfig::defaults() {
    return SigningConfig{};
}

SigningConfig SigningConfig::from_repo(Repository& repo) {
    SigningConfig config;

    git_config* cfg = nullptr;
    int error = git_repository_config(&cfg, repo.raw());
    if (error < 0) {
        spdlog::warn("Failed to get repository config, using defaults");
        return config;
    }

    // Helper to get string config value
    auto get_string = [cfg](const char* name) -> std::optional<std::string> {
        git_config_entry* entry = nullptr;
        if (git_config_get_entry(&entry, cfg, name) == 0 && entry && entry->value) {
            std::string value = entry->value;
            git_config_entry_free(entry);
            return value;
        }
        return std::nullopt;
    };

    // Helper to get bool config value
    auto get_bool = [cfg](const char* name) -> std::optional<bool> {
        int value = 0;
        if (git_config_get_bool(&value, cfg, name) == 0) {
            return value != 0;
        }
        return std::nullopt;
    };

    // Read gpg.format
    if (auto format = get_string("gpg.format")) {
        if (*format == "ssh") {
            config.format = SigningFormat::SSH;
        } else if (*format == "x509") {
            config.format = SigningFormat::X509;
        } else {
            config.format = SigningFormat::OpenPGP;
        }
    }

    // Read signing key
    if (auto key = get_string("user.signingkey")) {
        config.signing_key = *key;
    }

    // Read GPG program
    if (auto program = get_string("gpg.program")) {
        config.gpg_program = *program;
    }

    // Read SSH program
    if (auto program = get_string("gpg.ssh.program")) {
        config.ssh_program = *program;
    }

    // Read SSH allowed signers file
    if (auto file = get_string("gpg.ssh.allowedSignersFile")) {
        config.ssh_allowed_signers_file = *file;
    }

    // Read commit.gpgsign
    if (auto sign = get_bool("commit.gpgsign")) {
        config.sign_commits = *sign;
    }

    // Read tag.gpgsign
    if (auto sign = get_bool("tag.gpgsign")) {
        config.sign_tags = *sign;
    }

    git_config_free(cfg);
    return config;
}

SigningConfig SigningConfig::from_file(const std::filesystem::path& config_path) {
    SigningConfig config;

    git_config* cfg = nullptr;
    int error = git_config_open_ondisk(&cfg, config_path.string().c_str());
    if (error < 0) {
        throw GitError(ErrorCode::IoError,
                       fmt::format("Failed to open config file: {}", config_path.string()));
    }

    // Use the same logic as from_repo
    auto get_string = [cfg](const char* name) -> std::optional<std::string> {
        git_config_entry* entry = nullptr;
        if (git_config_get_entry(&entry, cfg, name) == 0 && entry && entry->value) {
            std::string value = entry->value;
            git_config_entry_free(entry);
            return value;
        }
        return std::nullopt;
    };

    if (auto format = get_string("gpg.format")) {
        if (*format == "ssh") {
            config.format = SigningFormat::SSH;
        } else if (*format == "x509") {
            config.format = SigningFormat::X509;
        }
    }

    if (auto key = get_string("user.signingkey")) {
        config.signing_key = *key;
    }

    if (auto program = get_string("gpg.program")) {
        config.gpg_program = *program;
    }

    if (auto program = get_string("gpg.ssh.program")) {
        config.ssh_program = *program;
    }

    git_config_free(cfg);
    return config;
}

// ============================================================================
// Signer Factory
// ============================================================================

std::unique_ptr<Signer> Signer::create(const SigningConfig& config) {
    if (!config.is_configured()) {
        throw GitError(ErrorCode::InvalidArgument,
                       "No signing key configured. Set user.signingkey in git config.");
    }

    switch (config.format) {
        case SigningFormat::SSH:
            return create_ssh(config.signing_key, config.ssh_program,
                              config.ssh_allowed_signers_file);
        case SigningFormat::OpenPGP:
            return create_gpg(config.signing_key, config.gpg_program);
        case SigningFormat::X509:
            throw GitError(ErrorCode::NotSupported, "X.509 signing not yet implemented");
    }

    return create_gpg(config.signing_key, config.gpg_program);
}

std::unique_ptr<Signer> Signer::create_gpg(std::string_view key_id,
                                            const std::filesystem::path& gpg_program) {
    return std::make_unique<GpgSigner>(std::string(key_id), gpg_program);
}

std::unique_ptr<Signer> Signer::create_ssh(
    const std::filesystem::path& key_path,
    const std::filesystem::path& ssh_program,
    std::optional<std::filesystem::path> allowed_signers_file) {
    return std::make_unique<SshSigner>(key_path, ssh_program, std::move(allowed_signers_file));
}

// ============================================================================
// GpgSigner
// ============================================================================

GpgSigner::GpgSigner(std::string key_id, std::filesystem::path gpg_program)
    : key_id_(std::move(key_id)), gpg_program_(std::move(gpg_program)) {}

std::string GpgSigner::sign(std::string_view data) {
    // Create temp file with data to sign
    auto data_file = create_temp_file(data);

    // Build GPG command
    // gpg --status-fd=2 -bsau <key> --output - <file>
    std::vector<std::string> args = {
        gpg_program_.string(),
        "--status-fd=2",
        "-bsau",
        key_id_,
        "--output",
        "-",
        data_file.string()
    };

    auto result = run_command(args);

    // Clean up temp file
    std::filesystem::remove(data_file);

    if (result.exit_code != 0) {
        throw GitError(ErrorCode::SigningError,
                       fmt::format("GPG signing failed: {}", result.stderr_data));
    }

    return result.stdout_data;
}

VerificationResult GpgSigner::verify(std::string_view data, std::string_view signature) {
    VerificationResult result;

    // Create temp files
    auto data_file = create_temp_file(data);
    auto sig_file = create_temp_file(signature, ".sig");

    // Build GPG verify command
    // gpg --status-fd=1 --keyid-format=long --verify <sig> <data>
    std::vector<std::string> args = {
        gpg_program_.string(),
        "--status-fd=1",
        "--keyid-format=long",
        "--verify",
        sig_file.string(),
        data_file.string()
    };

    auto proc_result = run_command(args);

    // Clean up temp files
    std::filesystem::remove(data_file);
    std::filesystem::remove(sig_file);

    // Parse GPG status output
    result.message = proc_result.stderr_data;

    // Check for GOODSIG, VALIDSIG, etc. in status output
    std::string status = proc_result.stdout_data;

    if (status.find("GOODSIG") != std::string::npos) {
        result.valid = true;

        // Extract signer identity from GOODSIG line
        std::regex goodsig_regex(R"(\[GNUPG:\] GOODSIG ([A-F0-9]+) (.+))");
        std::smatch match;
        if (std::regex_search(status, match, goodsig_regex)) {
            result.key_id = match[1].str();
            result.signer_identity = match[2].str();
        }
    }

    if (status.find("TRUST_ULTIMATE") != std::string::npos ||
        status.find("TRUST_FULLY") != std::string::npos) {
        result.trusted = true;
    }

    if (status.find("BADSIG") != std::string::npos) {
        result.valid = false;
        result.error = "Bad signature";
    }

    if (status.find("ERRSIG") != std::string::npos) {
        result.valid = false;
        result.error = "Signature verification error (missing key?)";
    }

    if (status.find("NO_PUBKEY") != std::string::npos) {
        result.valid = false;
        result.error = "Public key not found";
    }

    // Parse VALIDSIG for more details
    std::regex validsig_regex(R"(\[GNUPG:\] VALIDSIG ([A-F0-9]+) (\d{4}-\d{2}-\d{2}) (\d+))");
    std::smatch validsig_match;
    if (std::regex_search(status, validsig_match, validsig_regex)) {
        if (result.key_id.empty()) {
            result.key_id = validsig_match[1].str();
        }
        // Parse timestamp
        try {
            std::time_t timestamp = std::stoll(validsig_match[3].str());
            result.timestamp = std::chrono::system_clock::from_time_t(timestamp);
        } catch (...) {
            // Ignore timestamp parsing errors
        }
    }

    return result;
}

// ============================================================================
// SshSigner
// ============================================================================

SshSigner::SshSigner(std::filesystem::path key_path,
                     std::filesystem::path ssh_program,
                     std::optional<std::filesystem::path> allowed_signers_file)
    : key_path_(std::move(key_path)),
      ssh_program_(std::move(ssh_program)),
      allowed_signers_file_(std::move(allowed_signers_file)) {}

std::string SshSigner::key_id() const {
    return key_path_.string();
}

std::string SshSigner::sign(std::string_view data) {
    // Create temp file with data to sign
    auto data_file = create_temp_file(data);

    // Build ssh-keygen sign command
    // ssh-keygen -Y sign -n git -f <key> < <data>
    std::vector<std::string> args = {
        ssh_program_.string(),
        "-Y",
        "sign",
        "-n",
        "git",
        "-f",
        key_path_.string(),
        data_file.string()
    };

    auto result = run_command(args);

    // The signature is written to <data_file>.sig
    std::filesystem::path sig_file = data_file.string() + ".sig";

    std::string signature;
    if (std::filesystem::exists(sig_file)) {
        signature = read_file(sig_file);
        std::filesystem::remove(sig_file);
    }

    // Clean up data file
    std::filesystem::remove(data_file);

    if (result.exit_code != 0 || signature.empty()) {
        throw GitError(ErrorCode::SigningError,
                       fmt::format("SSH signing failed: {}", result.stderr_data));
    }

    return signature;
}

VerificationResult SshSigner::verify(std::string_view data, std::string_view signature) {
    VerificationResult result;

    if (!allowed_signers_file_) {
        result.error = "No allowed signers file configured (gpg.ssh.allowedSignersFile)";
        return result;
    }

    // Create temp files
    auto data_file = create_temp_file(data);
    auto sig_file = create_temp_file(signature, ".sig");

    // Build ssh-keygen verify command
    // ssh-keygen -Y verify -n git -f <allowed_signers> -I <identity> -s <sig> < <data>
    // Note: We need to find the identity from the signature or use a placeholder
    std::vector<std::string> args = {
        ssh_program_.string(),
        "-Y",
        "verify",
        "-n",
        "git",
        "-f",
        allowed_signers_file_->string(),
        "-I",
        "*",  // Match any identity
        "-s",
        sig_file.string(),
        "<",
        data_file.string()
    };

    // Actually, ssh-keygen verify reads from stdin, so we need a different approach
    // Let's use: ssh-keygen -Y verify -n git -f <allowed> -I <id> -s <sig> < data
    // This is tricky with our run_command, so let's use a wrapper

    // Simpler: use ssh-keygen -Y check-novalidate first to extract the principal
    std::vector<std::string> check_args = {
        ssh_program_.string(),
        "-Y",
        "find-principals",
        "-f",
        allowed_signers_file_->string(),
        "-s",
        sig_file.string()
    };

    auto check_result = run_command(check_args);

    std::string principal;
    if (check_result.exit_code == 0 && !check_result.stdout_data.empty()) {
        // First line is the principal
        std::istringstream iss(check_result.stdout_data);
        std::getline(iss, principal);
        // Trim whitespace
        while (!principal.empty() && (principal.back() == '\n' || principal.back() == '\r')) {
            principal.pop_back();
        }
    }

    if (principal.empty()) {
        // No matching principal found
        std::filesystem::remove(data_file);
        std::filesystem::remove(sig_file);
        result.error = "No matching principal found in allowed signers file";
        return result;
    }

    result.signer_identity = principal;

    // Now do the actual verification
    // We need to pipe the data to ssh-keygen
    // ssh-keygen -Y verify -n git -f <allowed> -I <principal> -s <sig> < data

    // Create a shell command for proper piping
    std::string cmd = fmt::format("{} -Y verify -n git -f {} -I \"{}\" -s {} < {}",
                                  ssh_program_.string(),
                                  allowed_signers_file_->string(),
                                  principal,
                                  sig_file.string(),
                                  data_file.string());

    std::vector<std::string> shell_args = {"/bin/sh", "-c", cmd};
    auto verify_result = run_command(shell_args);

    // Clean up temp files
    std::filesystem::remove(data_file);
    std::filesystem::remove(sig_file);

    if (verify_result.exit_code == 0) {
        result.valid = true;
        result.trusted = true;  // If in allowed_signers, it's trusted
        result.message = verify_result.stderr_data;
    } else {
        result.valid = false;
        result.error = verify_result.stderr_data;
    }

    return result;
}

// ============================================================================
// Convenience Functions
// ============================================================================

bool is_signed(const Commit& commit) {
    git_buf sig = GIT_BUF_INIT;
    git_buf data = GIT_BUF_INIT;

    // git_commit_extract_signature expects non-const git_oid*, copy it
    git_oid oid = *commit.id().raw();
    int error = git_commit_extract_signature(&sig, &data, commit.repository().raw(),
                                              &oid, nullptr);

    bool has_sig = (error == 0 && sig.size > 0);

    git_buf_dispose(&sig);
    git_buf_dispose(&data);

    return has_sig;
}

std::optional<CommitSignature> extract_signature(const Commit& commit) {
    git_buf sig = GIT_BUF_INIT;
    git_buf data = GIT_BUF_INIT;

    git_oid oid = *commit.id().raw();
    int error = git_commit_extract_signature(&sig, &data, commit.repository().raw(),
                                              &oid, nullptr);

    if (error != 0 || sig.size == 0) {
        git_buf_dispose(&sig);
        git_buf_dispose(&data);
        return std::nullopt;
    }

    CommitSignature result;
    result.signature = std::string(sig.ptr, sig.size);
    result.signed_data = std::string(data.ptr, data.size);
    result.format = detect_signature_format(result.signature);

    git_buf_dispose(&sig);
    git_buf_dispose(&data);

    return result;
}

VerificationResult verify_commit(const Commit& commit, Signer& signer) {
    auto sig = extract_signature(commit);
    if (!sig) {
        VerificationResult result;
        result.error = "Commit is not signed";
        return result;
    }

    return signer.verify(sig->signed_data, sig->signature);
}

std::string create_commit_buffer(Repository& repo,
                                  std::string_view message,
                                  const Oid& tree,
                                  std::span<const Oid> parents,
                                  const Signature& author,
                                  const Signature& committer) {
    git_buf buf = GIT_BUF_INIT;

    // Convert signature to git_signature
    git_signature* git_author = nullptr;
    git_signature* git_committer = nullptr;

    auto to_unix = [](const std::chrono::system_clock::time_point& tp) {
        return std::chrono::system_clock::to_time_t(tp);
    };

    git_signature_new(&git_author, author.name.c_str(), author.email.c_str(),
                      to_unix(author.time), author.offset_minutes);
    git_signature_new(&git_committer, committer.name.c_str(), committer.email.c_str(),
                      to_unix(committer.time), committer.offset_minutes);

    // Look up tree
    git_tree* git_tree_obj = nullptr;
    git_tree_lookup(&git_tree_obj, repo.raw(), tree.raw());

    // Build parent array
    std::vector<const git_commit*> parent_commits;
    for (const auto& parent_oid : parents) {
        git_commit* parent = nullptr;
        git_commit_lookup(&parent, repo.raw(), parent_oid.raw());
        parent_commits.push_back(parent);
    }

    // Create the commit buffer
    int error = git_commit_create_buffer(&buf, repo.raw(),
                                         git_author, git_committer,
                                         nullptr,  // Use default encoding
                                         std::string(message).c_str(),
                                         git_tree_obj,
                                         parent_commits.size(),
                                         parent_commits.data());

    // Clean up
    git_signature_free(git_author);
    git_signature_free(git_committer);
    git_tree_free(git_tree_obj);
    for (auto* parent : parent_commits) {
        git_commit_free(const_cast<git_commit*>(parent));
    }

    if (error < 0) {
        git_buf_dispose(&buf);
        detail::check_libgit2_error(error, "creating commit buffer");
    }

    std::string result(buf.ptr, buf.size);
    git_buf_dispose(&buf);

    return result;
}

Oid create_signed_commit(Repository& repo,
                          std::string_view message,
                          const Tree& tree,
                          std::span<const Commit* const> parents,
                          const Signature& author,
                          const Signature& committer,
                          Signer& signer) {
    // Get parent OIDs
    std::vector<Oid> parent_oids;
    parent_oids.reserve(parents.size());
    for (const auto* parent : parents) {
        parent_oids.push_back(parent->id());
    }

    // Create commit buffer
    std::string commit_content = create_commit_buffer(repo, message, tree.id(),
                                                       parent_oids, author, committer);

    // Sign the content
    std::string signature = signer.sign(commit_content);

    // Create the signed commit
    git_oid new_oid;
    int error = git_commit_create_with_signature(
        &new_oid,
        repo.raw(),
        commit_content.c_str(),
        signature.c_str(),
        nullptr  // Use default signature field name
    );

    detail::check_libgit2_error(error, "creating signed commit");

    return Oid(&new_oid);
}

Oid create_signed_commit(Repository& repo,
                          std::string_view message,
                          const Tree& tree,
                          std::span<const Commit* const> parents,
                          Signer& signer) {
    Signature sig = repo.default_signature();
    return create_signed_commit(repo, message, tree, parents, sig, sig, signer);
}

}  // namespace gitmanip
