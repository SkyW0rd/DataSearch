#include "TestFramework.h"

#include "datasearch/core/ContentExtractor.h"

#include <zlib.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <utility>
#include <vector>

using datasearch::core::ContentExtractor;
using datasearch::core::ExtractionOptions;

namespace {

std::filesystem::path makeScratchDir() {
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("datasearch_extractor_test_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

void writeBinaryFile(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string zlibCompress(const std::string& input) {
    uLongf boundLen = compressBound(static_cast<uLong>(input.size()));
    std::string out(boundLen, '\0');
    const int rc = compress(reinterpret_cast<Bytef*>(out.data()), &boundLen,
                             reinterpret_cast<const Bytef*>(input.data()), static_cast<uLong>(input.size()));
    if (rc != Z_OK) throw std::runtime_error("zlibCompress failed");
    out.resize(boundLen);
    return out;
}

void putU16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void putU32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

// Builds a minimal ZIP archive (stored/uncompressed entries) in-process, so
// DOCX/XLSX test fixtures don't depend on an external `zip` binary — the
// windows-latest GitHub Actions runner doesn't have one on PATH, unlike
// macOS/Linux. CRC32 comes from zlib, already linked for FlateDecode above.
void writeZip(const std::filesystem::path& zipPath,
              const std::vector<std::pair<std::string, std::string>>& entries) {
    struct CentralEntry {
        std::string name;
        std::uint32_t crc;
        std::uint32_t size;
        std::uint32_t offset;
    };
    std::string out;
    std::vector<CentralEntry> central;

    for (const auto& [name, content] : entries) {
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto crc = static_cast<std::uint32_t>(
            crc32(0, reinterpret_cast<const Bytef*>(content.data()), static_cast<uInt>(content.size())));
        const auto size = static_cast<std::uint32_t>(content.size());

        putU32(out, 0x04034b50); // local file header signature
        putU16(out, 20);         // version needed to extract
        putU16(out, 0);          // flags
        putU16(out, 0);          // method: stored
        putU16(out, 0);          // mod time
        putU16(out, 0);          // mod date
        putU32(out, crc);
        putU32(out, size); // compressed size
        putU32(out, size); // uncompressed size
        putU16(out, static_cast<std::uint16_t>(name.size()));
        putU16(out, 0); // extra field length
        out += name;
        out += content;

        central.push_back({name, crc, size, offset});
    }

    const auto cdStart = static_cast<std::uint32_t>(out.size());
    for (const auto& e : central) {
        putU32(out, 0x02014b50); // central directory header signature
        putU16(out, 20);         // version made by
        putU16(out, 20);         // version needed to extract
        putU16(out, 0);          // flags
        putU16(out, 0);          // method: stored
        putU16(out, 0);          // mod time
        putU16(out, 0);          // mod date
        putU32(out, e.crc);
        putU32(out, e.size); // compressed size
        putU32(out, e.size); // uncompressed size
        putU16(out, static_cast<std::uint16_t>(e.name.size()));
        putU16(out, 0); // extra field length
        putU16(out, 0); // comment length
        putU16(out, 0); // disk number start
        putU16(out, 0); // internal attributes
        putU32(out, 0); // external attributes
        putU32(out, e.offset);
        out += e.name;
    }
    const auto cdSize = static_cast<std::uint32_t>(out.size() - cdStart);

    putU32(out, 0x06054b50); // end of central directory signature
    putU16(out, 0);          // disk number
    putU16(out, 0);          // disk with central directory
    putU16(out, static_cast<std::uint16_t>(central.size()));
    putU16(out, static_cast<std::uint16_t>(central.size()));
    putU32(out, cdSize);
    putU32(out, cdStart);
    putU16(out, 0); // comment length

    std::ofstream file(zipPath, std::ios::binary);
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
}

} // namespace

void runContentExtractorTests() {
    const auto root = makeScratchDir();
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{root};

    // --- Plain text -----------------------------------------------------
    {
        const auto path = root / "note.txt";
        writeFile(path, "Hello plain text");
        DS_CHECK(ContentExtractor::isSupportedExtension(".txt"));
        auto extracted = ContentExtractor::extract(path, ".txt");
        DS_CHECK(extracted.has_value());
        DS_CHECK_EQ(*extracted, std::string("Hello plain text"));
    }

    // --- maxBytes cap -----------------------------------------------------
    {
        const auto path = root / "big.txt";
        writeFile(path, std::string(1000, 'x'));
        ExtractionOptions tinyLimit;
        tinyLimit.maxBytes = 10;
        auto extracted = ContentExtractor::extract(path, ".txt", tinyLimit);
        DS_CHECK(!extracted.has_value());
    }

    // --- Missing file -----------------------------------------------------
    {
        auto extracted = ContentExtractor::extract(root / "does_not_exist.txt", ".txt");
        DS_CHECK(!extracted.has_value());
    }

    // --- Unsupported extension --------------------------------------------
    DS_CHECK(!ContentExtractor::isSupportedExtension(".exe"));
    DS_CHECK(!ContentExtractor::isSupportedExtension(".png"));

    // --- DOCX (real ZIP container, built in-process — see writeZip) -------
    {
        const std::string documentXml =
            "<?xml version=\"1.0\"?>"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:body>"
            "<w:p><w:r><w:t>Hello world</w:t></w:r></w:p>"
            "<w:p><w:r><w:t>\xd0\x92\xd1\x82\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb9 "
            "\xd0\xb0\xd0\xb1\xd0\xb7\xd0\xb0\xd1\x86</w:t></w:r></w:p>"
            "</w:body></w:document>";
        const auto docxPath = root / "report.docx";
        writeZip(docxPath, {{"word/document.xml", documentXml}});

        auto extracted = ContentExtractor::extract(docxPath, ".docx");
        DS_CHECK(extracted.has_value());
        DS_CHECK(contains(*extracted, "Hello world"));
        // "Второй абзац" (UTF-8 bytes above) — proves docx extraction survives
        // through XML entity/UTF-8 handling for Cyrillic, not just ASCII.
        DS_CHECK(contains(*extracted, "\xd0\x92\xd1\x82\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb9"));
    }

    // --- XLSX (shared strings + inline string cell) ------------------------
    {
        const std::string sharedStrings =
            "<?xml version=\"1.0\"?>"
            "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
            "count=\"2\" uniqueCount=\"2\">"
            "<si><t>Apple</t></si>"
            "<si><t>\xd0\x91\xd0\xb0\xd0\xbd\xd0\xb0\xd0\xbd</t></si>"
            "</sst>";
        const std::string sheet1 =
            "<?xml version=\"1.0\"?>"
            "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
            "<sheetData>"
            "<row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"B1\" t=\"s\"><v>1</v></c></row>"
            "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>Extra note</t></is></c></row>"
            "</sheetData></worksheet>";
        const auto xlsxPath = root / "book.xlsx";
        writeZip(xlsxPath, {{"xl/sharedStrings.xml", sharedStrings}, {"xl/worksheets/sheet1.xml", sheet1}});

        auto extracted = ContentExtractor::extract(xlsxPath, ".xlsx");
        DS_CHECK(extracted.has_value());
        DS_CHECK(contains(*extracted, "Apple"));
        DS_CHECK(contains(*extracted, "\xd0\x91\xd0\xb0\xd0\xbd\xd0\xb0\xd0\xbd"));  // Банан
        DS_CHECK(contains(*extracted, "Extra note"));
    }

    // --- PDF: uncompressed content streams, Tj + TJ with a word gap --------
    {
        const std::string pdf =
            "%PDF-1.4\n"
            "1 0 obj\n<< /Type /Page >>\nstream\n"
            "BT /F1 12 Tf 72 700 Td (Hello PDF) Tj ET\n"
            "endstream\nendobj\n"
            "2 0 obj\n<< /Type /Page >>\nstream\n"
            "BT /F1 12 Tf 72 650 Td [(Hello) -500 (World)] TJ ET\n"
            "endstream\nendobj\n"
            "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
        const auto path = root / "plain.pdf";
        writeFile(path, pdf);

        auto extracted = ContentExtractor::extract(path, ".pdf");
        DS_CHECK(extracted.has_value());
        DS_CHECK(contains(*extracted, "Hello PDF"));
        DS_CHECK(contains(*extracted, "Hello"));
        DS_CHECK(contains(*extracted, "World"));
        // The -500 gap between "Hello" and "World" in the TJ array must not
        // collapse them into "HelloWorld" (would break word-level search).
        DS_CHECK(!contains(*extracted, "HelloWorld"));
    }

    // --- PDF: FlateDecode-compressed stream with a correct /Length ---------
    {
        const std::string rawContent = "BT /F1 12 Tf 72 700 Td (Compressed hello) Tj ET";
        const std::string compressed = zlibCompress(rawContent);

        std::string pdf = "%PDF-1.4\n1 0 obj\n<< /Type /Page /Filter /FlateDecode /Length ";
        pdf += std::to_string(compressed.size());
        pdf += " >>\nstream\n";
        pdf += compressed;
        pdf += "\nendstream\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%EOF\n";

        const auto path = root / "compressed.pdf";
        writeBinaryFile(path, pdf);

        auto extracted = ContentExtractor::extract(path, ".pdf");
        DS_CHECK(extracted.has_value());
        DS_CHECK(contains(*extracted, "Compressed hello"));
    }

    // --- PDF: /ToUnicode bfchar CMap remaps single-byte codes --------------
    {
        // Byte 0x41 ('A') -> U+0410 (Cyrillic А), 0x42 ('B') -> U+0411 (Cyrillic Б).
        const std::string pdf =
            "%PDF-1.4\n"
            "1 0 obj\n<< /Type /Page >>\nstream\n"
            "BT /F1 12 Tf 72 700 Td (AB) Tj ET\n"
            "endstream\nendobj\n"
            "2 0 obj\n<< /Type /Font >>\nstream\n"
            "beginbfchar\n<41> <0410>\n<42> <0411>\nendbfchar\n"
            "endstream\nendobj\n"
            "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
        const auto path = root / "cmap.pdf";
        writeFile(path, pdf);

        auto extracted = ContentExtractor::extract(path, ".pdf");
        DS_CHECK(extracted.has_value());
        // "АБ" (Cyrillic) via the CMap, not the raw "AB" ASCII passthrough.
        DS_CHECK(contains(*extracted, "\xd0\x90\xd0\x91"));
        DS_CHECK(!contains(*extracted, "AB"));
    }

    // --- PDF: 2-byte (Identity-H/CID) codes via /ToUnicode ------------------
    {
        // A hex string of two 2-byte codes (0x0001, 0x0002) shown via Tj —
        // how Type0/CID fonts represent text, the common way modern tools
        // embed Cyrillic in a PDF. Neither code is printable ASCII on its
        // own, so single-byte decoding would drop them entirely; the 2-byte
        // heuristic must recognize this and use the CMap instead.
        const std::string pdf =
            "%PDF-1.4\n"
            "1 0 obj\n<< /Type /Page >>\nstream\n"
            "BT /F1 12 Tf 72 700 Td <00010002> Tj ET\n"
            "endstream\nendobj\n"
            "2 0 obj\n<< /Type /Font >>\nstream\n"
            "beginbfchar\n<0001> <0412>\n<0002> <0413>\nendbfchar\n"
            "endstream\nendobj\n"
            "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
        const auto path = root / "cmap_cid.pdf";
        writeFile(path, pdf);

        auto extracted = ContentExtractor::extract(path, ".pdf");
        DS_CHECK(extracted.has_value());
        // "ВГ" (Cyrillic В, Г) via the 2-byte CMap lookup.
        DS_CHECK(contains(*extracted, "\xd0\x92\xd0\x93"));
    }
}
