add_executable(datasearch_core_tests
  core/main.cpp
  core/TestFileScanner.cpp
  core/TestIndexStorage.cpp
  core/TestSearchEngine.cpp
  core/TestRussianStemmer.cpp
  core/TestContentExtractor.cpp
  core/TestSourceRegistry.cpp
  core/TestIndexerReconcile.cpp
  core/TestSearchQueryParser.cpp
)

target_link_libraries(datasearch_core_tests PRIVATE datasearch_core)

add_test(NAME datasearch_core_tests COMMAND datasearch_core_tests)
