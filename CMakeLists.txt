cmake_minimum_required(VERSION 3.21)
project(DataSearch CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Source files contain literal Cyrillic text (UI strings, the Russian Snowball
# stemmer's suffix tables). Without this, MSVC assumes the system ANSI codepage
# for both reading the source and encoding narrow string-literal bytes, which
# silently corrupts that text; /utf-8 makes MSVC treat both source and
# execution charset as UTF-8, matching Clang/GCC's default behaviour.
if(MSVC)
  add_compile_options(/utf-8)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(DATASEARCH_BUILD_TESTS "Build unit tests for the platform-independent core" ON)
option(DATASEARCH_BUILD_APP "Build the Qt desktop application (Windows only)" ON)

# Platform-independent library: filesystem scanning, SQLite-backed index, search.
# Builds and is testable on any OS (macOS during development, Windows for release).
add_subdirectory(src/core)

# IPlatformService interface + OS-specific implementation.
# Per ТЗ п.9/12.1: only the Windows implementation is part of the deliverable for
# stage 1 — no macOS/Linux stub. The interface headers still parse everywhere so
# code that only depends on the interface can be written/reviewed on any OS.
add_subdirectory(src/platform)

if(DATASEARCH_BUILD_APP AND WIN32)
  add_subdirectory(src/app)
elseif(DATASEARCH_BUILD_APP)
  message(STATUS "DataSearch app: skipped (Windows-only, see docs/TZ_DataSearch.md п.9/12.1). Building core only on this platform.")
endif()

if(DATASEARCH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
