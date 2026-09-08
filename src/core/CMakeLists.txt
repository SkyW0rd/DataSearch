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

# CMake's bundled FindSQLite3 module has named its imported target
# differently across CMake versions (SQLite::SQLite3 vs SQLite3::SQLite3,
# and older versions define no imported target at all — only the
# SQLite3_INCLUDE_DIRS/SQLite3_LIBRARIES variables). Rather than hardcode one
# name and break on whichever CI image doesn't match, detect what's actually
# available and wrap it in a target of our own.
if(TARGET SQLite3::SQLite3)
  set(DATASEARCH_SQLITE3_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
  set(DATASEARCH_SQLITE3_TARGET SQLite::SQLite3)
else()
  add_library(datasearch_sqlite3_compat INTERFACE)
  target_include_directories(datasearch_sqlite3_compat INTERFACE ${SQLite3_INCLUDE_DIRS})
  target_link_libraries(datasearch_sqlite3_compat INTERFACE ${SQLite3_LIBRARIES})
  set(DATASEARCH_SQLITE3_TARGET datasearch_sqlite3_compat)
endif()

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

target_link_libraries(datasearch_core PUBLIC ${DATASEARCH_SQLITE3_TARGET} ZLIB::ZLIB)

find_package(Threads REQUIRED)
target_link_libraries(datasearch_core PUBLIC Threads::Threads)
