#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace datasearch::core::pdfcrypto {

// What the PDF standard security handler needs to open a document that is
// encrypted but has no password for opening — the common case of a PDF that
// only forbids copying or printing (every viewer opens it without asking).
// Small plain implementations; checked against published test vectors and
// against Python's hashlib (see tests).
std::array<std::uint8_t, 16> md5(std::string_view data);
std::array<std::uint8_t, 32> sha256(std::string_view data);
std::array<std::uint8_t, 48> sha384(std::string_view data);
std::array<std::uint8_t, 64> sha512(std::string_view data);
std::string rc4(std::string_view key, std::string_view data);

class Aes {
public:
    explicit Aes(std::string_view key);  // 16 or 32 bytes (AES-128 / AES-256)
    void encryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const;
    void decryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const;

private:
    int rounds_ = 0;
    std::array<std::uint8_t, 240> roundKeys_{};
};

// CBC without padding; `data` must be a whole number of 16-byte blocks.
std::string aesCbcEncrypt(std::string_view key, std::string_view iv, std::string_view data);
// CBC; nullopt if `data` isn't whole blocks or (with `removePadding`) its
// PKCS#7 padding is malformed.
std::optional<std::string> aesCbcDecrypt(std::string_view key, std::string_view iv, std::string_view data,
                                         bool removePadding);

// The parts of an /Encrypt dictionary (standard security handler) needed to
// decrypt streams.
struct PdfEncryption {
    enum class Cipher { Rc4, AesV2, AesV3, None };
    int v = 0;
    int r = 0;
    int keyBytes = 5;
    std::string o, u, oe, ue;
    std::int32_t p = 0;
    bool encryptMetadata = true;
    std::string id;  // the first string of the trailer's /ID
    Cipher streamCipher = Cipher::Rc4;
};

// The file's key if it opens with an empty user password (checked against
// /U, as a viewer does), or nullopt if a real password is needed or the
// revision isn't one this knows (2-6).
std::optional<std::string> fileKeyForEmptyPassword(const PdfEncryption& encryption);

// One stream's bytes decrypted (RC4/AES-128 use a key derived per object
// from its number and generation; AES-256 the file key itself).
std::optional<std::string> decryptStream(const PdfEncryption& encryption, const std::string& fileKey,
                                         std::uint32_t objectNumber, std::uint32_t generation, std::string_view data);

} // namespace datasearch::core::pdfcrypto
