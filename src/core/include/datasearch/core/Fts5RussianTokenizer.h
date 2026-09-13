#pragma once

struct sqlite3;

namespace datasearch::core {

// Registers an FTS5 tokenizer named "ru_snowball" on `db`: it wraps the
// built-in "unicode61" tokenizer (word splitting + casefolding, already
// correct for Cyrillic per ТЗ п.11.3) and additionally runs each resulting
// token through the Russian Snowball stemmer (see RussianStemmer.h), so a
// query for "практикум" also matches "практикума", "практикумом", etc.
//
// Also registers "ru_exact": the same word splitting and casefolding with
// "ё" folded to "е", but no stemming — whole words must match as typed
// ("Михайлов" is not "Михайлова"), still regardless of case.
//
// FTS5 tokenizers are registered per-connection, not stored in the database
// file, so this must be called on every sqlite3* connection before it creates
// or opens a `files_fts` table that references 'ru_snowball'. Returns false
// if this SQLite build lacks FTS5 (extremely unlikely for the SQLite version
// this project targets) or registration otherwise fails.
bool registerRussianFts5Tokenizer(sqlite3* db);

} // namespace datasearch::core
