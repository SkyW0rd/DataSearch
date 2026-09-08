#pragma once

#include <filesystem>
#include <string>

namespace datasearch::core {

// std::filesystem::path::string() converts through the *system ANSI codepage*
// on Windows, not UTF-8 — silently corrupting Cyrillic (and any non-ASCII)
// filenames, which ТЗ п.11.3 requires to survive intact end-to-end. These two
// functions are the one correct way to move between std::filesystem::path and
// the UTF-8 std::string used everywhere else in this codebase (index storage,
// search results, UI). On POSIX, native paths already are UTF-8 narrow
// strings, so both functions are effectively free.
std::string pathToUtf8(const std::filesystem::path& path);
std::filesystem::path pathFromUtf8(const std::string& utf8);

} // namespace datasearch::core
