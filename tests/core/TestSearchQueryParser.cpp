#include "TestFramework.h"

#include "datasearch/core/SearchQueryParser.h"

using datasearch::core::parseSearchQuery;
using datasearch::core::ParsedSearchQuery;

namespace {

const ParsedSearchQuery::Token* findToken(const ParsedSearchQuery& q, const std::string& text) {
    for (const auto& t : q.tokens) {
        if (t.text == text) return &t;
    }
    return nullptr;
}

} // namespace

void runSearchQueryParserTests() {
    // Plain barewords: implicit AND, prefix-matched (not phrases).
    {
        auto q = parseSearchQuery("practicum report");
        DS_CHECK_EQ(q.tokens.size(), std::size_t{2});
        DS_CHECK(!q.extensionFilter.has_value());
        DS_CHECK(!q.pathFilter.has_value());
        const auto* t1 = findToken(q, "practicum");
        DS_CHECK(t1 != nullptr && !t1->isPhrase && !t1->excluded);
    }

    // Exact phrase in quotes (ТЗ FR-13).
    {
        auto q = parseSearchQuery("\"quarterly report\"");
        DS_CHECK_EQ(q.tokens.size(), std::size_t{1});
        DS_CHECK(q.tokens[0].isPhrase);
        DS_CHECK_EQ(q.tokens[0].text, std::string("quarterly report"));
        DS_CHECK(!q.tokens[0].excluded);
    }

    // Exclusion: -word and -"phrase".
    {
        auto q = parseSearchQuery("report -draft -\"old version\"");
        DS_CHECK_EQ(q.tokens.size(), std::size_t{3});
        const auto* draft = findToken(q, "draft");
        DS_CHECK(draft != nullptr && draft->excluded && !draft->isPhrase);
        const auto* oldVersion = findToken(q, "old version");
        DS_CHECK(oldVersion != nullptr && oldVersion->excluded && oldVersion->isPhrase);
        const auto* report = findToken(q, "report");
        DS_CHECK(report != nullptr && !report->excluded);
    }

    // ext: filter, plain and quoted, with/without leading dot.
    {
        auto q = parseSearchQuery("report ext:docx");
        DS_CHECK(q.extensionFilter.has_value());
        DS_CHECK_EQ(*q.extensionFilter, std::string(".docx"));
        DS_CHECK_EQ(q.tokens.size(), std::size_t{1});  // "ext:docx" consumed, not a text token
    }
    {
        auto q = parseSearchQuery("ext:.PDF");
        DS_CHECK(q.extensionFilter.has_value());
        DS_CHECK_EQ(*q.extensionFilter, std::string(".pdf"));  // normalized lowercase
    }
    {
        auto q = parseSearchQuery("ext:\"xlsx\"");
        DS_CHECK(q.extensionFilter.has_value());
        DS_CHECK_EQ(*q.extensionFilter, std::string(".xlsx"));
    }

    // path: filter, plain and quoted (spaces inside quotes).
    {
        auto q = parseSearchQuery("path:D:\\Work\\ report");
        DS_CHECK(q.pathFilter.has_value());
        DS_CHECK_EQ(*q.pathFilter, std::string("D:\\Work\\"));
        DS_CHECK_EQ(q.tokens.size(), std::size_t{1});
    }
    {
        auto q = parseSearchQuery("path:\"D:\\My Files\\\" report");
        DS_CHECK(q.pathFilter.has_value());
        DS_CHECK_EQ(*q.pathFilter, std::string("D:\\My Files\\"));
        DS_CHECK_EQ(q.tokens.size(), std::size_t{1});
        DS_CHECK(findToken(q, "report") != nullptr);
    }

    // Repeated filters: only the first wins.
    {
        auto q = parseSearchQuery("ext:docx ext:pdf");
        DS_CHECK_EQ(*q.extensionFilter, std::string(".docx"));
    }

    // Combination of everything at once.
    {
        auto q = parseSearchQuery("\"практикум номер 1\" -черновик ext:docx path:D:\\Work\\");
        DS_CHECK_EQ(q.tokens.size(), std::size_t{2});
        DS_CHECK(q.extensionFilter.has_value());
        DS_CHECK(q.pathFilter.has_value());
        const auto* phrase = findToken(q, "\u043f\u0440\u0430\u043a\u0442\u0438\u043a\u0443\u043c \u043d\u043e\u043c\u0435\u0440 1");
        DS_CHECK(phrase != nullptr && phrase->isPhrase && !phrase->excluded);
    }

    // Edge cases: empty input, lone '-', empty quotes.
    {
        auto q = parseSearchQuery("");
        DS_CHECK_EQ(q.tokens.size(), std::size_t{0});
    }
    {
        auto q = parseSearchQuery("report - draft");
        // A lone '-' surrounded by spaces isn't an exclusion marker for anything.
        DS_CHECK_EQ(q.tokens.size(), std::size_t{2});
        const auto* draft = findToken(q, "draft");
        DS_CHECK(draft != nullptr && !draft->excluded);
    }
    {
        auto q = parseSearchQuery("\"\" report");
        DS_CHECK_EQ(q.tokens.size(), std::size_t{1});  // empty phrase dropped
    }
}
