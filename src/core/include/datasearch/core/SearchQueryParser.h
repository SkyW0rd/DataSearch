#pragma once

#include <optional>
#include <string>
#include <vector>

namespace datasearch::core {

// The result of parsing a raw search box string into structured pieces
// (ТЗ FR-13): free-text terms/phrases (each optionally negated), plus the
// ext:/path: filters.
struct ParsedSearchQuery {
    struct Token {
        std::string text;
        bool isPhrase = false;  // came from "double quotes": exact phrase, not prefix-matched
        bool excluded = false;  // preceded by '-': must NOT match
    };

    std::vector<Token> tokens;

    // Normalized to lowercase with a leading dot (ext:docx -> ".docx"), or
    // nullopt if the query has no ext: filter.
    std::optional<std::string> extensionFilter;

    // As typed after path: (quotes around the value are stripped), or
    // nullopt if the query has no path: filter. Matched as a case-insensitive
    // substring of the full path at query time.
    std::optional<std::string> pathFilter;
};

// Parses a raw search-box string into its structured pieces:
//   - bareword terms (implicitly ANDed, prefix-matched: практик -> практик*)
//   - "quoted phrases" (implicitly ANDed, matched exactly, not prefix-matched)
//   - -excluded or -"excluded phrase" (must NOT match)
//   - ext:docx (extension filter; ext:"docx" / ext:.docx also accepted)
//   - path:D:\Work\ or path:"D:\My Files\" (path substring filter)
// Only the first ext: and first path: filter in the string take effect;
// repeats are ignored rather than rejected.
ParsedSearchQuery parseSearchQuery(const std::string& raw);

} // namespace datasearch::core
