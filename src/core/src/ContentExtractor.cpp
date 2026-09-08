#include "datasearch/core/ContentExtractor.h"

#include <zlib.h>

#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace datasearch::core {

namespace {

// ---------------------------------------------------------------------------
// Shared utilities: UTF-8 encoding, XML entity decoding, zlib/deflate inflate.
// ---------------------------------------------------------------------------

void appendUtf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string decodeXmlEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            const std::size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 10) {
                const std::string entity = s.substr(i + 1, semi - i - 1);
                if (entity == "amp") { out += '&'; i = semi + 1; continue; }
                if (entity == "lt") { out += '<'; i = semi + 1; continue; }
                if (entity == "gt") { out += '>'; i = semi + 1; continue; }
                if (entity == "quot") { out += '"'; i = semi + 1; continue; }
                if (entity == "apos") { out += '\''; i = semi + 1; continue; }
                if (!entity.empty() && entity[0] == '#') {
                    const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                    const long code = std::strtol(entity.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
                    if (code > 0) appendUtf8(out, static_cast<std::uint32_t>(code));
                    i = semi + 1;
                    continue;
                }
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

// Grows an output buffer while inflating `size` bytes at `data`. `rawDeflate`
// selects raw DEFLATE (ZIP entries) vs zlib-wrapped DEFLATE (PDF FlateDecode).
// Lenient on purpose: a truncated/odd stream still yields whatever text was
// successfully decoded rather than failing the whole extraction outright.
std::optional<std::string> inflateToString(const unsigned char* data, std::size_t size, bool rawDeflate) {
    if (size == 0) return std::string{};

    z_stream strm{};
    const int initResult = rawDeflate ? inflateInit2(&strm, -15) : inflateInit(&strm);
    if (initResult != Z_OK) return std::nullopt;

    strm.next_in = const_cast<Bytef*>(data);
    strm.avail_in = static_cast<uInt>(size);

    std::string out;
    std::array<char, 64 * 1024> chunk{};
    int ret = Z_OK;
    while (true) {
        strm.next_out = reinterpret_cast<Bytef*>(chunk.data());
        strm.avail_out = static_cast<uInt>(chunk.size());
        ret = inflate(&strm, Z_NO_FLUSH);
        out.append(chunk.data(), chunk.size() - strm.avail_out);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) break;
        if (strm.avail_in == 0 && strm.avail_out != 0) break;
    }
    inflateEnd(&strm);

    if (out.empty()) return std::nullopt;
    return out;
}

// ---------------------------------------------------------------------------
// Minimal ZIP reader (DOCX/XLSX are ZIP containers) — reads the central
// directory and decompresses individual entries (stored or DEFLATE). No
// ZIP64 support: irrelevant for the small Office files this targets.
// ---------------------------------------------------------------------------

std::uint16_t readU16(const std::vector<std::uint8_t>& d, std::size_t off) {
    return static_cast<std::uint16_t>(d[off] | (d[off + 1] << 8));
}
std::uint32_t readU32(const std::vector<std::uint8_t>& d, std::size_t off) {
    return static_cast<std::uint32_t>(d[off]) | (static_cast<std::uint32_t>(d[off + 1]) << 8) |
           (static_cast<std::uint32_t>(d[off + 2]) << 16) | (static_cast<std::uint32_t>(d[off + 3]) << 24);
}

struct ZipEntry {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t localHeaderOffset = 0;
};

class ZipArchive {
public:
    bool open(const std::vector<std::uint8_t>& data) {
        data_ = &data;
        if (data.size() < 22) return false;

        const std::size_t searchStart = data.size() >= 22 + 65536 ? data.size() - 22 - 65536 : 0;
        std::size_t eocd = std::string::npos;
        std::size_t i = data.size() - 22;
        while (true) {
            if (readU32(data, i) == 0x06054b50u) { eocd = i; break; }
            if (i == searchStart) break;
            --i;
        }
        if (eocd == std::string::npos) return false;

        const std::uint16_t totalEntries = readU16(data, eocd + 10);
        const std::uint32_t centralDirOffset = readU32(data, eocd + 16);

        std::size_t pos = centralDirOffset;
        for (std::uint16_t n = 0; n < totalEntries; ++n) {
            if (pos + 46 > data.size() || readU32(data, pos) != 0x02014b50u) return false;

            ZipEntry entry;
            entry.method = readU16(data, pos + 10);
            const std::uint32_t compressedSize = readU32(data, pos + 20);
            entry.compressedSize = compressedSize;
            const std::uint16_t nameLen = readU16(data, pos + 28);
            const std::uint16_t extraLen = readU16(data, pos + 30);
            const std::uint16_t commentLen = readU16(data, pos + 32);
            entry.localHeaderOffset = readU32(data, pos + 42);

            if (pos + 46 + nameLen > data.size()) return false;
            entry.name.assign(reinterpret_cast<const char*>(data.data() + pos + 46), nameLen);

            entries_.push_back(std::move(entry));
            pos += 46 + nameLen + extraLen + commentLen;
        }
        return true;
    }

    std::vector<std::string> entryNames() const {
        std::vector<std::string> names;
        names.reserve(entries_.size());
        for (const auto& e : entries_) names.push_back(e.name);
        return names;
    }

    std::optional<std::string> readEntry(const std::string& name) const {
        if (data_ == nullptr) return std::nullopt;
        const ZipEntry* entry = nullptr;
        for (const auto& e : entries_) {
            if (e.name == name) { entry = &e; break; }
        }
        if (entry == nullptr) return std::nullopt;

        const auto& data = *data_;
        const std::size_t lh = entry->localHeaderOffset;
        if (lh + 30 > data.size() || readU32(data, lh) != 0x04034b50u) return std::nullopt;

        const std::uint16_t nameLen = readU16(data, lh + 26);
        const std::uint16_t extraLen = readU16(data, lh + 28);
        const std::size_t dataStart = lh + 30 + nameLen + extraLen;
        if (dataStart + entry->compressedSize > data.size()) return std::nullopt;

        const unsigned char* bytes = data.data() + dataStart;
        if (entry->method == 0) {
            return std::string(reinterpret_cast<const char*>(bytes), entry->compressedSize);
        }
        if (entry->method == 8) {
            return inflateToString(bytes, entry->compressedSize, /*rawDeflate=*/true);
        }
        return std::nullopt;
    }

private:
    const std::vector<std::uint8_t>* data_ = nullptr;
    std::vector<ZipEntry> entries_;
};

std::optional<std::vector<std::uint8_t>> readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0) return std::nullopt;
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!buffer.empty()) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    }
    return buffer;
}

// Finds the next occurrence of any of several fixed patterns in `haystack`,
// caching each pattern's last-found position so a pattern that's rare (or
// absent) in the remainder of a large document is only searched for once
// per "leg" instead of being rescanned to the end on every single call.
// Without this, a loop that calls plain std::string::find for N patterns on
// every one of M matches degrades to O(N*len) per call whenever a pattern
// doesn't occur again — O(len²) overall on a large real-world document (this
// is exactly what made extraction hang on some real .docx/.xlsx files: any
// tag that's rare in one particular document, like <w:tab> or a bare <c>
// with no attributes, turned every remaining iteration into a full rescan
// of the rest of the file).
class MultiFind {
public:
    MultiFind(const std::string& haystack, std::initializer_list<std::string> patterns) : haystack_(haystack) {
        for (const auto& p : patterns) cursors_.push_back({p, haystack_.find(p, 0)});
    }

    // Returns the index into the original pattern list and the position of
    // the earliest match at or after `pos`, or {-1, npos} if none remain.
    std::pair<int, std::size_t> next(std::size_t pos) {
        int bestIdx = -1;
        std::size_t bestPos = std::string::npos;
        for (std::size_t i = 0; i < cursors_.size(); ++i) {
            Cursor& c = cursors_[i];
            if (c.pos != std::string::npos && c.pos < pos) {
                c.pos = haystack_.find(c.pattern, pos);
            }
            if (c.pos != std::string::npos && (bestIdx == -1 || c.pos < bestPos)) {
                bestIdx = static_cast<int>(i);
                bestPos = c.pos;
            }
        }
        return {bestIdx, bestPos};
    }

private:
    struct Cursor {
        std::string pattern;
        std::size_t pos;
    };
    const std::string& haystack_;
    std::vector<Cursor> cursors_;
};

// ---------------------------------------------------------------------------
// DOCX: text runs from word/document.xml.
// ---------------------------------------------------------------------------

std::string extractDocxBody(const std::string& xml) {
    std::string out;
    MultiFind finder(xml, {"<w:t", "</w:p>", "<w:tab", "<w:br"});
    std::size_t pos = 0;
    while (pos < xml.size()) {
        const auto [idx, next] = finder.next(pos);
        if (idx == -1) break;

        if (idx == 0) {  // <w:t
            const std::size_t gt = xml.find('>', next);
            if (gt == std::string::npos) break;
            const bool selfClosing = gt > 0 && xml[gt - 1] == '/';
            if (selfClosing) { pos = gt + 1; continue; }
            const std::size_t closeTag = xml.find("</w:t>", gt + 1);
            if (closeTag == std::string::npos) { pos = gt + 1; continue; }
            out += decodeXmlEntities(xml.substr(gt + 1, closeTag - gt - 1));
            pos = closeTag + 6;
        } else if (idx == 1) {  // </w:p>
            out += '\n';
            pos = next + 6;
        } else if (idx == 2) {  // <w:tab
            out += '\t';
            const std::size_t gt = xml.find('>', next);
            pos = (gt == std::string::npos) ? next + 6 : gt + 1;
        } else {  // <w:br
            out += '\n';
            const std::size_t gt = xml.find('>', next);
            pos = (gt == std::string::npos) ? next + 5 : gt + 1;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// XLSX: shared strings + per-sheet cell text.
// ---------------------------------------------------------------------------

std::vector<std::string> parseSharedStrings(const std::string& xml) {
    std::vector<std::string> result;
    MultiFind finder(xml, {"<si>", "<si "});
    std::size_t pos = 0;
    while (true) {
        const auto [idx, start] = finder.next(pos);
        if (idx == -1) break;
        const std::size_t gt = xml.find('>', start);
        const std::size_t siEnd = xml.find("</si>", gt);
        if (gt == std::string::npos || siEnd == std::string::npos) break;

        const std::string block = xml.substr(gt + 1, siEnd - gt - 1);
        std::string text;
        std::size_t p = 0;
        while (true) {
            const std::size_t tPos = block.find("<t", p);
            if (tPos == std::string::npos) break;
            const std::size_t tGt = block.find('>', tPos);
            if (tGt == std::string::npos) break;
            const bool selfClosing = tGt > 0 && block[tGt - 1] == '/';
            if (selfClosing) { p = tGt + 1; continue; }
            const std::size_t tClose = block.find("</t>", tGt + 1);
            if (tClose == std::string::npos) break;
            text += decodeXmlEntities(block.substr(tGt + 1, tClose - tGt - 1));
            p = tClose + 4;
        }
        result.push_back(text);
        pos = siEnd + 5;
    }
    return result;
}

std::string extractSheetText(const std::string& xml, const std::vector<std::string>& sharedStrings) {
    std::string out;
    MultiFind finder(xml, {"<c ", "<c>"});
    std::size_t pos = 0;
    while (true) {
        const auto [idx, start] = finder.next(pos);
        if (idx == -1) break;
        const std::size_t gt = xml.find('>', start);
        if (gt == std::string::npos) break;

        const std::string openTag = xml.substr(start, gt - start + 1);
        const bool selfClosing = gt > 0 && xml[gt - 1] == '/';

        std::string cellType;
        const std::size_t tAttr = openTag.find("t=\"");
        if (tAttr != std::string::npos) {
            const std::size_t vEnd = openTag.find('"', tAttr + 3);
            if (vEnd != std::string::npos) cellType = openTag.substr(tAttr + 3, vEnd - tAttr - 3);
        }

        if (selfClosing) { pos = gt + 1; continue; }

        const std::size_t cClose = xml.find("</c>", gt + 1);
        if (cClose == std::string::npos) break;
        const std::string cellBody = xml.substr(gt + 1, cClose - gt - 1);

        auto extractBetween = [&](const std::string& openTagName, const std::string& closeTagName) -> std::optional<std::string> {
            const std::size_t o = cellBody.find(openTagName);
            const std::size_t c = cellBody.find(closeTagName);
            if (o == std::string::npos || c == std::string::npos || c < o) return std::nullopt;
            return cellBody.substr(o + openTagName.size(), c - o - openTagName.size());
        };

        if (cellType == "s") {
            if (auto v = extractBetween("<v>", "</v>")) {
                const int idx = std::atoi(v->c_str());
                if (idx >= 0 && static_cast<std::size_t>(idx) < sharedStrings.size()) {
                    out += sharedStrings[static_cast<std::size_t>(idx)];
                    out += ' ';
                }
            }
        } else if (cellType == "str") {
            if (auto v = extractBetween("<v>", "</v>")) {
                out += decodeXmlEntities(*v);
                out += ' ';
            }
        } else if (cellType == "inlineStr") {
            if (auto v = extractBetween("<t>", "</t>")) {
                out += decodeXmlEntities(*v);
                out += ' ';
            }
        }
        pos = cClose + 4;
    }
    return out;
}

// ---------------------------------------------------------------------------
// PDF: FlateDecode + Tj/TJ/'/" text-show operators, with best-effort
// /ToUnicode CMap (bfchar/simple bfrange) support for single-byte-encoded
// text. See ContentExtractor.h for the documented limitations (no CID/
// Identity-H 2-byte font decoding, no encryption support).
// ---------------------------------------------------------------------------

std::uint32_t bytesToUint(const std::string& b) {
    std::uint32_t v = 0;
    for (unsigned char c : b) v = (v << 8) | c;
    return v;
}

std::string utf16beBytesToUtf8(const std::string& bytes) {
    std::string out;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const std::uint32_t cu = (static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]);
        appendUtf8(out, cu);
    }
    return out;
}

std::vector<std::string> extractHexTokens(const std::string& region) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < region.size()) {
        if (region[i] == '<') {
            std::size_t j = i + 1;
            std::string hex;
            while (j < region.size() && region[j] != '>') {
                if (std::isxdigit(static_cast<unsigned char>(region[j]))) hex.push_back(region[j]);
                ++j;
            }
            if (j < region.size()) ++j;
            if (hex.size() % 2 == 1) hex.push_back('0');
            std::string raw;
            raw.reserve(hex.size() / 2);
            for (std::size_t k = 0; k + 1 < hex.size() + 1 && k < hex.size(); k += 2) {
                raw.push_back(static_cast<char>(std::strtol(hex.substr(k, 2).c_str(), nullptr, 16)));
            }
            tokens.push_back(std::move(raw));
            i = j;
        } else {
            ++i;
        }
    }
    return tokens;
}

void parseBfChar(const std::string& text, std::unordered_map<std::uint32_t, std::string>& out) {
    std::size_t pos = 0;
    while (true) {
        const std::size_t begin = text.find("beginbfchar", pos);
        if (begin == std::string::npos) break;
        const std::size_t end = text.find("endbfchar", begin);
        if (end == std::string::npos) break;
        const auto tokens = extractHexTokens(text.substr(begin, end - begin));
        for (std::size_t i = 0; i + 1 < tokens.size(); i += 2) {
            out[bytesToUint(tokens[i])] = utf16beBytesToUtf8(tokens[i + 1]);
        }
        pos = end + 9;
    }
}

void parseBfRange(const std::string& text, std::unordered_map<std::uint32_t, std::string>& out) {
    std::size_t pos = 0;
    while (true) {
        const std::size_t begin = text.find("beginbfrange", pos);
        if (begin == std::string::npos) break;
        const std::size_t end = text.find("endbfrange", begin);
        if (end == std::string::npos) break;
        const auto tokens = extractHexTokens(text.substr(begin, end - begin));
        // Only the simple linear form (srcStart, srcEnd, dstStart) is handled;
        // the array-destination form is a known, documented gap.
        for (std::size_t i = 0; i + 2 < tokens.size(); i += 3) {
            if (tokens[i + 2].size() != 2) continue;
            const std::uint32_t srcStart = bytesToUint(tokens[i]);
            const std::uint32_t srcEnd = bytesToUint(tokens[i + 1]);
            const std::uint32_t dstStart = bytesToUint(tokens[i + 2]);
            for (std::uint32_t code = srcStart; code <= srcEnd && code - srcStart < 65536; ++code) {
                std::string u16;
                u16.push_back(static_cast<char>((dstStart + (code - srcStart)) >> 8));
                u16.push_back(static_cast<char>((dstStart + (code - srcStart)) & 0xFF));
                out[code] = utf16beBytesToUtf8(u16);
            }
        }
        pos = end + 10;
    }
}

void parseToUnicodeCMap(const std::string& text, std::unordered_map<std::uint32_t, std::string>& out) {
    parseBfChar(text, out);
    parseBfRange(text, out);
}

struct DecodeAttempt {
    std::string text;
    std::size_t hits = 0;   // codes actually found in the ToUnicode map
    std::size_t total = 0;  // codes attempted
};

// Interprets `bytes` as single-byte character codes (the common case for
// simple/custom-encoded fonts).
DecodeAttempt decodeSingleByte(const std::string& bytes, const std::unordered_map<std::uint32_t, std::string>& cmap) {
    DecodeAttempt result;
    for (unsigned char b : bytes) {
        ++result.total;
        const auto it = cmap.find(b);
        if (it != cmap.end()) {
            result.text += it->second;
            ++result.hits;
        } else if (b >= 0x20 && b <= 0x7E) {
            result.text.push_back(static_cast<char>(b));
        }
        // Unmapped high byte with no CMap entry: dropped rather than guessed,
        // to avoid polluting the index with mojibake.
    }
    return result;
}

// Interprets `bytes` as big-endian 2-byte character codes — how Type0/CID
// fonts with Identity-H encoding represent text, which is how most modern
// tools embed non-Latin scripts (Cyrillic included) in a PDF. Unlike the
// single-byte path, an unmapped 2-byte code has no safe ASCII fallback (a
// raw code is not a character), so it's simply dropped.
DecodeAttempt decodeDoubleByte(const std::string& bytes, const std::unordered_map<std::uint32_t, std::string>& cmap) {
    DecodeAttempt result;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const std::uint32_t code = (static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]);
        ++result.total;
        const auto it = cmap.find(code);
        if (it != cmap.end()) {
            result.text += it->second;
            ++result.hits;
        }
    }
    return result;
}

// Best-effort decode of one shown string. There's no per-run font tracking
// (see header comment for why), so 1-byte vs 2-byte encoding is picked
// heuristically per string: 2-byte wins only when it fully resolves against
// the document's /ToUnicode map and the 1-byte reading doesn't — the
// signature of Identity-H CID text, which single-byte decoding would
// otherwise turn into unmapped-byte mojibake or drop entirely.
std::string decodeShown(const std::string& bytes, const std::unordered_map<std::uint32_t, std::string>& cmap) {
    if (bytes.empty()) return {};
    if (cmap.empty()) {
        // No ToUnicode map anywhere in the document: only ASCII passthrough
        // is safe to assume.
        return decodeSingleByte(bytes, cmap).text;
    }

    const DecodeAttempt oneByte = decodeSingleByte(bytes, cmap);
    const DecodeAttempt twoByte = bytes.size() >= 2 ? decodeDoubleByte(bytes, cmap) : DecodeAttempt{};

    if (twoByte.total > 0 && twoByte.hits == twoByte.total && oneByte.hits < oneByte.total) {
        return twoByte.text;
    }
    return oneByte.hits >= twoByte.hits ? oneByte.text : twoByte.text;
}

std::pair<std::string, std::size_t> consumeLiteralString(const std::string& text, std::size_t i) {
    std::size_t j = i + 1;
    int depth = 1;
    std::string raw;
    while (j < text.size() && depth > 0) {
        const char c = text[j];
        if (c == '\\' && j + 1 < text.size()) {
            const char e = text[j + 1];
            switch (e) {
                case 'n': raw.push_back('\n'); j += 2; break;
                case 'r': raw.push_back('\r'); j += 2; break;
                case 't': raw.push_back('\t'); j += 2; break;
                case 'b': raw.push_back('\b'); j += 2; break;
                case 'f': raw.push_back('\f'); j += 2; break;
                case '(': raw.push_back('('); j += 2; break;
                case ')': raw.push_back(')'); j += 2; break;
                case '\\': raw.push_back('\\'); j += 2; break;
                case '\r': j += 2; if (j < text.size() && text[j] == '\n') ++j; break;
                case '\n': j += 2; break;
                default:
                    if (e >= '0' && e <= '7') {
                        int val = 0, count = 0;
                        std::size_t k = j + 1;
                        while (count < 3 && k < text.size() && text[k] >= '0' && text[k] <= '7') {
                            val = val * 8 + (text[k] - '0');
                            ++k;
                            ++count;
                        }
                        raw.push_back(static_cast<char>(val & 0xFF));
                        j = k;
                    } else {
                        raw.push_back(e);
                        j += 2;
                    }
            }
        } else if (c == '(') {
            ++depth;
            raw.push_back(c);
            ++j;
        } else if (c == ')') {
            --depth;
            if (depth > 0) raw.push_back(c);
            ++j;
        } else {
            raw.push_back(c);
            ++j;
        }
    }
    return {raw, j};
}

std::pair<std::string, std::size_t> consumeHexString(const std::string& text, std::size_t i) {
    std::size_t j = i + 1;
    std::string hex;
    while (j < text.size() && text[j] != '>') {
        if (std::isxdigit(static_cast<unsigned char>(text[j]))) hex.push_back(text[j]);
        ++j;
    }
    if (j < text.size()) ++j;
    if (hex.size() % 2 == 1) hex.push_back('0');
    std::string raw;
    raw.reserve(hex.size() / 2);
    for (std::size_t k = 0; k < hex.size(); k += 2) {
        raw.push_back(static_cast<char>(std::strtol(hex.substr(k, 2).c_str(), nullptr, 16)));
    }
    return {raw, j};
}

std::string scanContentStreamText(const std::string& text, const std::unordered_map<std::uint32_t, std::string>& cmap) {
    struct Chunk {
        bool isGap;
        std::string bytes;
    };
    std::string out;
    std::vector<Chunk> pending;
    bool inArray = false;
    std::size_t i = 0;

    auto flush = [&]() {
        for (const auto& chunk : pending) {
            if (chunk.isGap) out += ' ';
            else out += decodeShown(chunk.bytes, cmap);
        }
        out += ' ';
        pending.clear();
    };

    while (i < text.size()) {
        const char c = text[i];
        if (c == '(') {
            auto [bytes, next] = consumeLiteralString(text, i);
            pending.push_back({false, std::move(bytes)});
            i = next;
        } else if (c == '<' && i + 1 < text.size() && text[i + 1] != '<') {
            auto [bytes, next] = consumeHexString(text, i);
            pending.push_back({false, std::move(bytes)});
            i = next;
        } else if (c == '[') {
            inArray = true;
            ++i;
        } else if (c == ']') {
            inArray = false;
            ++i;
        } else if (c == '-' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t start = i;
            ++i;
            while (i < text.size() &&
                   (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.' || text[i] == '-')) {
                ++i;
            }
            if (inArray) {
                const double value = std::strtod(text.substr(start, i - start).c_str(), nullptr);
                if (value < -100.0 || value > 100.0) pending.push_back({true, {}});
            }
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '\'' || c == '"') {
            const std::size_t start = i;
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '*')) ++i;
            if (i == start) ++i;  // lone ' or "
            const std::string token = text.substr(start, i - start);
            if (token == "Tj" || token == "'" || token == "\"") {
                flush();
            } else if (token == "TJ") {
                flush();
            } else {
                pending.clear();
            }
        } else {
            ++i;
        }
    }
    return out;
}

std::optional<std::string> extractPdfText(const std::vector<std::uint8_t>& rawBytes) {
    const std::string text(reinterpret_cast<const char*>(rawBytes.data()), rawBytes.size());

    static const std::regex kObjRe(R"((\d+)\s+(\d+)\s+obj\b)");
    std::vector<std::pair<std::size_t, std::size_t>> objectSpans;
    {
        auto it = std::sregex_iterator(text.begin(), text.end(), kObjRe);
        const auto end = std::sregex_iterator();
        for (; it != end; ++it) {
            const std::size_t start = static_cast<std::size_t>(it->position());
            const std::size_t endObj = text.find("endobj", start);
            if (endObj == std::string::npos) continue;
            objectSpans.emplace_back(start, endObj);
        }
    }

    std::vector<std::string> streams;
    streams.reserve(objectSpans.size());
    for (const auto& [start, endPos] : objectSpans) {
        const std::string body = text.substr(start, endPos - start);
        const std::size_t streamKw = body.find("stream");
        if (streamKw == std::string::npos) continue;

        std::size_t dataStart = streamKw + 6;
        if (dataStart < body.size() && body[dataStart] == '\r') ++dataStart;
        if (dataStart < body.size() && body[dataStart] == '\n') ++dataStart;
        const std::size_t endStreamPos = body.find("endstream", dataStart);
        if (endStreamPos == std::string::npos || endStreamPos < dataStart) continue;

        const std::string dictText = body.substr(0, streamKw);
        std::size_t length = endStreamPos - dataStart;
        {
            static const std::regex kLenDirectRe(R"(/Length\s+(\d+)(?!\s+\d+\s+R))");
            std::smatch m;
            if (std::regex_search(dictText, m, kLenDirectRe)) {
                const std::size_t candidate = static_cast<std::size_t>(std::stoul(m[1].str()));
                if (candidate <= length) length = candidate;
            }
        }
        while (length > 0 && (body[dataStart + length - 1] == '\n' || body[dataStart + length - 1] == '\r')) {
            --length;
        }

        const std::string streamBytes = body.substr(dataStart, length);
        const bool flate = dictText.find("FlateDecode") != std::string::npos;

        if (flate) {
            auto decompressed = inflateToString(reinterpret_cast<const unsigned char*>(streamBytes.data()),
                                                 streamBytes.size(), /*rawDeflate=*/false);
            if (decompressed) streams.push_back(std::move(*decompressed));
        } else {
            streams.push_back(streamBytes);
        }
    }

    std::unordered_map<std::uint32_t, std::string> cmap;
    for (const auto& s : streams) {
        if (s.find("beginbfchar") != std::string::npos || s.find("beginbfrange") != std::string::npos) {
            parseToUnicodeCMap(s, cmap);
        }
    }

    std::string result;
    for (const auto& s : streams) {
        if (s.find("beginbfchar") != std::string::npos || s.find("beginbfrange") != std::string::npos) continue;
        result += scanContentStreamText(s, cmap);
    }
    if (result.empty()) return std::nullopt;
    return result;
}

const std::unordered_set<std::string>& supportedExtensions() {
    static const std::unordered_set<std::string> kSupported = {
        ".txt", ".md", ".csv", ".log", ".ini", ".json", ".xml", ".cpp", ".h", ".hpp",
        ".c", ".cc", ".cxx", ".py", ".js", ".ts", ".java", ".cs", ".go", ".rs", ".rb",
        ".php", ".sql", ".yaml", ".yml", ".sh", ".bat", ".ps1", ".html", ".htm", ".css",
        ".docx", ".xlsx", ".pdf",
    };
    return kSupported;
}

} // namespace

bool ContentExtractor::isSupportedExtension(const std::string& extensionLowercase) {
    return supportedExtensions().count(extensionLowercase) != 0;
}

std::optional<std::string> ContentExtractor::extract(const std::filesystem::path& path,
                                                       const std::string& extensionLowercase,
                                                       const ExtractionOptions& options) {
    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize > options.maxBytes) return std::nullopt;

    if (extensionLowercase == ".docx") {
        auto raw = readWholeFile(path);
        if (!raw) return std::nullopt;
        ZipArchive archive;
        if (!archive.open(*raw)) return std::nullopt;
        auto doc = archive.readEntry("word/document.xml");
        if (!doc) return std::nullopt;
        auto body = extractDocxBody(*doc);
        return body.empty() ? std::nullopt : std::optional<std::string>(std::move(body));
    }

    if (extensionLowercase == ".xlsx") {
        auto raw = readWholeFile(path);
        if (!raw) return std::nullopt;
        ZipArchive archive;
        if (!archive.open(*raw)) return std::nullopt;

        std::vector<std::string> sharedStrings;
        if (auto ss = archive.readEntry("xl/sharedStrings.xml")) {
            sharedStrings = parseSharedStrings(*ss);
        }

        std::string out;
        for (const auto& name : archive.entryNames()) {
            if (name.rfind("xl/worksheets/sheet", 0) == 0 && name.size() > 4 &&
                name.compare(name.size() - 4, 4, ".xml") == 0) {
                if (auto sheet = archive.readEntry(name)) {
                    out += extractSheetText(*sheet, sharedStrings);
                }
            }
        }
        return out.empty() ? std::nullopt : std::optional<std::string>(std::move(out));
    }

    if (extensionLowercase == ".pdf") {
        auto raw = readWholeFile(path);
        if (!raw) return std::nullopt;
        return extractPdfText(*raw);
    }

    auto raw = readWholeFile(path);
    if (!raw || raw->empty()) return std::nullopt;
    return std::string(raw->begin(), raw->end());
}

} // namespace datasearch::core
