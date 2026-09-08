#pragma once

#include <string>

namespace datasearch::core {

// Russian Snowball stemmer (https://snowballstem.org/algorithms/russian/stemmer.html),
// implemented directly against Unicode codepoints (no dependency on the
// vendored libstemmer_c project). Operates on a single already-lowercased
// UTF-8 word; words that aren't (entirely) lowercase Cyrillic are returned
// unchanged — this is meant to be applied per-token by the FTS5 tokenizer
// (see Fts5RussianTokenizer), where token boundaries/casefolding are already
// handled by the wrapped unicode61 tokenizer (ТЗ п.11.3).
std::string stemRussianWordUtf8(const std::string& word);

} // namespace datasearch::core
