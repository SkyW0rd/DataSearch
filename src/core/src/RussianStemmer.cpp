#include "datasearch/core/RussianStemmer.h"

#include <vector>

namespace datasearch::core {

namespace {

using U32 = std::u32string;

// Cyrillic lowercase letter codepoints, written as \u escapes (interpreted by
// the compiler itself via universal-character-names, independent of the
// source file's or the target compiler's execution charset — MSVC in
// particular needs /utf-8 to interpret literal Cyrillic source text
// correctly, so this sidesteps that entirely).
constexpr char32_t kA = 0x0430;
constexpr char32_t kYa = 0x044F;

void appendUtf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool isVowel(char32_t c) {
    static const U32 vowels = U"аеиоуыэюя";  // а е и о у ы э ю я
    return vowels.find(c) != U32::npos;
}

bool endsWith(const U32& word, const U32& suffix) {
    if (suffix.size() > word.size()) return false;
    return word.compare(word.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::size_t suffixStart(const U32& word, const U32& suffix) {
    return word.size() - suffix.size();
}

struct EndingRule {
    U32 suffix;
    bool requiresPrecedingAYa;
};

std::vector<EndingRule> plain(std::initializer_list<const char32_t*> literals) {
    std::vector<EndingRule> rules;
    for (const auto* lit : literals) rules.push_back({U32(lit), false});
    return rules;
}

// Finds the longest ending among `rules` whose start lies at/after
// `regionStart`, honouring each rule's preceding-а/я requirement, and removes
// it. Returns true if something was removed.
bool applyEndingRules(U32& word, const std::vector<EndingRule>& rules, std::size_t regionStart) {
    const EndingRule* best = nullptr;
    for (const auto& rule : rules) {
        if (!endsWith(word, rule.suffix)) continue;
        const std::size_t start = suffixStart(word, rule.suffix);
        if (start < regionStart) continue;
        if (rule.requiresPrecedingAYa) {
            if (start == 0) continue;
            const char32_t prev = word[start - 1];
            if (prev != kA && prev != kYa) continue;
        }
        if (best == nullptr || rule.suffix.size() > best->suffix.size()) best = &rule;
    }
    if (best == nullptr) return false;
    word.erase(suffixStart(word, best->suffix));
    return true;
}

// ---------------------------------------------------------------------------
// Suffix tables (https://snowballstem.org/algorithms/russian/stemmer.html).
// ---------------------------------------------------------------------------

const std::vector<EndingRule>& perfectiveGerund() {
    static const std::vector<EndingRule> rules = {
        {U"в", true}, {U"вши", true}, {U"вшись", true},
        {U"ив", false}, {U"ыв", false},
        {U"ивши", false}, {U"ывши", false},
        {U"ившись", false}, {U"ывшись", false},
    };
    return rules;
}

const std::vector<EndingRule>& reflexive() {
    static const std::vector<EndingRule> rules = {{U"ся", false}, {U"сь", false}};
    return rules;
}

const std::vector<U32>& adjective() {
    static const std::vector<U32> list = {
        U"ее", U"ие", U"ые", U"ое", U"ими", U"ыми",
        U"ей", U"ий", U"ый", U"ой", U"ем", U"им",
        U"ым", U"ом", U"его", U"ого", U"ему",
        U"ому", U"их", U"ых", U"ую", U"юю", U"ая",
        U"яя", U"ою", U"ею",
    };
    return list;
}

const std::vector<EndingRule>& participle() {
    static const std::vector<EndingRule> rules = {
        {U"ем", true}, {U"нн", true}, {U"вш", true}, {U"ющ", true},
        {U"щ", true},
        {U"ивш", false}, {U"ывш", false}, {U"ующ", false},
    };
    return rules;
}

const std::vector<EndingRule>& verb() {
    static const std::vector<EndingRule> rules = {
        {U"ла", true}, {U"на", true}, {U"ете", true}, {U"йте", true},
        {U"ли", true}, {U"й", true}, {U"л", true}, {U"ем", true}, {U"н", true},
        {U"ло", true}, {U"но", true}, {U"ет", true}, {U"ют", true},
        {U"ны", true}, {U"ть", true}, {U"ешь", true}, {U"нно", true},
        {U"ила", false}, {U"ыла", false}, {U"ена", false},
        {U"ейте", false}, {U"уйте", false}, {U"ите", false},
        {U"или", false}, {U"ыли", false}, {U"ей", false}, {U"уй", false},
        {U"ил", false}, {U"ыл", false}, {U"им", false}, {U"ым", false},
        {U"ен", false}, {U"ило", false}, {U"ыло", false},
        {U"ено", false}, {U"ят", false}, {U"ует", false},
        {U"уют", false}, {U"ит", false}, {U"ыт", false}, {U"ены", false},
        {U"ить", false}, {U"ыть", false}, {U"ишь", false},
        {U"ую", false}, {U"ю", false},
    };
    return rules;
}

const std::vector<U32>& noun() {
    static const std::vector<U32> list = {
        U"а", U"ев", U"ов", U"ие", U"ье", U"е",
        U"иями", U"ями", U"ами", U"еи", U"ии",
        U"и", U"ией", U"ей", U"ой", U"ий", U"й",
        U"иям", U"ям", U"ием", U"ем", U"ам", U"ом",
        U"о", U"у", U"ах", U"иях", U"ях", U"ы", U"ь",
        U"ию", U"ью", U"ю", U"ия", U"ья", U"я",
    };
    return list;
}

// ---------------------------------------------------------------------------
// Regions.
// ---------------------------------------------------------------------------

struct Regions {
    std::size_t rv;
    std::size_t r1;
    std::size_t r2;
};

std::size_t findRegionAfterVowelNonVowel(const U32& word, std::size_t start) {
    std::size_t i = start;
    while (i < word.size() && !isVowel(word[i])) ++i;
    if (i >= word.size()) return word.size();
    ++i;
    while (i < word.size() && isVowel(word[i])) ++i;
    if (i >= word.size()) return word.size();
    return i + 1;
}

Regions computeRegions(const U32& word) {
    Regions r{};
    std::size_t i = 0;
    while (i < word.size() && !isVowel(word[i])) ++i;
    r.rv = (i < word.size()) ? i + 1 : word.size();
    r.r1 = findRegionAfterVowelNonVowel(word, 0);
    r.r2 = findRegionAfterVowelNonVowel(word, r.r1);
    return r;
}

// ---------------------------------------------------------------------------
// The four steps.
// ---------------------------------------------------------------------------

bool tryAdjectival(U32& word, std::size_t rv) {
    const EndingRule* bestParticiple = nullptr;
    const U32* bestAdjective = nullptr;
    std::size_t bestLen = 0;

    for (const auto& adj : adjective()) {
        if (!endsWith(word, adj)) continue;
        const U32 stem = word.substr(0, word.size() - adj.size());
        for (const auto& part : participle()) {
            if (!endsWith(stem, part.suffix)) continue;
            const std::size_t combinedStart = stem.size() - part.suffix.size();
            if (combinedStart < rv) continue;
            if (part.requiresPrecedingAYa) {
                if (combinedStart == 0) continue;
                const char32_t prev = stem[combinedStart - 1];
                if (prev != kA && prev != kYa) continue;
            }
            const std::size_t totalLen = part.suffix.size() + adj.size();
            if (totalLen > bestLen) {
                bestLen = totalLen;
                bestParticiple = &part;
                bestAdjective = &adj;
            }
        }
    }
    if (bestLen > 0) {
        word.erase(word.size() - bestLen);
        return true;
    }
    (void)bestParticiple;
    (void)bestAdjective;

    // No participle+adjective combo: try a plain adjective ending alone.
    const U32* best = nullptr;
    for (const auto& adj : adjective()) {
        if (!endsWith(word, adj)) continue;
        if (suffixStart(word, adj) < rv) continue;
        if (best == nullptr || adj.size() > best->size()) best = &adj;
    }
    if (best == nullptr) return false;
    word.erase(suffixStart(word, *best));
    return true;
}

void step1(U32& word, const Regions& r) {
    if (applyEndingRules(word, perfectiveGerund(), r.rv)) return;

    applyEndingRules(word, reflexive(), r.rv);  // removed if present either way

    if (tryAdjectival(word, r.rv)) return;
    if (applyEndingRules(word, verb(), r.rv)) return;

    const U32* best = nullptr;
    for (const auto& n : noun()) {
        if (!endsWith(word, n)) continue;
        if (suffixStart(word, n) < r.rv) continue;
        if (best == nullptr || n.size() > best->size()) best = &n;
    }
    if (best != nullptr) word.erase(suffixStart(word, *best));
}

void step2(U32& word, const Regions& r) {
    static const U32 kI = U"и";  // и
    if (endsWith(word, kI) && suffixStart(word, kI) >= r.rv) {
        word.erase(word.size() - 1);
    }
}

void step3(U32& word, const Regions& r) {
    static const std::vector<U32> kDerivational = {U"ость", U"ост"};  // ость, ост
    const U32* best = nullptr;
    for (const auto& e : kDerivational) {
        if (!endsWith(word, e)) continue;
        if (suffixStart(word, e) < r.r2) continue;
        if (best == nullptr || e.size() > best->size()) best = &e;
    }
    if (best != nullptr) word.erase(suffixStart(word, *best));
}

void step4(U32& word, const Regions& r) {
    static const U32 kEyshe = U"ейше";  // ейше
    static const U32 kEysh = U"ейш";         // ейш
    static const U32 kNN = U"нн";                 // нн
    static const U32 kSoft = U"ь";                     // ь

    bool superlativeRemoved = false;
    if (endsWith(word, kEyshe) && suffixStart(word, kEyshe) >= r.rv) {
        word.erase(suffixStart(word, kEyshe));
        superlativeRemoved = true;
    } else if (endsWith(word, kEysh) && suffixStart(word, kEysh) >= r.rv) {
        word.erase(suffixStart(word, kEysh));
        superlativeRemoved = true;
    }

    if (superlativeRemoved) {
        if (endsWith(word, kNN) && suffixStart(word, kNN) >= r.rv) word.erase(word.size() - 1);
        return;
    }
    if (endsWith(word, kNN) && suffixStart(word, kNN) >= r.rv) {
        word.erase(word.size() - 1);
        return;
    }
    if (endsWith(word, kSoft) && suffixStart(word, kSoft) >= r.rv) {
        word.erase(word.size() - 1);
    }
}

} // namespace

std::string stemRussianWordUtf8(const std::string& wordUtf8) {
    U32 word;
    word.reserve(wordUtf8.size());
    bool allCyrillic = !wordUtf8.empty();

    std::size_t i = 0;
    while (i < wordUtf8.size()) {
        const unsigned char c0 = static_cast<unsigned char>(wordUtf8[i]);
        char32_t cp;
        std::size_t len;
        if (c0 < 0x80) {
            cp = c0;
            len = 1;
        } else if ((c0 & 0xE0) == 0xC0 && i + 1 < wordUtf8.size()) {
            cp = static_cast<char32_t>((c0 & 0x1F) << 6) |
                 static_cast<char32_t>(static_cast<unsigned char>(wordUtf8[i + 1]) & 0x3F);
            len = 2;
        } else if ((c0 & 0xF0) == 0xE0 && i + 2 < wordUtf8.size()) {
            cp = static_cast<char32_t>((c0 & 0x0F) << 12) |
                 static_cast<char32_t>((static_cast<unsigned char>(wordUtf8[i + 1]) & 0x3F) << 6) |
                 static_cast<char32_t>(static_cast<unsigned char>(wordUtf8[i + 2]) & 0x3F);
            len = 3;
        } else {
            return wordUtf8;  // 4-byte codepoints and malformed UTF-8: not Cyrillic, bail out unchanged
        }

        if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20;         // А-Я -> а-я
        else if (cp == 0x0401) cp = 0x0451;                    // Ё -> ё

        if (!((cp >= 0x0430 && cp <= 0x044F) || cp == 0x0451)) allCyrillic = false;
        word.push_back(cp);
        i += len;
    }

    if (!allCyrillic || word.size() < 3) return wordUtf8;

    for (auto& c : word) {
        if (c == 0x0451) c = 0x0435;  // treat ё as е for suffix purposes
    }

    const Regions r = computeRegions(word);
    step1(word, r);
    step2(word, r);
    step3(word, r);
    step4(word, r);

    std::string out;
    out.reserve(word.size() * 2);
    for (char32_t cp : word) appendUtf8(out, cp);
    return out;
}

} // namespace datasearch::core
