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

    // --- DOCX: table markup must not leak into the text -------------------
    // "<w:t" is the prefix of <w:tbl>, <w:tc>, <w:tr> and <w:tab/> as well as
    // of a real <w:t> text run. Matching it loosely made the extractor read
    // everything up to the next "</w:t>" as text, so a document with tables
    // (most real ones) had raw XML indexed as its content.
    {
        const std::string documentXml =
            "<?xml version=\"1.0\"?>"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:body><w:tbl><w:tblPr><w:tblW w:w=\"9781\" w:type=\"dxa\"/></w:tblPr>"
            "<w:tr><w:tc><w:tcPr><w:vAlign w:val=\"center\"/></w:tcPr>"
            "<w:p><w:r><w:t>Cell text</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
            "<w:p><w:r><w:t>After table</w:t><w:tab/><w:t xml:space=\"preserve\">Tabbed</w:t></w:r></w:p>"
            "</w:body></w:document>";
        const auto docxPath = root / "table.docx";
        writeZip(docxPath, {{"word/document.xml", documentXml}});

        auto extracted = ContentExtractor::extract(docxPath, ".docx");
        DS_CHECK(extracted.has_value());
        DS_CHECK(contains(*extracted, "Cell text"));
        DS_CHECK(contains(*extracted, "After table"));
        DS_CHECK(contains(*extracted, "Tabbed"));
        DS_CHECK(!contains(*extracted, "tblPr"));
        DS_CHECK(!contains(*extracted, "dxa"));
        DS_CHECK(!contains(*extracted, "w:val"));
    }

    // --- DOCX: headers/footers/footnotes/comments, image ahead of the body --
    // The archive is read by offset rather than loaded whole, so entries
    // sitting after a large media file must still be found where the central
    // directory says they are.
    {
        auto wordPart = [](const std::string& root, const std::string& text) {
            return "<?xml version=\"1.0\"?><w:" + root +
                   " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                   "<w:p><w:r><w:t>" + text + "</w:t></w:r></w:p></w:" + root + ">";
        };
        const auto docxPath = root / "parts.docx";
        writeZip(docxPath, {{"word/media/image1.png", std::string(3 * 1024 * 1024, '\x7f')},
                            {"word/document.xml", wordPart("document", "Body text")},
                            {"word/header1.xml", wordPart("hdr", "Header text")},
                            {"word/footer2.xml", wordPart("ftr", "Footer text")},
                            {"word/footnotes.xml", wordPart("footnotes", "Footnote text")},
                            {"word/comments.xml", wordPart("comments", "Comment text")}});

        auto extracted = ContentExtractor::extract(docxPath, ".docx");
        DS_CHECK(extracted.has_value());
        DS_CHECK(contains(*extracted, "Body text"));
        DS_CHECK(contains(*extracted, "Header text"));
        DS_CHECK(contains(*extracted, "Footer text"));
        DS_CHECK(contains(*extracted, "Footnote text"));
        DS_CHECK(contains(*extracted, "Comment text"));
        DS_CHECK(!contains(*extracted, "\x7f\x7f"));
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

    // --- PDF: image/font streams must not be mined for "text" --------------
    // Pixel data and embedded font programs are binary, but binary is full of
    // incidental parentheses, so scanning them for Tj/TJ pulled megabytes of
    // noise (runs like `"""%%%PPP`, one repeat per RGB channel) out of a
    // single screenshot and into the index.
    {
        const std::string pdf =
            "%PDF-1.4\n"
            "1 0 obj\n<< /Type /Page >>\nstream\n"
            "BT /F1 12 Tf 72 700 Td (Real page text) Tj ET\n"
            "endstream\nendobj\n"
            "2 0 obj\n<< /Type /XObject /Subtype /Image /Width 4 /Height 4 >>\nstream\n"
            "(IMAGEPIXELS) Tj (MOREPIXELS) Tj\n"
            "endstream\nendobj\n"
            "3 0 obj\n<< /Length1 4096 >>\nstream\n"
            "(FONTPROGRAM) Tj\n"
            "endstream\nendobj\n"
            "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
        const auto path = root / "with_image.pdf";
        writeFile(path, pdf);

        auto extracted = ContentExtractor::extract(path, ".pdf");
        DS_CHECK(extracted.has_value());
        DS_CHECK(contains(*extracted, "Real page text"));
        DS_CHECK(!contains(*extracted, "IMAGEPIXELS"));
        DS_CHECK(!contains(*extracted, "MOREPIXELS"));
        DS_CHECK(!contains(*extracted, "FONTPROGRAM"));
    }

    // --- PDF: object scan tolerates megabytes of digit/whitespace noise ----
    // The object-header scan used to run std::regex (kObjRe) over the whole
    // file. MSVC's <regex> backtracks recursively, and on a large real-world
    // PDF full of digit runs (xref tables, binary streams that happen to look
    // like ASCII digits) that could blow the stack — a native crash, not a
    // catchable C++ exception, which is what actually killed the app on
    // Windows during indexing. The scan is now manual string search instead;
    // this guards against that regressing.
    {
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> digitDist('0', '9');
        std::string pdf = "%PDF-1.4\n";
        std::string noise;
        noise.reserve(4ull * 1024 * 1024);
        for (std::size_t i = 0; i < 4ull * 1024 * 1024; ++i) {
            noise.push_back(i % 37 == 0 ? ' ' : static_cast<char>(digitDist(rng)));
        }
        pdf += noise;
        const std::string streamBody = "BT /F1 12 Tf 72 700 Td (Found after noise) Tj ET\n";
        pdf += "1 0 obj\n<< /Length " + std::to_string(streamBody.size()) + " >>\nstream\n";
        pdf += streamBody;
        pdf += "endstream\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%EOF\n";

        const auto path = root / "noisy_large.pdf";
        writeBinaryFile(path, pdf);

        auto extracted = ContentExtractor::extract(path, ".pdf");
        DS_CHECK(extracted.has_value());
        DS_CHECK(contains(*extracted, "Found after noise"));
    }
}
