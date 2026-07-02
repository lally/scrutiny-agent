#pragma once

#include <gitmanip/signing.hpp>
#include <map>
#include <string>

namespace gitmanip {
namespace test {

/// A mock signer for testing that doesn't require real GPG/SSH setup.
/// It generates deterministic fake signatures that can be verified.
class MockSigner : public Signer {
public:
    explicit MockSigner(std::string_view key_id,
                        SigningFormat fmt = SigningFormat::OpenPGP)
        : key_id_(key_id), format_(fmt) {}

    std::string sign(std::string_view data) override {
        // Create a deterministic "signature" based on the data
        // In a real scenario, this would be a cryptographic signature
        std::string hash = compute_mock_hash(data);

        if (format_ == SigningFormat::OpenPGP) {
            return create_pgp_signature(hash);
        } else if (format_ == SigningFormat::SSH) {
            return create_ssh_signature(hash);
        }
        return create_pgp_signature(hash);
    }

    VerificationResult verify(std::string_view data,
                              std::string_view signature) override {
        VerificationResult result;

        // Extract the hash from the signature
        std::string expected_hash = compute_mock_hash(data);
        std::string sig_str(signature);

        // Check if signature contains our expected hash
        if (sig_str.find(expected_hash) != std::string::npos) {
            result.valid = true;
            result.trusted = true;
            result.signer_identity = key_id_;
            result.key_id = "MOCK" + key_id_.substr(0, std::min(size_t(8), key_id_.size()));
            result.message = "Good signature from mock signer";
            result.timestamp = std::chrono::system_clock::now();
        } else {
            result.valid = false;
            result.trusted = false;
            result.message = "Bad signature - hash mismatch";
            result.error = "Signature verification failed";
        }

        return result;
    }

    SigningFormat format() const override { return format_; }
    std::string key_id() const override { return key_id_; }

    /// Set whether sign() should fail (for testing error paths)
    void set_should_fail(bool fail) { should_fail_ = fail; }

private:
    std::string key_id_;
    SigningFormat format_;
    bool should_fail_ = false;

    /// Compute a simple deterministic hash for testing
    std::string compute_mock_hash(std::string_view data) const {
        // Simple hash: sum of bytes mod 2^32, formatted as hex
        uint32_t hash = 0;
        for (char c : data) {
            hash = hash * 31 + static_cast<uint8_t>(c);
        }
        // Include key_id in hash so different keys produce different signatures
        for (char c : key_id_) {
            hash = hash * 31 + static_cast<uint8_t>(c);
        }

        char buf[9];
        snprintf(buf, sizeof(buf), "%08X", hash);
        return buf;
    }

    std::string create_pgp_signature(const std::string& hash) const {
        // Create a fake but parseable PGP signature
        return "-----BEGIN PGP SIGNATURE-----\n"
               "\n"
               "iQMOCK" + hash + "MOCK\n"
               "MOCK" + key_id_ + "MOCK\n"
               "=MOCK\n"
               "-----END PGP SIGNATURE-----\n";
    }

    std::string create_ssh_signature(const std::string& hash) const {
        // Create a fake but parseable SSH signature
        return "-----BEGIN SSH SIGNATURE-----\n"
               "U1NITVNJ" + hash + "MOCK\n"
               "MOCK" + key_id_ + "MOCK\n"
               "-----END SSH SIGNATURE-----\n";
    }
};

/// A mock signer factory that creates MockSigner instances
class MockSignerFactory {
public:
    static std::unique_ptr<Signer> create_gpg(std::string_view key_id) {
        return std::make_unique<MockSigner>(key_id, SigningFormat::OpenPGP);
    }

    static std::unique_ptr<Signer> create_ssh(std::string_view key_path) {
        return std::make_unique<MockSigner>(key_path, SigningFormat::SSH);
    }
};

}  // namespace test
}  // namespace gitmanip
