#include "TestFramework.h"

#include "datasearch/core/RussianStemmer.h"

using datasearch::core::stemRussianWordUtf8;

namespace {

void checkSameStem(std::initializer_list<std::string> forms) {
    std::string firstStem;
    std::string firstWord;
    for (const auto& word : forms) {
        const std::string stem = stemRussianWordUtf8(word);
        if (firstStem.empty()) {
            firstStem = stem;
            firstWord = word;
        } else if (stem != firstStem) {
            throw std::runtime_error("stem mismatch: '" + firstWord + "' -> '" + firstStem +
                                      "' vs '" + word + "' -> '" + stem + "'");
        }
    }
}

} // namespace

void runRussianStemmerTests() {
    // Noun declension: практикум/практикума/практикуму/практикумом/практикумы.
    checkSameStem({"практикум", "практикума", "практикуму", "практикумом", "практикумы"});

    // Noun declension: программа/программы/программу/программой/программ.
    checkSameStem({"программа", "программы", "программу", "программой", "программ"});

    // Verb conjugation: читать/читал/читала/читали/читаю/читает.
    checkSameStem({"читать", "читал", "читала", "читали", "читаю", "читает"});

    // Adjective agreement: красивый/красивая/красивое/красивые/красивых.
    checkSameStem({"красивый", "красивая", "красивое", "красивые", "красивых"});

    // Non-Cyrillic input passes through unchanged (mixed-script/Latin tokens).
    DS_CHECK_EQ(stemRussianWordUtf8("hello"), std::string("hello"));
    DS_CHECK_EQ(stemRussianWordUtf8("practicum2"), std::string("practicum2"));

    // Very short input (below the algorithm's minimum) is returned unchanged.
    DS_CHECK_EQ(stemRussianWordUtf8("и"), std::string("и"));
    DS_CHECK_EQ(stemRussianWordUtf8(""), std::string(""));

    // Distinct roots must not collapse onto the same stem.
    const std::string a = stemRussianWordUtf8("практикум");
    const std::string b = stemRussianWordUtf8("документ");
    DS_CHECK(a != b);
}
