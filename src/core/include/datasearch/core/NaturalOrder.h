#pragma once

#include <string_view>

struct sqlite3;

namespace datasearch::core {

// File-list order as a person expects it (much like Windows Explorer):
// case-insensitive for Latin and Cyrillic, ё sorted as е, runs of digits
// compared by value ("Договор 2" before "Договор 10"). Strings that differ
// only in what this ignores are ordered by their bytes, so this is a total
// order. Both return <0, 0 or >0.
int compareNatural(std::string_view a, std::string_view b);

// How comparePaths() lays out a folder tree.
struct PathOrder {
    // true: inside each folder, its subfolders (with everything in them)
    // come before its own files, as in Explorer; false: its files first.
    bool foldersFirst = true;
    // Names from Я to А (and numbers from high to low) at every level; the
    // folders-before-files grouping stays as it is.
    bool descending = false;
};

// Paths grouped by folder: compared folder by folder, each folder's
// contents kept together. '/' and '\' both separate folders.
int comparePaths(std::string_view a, std::string_view b, PathOrder order = {});

// The SQLite collation that sorts by comparePaths() with `order` (always
// used with ORDER BY ... ASC), registered by registerPathCollations().
const char* pathCollation(PathOrder order);
bool registerPathCollations(sqlite3* db);

} // namespace datasearch::core
