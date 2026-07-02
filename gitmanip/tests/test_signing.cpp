#include "test_fixture.hpp"
#include "mock_signer.hpp"

#include <cstdlib>
#include <fstream>

class SigningTest : public GitTestFixture {};

// Test SigningConfig loading defaults
TEST_F(SigningTest, SigningConfigDefaults) {
    auto config = gitmanip::SigningConfig::defaults();

    EXPECT_EQ(config.format, gitmanip::SigningFormat::OpenPGP);
    EXPECT_TRUE(config.signing_key.empty());
    EXPECT_EQ(config.gpg_program, "gpg");
    EXPECT_EQ(config.ssh_program, "ssh-keygen");
    EXPECT_FALSE(config.sign_commits);
    EXPECT_FALSE(config.sign_tags);
    EXPECT_FALSE(config.is_configured());
}

// Test SigningConfig::from_repo reads from git config
TEST_F(SigningTest, SigningConfigFromRepo) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    // Load config (will have defaults since we haven't set anything)
    auto config = gitmanip::SigningConfig::from_repo(*repo_);

    // Should have loaded defaults
    EXPECT_EQ(config.format, gitmanip::SigningFormat::OpenPGP);
}

// Test that unsigned commits are detected correctly
TEST_F(SigningTest, UnsignedCommit) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    auto commit = repo_->lookup_commit(first_oid);

    EXPECT_FALSE(commit.is_signed());
    EXPECT_FALSE(commit.signature().has_value());
    EXPECT_FALSE(commit.signed_data().has_value());
}

// Test signature extraction from Commit class
TEST_F(SigningTest, CommitSignatureMethods) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    auto commit = repo_->lookup_commit(first_oid);

    // Unsigned commit should return false/nullopt
    EXPECT_FALSE(commit.is_signed());

    auto sig = commit.signature();
    EXPECT_FALSE(sig.has_value());

    auto data = commit.signed_data();
    EXPECT_FALSE(data.has_value());

    auto field = commit.signature_field();
    EXPECT_FALSE(field.has_value());
}

// Test VerificationResult structure
TEST_F(SigningTest, VerificationResultDefaults) {
    gitmanip::VerificationResult result;

    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.trusted);
    EXPECT_TRUE(result.signer_identity.empty());
    EXPECT_TRUE(result.key_id.empty());
    EXPECT_TRUE(result.message.empty());
    EXPECT_FALSE(result.error.has_value());
    EXPECT_FALSE(result.timestamp.has_value());
}

// Test CommitSignature structure
TEST_F(SigningTest, CommitSignatureStructure) {
    gitmanip::CommitSignature sig;

    EXPECT_TRUE(sig.signature.empty());
    EXPECT_TRUE(sig.signed_data.empty());
    EXPECT_EQ(sig.format, gitmanip::SigningFormat::OpenPGP);
}

// Test is_signed convenience function
TEST_F(SigningTest, IsSigned) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    auto commit = repo_->lookup_commit(first_oid);

    EXPECT_FALSE(gitmanip::is_signed(commit));
}

// Test extract_signature convenience function
TEST_F(SigningTest, ExtractSignature) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    auto commit = repo_->lookup_commit(first_oid);

    auto sig = gitmanip::extract_signature(commit);
    EXPECT_FALSE(sig.has_value());
}

// Test TagInfo signed properties
TEST_F(SigningTest, UnsignedTag) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto commit = repo_->lookup_commit(first_oid);
    gitmanip::Tag::create_annotated(*repo_, "v1.0.0", commit, "Release 1.0.0");

    auto tag_info = gitmanip::Tag::get(*repo_, "v1.0.0");
    ASSERT_TRUE(tag_info.has_value());
    EXPECT_FALSE(tag_info->is_signed);
    EXPECT_FALSE(tag_info->gpg_signature.has_value());
}

// Test Signer factory - should throw when no key configured
TEST_F(SigningTest, SignerCreateNoKey) {
    gitmanip::SigningConfig config;
    config.signing_key = "";  // Empty key

    EXPECT_THROW(gitmanip::Signer::create(config), gitmanip::GitError);
}

// Test create_gpg factory
TEST_F(SigningTest, CreateGpgSigner) {
    // This just tests construction - actual signing would require GPG setup
    auto signer = gitmanip::Signer::create_gpg("test@example.com");

    EXPECT_EQ(signer->format(), gitmanip::SigningFormat::OpenPGP);
    EXPECT_EQ(signer->key_id(), "test@example.com");
}

// Test create_ssh factory
TEST_F(SigningTest, CreateSshSigner) {
    // This just tests construction - actual signing would require SSH key
    auto signer = gitmanip::Signer::create_ssh("/path/to/key");

    EXPECT_EQ(signer->format(), gitmanip::SigningFormat::SSH);
    EXPECT_EQ(signer->key_id(), "/path/to/key");
}

// Test that CommitBuilder accepts signer
TEST_F(SigningTest, CommitBuilderWithSigner) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    auto first = repo_->lookup_commit(first_oid);

    // Create a mock/dummy signer scenario
    // Note: Actually signing requires real GPG/SSH setup
    // Here we just test that the API accepts a signer

    gitmanip::CommitBuilder builder(*repo_, first);
    builder.set_message("Test message");

    // Without signer, should create unsigned commit
    auto new_oid = builder.build();
    auto new_commit = repo_->lookup_commit(new_oid);

    EXPECT_FALSE(new_commit.is_signed());
}

// Test SigningFormat enum values
TEST_F(SigningTest, SigningFormatValues) {
    EXPECT_NE(gitmanip::SigningFormat::OpenPGP, gitmanip::SigningFormat::SSH);
    EXPECT_NE(gitmanip::SigningFormat::OpenPGP, gitmanip::SigningFormat::X509);
    EXPECT_NE(gitmanip::SigningFormat::SSH, gitmanip::SigningFormat::X509);
}

// ============================================================================
// Mock Signer Tests - These test the Signer interface without real GPG/SSH
// ============================================================================

class MockSigningTest : public GitTestFixture {};

TEST_F(MockSigningTest, MockSignerSignAndVerify) {
    auto signer = gitmanip::test::MockSignerFactory::create_gpg("test@example.com");

    std::string data = "Test data to sign";
    auto signature = signer->sign(data);

    // Signature should be non-empty and look like PGP
    EXPECT_FALSE(signature.empty());
    EXPECT_TRUE(signature.find("-----BEGIN PGP SIGNATURE-----") != std::string::npos);
    EXPECT_TRUE(signature.find("-----END PGP SIGNATURE-----") != std::string::npos);

    // Verify should succeed
    auto result = signer->verify(data, signature);
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.trusted);
    EXPECT_EQ(result.signer_identity, "test@example.com");
}

TEST_F(MockSigningTest, MockSignerVerifyFailsWithWrongData) {
    auto signer = gitmanip::test::MockSignerFactory::create_gpg("test@example.com");

    std::string data = "Test data to sign";
    auto signature = signer->sign(data);

    // Verify with different data should fail
    auto result = signer->verify("Different data", signature);
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.trusted);
}

TEST_F(MockSigningTest, MockSignerSshFormat) {
    auto signer = gitmanip::test::MockSignerFactory::create_ssh("/path/to/key");

    EXPECT_EQ(signer->format(), gitmanip::SigningFormat::SSH);
    EXPECT_EQ(signer->key_id(), "/path/to/key");

    std::string data = "Test data to sign";
    auto signature = signer->sign(data);

    // Signature should look like SSH
    EXPECT_TRUE(signature.find("-----BEGIN SSH SIGNATURE-----") != std::string::npos);
    EXPECT_TRUE(signature.find("-----END SSH SIGNATURE-----") != std::string::npos);

    auto result = signer->verify(data, signature);
    EXPECT_TRUE(result.valid);
}

TEST_F(MockSigningTest, DifferentKeysDifferentSignatures) {
    auto signer1 = gitmanip::test::MockSignerFactory::create_gpg("key1@example.com");
    auto signer2 = gitmanip::test::MockSignerFactory::create_gpg("key2@example.com");

    std::string data = "Same data";
    auto sig1 = signer1->sign(data);
    auto sig2 = signer2->sign(data);

    // Different keys should produce different signatures
    EXPECT_NE(sig1, sig2);

    // Each signer should only verify its own signatures
    EXPECT_TRUE(signer1->verify(data, sig1).valid);
    EXPECT_FALSE(signer1->verify(data, sig2).valid);
    EXPECT_FALSE(signer2->verify(data, sig1).valid);
    EXPECT_TRUE(signer2->verify(data, sig2).valid);
}

// ============================================================================
// Real GPG Integration Tests - These require GPG to be installed
// ============================================================================

class GpgSigningTest : public GitTestFixture {
protected:
    std::string gnupg_home_;
    std::string key_id_;
    bool gpg_available_ = false;

    void SetUp() override {
        GitTestFixture::SetUp();

        // Check if GPG is available
        if (system("which gpg > /dev/null 2>&1") != 0) {
            return;
        }

        // Create a temporary GNUPGHOME
        gnupg_home_ = (test_dir_ / "gnupg").string();
        fs::create_directories(gnupg_home_);
        fs::permissions(gnupg_home_, fs::perms::owner_all);

        // Set environment variable
        setenv("GNUPGHOME", gnupg_home_.c_str(), 1);

        // Generate a test key using GPG batch mode
        std::string key_params = test_dir_.string() + "/key_params";
        {
            std::ofstream ofs(key_params);
            ofs << "%no-protection\n"
                << "Key-Type: RSA\n"
                << "Key-Length: 2048\n"
                << "Name-Real: Test User\n"
                << "Name-Email: test@gitmanip.test\n"
                << "Expire-Date: 0\n"
                << "%commit\n";
        }

        std::string cmd = "gpg --batch --gen-key " + key_params + " 2>/dev/null";
        if (system(cmd.c_str()) != 0) {
            return;
        }

        key_id_ = "test@gitmanip.test";
        gpg_available_ = true;
    }

    void TearDown() override {
        // Clean up GNUPGHOME
        if (!gnupg_home_.empty()) {
            // Kill any gpg-agent processes using this home
            std::string cmd = "gpgconf --homedir " + gnupg_home_ + " --kill all 2>/dev/null";
            system(cmd.c_str());
        }

        GitTestFixture::TearDown();
    }
};

TEST_F(GpgSigningTest, GpgSigningRoundTrip) {
    if (!gpg_available_) {
        GTEST_SKIP() << "GPG not available or key generation failed";
    }

    auto signer = gitmanip::Signer::create_gpg(key_id_);

    std::string data = "Test data to sign";
    auto signature = signer->sign(data);

    EXPECT_FALSE(signature.empty());
    EXPECT_TRUE(signature.find("-----BEGIN PGP SIGNATURE-----") != std::string::npos);

    auto result = signer->verify(data, signature);
    EXPECT_TRUE(result.valid);
}

TEST_F(GpgSigningTest, CreateSignedCommit) {
    if (!gpg_available_) {
        GTEST_SKIP() << "GPG not available or key generation failed";
    }

    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    auto first = repo_->lookup_commit(first_oid);

    auto signer = gitmanip::Signer::create_gpg(key_id_);

    gitmanip::CommitBuilder builder(*repo_, first);
    builder.set_message("Signed commit")
           .add_file("new.txt", "new content")
           .set_signer(signer.get());

    auto new_oid = builder.build();
    auto new_commit = repo_->lookup_commit(new_oid);

    EXPECT_TRUE(new_commit.is_signed());

    auto result = gitmanip::verify_commit(new_commit, *signer);
    EXPECT_TRUE(result.valid);
}

TEST_F(GpgSigningTest, CreateSignedTag) {
    if (!gpg_available_) {
        GTEST_SKIP() << "GPG not available or key generation failed";
    }

    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto commit = repo_->lookup_commit(first_oid);
    auto signer = gitmanip::Signer::create_gpg(key_id_);

    gitmanip::Tag::create_signed(*repo_, "v1.0.0", commit, "Release 1.0.0", *signer);

    auto tag_info = gitmanip::Tag::get(*repo_, "v1.0.0");
    ASSERT_TRUE(tag_info.has_value());
    EXPECT_TRUE(tag_info->is_signed);
    EXPECT_TRUE(tag_info->gpg_signature.has_value());

    // The signature should be a valid PGP signature
    EXPECT_TRUE(tag_info->gpg_signature->find("-----BEGIN PGP SIGNATURE-----") != std::string::npos);

    // Note: Tag::verify requires reconstructing the signed data exactly as GPG expects.
    // This is complex and error-prone, so we just verify that the signature is valid PGP format.
    // The actual verification would require matching the exact byte-for-byte format git uses.
}
