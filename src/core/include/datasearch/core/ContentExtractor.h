#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace datasearch::core {

struct ExtractionOptions {
    // Files larger than this are indexed by name/metadata only (ТЗ: избегать
    // непропорциональной нагрузки на RAM/CPU при индексации огромных файлов).
    // The PDF object-header scan used to be the limiting factor here (a
    // recursive std::regex that risked a stack overflow on large inputs —
    // see ContentExtractor.cpp); now that it's a linear manual scan, this can
    // sit much higher than the old 20 MB without the same risk.
    std::uint64_t maxBytes = 100ull * 1024 * 1024;
};

// Best-effort plain-text extraction for the formats required by ТЗ п.3.1.1:
//  - plain text / source code: read as-is (UTF-8 assumed).
//  - .docx: text runs from word/document.xml inside the zip container.
//  - .xlsx: cell text from xl/sharedStrings.xml + inline strings in each sheet.
//  - .pdf: text-showing operators (Tj/TJ/'/") from each stream's content,
//    after FlateDecode decompression; each font's /ToUnicode CMap (bfchar/
//    bfrange) is applied when present. This is NOT a full PDF engine: it does
//    not track per-run font selection (no resource-dictionary/object-graph
//    parsing, so a /F1 name in a content stream can't be resolved to the
//    font object that declared it), so per shown string it tries every
//    font's CMap found in the document, each as both 1-byte and 2-byte
//    (Identity-H/CID — the most common way modern tools embed Cyrillic in a
//    PDF) codes, and keeps whichever decoding resolves the most characters.
//    This handles the common case correctly, including documents that use
//    several different fonts, but can still mismatch a string whose byte
//    codes happen to resolve under more than one font's CMap.
//    Encrypted/password-protected PDFs and scanned pages without a text
//    layer are out of scope (matches ТЗ п.3.1.1).
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
