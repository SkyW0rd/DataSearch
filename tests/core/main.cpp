#include <exception>
#include <iostream>
#include <string>
#include <vector>

void runFileScannerTests();
void runIndexStorageTests();
void runIndexStorageContentSearchTests();
void runIndexStorageQueryOperatorTests();
void runSearchEngineTests();
void runRussianStemmerTests();
void runContentExtractorTests();
void runSourceRegistryTests();
void runIndexerReconcileTests();
void runIndexerUnavailableRootTests();
void runIndexerPauseResumeTests();
void runSearchPerformanceTests();
void runSearchQueryParserTests();

namespace {

struct TestCase {
    std::string name;
    void (*fn)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"FileScanner", runFileScannerTests},
        {"IndexStorage", runIndexStorageTests},
        {"IndexStorageContentSearch", runIndexStorageContentSearchTests},
        {"IndexStorageQueryOperators", runIndexStorageQueryOperatorTests},
        {"SearchEngine", runSearchEngineTests},
        {"RussianStemmer", runRussianStemmerTests},
        {"ContentExtractor", runContentExtractorTests},
        {"SourceRegistry", runSourceRegistryTests},
        {"IndexerReconcile", runIndexerReconcileTests},
        {"IndexerUnavailableRoot", runIndexerUnavailableRootTests},
        {"IndexerPauseResume", runIndexerPauseResumeTests},
        {"SearchPerformance", runSearchPerformanceTests},
        {"SearchQueryParser", runSearchQueryParserTests},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << test.name << ": " << e.what() << "\n";
            ++failures;
        }
    }

    if (failures > 0) {
        std::cout << failures << " test suite(s) failed.\n";
        return 1;
    }
    std::cout << "All test suites passed.\n";
    return 0;
}
