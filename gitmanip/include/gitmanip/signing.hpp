#pragma once

/// @file signing.hpp
/// @brief Commit and tag signing support using GPG or SSH.
///
/// This header provides functionality for signing Git objects and verifying
/// signatures. It reads signing configuration from Git config and uses the
/// same mechanisms as Git itself (shelling out to gpg or ssh-keygen).
///
/// @section signing_config Configuration
///
/// Signing configuration is read from Git config:
/// - `user.signingkey`: Key ID (GPG) or path to SSH key
/// - `gpg.format`: "openpgp" (default) or "ssh"
/// - `gpg.program`: Custom GPG binary path (default: "gpg")
/// - `gpg.ssh.program`: Custom SSH signing binary (default: "ssh-keygen")
/// - `gpg.ssh.allowedSignersFile`: Path to allowed signers for SSH verification
/// - `commit.gpgsign`: If true, sign commits by default
/// - `tag.gpgsign`: If true, sign tags by default
///
/// @section signing_example Example Usage
/// @code{.cpp}
/// auto repo = gitmanip::Repository::open("/path/to/repo");
///
/// // Load signing config from repository
/// auto config = gitmanip::SigningConfig::from_repo(repo);
/// std::cout << "Format: " << (config.format == SigningFormat::SSH ? "ssh" : "gpg") << "\n";
/// std::cout << "Key: " << config.signing_key << "\n";
///
/// // Create a signer
/// auto signer = gitmanip::Signer::create(config);
///
/// // Sign some data
/// auto signature = signer->sign("data to sign");
///
/// // Verify a signature
/// auto result = signer->verify("data", signature);
/// if (result.valid) {
///     std::cout << "Valid signature by " << result.signer_identity << "\n";
/// }
///
/// // Create a signed commit
/// gitmanip::CommitBuilder builder(repo, base_commit);
/// builder.set_message("Signed commit");
/// builder.set_signer(signer);
/// auto commit = builder.build_commit();
///
/// // Check if a commit is signed
/// if (auto sig = commit.signature()) {
///     std::cout << "Commit is signed\n";
///     auto verify_result = signer->verify(commit.signed_data(), *sig);
/// }
/// @endcode
///
/// @section signing_gpg GPG Signing
///
/// GPG signing requires:
/// - gpg binary installed and in PATH (or configured via gpg.program)
/// - A GPG key configured via user.signingkey or available as default
///
/// @section signing_ssh SSH Signing
///
/// SSH signing (Git 2.34+) requires:
/// - ssh-keygen binary (or configured via gpg.ssh.program)
/// - An SSH key configured via user.signingkey
/// - For verification: gpg.ssh.allowedSignersFile configured

#include "types.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gitmanip {

class Repository;
class Commit;

/// @brief Signing format (algorithm type).
enum class SigningFormat {
    OpenPGP,  ///< GPG/PGP signing (default)
    SSH,      ///< SSH key signing (Git 2.34+)
    X509,     ///< X.509 certificate signing
};

/// @brief Result of signature verification.
struct VerificationResult {
    /// @brief Whether the signature is valid.
    bool valid = false;

    /// @brief Whether the signature was made by a trusted key.
    ///
    /// For GPG: Key is in the user's keyring with ultimate/full trust.
    /// For SSH: Signer is listed in allowedSignersFile.
    bool trusted = false;

    /// @brief Identity of the signer (email, key ID, or fingerprint).
    std::string signer_identity;

    /// @brief Key fingerprint or ID used for signing.
    std::string key_id;

    /// @brief Detailed verification message from the signing program.
    std::string message;

    /// @brief Error message if verification failed.
    std::optional<std::string> error;

    /// @brief Timestamp of the signature (if available).
    std::optional<std::chrono::system_clock::time_point> timestamp;
};

/// @brief Signing configuration loaded from Git config.
struct SigningConfig {
    /// @brief Signing format to use.
    SigningFormat format = SigningFormat::OpenPGP;

    /// @brief Key ID (GPG) or path to SSH key.
    std::string signing_key;

    /// @brief Path to GPG binary.
    std::filesystem::path gpg_program = "gpg";

    /// @brief Path to SSH signing program (usually ssh-keygen).
    std::filesystem::path ssh_program = "ssh-keygen";

    /// @brief Path to allowed signers file for SSH verification.
    std::optional<std::filesystem::path> ssh_allowed_signers_file;

    /// @brief Whether to sign commits by default.
    bool sign_commits = false;

    /// @brief Whether to sign tags by default.
    bool sign_tags = false;

    /// @brief Whether no-reply emails are allowed for SSH signing.
    bool ssh_allow_no_reply_email = false;

    /// @brief Load configuration from a repository.
    ///
    /// Reads signing settings from the repository's Git config,
    /// including global and system config files.
    ///
    /// @param repo The repository.
    /// @return Signing configuration.
    [[nodiscard]] static SigningConfig from_repo(Repository& repo);

    /// @brief Load configuration from a specific config file.
    ///
    /// @param config_path Path to a Git config file.
    /// @return Signing configuration.
    [[nodiscard]] static SigningConfig from_file(const std::filesystem::path& config_path);

    /// @brief Create a default configuration.
    ///
    /// @return Default configuration (GPG format, no key specified).
    [[nodiscard]] static SigningConfig defaults();

    /// @brief Check if signing is properly configured.
    ///
    /// @return true if a signing key is available.
    [[nodiscard]] bool is_configured() const {
        return !signing_key.empty();
    }
};

/// @brief Signature data extracted from a commit or tag.
struct CommitSignature {
    /// @brief The ASCII-armored signature.
    std::string signature;

    /// @brief The data that was signed (commit/tag content).
    std::string signed_data;

    /// @brief The signing format (detected from signature header).
    SigningFormat format = SigningFormat::OpenPGP;
};

/// @brief Abstract interface for signing operations.
///
/// Implementations handle the actual signing and verification by calling
/// external programs (GPG, ssh-keygen, etc.).
class Signer {
public:
    virtual ~Signer() = default;

    /// @brief Sign data and return the detached signature.
    ///
    /// @param data Data to sign.
    /// @return ASCII-armored signature.
    /// @throws GitError if signing fails.
    [[nodiscard]] virtual std::string sign(std::string_view data) = 0;

    /// @brief Verify a detached signature.
    ///
    /// @param data The signed data.
    /// @param signature The ASCII-armored signature.
    /// @return Verification result.
    [[nodiscard]] virtual VerificationResult verify(std::string_view data,
                                                     std::string_view signature) = 0;

    /// @brief Get the signing format this signer uses.
    [[nodiscard]] virtual SigningFormat format() const = 0;

    /// @brief Get the key ID or identifier being used.
    [[nodiscard]] virtual std::string key_id() const = 0;

    /// @brief Create a signer from configuration.
    ///
    /// @param config Signing configuration.
    /// @return Signer implementation.
    /// @throws GitError if the configuration is invalid or required programs are missing.
    [[nodiscard]] static std::unique_ptr<Signer> create(const SigningConfig& config);

    /// @brief Create a GPG signer.
    ///
    /// @param key_id GPG key ID to use for signing.
    /// @param gpg_program Path to GPG binary (default: "gpg").
    /// @return GPG signer.
    [[nodiscard]] static std::unique_ptr<Signer> create_gpg(
        std::string_view key_id,
        const std::filesystem::path& gpg_program = "gpg");

    /// @brief Create an SSH signer.
    ///
    /// @param key_path Path to SSH private key.
    /// @param ssh_program Path to ssh-keygen binary (default: "ssh-keygen").
    /// @param allowed_signers_file Path to allowed signers file for verification.
    /// @return SSH signer.
    [[nodiscard]] static std::unique_ptr<Signer> create_ssh(
        const std::filesystem::path& key_path,
        const std::filesystem::path& ssh_program = "ssh-keygen",
        std::optional<std::filesystem::path> allowed_signers_file = std::nullopt);
};

/// @brief GPG signer implementation.
///
/// Signs data using gpg and verifies signatures using gpg --verify.
class GpgSigner : public Signer {
public:
    /// @brief Create a GPG signer.
    ///
    /// @param key_id Key ID (email, fingerprint, or key ID).
    /// @param gpg_program Path to GPG binary.
    GpgSigner(std::string key_id, std::filesystem::path gpg_program = "gpg");

    [[nodiscard]] std::string sign(std::string_view data) override;
    [[nodiscard]] VerificationResult verify(std::string_view data,
                                            std::string_view signature) override;
    [[nodiscard]] SigningFormat format() const override { return SigningFormat::OpenPGP; }
    [[nodiscard]] std::string key_id() const override { return key_id_; }

private:
    std::string key_id_;
    std::filesystem::path gpg_program_;
};

/// @brief SSH signer implementation.
///
/// Signs data using ssh-keygen -Y sign and verifies using ssh-keygen -Y verify.
class SshSigner : public Signer {
public:
    /// @brief Create an SSH signer.
    ///
    /// @param key_path Path to SSH private key file.
    /// @param ssh_program Path to ssh-keygen binary.
    /// @param allowed_signers_file Path to allowed signers file (for verification).
    SshSigner(std::filesystem::path key_path,
              std::filesystem::path ssh_program = "ssh-keygen",
              std::optional<std::filesystem::path> allowed_signers_file = std::nullopt);

    [[nodiscard]] std::string sign(std::string_view data) override;
    [[nodiscard]] VerificationResult verify(std::string_view data,
                                            std::string_view signature) override;
    [[nodiscard]] SigningFormat format() const override { return SigningFormat::SSH; }
    [[nodiscard]] std::string key_id() const override;

private:
    std::filesystem::path key_path_;
    std::filesystem::path ssh_program_;
    std::optional<std::filesystem::path> allowed_signers_file_;
};

// ============================================================================
// Convenience Functions
// ============================================================================

/// @brief Check if a commit has a signature.
///
/// @param commit The commit to check.
/// @return true if the commit is signed.
[[nodiscard]] bool is_signed(const Commit& commit);

/// @brief Extract signature from a commit.
///
/// @param commit The commit.
/// @return Signature data, or nullopt if not signed.
[[nodiscard]] std::optional<CommitSignature> extract_signature(const Commit& commit);

/// @brief Verify a commit's signature.
///
/// @param commit The commit to verify.
/// @param signer Signer to use for verification (must match signature format).
/// @return Verification result.
/// @throws GitError if commit is not signed.
[[nodiscard]] VerificationResult verify_commit(const Commit& commit, Signer& signer);

/// @brief Create a signed commit buffer.
///
/// Creates the commit content that will be signed and written.
///
/// @param repo Repository.
/// @param message Commit message.
/// @param tree Tree OID.
/// @param parents Parent commit OIDs.
/// @param author Author signature.
/// @param committer Committer signature.
/// @return The commit buffer to be signed.
[[nodiscard]] std::string create_commit_buffer(
    Repository& repo,
    std::string_view message,
    const Oid& tree,
    std::span<const Oid> parents,
    const Signature& author,
    const Signature& committer);

/// @brief Create a signed commit.
///
/// @param repo Repository.
/// @param message Commit message.
/// @param tree Tree for the commit.
/// @param parents Parent commits.
/// @param author Author signature.
/// @param committer Committer signature.
/// @param signer Signer to use.
/// @return OID of the new signed commit.
[[nodiscard]] Oid create_signed_commit(
    Repository& repo,
    std::string_view message,
    const Tree& tree,
    std::span<const Commit* const> parents,
    const Signature& author,
    const Signature& committer,
    Signer& signer);

/// @brief Create a signed commit using default signatures.
[[nodiscard]] Oid create_signed_commit(
    Repository& repo,
    std::string_view message,
    const Tree& tree,
    std::span<const Commit* const> parents,
    Signer& signer);

}  // namespace gitmanip
