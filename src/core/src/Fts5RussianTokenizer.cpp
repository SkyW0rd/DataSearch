#include "datasearch/core/Fts5RussianTokenizer.h"

#include "datasearch/core/RussianStemmer.h"

#include <sqlite3.h>

#include <string>

namespace datasearch::core {

namespace {

// Holds the wrapped "unicode61" tokenizer's vtable + its own context, so our
// tokenizer instances can delegate word-splitting/casefolding to it.
struct TokenizerRegistration {
    fts5_tokenizer inner{};
    void* innerUserData = nullptr;
    bool stem = true;  // false: "ru_exact" — casefold and ё→е only
};

struct RussianTokenizerContext {
    fts5_tokenizer inner{};
    Fts5Tokenizer* innerInstance = nullptr;
    bool stem = true;
};

// Bridges the inner (unicode61) tokenizer's xToken callback: stems each token
// before forwarding it to whatever callback FTS5 originally asked for.
struct ForwardCallbackContext {
    void* outerCtx = nullptr;
    int (*outerXToken)(void*, int, const char*, int, int, int) = nullptr;
};

int forwardStemmedToken(void* pCtx, int tflags, const char* pToken, int nToken, int iStart, int iEnd) {
    auto* ctx = static_cast<ForwardCallbackContext*>(pCtx);
    const std::string stemmed = stemRussianWordUtf8(std::string(pToken, static_cast<std::size_t>(nToken)));
    return ctx->outerXToken(ctx->outerCtx, tflags, stemmed.data(), static_cast<int>(stemmed.size()), iStart, iEnd);
}

// The same token with every "ё" folded to "е" (both two bytes in UTF-8, so in
// place) — Russian text uses the two interchangeably, and a search for
// "еще" must find "ещё". unicode61 has already lowercased it.
int forwardExactToken(void* pCtx, int tflags, const char* pToken, int nToken, int iStart, int iEnd) {
    auto* ctx = static_cast<ForwardCallbackContext*>(pCtx);
    std::string token(pToken, static_cast<std::size_t>(nToken));
    for (std::size_t i = 0; i + 1 < token.size(); ++i) {
        if (static_cast<unsigned char>(token[i]) == 0xD1 && static_cast<unsigned char>(token[i + 1]) == 0x91) {
            token[i] = static_cast<char>(0xD0);
            token[i + 1] = static_cast<char>(0xB5);
        }
    }
    return ctx->outerXToken(ctx->outerCtx, tflags, token.data(), static_cast<int>(token.size()), iStart, iEnd);
}

int xCreate(void* pUserData, const char** azArg, int nArg, Fts5Tokenizer** ppOut) {
    auto* registration = static_cast<TokenizerRegistration*>(pUserData);
    auto* ctx = new RussianTokenizerContext();
    ctx->inner = registration->inner;
    ctx->stem = registration->stem;

    const int rc = ctx->inner.xCreate(registration->innerUserData, azArg, nArg, &ctx->innerInstance);
    if (rc != SQLITE_OK) {
        delete ctx;
        return rc;
    }
    *ppOut = reinterpret_cast<Fts5Tokenizer*>(ctx);
    return SQLITE_OK;
}

void xDelete(Fts5Tokenizer* pTokenizer) {
    auto* ctx = reinterpret_cast<RussianTokenizerContext*>(pTokenizer);
    if (ctx == nullptr) return;
    ctx->inner.xDelete(ctx->innerInstance);
    delete ctx;
}

int xTokenize(Fts5Tokenizer* pTokenizer, void* pCtx, int flags, const char* pText, int nText,
              int (*xToken)(void*, int, const char*, int, int, int)) {
    auto* ctx = reinterpret_cast<RussianTokenizerContext*>(pTokenizer);
    ForwardCallbackContext forwardCtx{pCtx, xToken};
    return ctx->inner.xTokenize(ctx->innerInstance, &forwardCtx, flags, pText, nText,
                                ctx->stem ? forwardStemmedToken : forwardExactToken);
}

fts5_api* fetchFts5Api(sqlite3* db) {
    fts5_api* api = nullptr;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &stmt, nullptr) != SQLITE_OK) return nullptr;
    sqlite3_bind_pointer(stmt, 1, &api, "fts5_api_ptr", nullptr);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return api;
}

} // namespace

bool registerRussianFts5Tokenizer(sqlite3* db) {
    fts5_api* api = fetchFts5Api(db);
    if (api == nullptr) return false;

    for (const bool stem : {true, false}) {
        auto* registration = new TokenizerRegistration();
        registration->stem = stem;
        if (api->xFindTokenizer(api, "unicode61", &registration->innerUserData, &registration->inner) != SQLITE_OK) {
            delete registration;
            return false;
        }

        fts5_tokenizer ourVtable{};
        ourVtable.xCreate = &xCreate;
        ourVtable.xDelete = &xDelete;
        ourVtable.xTokenize = &xTokenize;

        const int rc = api->xCreateTokenizer(
            api, stem ? "ru_snowball" : "ru_exact", registration, &ourVtable,
            [](void* p) { delete static_cast<TokenizerRegistration*>(p); });
        if (rc != SQLITE_OK) {
            delete registration;
            return false;
        }
    }
    return true;
}

} // namespace datasearch::core
