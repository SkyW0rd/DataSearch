#include "datasearch/core/NaturalOrder.h"

#include <sqlite3.h>

#include <cstdint>
#include <iterator>

namespace datasearch::core {

namespace {

bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
bool isSeparator(char c) { return c == '/' || c == '\\'; }
bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// Next code point at `i` (advancing `i`); invalid bytes decode as themselves.
std::uint32_t nextCodePoint(std::string_view s, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    int extra = b0 >= 0xF0 ? 3 : b0 >= 0xE0 ? 2 : b0 >= 0xC0 ? 1 : 0;
    if (i + extra >= s.size()) extra = 0;
    std::uint32_t cp = extra == 0 ? b0 : b0 & (0x3F >> extra);
    for (int k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += 1 + extra;
    return cp;
}

// The sort key of one character: lowercase, ё as е, separators lowest.
std::uint32_t sortKey(std::uint32_t cp) {
    if (cp == '/' || cp == '\\') return 0;
    if (cp >= 'A' && cp <= 'Z') return cp + 0x20;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;           // А-Я
    if (cp == 0x0401 || cp == 0x0451) return 0x0435;               // Ё, ё -> е
    if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;           // Ѐ-Џ
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;  // Latin-1 capitals
    return cp;
}

int sign(int v) { return (v > 0) - (v < 0); }

// compareNatural() without the byte tie-break.
int naturalOnly(std::string_view a, std::string_view b) {
    // Identical leading bytes compare equal however they're read — skip
    // them (paths share long folder prefixes), then step back to where a
    // character and a run of digits begin.
    std::size_t k = 0;
    while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
    while (k > 0 && ((k < a.size() && isContinuation(a[k])) || (k < b.size() && isContinuation(b[k])) ||
                     isDigit(static_cast<unsigned char>(a[k - 1])))) {
        --k;
    }

    std::size_t i = k, j = k;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            std::size_t endA = i, endB = j;
            while (endA < a.size() && isDigit(a[endA])) ++endA;
            while (endB < b.size() && isDigit(b[endB])) ++endB;
            while (i + 1 < endA && a[i] == '0') ++i;  // leading zeros don't change the value
            while (j + 1 < endB && b[j] == '0') ++j;
            if (endA - i != endB - j) return endA - i < endB - j ? -1 : 1;
            if (const int c = a.substr(i, endA - i).compare(b.substr(j, endB - j))) return sign(c);
            i = endA;
            j = endB;
            continue;
        }
        const std::uint32_t ka = sortKey(nextCodePoint(a, i));
        const std::uint32_t kb = sortKey(nextCodePoint(b, j));
        if (ka != kb) return ka < kb ? -1 : 1;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

const PathOrder kCollationOrders[] = {{true, false}, {true, true}, {false, false}, {false, true}};
const char* const kCollationNames[] = {"ds_path", "ds_path_desc", "ds_path_files", "ds_path_files_desc"};

int collatePaths(void* order, int lengthA, const void* a, int lengthB, const void* b) {
    return comparePaths(std::string_view(static_cast<const char*>(a), static_cast<std::size_t>(lengthA)),
                        std::string_view(static_cast<const char*>(b), static_cast<std::size_t>(lengthB)),
                        *static_cast<const PathOrder*>(order));
}

} // namespace

int compareNatural(std::string_view a, std::string_view b) {
    if (const int c = naturalOnly(a, b)) return c;
    return sign(a.compare(b));
}

int comparePaths(std::string_view a, std::string_view b, PathOrder order) {
    const int direction = order.descending ? -1 : 1;
    std::size_t i = 0, j = 0;
    while (true) {
        std::size_t endA = i;
        while (endA < a.size() && !isSeparator(a[endA])) ++endA;
        std::size_t endB = j;
        while (endB < b.size() && !isSeparator(b[endB])) ++endB;
        // At this level one path names its file and the other a subfolder.
        const bool fileA = endA == a.size();
        const bool fileB = endB == b.size();
        if (fileA != fileB) return fileA == order.foldersFirst ? 1 : -1;
        if (const int c = naturalOnly(a.substr(i, endA - i), b.substr(j, endB - j))) return c * direction;
        if (fileA) break;
        i = endA + 1;
        j = endB + 1;
    }
    return sign(a.compare(b)) * direction;
}

const char* pathCollation(PathOrder order) {
    for (std::size_t k = 0; k < std::size(kCollationOrders); ++k) {
        if (kCollationOrders[k].foldersFirst == order.foldersFirst &&
            kCollationOrders[k].descending == order.descending) {
            return kCollationNames[k];
        }
    }
    return kCollationNames[0];
}

bool registerPathCollations(sqlite3* db) {
    for (std::size_t k = 0; k < std::size(kCollationOrders); ++k) {
        // SQLite keeps the pointer; the orders are static.
        void* order = const_cast<PathOrder*>(&kCollationOrders[k]);
        if (sqlite3_create_collation_v2(db, kCollationNames[k], SQLITE_UTF8, order, collatePaths, nullptr) !=
            SQLITE_OK) {
            return false;
        }
    }
    return true;
}

} // namespace datasearch::core
