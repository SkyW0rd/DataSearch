#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace datasearch::core {

struct ExtractionOptions {
    // Plain-text and PDF files larger than this are indexed by name/metadata
    // only (ТЗ: избегать непропорциональной нагрузки на RAM/CPU при
    // индексации огромных файлов).
    // The PDF object-header scan used to be the limiting factor here (a
    // recursive std::regex that risked a stack overflow on large inputs —
    // see ContentExtractor.cpp); now that it's a linear manual scan, this can
    // sit much higher than the old 20 MB without the same risk.
    std::uint64_t maxBytes = 100ull * 1024 * 1024;

    // DOCX/XLSX instead: the unpacked size of their text-bearing XML parts.
    // Embedded images are never read, so the file's size on disk (mostly
    // pictures, often) says nothing about the cost; the XML does — an XLSX
    // sheet routinely unpacks to several times the file's size.
    std::uint64_t maxUnpackedBytes = 512ull * 1024 * 1024;
};

// Where extraction's time went, for the indexing monitor: reading the file
// (opening included — on Windows that's also where an antivirus scans it)
// versus everything else, i.e. parsing.
struct ExtractionTiming {
    double readSeconds = 0;
    std::uint64_t bytesRead = 0;
};

// Why a file's text couldn't be extracted — for the list of such files the
// user can review and retry. Stored in the index as its number, so values
// must not be renumbered.
enum class ExtractionProblem {
    None = 0,        // text extracted, or the file simply has none (a scanned PDF)
    CannotOpen = 1,  // couldn't be opened or read: no access, in use, I/O error
    Damaged = 2,     // not a valid file of its type
    Protected = 3,   // needs a password to open (encrypted DOCX/XLSX/PDF)
    TooLarge = 4,    // over the ExtractionOptions limit; text not extracted
    Failed = 5,      // extraction broke off with an error (e.g. out of memory)
    ImagesOnly = 6,  // a PDF of page images (a scan) with no text layer: only OCR would read it
};

// Best-effort plain-text extraction for the formats required by ТЗ п.3.1.1:
//  - plain text / source code: read as-is (UTF-8 assumed).
//  - .docx: text runs of the body, headers/footers, footnotes/endnotes,
//    comments, charts and SmartArt. Embedded images are never read.
//  - .xlsx: cell text and numbers from every sheet (shared and inline
//    strings, numeric/date values), sheet names, cell notes, and the text of
//    charts, shapes and SmartArt. Embedded images are never read.
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
//    A PDF encrypted without a password for opening (one that merely
//    forbids copying or printing) is decrypted — RC4 and AES-128/256, see
//    PdfCrypto.h — as any viewer does. One that needs a password is
//    reported as ExtractionProblem::Protected; scanned pages without a text
//    layer as ExtractionProblem::ImagesOnly.
// Binary formats with no extractable text return std::nullopt — ТЗ says such
// files stay indexed by name/metadata only.
class ContentExtractor {
public:
    static bool isSupportedExtension(const std::string& extensionLowercase);
    // `timing`, if given, is added to (not reset). `problem`, if given, is
    // set to why no text came out, or None. Errors while parsing escape as
    // exceptions; callers report those as ExtractionProblem::Failed.
    static std::optional<std::string> extract(const std::filesystem::path& path,
                                                const std::string& extensionLowercase,
                                                const ExtractionOptions& options = {},
                                                ExtractionTiming* timing = nullptr,
                                                ExtractionProblem* problem = nullptr);
};

} // namespace datasearch::core
