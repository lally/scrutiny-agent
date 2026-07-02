// indexer_tests.cpp
// Test suite for the source code indexer

#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <filesystem>
#include <cassert>

#include "indexer/Indexer.hpp"
#include "db/LSPCache.hpp"
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

// Use gitreview namespaces
using namespace gitreview;
using namespace gitreview::indexer;
using namespace gitreview::lsp;

// Test result tracking
struct TestResults {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;
};

static TestResults g_results;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  FAIL: " << msg << std::endl; \
            g_results.failed++; \
            g_results.failures.push_back(std::string(__FUNCTION__) + ": " + msg); \
            return false; \
        } \
    } while(0)

#define TEST_PASS() \
    do { \
        g_results.passed++; \
        std::cout << "  PASS" << std::endl; \
        return true; \
    } while(0)

// Helper to check if a definition with given name exists
bool hasDefinition(const std::vector<IndexDefinition>& defs, const std::string& name) {
    for (const auto& def : defs) {
        if (def.name == name) {
            return true;
        }
    }
    return false;
}

// Helper to check if a definition with given name and kind exists
bool hasDefinitionOfKind(const std::vector<IndexDefinition>& defs,
                          const std::string& name,
                          SymbolKind kind) {
    for (const auto& def : defs) {
        if (def.name == name && def.kind == kind) {
            return true;
        }
    }
    return false;
}

// Helper to get all definition names
std::set<std::string> getDefinitionNames(const std::vector<IndexDefinition>& defs) {
    std::set<std::string> names;
    for (const auto& def : defs) {
        names.insert(def.name);
    }
    return names;
}

// Helper to check if references exist for a symbol
bool hasReferenceTo(const std::vector<IndexReference>& refs, const std::string& symbolName) {
    for (const auto& ref : refs) {
        if (ref.symbolName == symbolName) {
            return true;
        }
    }
    return false;
}

// Get the fixtures directory path
std::string getFixturesPath() {
    // Try to find fixtures relative to executable or use environment
    fs::path currentPath = fs::current_path();

    // Try common locations
    std::vector<std::string> possiblePaths = {
        "tests/fixtures",
        "../tests/fixtures",
        "Core/tests/fixtures",
        "../Core/tests/fixtures",
    };

    for (const auto& p : possiblePaths) {
        fs::path testPath = currentPath / p;
        if (fs::exists(testPath)) {
            return fs::canonical(testPath).string();
        }
    }

    // Fallback to environment variable
    if (const char* env = std::getenv("GRC_TEST_FIXTURES")) {
        return env;
    }

    return "";
}

// =============================================================================
// C++ Indexer Tests
// =============================================================================

bool test_cpp_finds_classes() {
    std::cout << "test_cpp_finds_classes..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string cppPath = fixturesPath + "/cpp";
    TEST_ASSERT(fs::exists(cppPath), "C++ fixtures directory not found: " + cppPath);

    Indexer idx(cppPath, Language::Cpp);
    auto result = idx.run();

    TEST_ASSERT(result.filesIndexed > 0, "No files were indexed");

    auto defs = idx.getDefinitions();
    TEST_ASSERT(!defs.empty(), "No definitions found");

    // Check for Vector2D class
    TEST_ASSERT(hasDefinitionOfKind(defs, "Vector2D", SymbolKind::Class),
                "Vector2D class not found");

    TEST_PASS();
}

bool test_cpp_finds_functions() {
    std::cout << "test_cpp_finds_functions..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string cppPath = fixturesPath + "/cpp";
    Indexer idx(cppPath, Language::Cpp);
    auto result = idx.run();

    auto defs = idx.getDefinitions();

    // Check for free functions
    TEST_ASSERT(hasDefinition(defs, "degrees_to_radians"), "degrees_to_radians not found");
    TEST_ASSERT(hasDefinition(defs, "radians_to_degrees"), "radians_to_degrees not found");
    TEST_ASSERT(hasDefinition(defs, "mean"), "mean function not found");
    TEST_ASSERT(hasDefinition(defs, "variance"), "variance function not found");
    TEST_ASSERT(hasDefinition(defs, "standard_deviation"), "standard_deviation not found");

    TEST_PASS();
}

bool test_cpp_finds_methods() {
    std::cout << "test_cpp_finds_methods..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string cppPath = fixturesPath + "/cpp";
    Indexer idx(cppPath, Language::Cpp);
    auto result = idx.run();

    auto defs = idx.getDefinitions();

    // Check for Vector2D methods
    TEST_ASSERT(hasDefinition(defs, "length"), "length method not found");
    TEST_ASSERT(hasDefinition(defs, "normalize"), "normalize method not found");
    TEST_ASSERT(hasDefinition(defs, "dot"), "dot method not found");

    TEST_PASS();
}

bool test_cpp_finds_templates() {
    std::cout << "test_cpp_finds_templates..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string cppPath = fixturesPath + "/cpp";
    Indexer idx(cppPath, Language::Cpp);
    auto result = idx.run();

    auto defs = idx.getDefinitions();

    // Check for template function
    TEST_ASSERT(hasDefinition(defs, "clamp"), "clamp template function not found");

    TEST_PASS();
}

bool test_cpp_finds_constants() {
    std::cout << "test_cpp_finds_constants..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string cppPath = fixturesPath + "/cpp";
    Indexer idx(cppPath, Language::Cpp);
    auto result = idx.run();

    auto defs = idx.getDefinitions();

    // Check for constexpr constants
    TEST_ASSERT(hasDefinition(defs, "PI"), "PI constant not found");
    TEST_ASSERT(hasDefinition(defs, "E"), "E constant not found");

    TEST_PASS();
}

bool test_cpp_finds_references() {
    std::cout << "test_cpp_finds_references..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string cppPath = fixturesPath + "/cpp";
    Indexer idx(cppPath, Language::Cpp);
    auto result = idx.run();

    TEST_ASSERT(result.referencesFound > 0, "No references found");

    auto refs = idx.getReferences();

    // Check that Vector2D is referenced (used in main)
    TEST_ASSERT(hasReferenceTo(refs, "Vector2D"), "No references to Vector2D found");

    // Check that mean is referenced
    TEST_ASSERT(hasReferenceTo(refs, "mean"), "No references to mean found");

    TEST_PASS();
}

// =============================================================================
// Python Indexer Tests
// =============================================================================

bool test_python_finds_classes() {
    std::cout << "test_python_finds_classes..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string pythonPath = fixturesPath + "/python";
    TEST_ASSERT(fs::exists(pythonPath), "Python fixtures directory not found");

    Indexer idx(pythonPath, Language::Python);
    auto result = idx.run();

    TEST_ASSERT(result.filesIndexed > 0, "No files were indexed");

    auto defs = idx.getDefinitions();
    TEST_ASSERT(!defs.empty(), "No definitions found");

    // Check for classes
    TEST_ASSERT(hasDefinitionOfKind(defs, "Calculator", SymbolKind::Class),
                "Calculator class not found");
    TEST_ASSERT(hasDefinitionOfKind(defs, "Operation", SymbolKind::Class) ||
                hasDefinitionOfKind(defs, "Operation", SymbolKind::Enum),
                "Operation enum not found");
    TEST_ASSERT(hasDefinitionOfKind(defs, "CalculationResult", SymbolKind::Class),
                "CalculationResult class not found");

    TEST_PASS();
}

bool test_python_finds_methods() {
    std::cout << "test_python_finds_methods..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string pythonPath = fixturesPath + "/python";
    Indexer idx(pythonPath, Language::Python);
    auto result = idx.run();

    auto defs = idx.getDefinitions();

    // Check for Calculator methods
    TEST_ASSERT(hasDefinition(defs, "add"), "add method not found");
    TEST_ASSERT(hasDefinition(defs, "subtract"), "subtract method not found");
    TEST_ASSERT(hasDefinition(defs, "multiply"), "multiply method not found");
    TEST_ASSERT(hasDefinition(defs, "divide"), "divide method not found");
    TEST_ASSERT(hasDefinition(defs, "power"), "power method not found");

    TEST_PASS();
}

bool test_python_finds_functions() {
    std::cout << "test_python_finds_functions..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string pythonPath = fixturesPath + "/python";
    Indexer idx(pythonPath, Language::Python);
    auto result = idx.run();

    auto defs = idx.getDefinitions();

    // Check for free functions
    TEST_ASSERT(hasDefinition(defs, "factorial"), "factorial function not found");
    TEST_ASSERT(hasDefinition(defs, "fibonacci"), "fibonacci function not found");
    TEST_ASSERT(hasDefinition(defs, "is_prime"), "is_prime function not found");

    TEST_PASS();
}

bool test_python_finds_references() {
    std::cout << "test_python_finds_references..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string pythonPath = fixturesPath + "/python";
    Indexer idx(pythonPath, Language::Python);
    auto result = idx.run();

    TEST_ASSERT(result.referencesFound > 0, "No references found");

    auto refs = idx.getReferences();

    // Calculator is used in __main__
    TEST_ASSERT(hasReferenceTo(refs, "Calculator"), "No references to Calculator found");

    TEST_PASS();
}

// =============================================================================
// Rust Indexer Tests
// =============================================================================

bool test_rust_finds_structs() {
    std::cout << "test_rust_finds_structs..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string rustPath = fixturesPath + "/rust";
    TEST_ASSERT(fs::exists(rustPath), "Rust fixtures directory not found");

    Indexer idx(rustPath, Language::Rust);
    auto result = idx.run();

    TEST_ASSERT(result.filesIndexed > 0, "No files were indexed");

    auto defs = idx.getDefinitions();
    TEST_ASSERT(!defs.empty(), "No definitions found");

    // Check for structs
    TEST_ASSERT(hasDefinition(defs, "Point"), "Point struct not found");
    TEST_ASSERT(hasDefinition(defs, "Circle"), "Circle struct not found");
    TEST_ASSERT(hasDefinition(defs, "Rectangle"), "Rectangle struct not found");

    TEST_PASS();
}

bool test_rust_finds_impl_methods() {
    std::cout << "test_rust_finds_impl_methods..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string rustPath = fixturesPath + "/rust";
    Indexer idx(rustPath, Language::Rust);
    auto result = idx.run();

    auto defs = idx.getDefinitions();

    // Check for impl methods
    TEST_ASSERT(hasDefinition(defs, "new"), "new method not found");
    TEST_ASSERT(hasDefinition(defs, "origin"), "origin method not found");
    TEST_ASSERT(hasDefinition(defs, "distance_to"), "distance_to method not found");
    TEST_ASSERT(hasDefinition(defs, "area"), "area method not found");
    TEST_ASSERT(hasDefinition(defs, "contains"), "contains method not found");

    TEST_PASS();
}

bool test_rust_finds_traits() {
    std::cout << "test_rust_finds_traits..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string rustPath = fixturesPath + "/rust";
    Indexer idx(rustPath, Language::Rust);
    auto result = idx.run();

    auto defs = idx.getDefinitions();

    // Check for trait
    TEST_ASSERT(hasDefinition(defs, "Shape"), "Shape trait not found");

    TEST_PASS();
}

bool test_rust_finds_functions() {
    std::cout << "test_rust_finds_functions..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string rustPath = fixturesPath + "/rust";
    Indexer idx(rustPath, Language::Rust);
    auto result = idx.run();

    auto defs = idx.getDefinitions();

    // Check for free functions
    TEST_ASSERT(hasDefinition(defs, "total_area"), "total_area function not found");
    TEST_ASSERT(hasDefinition(defs, "lerp"), "lerp function not found");
    TEST_ASSERT(hasDefinition(defs, "clamp"), "clamp function not found");
    TEST_ASSERT(hasDefinition(defs, "main"), "main function not found");

    TEST_PASS();
}

bool test_rust_finds_references() {
    std::cout << "test_rust_finds_references..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string rustPath = fixturesPath + "/rust";
    Indexer idx(rustPath, Language::Rust);
    auto result = idx.run();

    // Note: rust-analyzer returns LocationLink format (targetUri/targetRange) instead of
    // the standard Location format (uri/range). The LSPClient handles both formats.

    // Verify that definitions were found (this confirms rust-analyzer is working)
    TEST_ASSERT(result.definitionsFound > 0, "No definitions found from rust-analyzer");

    // Verify references were found
    TEST_ASSERT(result.referencesFound > 0, "No references found from rust-analyzer");

    // Verify expected symbols are referenced
    auto refs = idx.getReferences();
    TEST_ASSERT(hasReferenceTo(refs, "Point"), "No references to Point found");
    TEST_ASSERT(hasReferenceTo(refs, "Circle"), "No references to Circle found");

    TEST_PASS();
}

// =============================================================================
// Cross-cutting Tests
// =============================================================================

bool test_index_result_counts() {
    std::cout << "test_index_result_counts..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string cppPath = fixturesPath + "/cpp";
    Indexer idx(cppPath, Language::Cpp);
    auto result = idx.run();

    auto defs = idx.getDefinitions();
    auto refs = idx.getReferences();

    // Verify counts match
    TEST_ASSERT(static_cast<int>(defs.size()) == result.definitionsFound,
                "Definition count mismatch");
    TEST_ASSERT(static_cast<int>(refs.size()) == result.referencesFound,
                "Reference count mismatch");

    TEST_PASS();
}

bool test_definition_has_location() {
    std::cout << "test_definition_has_location..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string cppPath = fixturesPath + "/cpp";
    Indexer idx(cppPath, Language::Cpp);
    idx.run();

    auto defs = idx.getDefinitions();
    TEST_ASSERT(!defs.empty(), "No definitions found");

    // Every definition should have a file URI and valid range
    for (const auto& def : defs) {
        TEST_ASSERT(!def.fileUri.empty(), "Definition has empty file URI: " + def.name);
        TEST_ASSERT(def.range.start.line >= 0, "Definition has invalid start line: " + def.name);
    }

    TEST_PASS();
}

bool test_reference_has_location() {
    std::cout << "test_reference_has_location..." << std::endl;

    std::string fixturesPath = getFixturesPath();
    TEST_ASSERT(!fixturesPath.empty(), "Could not find fixtures directory");

    std::string cppPath = fixturesPath + "/cpp";
    Indexer idx(cppPath, Language::Cpp);
    idx.run();

    auto refs = idx.getReferences();

    // Every reference should have source and target file URIs
    for (const auto& ref : refs) {
        TEST_ASSERT(!ref.fromFileUri.empty(), "Reference has empty source file URI");
        TEST_ASSERT(!ref.toFileUri.empty(), "Reference has empty target file URI");
        TEST_ASSERT(!ref.symbolName.empty(), "Reference has empty symbol name");
    }

    TEST_PASS();
}

// =============================================================================
// Main test runner
// =============================================================================

int main(int argc, char* argv[]) {
    // Initialize logging from SPDLOG_LEVEL environment variable
    if (const char* level = std::getenv("SPDLOG_LEVEL")) {
        std::string levelStr(level);
        if (levelStr == "trace") spdlog::set_level(spdlog::level::trace);
        else if (levelStr == "debug") spdlog::set_level(spdlog::level::debug);
        else if (levelStr == "info") spdlog::set_level(spdlog::level::info);
        else if (levelStr == "warn") spdlog::set_level(spdlog::level::warn);
        else if (levelStr == "error") spdlog::set_level(spdlog::level::err);
    }

    std::cout << "==================================================" << std::endl;
    std::cout << "Git Review Core - Indexer Test Suite" << std::endl;
    std::cout << "==================================================" << std::endl;

    std::string fixturesPath = getFixturesPath();
    if (fixturesPath.empty()) {
        std::cerr << "ERROR: Could not find test fixtures directory." << std::endl;
        std::cerr << "Please run from the project root or set GRC_TEST_FIXTURES environment variable." << std::endl;
        return 1;
    }
    std::cout << "Using fixtures from: " << fixturesPath << std::endl;
    std::cout << std::endl;

    // C++ Tests
    std::cout << "--- C++ Indexer Tests ---" << std::endl;
    test_cpp_finds_classes();
    test_cpp_finds_functions();
    test_cpp_finds_methods();
    test_cpp_finds_templates();
    test_cpp_finds_constants();
    test_cpp_finds_references();
    std::cout << std::endl;

    // Python Tests
    std::cout << "--- Python Indexer Tests ---" << std::endl;
    test_python_finds_classes();
    test_python_finds_methods();
    test_python_finds_functions();
    test_python_finds_references();
    std::cout << std::endl;

    // Rust Tests
    std::cout << "--- Rust Indexer Tests ---" << std::endl;
    test_rust_finds_structs();
    test_rust_finds_impl_methods();
    test_rust_finds_traits();
    test_rust_finds_functions();
    test_rust_finds_references();
    std::cout << std::endl;

    // Cross-cutting Tests
    std::cout << "--- Cross-cutting Tests ---" << std::endl;
    test_index_result_counts();
    test_definition_has_location();
    test_reference_has_location();
    std::cout << std::endl;

    // Summary
    std::cout << "==================================================" << std::endl;
    std::cout << "Test Results: " << g_results.passed << " passed, "
              << g_results.failed << " failed" << std::endl;

    if (!g_results.failures.empty()) {
        std::cout << std::endl;
        std::cout << "Failures:" << std::endl;
        for (const auto& f : g_results.failures) {
            std::cout << "  - " << f << std::endl;
        }
    }

    std::cout << "==================================================" << std::endl;

    return g_results.failed > 0 ? 1 : 0;
}
