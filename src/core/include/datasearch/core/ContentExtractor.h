#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace datasearch::core {

struct ExtractionOptions {
    // Files larger than this are indexed by name/metadata only (ТЗ: избегать
    // непропорциональной нагрузки на RAM/CPU при индексации огромных файлов).
    std::uint64_t maxBytes = 20ull * 1024 * 1024;
};

// Best-effort plain-text extraction for the formats required by ТЗ п.3.1.1:
//  - plain text / source code: read as-is (UTF-8 assumed).
//  - .docx: text runs from word/document.xml inside the zip container.
//  - .xlsx: cell text from xl/sharedStrings.xml + inline strings in each sheet.
//  - .pdf: text-showing operators (Tj/TJ/'/") from each stream's content,
//    after FlateDecode decompression; a /ToUnicode CMap (bfchar mappings) is
//    applied when present. This is NOT a full PDF engine: it does not track
//    per-run font selection, so 2-byte CID/Identity-H encoded text (the most
//    common way modern tools embed Cyrillic in a PDF) is not decoded — only
//    single-byte-encoded text is. Encrypted/password-protected PDFs and
//    scanned pages without a text layer are out of scope (matches ТЗ п.3.1.1).
// Binary formats with no extractable text return std::nullopt — ТЗ says such
// files stay indexed by name/metadata only.
class ContentExtractor {
public:
    static bool isSupportedExtension(const std::string& extensionLowercase);
    static std::optional<std::string> extract(const std::filesystem::path& path,
                                                const std::string& extensionLowercase,
                                                const ExtractionOptions& options = {});
};

} // namespace datasearch::core
