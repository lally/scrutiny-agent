// Unit tests for db/LSPCache: one SQLite connection per repository
// that every language client, the indexer and each async wrapper call
// from whatever thread they happen to run on. The cache must therefore
// serialize its own access -- the Mac's libsqlite3 is compiled in
// multi-thread mode and aborts the process on a connection used from
// two threads at once ("BUG IN CLIENT OF libsqlite3.dylib: illegal
// multi-threaded access to database connection"), and even a
// serialized sqlite would still let one thread's reads observe another
// thread's half-written transaction on a shared connection.
#include "db/LSPCache.hpp"
#include "lsp/LSPTypes.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace gitreview::db;

namespace {
int g_failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::cerr << "FAIL " << __FUNCTION__ << ": " << msg << std::endl;   \
        }                                                                       \
    } while (0)

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("lspcache-" + std::to_string(::getpid()) + "-" +
                                            std::to_string(std::rand()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

constexpr int kSymbolsPerFile = 40;

std::vector<CachedDocumentSymbol> symbolsFor(const std::string& file, int generation) {
    std::vector<CachedDocumentSymbol> out;
    for (int i = 0; i < kSymbolsPerFile; ++i) {
        CachedDocumentSymbol s;
        s.filePath = file;
        s.name = "sym" + std::to_string(i) + "-g" + std::to_string(generation);
        s.kind = "12";
        s.line = i;
        s.endLine = i;
        out.push_back(s);
    }
    return out;
}

// Writers rewrite each file's outline while readers read the same
// files and an "indexer" thread streams index rows into the same
// connection. Every outline read must see either nothing or a whole
// generation: a partial set means a reader ran inside a writer's
// transaction, i.e. the connection was shared between threads.
void testConcurrentReadersWritersAndIndexerSeeWholeOutlinesOnly() {
    TempDir t;
    LSPCache cache((t.path / "lsp_cache.db").string());
    CHECK(cache.isOpen(), "cache opens");
    cache.setCurrentGitSha("deadbeef");

    const int kFiles = 4;
    const int kRounds = 150;
    std::vector<std::string> files;
    for (int f = 0; f < kFiles; ++f) {
        files.push_back("file:///repo/src/File" + std::to_string(f) + ".swift");
    }
    for (const auto& f : files) cache.cacheDocumentSymbols(f, symbolsFor(f, 0));

    std::atomic<int> partialReads{0};
    std::atomic<int> nonEmptyReads{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    for (int w = 0; w < kFiles; ++w) {
        threads.emplace_back([&, w] {
            for (int g = 1; g <= kRounds; ++g) {
                cache.cacheDocumentSymbols(files[w], symbolsFor(files[w], g));
                cache.cacheFoldingRanges(files[w], {});
                (void)cache.isCacheValid(files[w]);
            }
        });
    }
    for (int r = 0; r < kFiles; ++r) {
        threads.emplace_back([&, r] {
            while (!stop.load()) {
                auto got = cache.getCachedDocumentSymbols(files[r]);
                if (!got) continue;
                ++nonEmptyReads;
                if (got->size() != static_cast<size_t>(kSymbolsPerFile)) {
                    if (partialReads.fetch_add(1) < 5) {
                        std::cerr << "partial read of " << files[r] << ": " << got->size()
                                  << " rows, first=" << got->front().name
                                  << " last=" << got->back().name << std::endl;
                    }
                }
                (void)cache.getCachedFoldingRanges(files[r]);
                (void)cache.getCachedHover(files[r], 1, 1);
            }
        });
    }
    threads.emplace_back([&] {
        int n = 0;
        while (!stop.load()) {
            LSPCache::IndexedDefinition def;
            def.name = "indexed" + std::to_string(n);
            def.fileUri = files[n % kFiles];
            def.kind = 12;
            cache.storeIndexDefinition(def);
            LSPCache::IndexedReference ref;
            ref.symbolName = def.name;
            ref.fromFileUri = files[(n + 1) % kFiles];
            ref.toFileUri = def.fileUri;
            cache.storeIndexReference(ref);
            cache.markFileIndexed(def.fileUri);
            (void)cache.isFileIndexed(def.fileUri);
            (void)cache.getIndexDefinitionsForFile(def.fileUri);
            (void)cache.getStats();
            (void)cache.pruneOlderThan(30);
            ++n;
        }
    });

    for (int w = 0; w < kFiles; ++w) threads[w].join();
    stop.store(true);
    for (size_t i = kFiles; i < threads.size(); ++i) threads[i].join();

    CHECK(nonEmptyReads.load() > 0, "readers observed data");
    CHECK(partialReads.load() == 0,
          "every outline read was whole; partial reads: " + std::to_string(partialReads.load()));
    for (const auto& f : files) {
        auto got = cache.getCachedDocumentSymbols(f);
        CHECK(got && got->size() == static_cast<size_t>(kSymbolsPerFile),
              "final outline for " + f + " is one whole generation");
        if (got) {
            CHECK(got->front().name == "sym0-g" + std::to_string(kRounds),
                  "final outline is the last generation written");
        }
    }
    auto stats = cache.getIndexStats();
    CHECK(stats.definitionCount > 0 && stats.definitionCount == stats.referenceCount,
          "every index definition landed with its reference");
}

// The git SHA is written by the Swift side (HEAD watcher) while
// queries read it; without serialization that is a data race on a
// std::string. With it, every query runs against one SHA or the other
// and a write under the new SHA is visible under that SHA alone.
void testGitShaChangesAreSerializedWithQueries() {
    TempDir t;
    LSPCache cache((t.path / "lsp_cache.db").string());
    const std::string file = "file:///repo/a.cpp";
    cache.setCurrentGitSha("sha-a");
    cache.cacheDocumentSymbols(file, symbolsFor(file, 0));

    std::atomic<bool> stop{false};
    std::thread flipper([&] {
        for (int i = 0; i < 400; ++i) {
            cache.setCurrentGitSha(i % 2 ? "sha-b" : "sha-a");
        }
        stop.store(true);
    });
    std::thread reader([&] {
        while (!stop.load()) {
            auto got = cache.getCachedDocumentSymbols(file);
            if (got) {
                CHECK(got->size() == static_cast<size_t>(kSymbolsPerFile), "whole outline under sha-a");
            }
            (void)cache.currentGitSha();
        }
    });
    flipper.join();
    reader.join();

    cache.setCurrentGitSha("sha-b");
    CHECK(!cache.getCachedDocumentSymbols(file), "nothing cached under sha-b");
    cache.setCurrentGitSha("sha-a");
    CHECK(cache.getCachedDocumentSymbols(file).has_value(), "sha-a outline survives");
}

// Every public method takes the cache's (non-recursive) lock, so a
// public method calling another would deadlock. clearAll used to call
// vacuum(); exercise the composite methods so a regression hangs the
// test instead of the app.
void testCompositeMaintenanceMethodsDoNotSelfDeadlock() {
    TempDir t;
    LSPCache cache((t.path / "lsp_cache.db").string());
    const std::string file = "file:///repo/b.cpp";
    cache.setCurrentGitSha("sha");
    cache.cacheDocumentSymbols(file, symbolsFor(file, 0));
    CHECK(cache.isCacheValid(file), "cached before clear");
    cache.clearAll();
    CHECK(!cache.getCachedDocumentSymbols(file), "outline gone after clearAll");
    CHECK(!cache.isCacheValid(file), "status gone after clearAll");
    cache.cacheDocumentSymbols(file, symbolsFor(file, 1));
    cache.invalidateFile(file);
    CHECK(!cache.isCacheValid(file), "invalidateFile clears status");
    cache.cacheDocumentSymbols(file, symbolsFor(file, 2));
    CHECK(cache.pruneOlderThan(30) == 0, "fresh rows survive a 30-day prune");
    CHECK(cache.pruneOlderThan(0) > 0, "prune(0) force-evicts");
    CHECK(!cache.getCachedDocumentSymbols(file), "outline gone after prune(0)");
    cache.clearIndex();
    cache.vacuum();
    CHECK(cache.getStats().symbolCount == 0, "stats readable after maintenance");
}

// invalidateFile runs on every didChange. Its old single WHERE clause
// named columns from every table at once, so no data table's
// statement ever prepared and only file_cache_status was cleared --
// stale definitions, hovers and outlines survived every edit.
void testInvalidateFileRemovesEveryOnDemandRowForThatFileOnly() {
    TempDir t;
    LSPCache cache((t.path / "lsp_cache.db").string());
    cache.setCurrentGitSha("sha");
    const std::string a = "file:///repo/a.cpp";
    const std::string b = "file:///repo/b.cpp";

    gitreview::lsp::LSPLocation inA; inA.uri = a; inA.range.start.line = 3; inA.range.end.line = 3;
    gitreview::lsp::LSPLocation inB; inB.uri = b; inB.range.start.line = 7; inB.range.end.line = 7;
    for (const auto& f : {a, b}) {
        const auto& self = (f == a) ? inA : inB;
        cache.cacheDefinition(f, 1, 1, self);
        cache.cacheReferences(f, 1, 1, {inA, inB});
        cache.cacheHover(f, 1, 1, "docs for " + f);
        cache.cacheDocumentSymbols(f, symbolsFor(f, 0));
        CachedFoldingRange fr; fr.filePath = f; fr.startLine = 1; fr.endLine = 2; fr.kind = "region";
        cache.cacheFoldingRanges(f, {fr});
    }
    // b also defines something located in a: that row goes with a too.
    cache.cacheDefinition(b, 9, 9, inA);
    CachedSymbol sa; sa.filePath = a; sa.name = "fromA"; sa.kind = "12";
    CachedSymbol sb; sb.filePath = b; sb.name = "fromB"; sb.kind = "12";
    cache.cacheWorkspaceSymbols("mixed", {sa, sb});
    cache.cacheWorkspaceSymbols("onlyB", {sb});
    CHECK(cache.getCachedDefinitions(a, 1, 1).has_value(), "precondition: a cached");
    CHECK(cache.getCachedDefinitions(b, 9, 9).has_value(), "precondition: b->a cached");

    cache.invalidateFile(a);

    CHECK(!cache.getCachedDefinitions(a, 1, 1), "a's definition gone");
    CHECK(!cache.getCachedDefinitions(b, 9, 9), "definition targeting a gone");
    CHECK(!cache.getCachedReferences(a, 1, 1), "a's references gone");
    CHECK(!cache.getCachedHover(a, 1, 1), "a's hover gone");
    CHECK(!cache.getCachedDocumentSymbols(a), "a's outline gone");
    CHECK(!cache.getCachedFoldingRanges(a), "a's folding ranges gone");
    CHECK(!cache.isCacheValid(a), "a's status gone");
    CHECK(!cache.getCachedWorkspaceSymbols("mixed"), "query that included a re-asks");

    CHECK(cache.getCachedDefinitions(b, 1, 1).has_value(), "b's own definition kept");
    auto refsB = cache.getCachedReferences(b, 1, 1);
    CHECK(!refsB.has_value(), "b's references mentioned a and are dropped as a unit");
    CHECK(cache.getCachedHover(b, 1, 1).has_value(), "b's hover kept");
    CHECK(cache.getCachedDocumentSymbols(b).has_value(), "b's outline kept");
    CHECK(cache.getCachedFoldingRanges(b).has_value(), "b's folding ranges kept");
    CHECK(cache.isCacheValid(b), "b's status kept");
    CHECK(cache.getCachedWorkspaceSymbols("onlyB").has_value(), "query without a kept");
}

} // namespace

int main() {
    testInvalidateFileRemovesEveryOnDemandRowForThatFileOnly();
    testConcurrentReadersWritersAndIndexerSeeWholeOutlinesOnly();
    testGitShaChangesAreSerializedWithQueries();
    testCompositeMaintenanceMethodsDoNotSelfDeadlock();
    if (g_failures) {
        std::cerr << g_failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "lsp_cache_tests: all passed" << std::endl;
    return 0;
}
