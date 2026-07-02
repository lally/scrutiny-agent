// grc-index.cpp
// Command-line tool for indexing a codebase using LSP

#include <iostream>
#include <string>
#include <vector>
#include <cstring>

#include "GitReviewCore.h"

static void printUsage(const char* programName) {
    std::cerr << "Usage: " << programName << " [options] <workspace-path>\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "  -l, --language <lang>   Language server to use:\n"
              << "                          rust, python, javascript, typescript,\n"
              << "                          go, cpp, c, swift\n"
              << "  -d, --database <path>   Path to SQLite database for caching\n"
              << "  -i, --include <pattern> Include file pattern (can be repeated)\n"
              << "  -e, --exclude <pattern> Exclude file pattern (can be repeated)\n"
              << "  -v, --verbose           Verbose output\n"
              << "\n"
              << "Example:\n"
              << "  " << programName << " -l rust -d ./index.db ./my-project\n"
              << "\n";
}

static GRCLanguage parseLanguage(const char* lang) {
    if (strcmp(lang, "rust") == 0) return GRC_LANG_RUST;
    if (strcmp(lang, "python") == 0) return GRC_LANG_PYTHON;
    if (strcmp(lang, "javascript") == 0) return GRC_LANG_JAVASCRIPT;
    if (strcmp(lang, "typescript") == 0) return GRC_LANG_TYPESCRIPT;
    if (strcmp(lang, "go") == 0) return GRC_LANG_GO;
    if (strcmp(lang, "cpp") == 0 || strcmp(lang, "c++") == 0) return GRC_LANG_CPP;
    if (strcmp(lang, "c") == 0) return GRC_LANG_C;
    if (strcmp(lang, "swift") == 0) return GRC_LANG_SWIFT;
    return GRC_LANG_UNKNOWN;
}

struct Options {
    std::string workspacePath;
    std::string databasePath;
    GRCLanguage language = GRC_LANG_UNKNOWN;
    std::vector<std::string> includePatterns;
    std::vector<std::string> excludePatterns;
    bool verbose = false;
};

static bool progressCallback(
    const char* filePath,
    int32_t current,
    int32_t total,
    void* context
) {
    bool verbose = *static_cast<bool*>(context);
    if (verbose) {
        std::cout << "[" << current << "/" << total << "] " << filePath << std::endl;
    } else {
        // Print progress bar
        int percent = (current * 100) / total;
        std::cout << "\rIndexing: " << percent << "% (" << current << "/" << total << ")";
        std::cout.flush();
        if (current == total) {
            std::cout << std::endl;
        }
    }
    return true; // Continue indexing
}

int main(int argc, char* argv[]) {
    Options options;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--language") == 0) {
            if (++i >= argc) {
                std::cerr << "Error: --language requires an argument\n";
                return 1;
            }
            options.language = parseLanguage(argv[i]);
            if (options.language == GRC_LANG_UNKNOWN) {
                std::cerr << "Error: Unknown language: " << argv[i] << "\n";
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--database") == 0) {
            if (++i >= argc) {
                std::cerr << "Error: --database requires an argument\n";
                return 1;
            }
            options.databasePath = argv[i];
            continue;
        }
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--include") == 0) {
            if (++i >= argc) {
                std::cerr << "Error: --include requires an argument\n";
                return 1;
            }
            options.includePatterns.push_back(argv[i]);
            continue;
        }
        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--exclude") == 0) {
            if (++i >= argc) {
                std::cerr << "Error: --exclude requires an argument\n";
                return 1;
            }
            options.excludePatterns.push_back(argv[i]);
            continue;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            options.verbose = true;
            continue;
        }
        if (argv[i][0] == '-') {
            std::cerr << "Error: Unknown option: " << argv[i] << "\n";
            return 1;
        }
        // Positional argument: workspace path
        options.workspacePath = argv[i];
    }

    // Validate arguments
    if (options.workspacePath.empty()) {
        std::cerr << "Error: workspace path is required\n";
        printUsage(argv[0]);
        return 1;
    }

    if (options.language == GRC_LANG_UNKNOWN) {
        std::cerr << "Error: --language is required\n";
        printUsage(argv[0]);
        return 1;
    }

    // Create cache if database path specified
    GRCLSPCache* cache = nullptr;
    if (!options.databasePath.empty()) {
        GRCError err;
        cache = grc_cache_create(options.databasePath.c_str(), &err);
        if (!cache) {
            std::cerr << "Error: Failed to create database: " << options.databasePath << "\n";
            return 1;
        }
        std::cout << "Using database: " << options.databasePath << "\n";
    }

    // Create indexer
    GRCError err;
    GRCIndexer* indexer = grc_indexer_create(
        options.workspacePath.c_str(),
        options.language,
        cache,
        &err
    );

    if (!indexer) {
        std::cerr << "Error: Failed to create indexer (error " << err << ")\n";
        if (cache) grc_cache_destroy(cache);
        return 1;
    }

    // Set include patterns
    if (!options.includePatterns.empty()) {
        std::vector<const char*> patterns;
        for (const auto& p : options.includePatterns) {
            patterns.push_back(p.c_str());
        }
        patterns.push_back(nullptr);
        grc_indexer_set_include_patterns(indexer, patterns.data());
    }

    // Set exclude patterns
    if (!options.excludePatterns.empty()) {
        std::vector<const char*> patterns;
        for (const auto& p : options.excludePatterns) {
            patterns.push_back(p.c_str());
        }
        patterns.push_back(nullptr);
        grc_indexer_set_exclude_patterns(indexer, patterns.data());
    }

    std::cout << "Indexing: " << options.workspacePath << "\n";

    // Run indexing
    int32_t filesIndexed = 0;
    int32_t definitionsFound = 0;
    int32_t referencesFound = 0;

    err = grc_indexer_run(
        indexer,
        progressCallback,
        &options.verbose,
        &filesIndexed,
        &definitionsFound,
        &referencesFound
    );

    if (err != GRC_SUCCESS) {
        std::cerr << "Error: Indexing failed (error " << err << ")\n";
        grc_indexer_destroy(indexer);
        if (cache) grc_cache_destroy(cache);
        return 1;
    }

    std::cout << "\n";
    std::cout << "Indexing complete!\n";
    std::cout << "  Files indexed:    " << filesIndexed << "\n";
    std::cout << "  Definitions:      " << definitionsFound << "\n";
    std::cout << "  References:       " << referencesFound << "\n";

    // Show some sample definitions if verbose
    if (options.verbose && definitionsFound > 0) {
        GRCIndexDefinitionArray definitions;
        err = grc_indexer_get_definitions(indexer, &definitions);
        if (err == GRC_SUCCESS) {
            std::cout << "\nSample definitions (first 10):\n";
            int count = std::min(static_cast<int32_t>(10), definitions.count);
            for (int i = 0; i < count; ++i) {
                const auto& def = definitions.definitions[i];
                std::cout << "  - " << def.name;
                if (def.container_name && strlen(def.container_name) > 0) {
                    std::cout << " (in " << def.container_name << ")";
                }
                std::cout << " at " << def.file_uri << ":"
                          << def.range.start.line << "\n";
            }
            grc_free_index_definitions(&definitions);
        }
    }

    // Cleanup
    grc_indexer_destroy(indexer);
    if (cache) grc_cache_destroy(cache);

    return 0;
}
