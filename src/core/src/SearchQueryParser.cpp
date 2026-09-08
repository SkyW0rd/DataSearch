#include "datasearch/core/SearchQueryParser.h"

#include <algorithm>
#include <cctype>

namespace datasearch::core {

namespace {

bool prefixMatchesCI(const std::string& s, std::size_t pos, const std::string& prefix) {
    if (pos + prefix.size() > s.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[pos + i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// Reads a filter value starting at `pos`: either a "quoted value" (stops at
// the closing quote) or a bareword (stops at the next whitespace). Returns
// the value and the position right after it.
std::pair<std::string, std::size_t> readFilterValue(const std::string& s, std::size_t pos) {
    if (pos < s.size() && s[pos] == '"') {
        const std::size_t start = pos + 1;
        std::size_t j = start;
        while (j < s.size() && s[j] != '"') ++j;
        const std::string value = s.substr(start, j - start);
        if (j < s.size()) ++j;  // skip closing quote
        return {value, j};
    }
    const std::size_t start = pos;
    std::size_t j = start;
    while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
    return {s.substr(start, j - start), j};
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string normalizeExtension(const std::string& raw) {
    std::string ext = toLower(raw);
    if (!ext.empty() && ext.front() != '.') ext = "." + ext;
    return ext;
}

} // namespace

ParsedSearchQuery parseSearchQuery(const std::string& raw) {
    ParsedSearchQuery result;
    const std::size_t n = raw.size();
    std::size_t i = 0;

    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i >= n) break;

        bool excluded = false;
        if (raw[i] == '-' && i + 1 < n && !std::isspace(static_cast<unsigned char>(raw[i + 1]))) {
            excluded = true;
            ++i;
        }

        if (i < n && raw[i] == '"') {
            const std::size_t start = i + 1;
            std::size_t j = start;
            while (j < n && raw[j] != '"') ++j;
            const std::string phrase = raw.substr(start, j - start);
            i = (j < n) ? j + 1 : j;
            if (!phrase.empty()) {
                result.tokens.push_back({phrase, /*isPhrase=*/true, excluded});
            }
            continue;
        }

        if (!excluded && prefixMatchesCI(raw, i, "ext:")) {
            auto [value, next] = readFilterValue(raw, i + 4);
            i = next;
            if (!value.empty() && !result.extensionFilter) {
                result.extensionFilter = normalizeExtension(value);
            }
            continue;
        }

        if (!excluded && prefixMatchesCI(raw, i, "path:")) {
            auto [value, next] = readFilterValue(raw, i + 5);
            i = next;
            if (!value.empty() && !result.pathFilter) {
                result.pathFilter = value;
            }
            continue;
        }

        const std::size_t start = i;
        while (i < n && !std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        const std::string word = raw.substr(start, i - start);
        if (!word.empty() && word != "-") {
            result.tokens.push_back({word, /*isPhrase=*/false, excluded});
        }
    }

    return result;
}

} // namespace datasearch::core
