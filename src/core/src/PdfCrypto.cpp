#include "datasearch/core/PdfCrypto.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace datasearch::core::pdfcrypto {

namespace {

std::uint32_t rotl32(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
std::uint32_t rotr32(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
std::uint64_t rotr64(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

std::string_view view(const std::uint8_t* bytes, std::size_t size) {
    return std::string_view(reinterpret_cast<const char*>(bytes), size);
}

// --- MD5 (RFC 1321) ---------------------------------------------------------

constexpr std::uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr int kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                           5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                           4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                           6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

// --- SHA-2 (FIPS 180-4) -----------------------------------------------------

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint64_t kSha512K[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538,
    0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe,
    0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
    0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab,
    0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed,
    0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
    0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8, 0x1e376c085141ab53,
    0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373,
    0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c,
    0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6,
    0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
    0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817};

// The message padded to whole blocks, its bit length stored big-endian in
// the last `lengthBytes` (SHA-2) or little-endian in the last 8 (MD5).
std::vector<std::uint8_t> padded(std::string_view data, std::size_t block, std::size_t lengthBytes, bool littleEndian) {
    std::vector<std::uint8_t> out(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    out.push_back(0x80);
    while (out.size() % block != block - lengthBytes) out.push_back(0);
    for (std::size_t i = 0; i < lengthBytes; ++i) {
        const std::size_t shift = littleEndian ? i : lengthBytes - 1 - i;
        out.push_back(shift < 8 ? static_cast<std::uint8_t>(bits >> (8 * shift)) : 0);
    }
    return out;
}

std::array<std::uint64_t, 8> sha512Blocks(std::string_view data, std::array<std::uint64_t, 8> h) {
    const auto msg = padded(data, 128, 16, false);
    std::uint64_t w[80];
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 128) {
        for (int i = 0; i < 16; ++i) {
            w[i] = 0;
            for (int b = 0; b < 8; ++b) w[i] = (w[i] << 8) | msg[chunk + 8 * i + b];
        }
        for (int i = 16; i < 80; ++i) {
            const std::uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const std::uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; ++i) {
            const std::uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
            const std::uint64_t ch = (e & f) ^ (~e & g);
            const std::uint64_t t1 = hh + S1 + ch + kSha512K[i] + w[i];
            const std::uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
            const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint64_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    return h;
}

// --- AES (FIPS 197) ---------------------------------------------------------

struct AesTables {
    std::uint8_t sbox[256];
    std::uint8_t inverse[256];
    AesTables() {
        // S-box generated rather than typed in: each byte's inverse in
        // GF(2^8), then the affine map (the construction FIPS 197 gives).
        std::uint8_t p = 1, q = 1;
        do {
            p = static_cast<std::uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));  // p * 3
            q ^= static_cast<std::uint8_t>(q << 1);                                  // q / 3
            q ^= static_cast<std::uint8_t>(q << 2);
            q ^= static_cast<std::uint8_t>(q << 4);
            if (q & 0x80) q ^= 0x09;
            auto rotl8 = [](std::uint8_t x, int s) { return static_cast<std::uint8_t>((x << s) | (x >> (8 - s))); };
            sbox[p] = static_cast<std::uint8_t>(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inverse[sbox[i]] = static_cast<std::uint8_t>(i);
    }
};

const AesTables& aesTables() {
    static const AesTables tables;
    return tables;
}

std::uint8_t xtime(std::uint8_t x) { return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

std::uint8_t gmul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t result = 0;
    while (b) {
        if (b & 1) result ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return result;
}

// --- PDF standard security handler (ISO 32000-2, 7.6.4) -----------------------

constexpr std::uint8_t kPasswordPadding[32] = {0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E,
                                               0x56, 0xFF, 0xFA, 0x01, 0x08, 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68,
                                               0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};

template <std::size_t N>
std::string str(const std::array<std::uint8_t, N>& a, std::size_t size = N) {
    return std::string(reinterpret_cast<const char*>(a.data()), std::min(size, N));
}

// Algorithm 2: the file key of revisions 2-4, for the empty password.
std::string rc4FileKey(const PdfEncryption& e) {
    std::string input(reinterpret_cast<const char*>(kPasswordPadding), 32);
    input += e.o.substr(0, 32);
    const auto p = static_cast<std::uint32_t>(e.p);
    for (int i = 0; i < 4; ++i) input.push_back(static_cast<char>((p >> (8 * i)) & 0xFF));
    input += e.id;
    if (e.r >= 4 && !e.encryptMetadata) input += std::string(4, '\xFF');
    const std::size_t n = e.r == 2 ? 5 : static_cast<std::size_t>(std::clamp(e.keyBytes, 5, 16));
    auto hash = md5(input);
    if (e.r >= 3) {
        for (int i = 0; i < 50; ++i) hash = md5(str(hash, n));
    }
    return str(hash, n);
}

// Algorithms 6 and 7: does the empty password match /U?
bool rc4UserPasswordMatches(const PdfEncryption& e, const std::string& key) {
    const std::string padding(reinterpret_cast<const char*>(kPasswordPadding), 32);
    if (e.r == 2) return e.u.size() >= 32 && rc4(key, padding) == e.u.substr(0, 32);
    std::string x = str(md5(padding + e.id));
    x = rc4(key, x);
    for (int i = 1; i <= 19; ++i) {
        std::string k = key;
        for (char& c : k) c = static_cast<char>(static_cast<std::uint8_t>(c) ^ i);
        x = rc4(k, x);
    }
    return e.u.size() >= 16 && x == e.u.substr(0, 16);
}

// Algorithm 2.B (revision 6): the hash of a password, salt and user data.
std::string hash2B(std::string_view password, std::string_view salt, std::string_view userData) {
    std::string k = str(sha256(std::string(password) + std::string(salt) + std::string(userData)));
    std::string e;
    for (int round = 0; round < 64 || static_cast<std::uint8_t>(e.back()) > round - 32; ++round) {
        std::string once = std::string(password) + k + std::string(userData);
        std::string k1;
        k1.reserve(once.size() * 64);
        for (int i = 0; i < 64; ++i) k1 += once;
        e = aesCbcEncrypt(std::string_view(k).substr(0, 16), std::string_view(k).substr(16, 16), k1);
        unsigned sum = 0;
        for (int i = 0; i < 16; ++i) sum += static_cast<std::uint8_t>(e[i]);
        switch (sum % 3) {
            case 0: k = str(sha256(e)); break;
            case 1: k = str(sha384(e)); break;
            default: k = str(sha512(e)); break;
        }
    }
    return k.substr(0, 32);
}

} // namespace

std::array<std::uint8_t, 16> md5(std::string_view data) {
    const auto msg = padded(data, 64, 8, true);
    std::uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(msg[chunk + 4 * i]) | (static_cast<std::uint32_t>(msg[chunk + 4 * i + 1]) << 8) |
                   (static_cast<std::uint32_t>(msg[chunk + 4 * i + 2]) << 16) |
                   (static_cast<std::uint32_t>(msg[chunk + 4 * i + 3]) << 24);
        }
        std::uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            f = f + a + kMd5K[i] + m[g];
            a = d;
            d = c;
            c = b;
            b = b + rotl32(f, kMd5S[i]);
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    }
    std::array<std::uint8_t, 16> out{};
    const std::uint32_t words[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(words[i / 4] >> (8 * (i % 4)));
    return out;
}

std::array<std::uint8_t, 32> sha256(std::string_view data) {
    const auto msg = padded(data, 64, 8, false);
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint32_t w[64];
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(msg[chunk + 4 * i]) << 24) |
                   (static_cast<std::uint32_t>(msg[chunk + 4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(msg[chunk + 4 * i + 2]) << 8) | msg[chunk + 4 * i + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + kSha256K[i] + w[i];
            const std::uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 32; ++i) out[i] = static_cast<std::uint8_t>(h[i / 4] >> (24 - 8 * (i % 4)));
    return out;
}

std::array<std::uint8_t, 64> sha512(std::string_view data) {
    const auto h = sha512Blocks(data, {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                                       0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179});
    std::array<std::uint8_t, 64> out{};
    for (int i = 0; i < 64; ++i) out[i] = static_cast<std::uint8_t>(h[i / 8] >> (56 - 8 * (i % 8)));
    return out;
}

std::array<std::uint8_t, 48> sha384(std::string_view data) {
    const auto h = sha512Blocks(data, {0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
                                       0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4});
    std::array<std::uint8_t, 48> out{};
    for (int i = 0; i < 48; ++i) out[i] = static_cast<std::uint8_t>(h[i / 8] >> (56 - 8 * (i % 8)));
    return out;
}

std::string rc4(std::string_view key, std::string_view data) {
    std::uint8_t s[256];
    for (int i = 0; i < 256; ++i) s[i] = static_cast<std::uint8_t>(i);
    if (!key.empty()) {
        std::uint8_t j = 0;
        for (int i = 0; i < 256; ++i) {
            j = static_cast<std::uint8_t>(j + s[i] + static_cast<std::uint8_t>(key[i % key.size()]));
            std::swap(s[i], s[j]);
        }
    }
    std::string out(data);
    std::uint8_t i = 0, j = 0;
    for (char& c : out) {
        i = static_cast<std::uint8_t>(i + 1);
        j = static_cast<std::uint8_t>(j + s[i]);
        std::swap(s[i], s[j]);
        c = static_cast<char>(static_cast<std::uint8_t>(c) ^ s[static_cast<std::uint8_t>(s[i] + s[j])]);
    }
    return out;
}

Aes::Aes(std::string_view key) {
    const auto& t = aesTables();
    const int nk = key.size() == 32 ? 8 : 4;
    rounds_ = nk + 6;
    const int words = 4 * (rounds_ + 1);
    std::uint8_t w[240];
    std::memcpy(w, key.data(), static_cast<std::size_t>(4 * nk));
    std::uint8_t rcon = 1;
    for (int i = nk; i < words; ++i) {
        std::uint8_t temp[4] = {w[4 * (i - 1)], w[4 * (i - 1) + 1], w[4 * (i - 1) + 2], w[4 * (i - 1) + 3]};
        if (i % nk == 0) {
            const std::uint8_t first = temp[0];  // RotWord, SubWord, Rcon
            temp[0] = static_cast<std::uint8_t>(t.sbox[temp[1]] ^ rcon);
            temp[1] = t.sbox[temp[2]];
            temp[2] = t.sbox[temp[3]];
            temp[3] = t.sbox[first];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (auto& b : temp) b = t.sbox[b];
        }
        for (int b = 0; b < 4; ++b) w[4 * i + b] = static_cast<std::uint8_t>(w[4 * (i - nk) + b] ^ temp[b]);
    }
    std::memcpy(roundKeys_.data(), w, static_cast<std::size_t>(4 * words));
}

void Aes::encryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const {
    const auto& t = aesTables();
    std::uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = static_cast<std::uint8_t>(in[i] ^ roundKeys_[i]);
    for (int round = 1; round <= rounds_; ++round) {
        std::uint8_t n[16];
        // SubBytes + ShiftRows: row r of column c comes from column c + r.
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) n[4 * c + r] = t.sbox[s[4 * ((c + r) % 4) + r]];
        }
        if (round != rounds_) {
            for (int c = 0; c < 4; ++c) {
                const std::uint8_t a0 = n[4 * c], a1 = n[4 * c + 1], a2 = n[4 * c + 2], a3 = n[4 * c + 3];
                n[4 * c] = static_cast<std::uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                n[4 * c + 1] = static_cast<std::uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                n[4 * c + 2] = static_cast<std::uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                n[4 * c + 3] = static_cast<std::uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        for (int i = 0; i < 16; ++i) s[i] = static_cast<std::uint8_t>(n[i] ^ roundKeys_[16 * round + i]);
    }
    std::memcpy(out, s, 16);
}

void Aes::decryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const {
    const auto& t = aesTables();
    std::uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = static_cast<std::uint8_t>(in[i] ^ roundKeys_[16 * rounds_ + i]);
    for (int round = rounds_ - 1; round >= 0; --round) {
        std::uint8_t n[16];
        // InvShiftRows + InvSubBytes: row r of column c goes back to column c + r.
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) n[4 * ((c + r) % 4) + r] = t.inverse[s[4 * c + r]];
        }
        for (int i = 0; i < 16; ++i) n[i] = static_cast<std::uint8_t>(n[i] ^ roundKeys_[16 * round + i]);
        if (round != 0) {
            for (int c = 0; c < 4; ++c) {
                const std::uint8_t a0 = n[4 * c], a1 = n[4 * c + 1], a2 = n[4 * c + 2], a3 = n[4 * c + 3];
                n[4 * c] = static_cast<std::uint8_t>(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                n[4 * c + 1] = static_cast<std::uint8_t>(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                n[4 * c + 2] = static_cast<std::uint8_t>(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                n[4 * c + 3] = static_cast<std::uint8_t>(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
        std::memcpy(s, n, 16);
    }
    std::memcpy(out, s, 16);
}

std::string aesCbcEncrypt(std::string_view key, std::string_view iv, std::string_view data) {
    const Aes aes(key);
    std::string out(data.size(), '\0');
    std::uint8_t chain[16];
    std::memcpy(chain, iv.data(), 16);
    for (std::size_t off = 0; off + 16 <= data.size(); off += 16) {
        std::uint8_t block[16];
        for (int i = 0; i < 16; ++i) block[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(data[off + i]) ^ chain[i]);
        aes.encryptBlock(block, chain);
        std::memcpy(out.data() + off, chain, 16);
    }
    return out;
}

std::optional<std::string> aesCbcDecrypt(std::string_view key, std::string_view iv, std::string_view data,
                                         bool removePadding) {
    if (data.size() % 16 != 0 || iv.size() != 16) return std::nullopt;
    const Aes aes(key);
    std::string out(data.size(), '\0');
    std::uint8_t chain[16];
    std::memcpy(chain, iv.data(), 16);
    for (std::size_t off = 0; off < data.size(); off += 16) {
        std::uint8_t plain[16];
        aes.decryptBlock(reinterpret_cast<const std::uint8_t*>(data.data() + off), plain);
        for (int i = 0; i < 16; ++i) out[off + i] = static_cast<char>(plain[i] ^ chain[i]);
        std::memcpy(chain, data.data() + off, 16);
    }
    if (removePadding) {
        if (out.empty()) return out;
        const auto pad = static_cast<std::uint8_t>(out.back());
        if (pad == 0 || pad > 16 || pad > out.size()) return std::nullopt;
        out.resize(out.size() - pad);
    }
    return out;
}

std::optional<std::string> fileKeyForEmptyPassword(const PdfEncryption& e) {
    if (e.r >= 2 && e.r <= 4) {
        std::string key = rc4FileKey(e);
        if (!rc4UserPasswordMatches(e, key)) return std::nullopt;
        return key;
    }
    if (e.r == 5 || e.r == 6) {
        if (e.u.size() < 48 || e.ue.size() < 32) return std::nullopt;
        const std::string_view u = e.u;
        const std::string_view validationSalt = u.substr(32, 8);
        const std::string_view keySalt = u.substr(40, 8);
        // Revision 5 (Adobe's first AES-256): plain SHA-256; 6: Algorithm 2.B.
        const std::string check = e.r == 5 ? str(sha256(validationSalt)) : hash2B({}, validationSalt, {});
        if (check != u.substr(0, 32)) return std::nullopt;
        const std::string intermediate = e.r == 5 ? str(sha256(keySalt)) : hash2B({}, keySalt, {});
        return aesCbcDecrypt(intermediate, std::string(16, '\0'), std::string_view(e.ue).substr(0, 32), false);
    }
    return std::nullopt;
}

std::optional<std::string> decryptStream(const PdfEncryption& e, const std::string& fileKey, std::uint32_t objectNumber,
                                         std::uint32_t generation, std::string_view data) {
    if (e.streamCipher == PdfEncryption::Cipher::None) return std::string(data);
    if (e.streamCipher == PdfEncryption::Cipher::AesV3) {
        if (data.size() < 16) return std::nullopt;
        return aesCbcDecrypt(fileKey, data.substr(0, 16), data.substr(16), true);
    }
    // Algorithm 1: a key of the file key + object number + generation.
    std::string input = fileKey;
    for (int i = 0; i < 3; ++i) input.push_back(static_cast<char>((objectNumber >> (8 * i)) & 0xFF));
    for (int i = 0; i < 2; ++i) input.push_back(static_cast<char>((generation >> (8 * i)) & 0xFF));
    if (e.streamCipher == PdfEncryption::Cipher::AesV2) input += "sAlT";
    const std::string objectKey = str(md5(input), std::min<std::size_t>(fileKey.size() + 5, 16));
    if (e.streamCipher == PdfEncryption::Cipher::Rc4) return rc4(objectKey, data);
    if (data.size() < 16) return std::nullopt;
    return aesCbcDecrypt(objectKey, data.substr(0, 16), data.substr(16), true);
}

} // namespace datasearch::core::pdfcrypto
