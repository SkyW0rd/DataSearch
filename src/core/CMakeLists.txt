if(APPLE)
  execute_process(
    COMMAND brew --prefix sqlite3
    OUTPUT_VARIABLE HOMEBREW_SQLITE3_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(HOMEBREW_SQLITE3_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${HOMEBREW_SQLITE3_PREFIX}")
    set(SQLite3_INCLUDE_DIR "${HOMEBREW_SQLITE3_PREFIX}/include")
    set(SQLite3_LIBRARY "${HOMEBREW_SQLITE3_PREFIX}/lib/libsqlite3.dylib")
  endif()
endif()

find_package(SQLite3 REQUIRED)
find_package(ZLIB REQUIRED)

add_library(datasearch_core STATIC
  src/FileScanner.cpp
  src/IndexStorage.cpp
  src/Indexer.cpp
  src/SearchEngine.cpp
  src/Utf8.cpp
  src/ContentExtractor.cpp
  src/RussianStemmer.cpp
  src/Fts5RussianTokenizer.cpp
  src/SourceRegistry.cpp
  src/SearchQueryParser.cpp
)

target_include_directories(datasearch_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(datasearch_core PUBLIC SQLite3::SQLite3 ZLIB::ZLIB)

find_package(Threads REQUIRED)
target_link_libraries(datasearch_core PUBLIC Threads::Threads)
