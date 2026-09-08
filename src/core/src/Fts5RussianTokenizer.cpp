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
};

struct RussianTokenizerContext {
    fts5_tokenizer inner{};
    Fts5Tokenizer* innerInstance = nullptr;
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

int xCreate(void* pUserData, const char** azArg, int nArg, Fts5Tokenizer** ppOut) {
    auto* registration = static_cast<TokenizerRegistration*>(pUserData);
    auto* ctx = new RussianTokenizerContext();
    ctx->inner = registration->inner;

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
    return ctx->inner.xTokenize(ctx->innerInstance, &forwardCtx, flags, pText, nText, forwardStemmedToken);
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

    auto* registration = new TokenizerRegistration();
    if (api->xFindTokenizer(api, "unicode61", &registration->innerUserData, &registration->inner) != SQLITE_OK) {
        delete registration;
        return false;
    }

    fts5_tokenizer ourVtable{};
    ourVtable.xCreate = &xCreate;
    ourVtable.xDelete = &xDelete;
    ourVtable.xTokenize = &xTokenize;

    const int rc = api->xCreateTokenizer(
        api, "ru_snowball", registration, &ourVtable,
        [](void* p) { delete static_cast<TokenizerRegistration*>(p); });
    if (rc != SQLITE_OK) {
        delete registration;
        return false;
    }
    return true;
}

} // namespace datasearch::core
