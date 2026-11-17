// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuzzy_catalog_index.hpp"

#include <waywal/path.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <sstream>
#include <system_error>
#include <thread>

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define WWAL_HAVE_X86_RUNTIME_AVX2 1
#else
#define WWAL_HAVE_X86_RUNTIME_AVX2 0
#endif

namespace wwal::studio {

namespace {

inline char toLowerAscii(char c) noexcept
{
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c + ('a' - 'A'));
    }
    return c;
}

inline std::string toLowerString(std::string_view sv)
{
    std::string result;
    result.reserve(sv.size());
    for (char c : sv) {
        result.push_back(toLowerAscii(c));
    }
    return result;
}

inline void copyUtf8Safe(char *dst, size_t dstCap, std::string_view src) noexcept
{
    if (dstCap == 0U) {
        return;
    }
    const size_t maxBytes = dstCap - 1U;
    size_t len = std::min(src.size(), maxBytes);
    if (src.size() > maxBytes && len > 0U) {
        size_t cut = len;
        while (cut > 0U && (static_cast<uint8_t>(src[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        if (cut > 0U) {
            const uint8_t b = static_cast<uint8_t>(src[cut - 1U]);
            size_t expected = 1U;
            if ((b & 0xE0u) == 0xC0u) {
                expected = 2U;
            } else if ((b & 0xF0u) == 0xE0u) {
                expected = 3U;
            } else if ((b & 0xF8u) == 0xF0u) {
                expected = 4U;
            }
            if (expected > 1U && (cut - 1U) + expected > maxBytes) {
                cut = cut - 1U;
            }
        }
        len = cut;
    }
    if (len > 0U) {
        std::memcpy(dst, src.data(), len);
    }
    dst[len] = '\0';
}

inline uint16_t quantizeHueQ16(const CatalogItem &item) noexcept
{
    const float hueDeg = item.computeHue();
    const float norm = std::clamp(hueDeg / 360.0f, 0.0f, 1.0f);
    return static_cast<uint16_t>(std::lround(norm * 65535.0f));
}

inline uint64_t charBit64(unsigned char c) noexcept
{
    const unsigned char lc = static_cast<unsigned char>(toLowerAscii(static_cast<char>(c)));
    unsigned bit = 0U;
    if (lc >= 'a' && lc <= 'z') {
        bit = static_cast<unsigned>(lc - 'a');
    } else if (lc >= '0' && lc <= '9') {
        bit = 26U + static_cast<unsigned>(lc - '0');
    } else if (lc == ' ') {
        bit = 36U;
    } else if (lc == '_') {
        bit = 37U;
    } else if (lc == '-') {
        bit = 38U;
    } else if (lc == '/') {
        bit = 39U;
    } else if (lc == '.') {
        bit = 40U;
    } else {
        bit = 41U + (static_cast<unsigned>(lc) % 23U);
    }
    return 1ULL << bit;
}

struct CharMaskPair {
    uint64_t m1{0ULL};
    uint64_t m2{0ULL};
};

// Track whether each character class appears >=1 or >=2 times so repeated-letter queries reject
// insufficient multiplicity in O(1) without false negatives.
inline CharMaskPair computeAsciiCharMaskPair64(std::string_view sv) noexcept
{
    CharMaskPair out{};
    for (unsigned char c : sv) {
        const uint64_t bit = charBit64(c);
        out.m2 |= (out.m1 & bit);
        out.m1 |= bit;
    }
    return out;
}

inline uint64_t bigramBitSlice(unsigned char a, unsigned char b, uint64_t mult,
                               uint64_t salt) noexcept
{
    const uint64_t pair = (static_cast<uint64_t>(a) << 8U) | b | salt;
    const uint64_t h = ((pair * mult) >> 58U) & 63U;
    return 1ULL << h;
}

struct BigramMask3 {
    uint64_t m0{0ULL};
    uint64_t m1{0ULL};
    uint64_t m2{0ULL};
};

// Partition adjacent bigrams across three independent 64-bit filters so pat_len >= 6
// keeps false positives near ~5% instead of saturating a single 64-bit word.
inline BigramMask3 computeBigramMask3(std::string_view sv) noexcept
{
    BigramMask3 out{};
    if (sv.size() < 2U) {
        return out;
    }
    for (size_t i = 0; i + 1U < sv.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(sv[i]);
        const unsigned char b = static_cast<unsigned char>(sv[i + 1U]);
        out.m0 |= bigramBitSlice(a, b, 0x9E3779B97F4A7C15ULL, 0ULL);
        out.m1 |= bigramBitSlice(a, b, 0xBF58476D1CE4E5B9ULL, 0x10000ULL);
        out.m2 |= bigramBitSlice(a, b, 0x94D049BB133111EBULL, 0x20000ULL);
    }
    return out;
}

struct QueryBigramPieces {
    bool active{false};
    uint64_t q0_all{0ULL};
    uint64_t q0[3]{};
    uint64_t q1[3]{};
    uint64_t q2[3]{};
};

inline bool matchesAnyBigramPiece(const QueryBigramPieces &q, uint64_t b0, uint64_t b1,
                                  uint64_t b2) noexcept
{
    if (!q.active) {
        return true;
    }
    const uint64_t h0 = b0 & q.q0_all;
    if (h0 == 0ULL) {
        return false;
    }
    return (((h0 & q.q0[0]) != 0ULL) && ((b1 & q.q1[0]) != 0ULL) && ((b2 & q.q2[0]) != 0ULL)) ||
           (((h0 & q.q0[1]) != 0ULL) && ((b1 & q.q1[1]) != 0ULL) && ((b2 & q.q2[1]) != 0ULL)) ||
           (((h0 & q.q0[2]) != 0ULL) && ((b1 & q.q1[2]) != 0ULL) && ((b2 & q.q2[2]) != 0ULL));
}

inline char foldDigitCanon(char c) noexcept { return (c >= '0' && c <= '9') ? '0' : c; }

inline uint64_t hashSeamCanonDigits(std::string_view sv) noexcept
{
    uint64_t h = 14695981039346656037ULL;
    for (char c : sv) {
        h ^= static_cast<uint8_t>(foldDigitCanon(c));
        h *= 1099511628211ULL;
    }
    return h ^ (h >> 33U);
}

inline bool equalSeamCanonDigits(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (foldDigitCanon(a[i]) != foldDigitCanon(b[i])) {
            return false;
        }
    }
    return true;
}

inline bool isWordBoundaryChar(char ch) noexcept
{
    return ch == ' ' || ch == '_' || ch == '-' || ch == '/' || ch == '.';
}

// Compute standard SHA-256 digest using thumb_cache.h's inline block compressor so identical
// filename+size collisions across directories never drop distinct wallpapers (AUD-325).
std::string computeFileSha256Hex(const std::filesystem::path &p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) {
        return {};
    }
    uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint8_t block[64]{};
    uint64_t totalBytes = 0;
    size_t fill = 0;
    char buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        size_t n = static_cast<size_t>(f.gcount());
        totalBytes += n;
        size_t pos = 0;
        while (pos < n) {
            const size_t take = std::min(64U - fill, n - pos);
            std::memcpy(block + fill, buf + pos, take);
            fill += take;
            pos += take;
            if (fill == 64U) {
                waywal_thumb_sha256_block(state, block);
                fill = 0;
            }
        }
    }
    block[fill++] = 0x80u;
    if (fill > 56U) {
        while (fill < 64U) {
            block[fill++] = 0;
        }
        waywal_thumb_sha256_block(state, block);
        fill = 0;
    }
    while (fill < 56U) {
        block[fill++] = 0;
    }
    const uint64_t totalBits = totalBytes * 8ULL;
    for (int i = 0; i < 8; ++i) {
        block[56 + i] = static_cast<uint8_t>((totalBits >> ((7 - i) * 8)) & 0xFFu);
    }
    waywal_thumb_sha256_block(state, block);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; ++i) {
        for (int n = 0; n < 8; ++n) {
            out[static_cast<size_t>(i * 8 + n)] = kHex[(state[i] >> ((7 - n) * 4)) & 0x0Fu];
        }
    }
    return out;
}

uint32_t calculateSampleDominantColor(const std::filesystem::path &path)
{
    std::string s = path.string();
    uint32_t hash = 2166136261u;
    for (char c : s) {
        hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    uint8_t r = 60 + ((hash >> 16) % 140);
    uint8_t g = 60 + ((hash >> 8) % 140);
    uint8_t b = 70 + (hash % 140);
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

bool hasExtension(const std::filesystem::path &path, const std::vector<std::string> &exts)
{
    std::string ext = path.extension().string();
    for (char &c : ext)
        c = toLowerAscii(c);
    for (const auto &target : exts) {
        if (ext == target)
            return true;
    }
    return false;
}

// Handles queries longer than 64 bytes without truncation (AUD-159).
int scoreLongQuerySemiGlobal(std::string_view textLower, std::string_view patternLower)
{
    const size_t n = textLower.size();
    const size_t m = patternLower.size();
    if (m == 0 || n + 2U < m) {
        return -1;
    }
    if (const size_t pos = textLower.find(patternLower); pos != std::string_view::npos) {
        if (pos == 0U) {
            return 1000 - static_cast<int>(std::min<size_t>(n - m, 400U));
        }
        const bool wb = isWordBoundaryChar(textLower[pos - 1U]);
        const int base = wb ? 500 : 250;
        const size_t cap = wb ? 200U : 90U;
        return base - static_cast<int>(std::min<size_t>(pos, cap));
    }

    std::vector<uint32_t> prev(m + 1U);
    std::vector<uint32_t> curr(m + 1U);
    for (size_t j = 0U; j <= m; ++j) {
        prev[j] = static_cast<uint32_t>(j);
    }
    uint32_t best = static_cast<uint32_t>(m + 1U);
    for (size_t i = 0U; i < n; ++i) {
        curr[0] = 0U;
        const char ch = textLower[i];
        for (size_t j = 1U; j <= m; ++j) {
            const uint32_t cost = (ch == patternLower[j - 1U]) ? 0U : 1U;
            curr[j] = std::min({prev[j] + 1U, curr[j - 1U] + 1U, prev[j - 1U] + cost});
        }
        best = std::min(best, curr[m]);
        if (best == 0U) {
            return 300;
        }
        prev.swap(curr);
    }
    if (best <= 1U) {
        return 150;
    }
    if (best <= 2U) {
        return 80;
    }
    return -1;
}

#if WWAL_HAVE_X86_RUNTIME_AVX2
[[gnu::target("avx2")]] inline bool stepExact4Avx2(const std::string_view texts[4], size_t i,
                                                   size_t pat_len, uint64_t match_mask,
                                                   const __m256i &cm, __m256i &r, __m256i &seen,
                                                   const __m256i &mmask, const __m256i &zero,
                                                   int scores[4])
{
    (void)match_mask;
    r = _mm256_or_si256(_mm256_slli_epi64(r, 1), cm);
    seen = _mm256_and_si256(seen, cm);
    const __m256i matched = _mm256_cmpeq_epi64(_mm256_and_si256(r, mmask), zero);
    const int mask = _mm256_movemask_pd(_mm256_castsi256_pd(matched));
    if (mask != 0) {
        for (int k = 0; k < 4; ++k) {
            if (((mask >> k) & 1) != 0 && scores[k] < 0 && i < texts[k].size()) {
                if (i + 1U == pat_len) {
                    scores[k] =
                        1000 - static_cast<int>(std::min<size_t>(texts[k].size() - pat_len, 400U));
                } else {
                    const bool wb = (i >= pat_len) && isWordBoundaryChar(texts[k][i - pat_len]);
                    const int base = wb ? 500 : 250;
                    const size_t cap = wb ? 200U : 90U;
                    scores[k] = base - static_cast<int>(std::min<size_t>(i, cap));
                }
            }
        }
        return (scores[0] >= 0 && scores[1] >= 0 && scores[2] >= 0 && scores[3] >= 0);
    }
    return false;
}

[[gnu::target("avx2")]] inline void score4ItemsAVX2(const std::string_view texts[4],
                                                    const std::array<uint64_t, 256> &char_mask,
                                                    uint64_t match_mask, size_t pat_len,
                                                    int scores[4], uint64_t *outSeenBits = nullptr)
{
    for (int k = 0; k < 4; ++k)
        scores[k] = -1;

    size_t max_len = 0;
    for (int k = 0; k < 4; ++k) {
        if (texts[k].size() > max_len)
            max_len = texts[k].size();
    }
    if (max_len < pat_len) {
        if (outSeenBits) {
            for (int k = 0; k < 4; ++k)
                outSeenBits[k] = ~0ULL;
        }
        return;
    }

    __m256i r = _mm256_set1_epi64x(~0ULL);
    __m256i seen = _mm256_set1_epi64x(~0ULL);
    const __m256i mmask = _mm256_set1_epi64x(static_cast<long long>(match_mask));
    const __m256i zero = _mm256_setzero_si256();
    const char *p0 = texts[0].data();
    const char *p1 = texts[1].data();
    const char *p2 = texts[2].data();
    const char *p3 = texts[3].data();
    const size_t min_len =
        std::min({texts[0].size(), texts[1].size(), texts[2].size(), texts[3].size()});
    bool all_matched = false;

    for (size_t i = 0; i < min_len; ++i) {
        const __m256i cm =
            _mm256_set_epi64x(static_cast<long long>(char_mask[static_cast<uint8_t>(p3[i])]),
                              static_cast<long long>(char_mask[static_cast<uint8_t>(p2[i])]),
                              static_cast<long long>(char_mask[static_cast<uint8_t>(p1[i])]),
                              static_cast<long long>(char_mask[static_cast<uint8_t>(p0[i])]));
        all_matched =
            stepExact4Avx2(texts, i, pat_len, match_mask, cm, r, seen, mmask, zero, scores);
        if (all_matched) {
            break;
        }
    }
    if (!all_matched) {
        for (size_t i = min_len; i < max_len; ++i) {
            const uint64_t c0 =
                (i < texts[0].size()) ? char_mask[static_cast<uint8_t>(p0[i])] : ~0ULL;
            const uint64_t c1 =
                (i < texts[1].size()) ? char_mask[static_cast<uint8_t>(p1[i])] : ~0ULL;
            const uint64_t c2 =
                (i < texts[2].size()) ? char_mask[static_cast<uint8_t>(p2[i])] : ~0ULL;
            const uint64_t c3 =
                (i < texts[3].size()) ? char_mask[static_cast<uint8_t>(p3[i])] : ~0ULL;
            const __m256i cm =
                _mm256_set_epi64x(static_cast<long long>(c3), static_cast<long long>(c2),
                                  static_cast<long long>(c1), static_cast<long long>(c0));
            all_matched =
                stepExact4Avx2(texts, i, pat_len, match_mask, cm, r, seen, mmask, zero, scores);
            if (all_matched) {
                break;
            }
        }
    }
    if (outSeenBits) {
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(outSeenBits), seen);
    }
}

[[gnu::target("avx2")]] inline void advanceWuManber4Avx2(const __m256i &cm, __m256i &r0,
                                                         __m256i &r1, __m256i &r2)
{
    const __m256i r0_old = r0;
    const __m256i r1_old = r1;
    r0 = _mm256_or_si256(_mm256_slli_epi64(r0, 1), cm);
    r1 = _mm256_and_si256(_mm256_and_si256(_mm256_or_si256(_mm256_slli_epi64(r1, 1), cm),
                                           _mm256_slli_epi64(r0_old, 1)),
                          _mm256_and_si256(r0_old, _mm256_slli_epi64(r0, 1)));
    r2 = _mm256_and_si256(_mm256_and_si256(_mm256_or_si256(_mm256_slli_epi64(r2, 1), cm),
                                           _mm256_slli_epi64(r1_old, 1)),
                          _mm256_and_si256(r1_old, _mm256_slli_epi64(r1, 1)));
}

// Run 1/2-edit Wu-Manber across 4 candidates simultaneously only when exact Pass 1 missed
// and pigeonhole seen_bits stayed <= 2.
[[gnu::target("avx2")]] inline void
score4ItemsWuManberAVX2(const std::string_view texts[4], const std::array<uint64_t, 256> &char_mask,
                        uint64_t match_mask, size_t pat_len, int scores[4])
{
    for (int k = 0; k < 4; ++k) {
        scores[k] = -1;
    }
    size_t max_len = 0;
    for (int k = 0; k < 4; ++k) {
        if (texts[k].size() > max_len) {
            max_len = texts[k].size();
        }
    }
    if (max_len == 0U || max_len + 2U < pat_len) {
        return;
    }

    __m256i r0 = _mm256_set1_epi64x(~0ULL);
    __m256i r1 = _mm256_set1_epi64x(~1ULL);
    __m256i r2 = _mm256_set1_epi64x(~3ULL);
    __m256i r1_acc = _mm256_set1_epi64x(~0ULL);
    __m256i r2_acc = _mm256_set1_epi64x(~0ULL);
    const __m256i allOnes = _mm256_set1_epi64x(~0ULL);
    const __m256i lenVec = _mm256_set_epi64x(
        static_cast<long long>(texts[3].size()), static_cast<long long>(texts[2].size()),
        static_cast<long long>(texts[1].size()), static_cast<long long>(texts[0].size()));
    const __m256i mmask = _mm256_set1_epi64x(static_cast<long long>(match_mask));
    const __m256i zero = _mm256_setzero_si256();
    const char *p0 = texts[0].data();
    const char *p1 = texts[1].data();
    const char *p2 = texts[2].data();
    const char *p3 = texts[3].data();
    const size_t min_len =
        std::min({texts[0].size(), texts[1].size(), texts[2].size(), texts[3].size()});

    for (size_t i = 0; i < min_len; ++i) {
        const __m256i cm =
            _mm256_set_epi64x(static_cast<long long>(char_mask[static_cast<uint8_t>(p3[i])]),
                              static_cast<long long>(char_mask[static_cast<uint8_t>(p2[i])]),
                              static_cast<long long>(char_mask[static_cast<uint8_t>(p1[i])]),
                              static_cast<long long>(char_mask[static_cast<uint8_t>(p0[i])]));
        advanceWuManber4Avx2(cm, r0, r1, r2);
        r1_acc = _mm256_and_si256(r1_acc, r1);
        r2_acc = _mm256_and_si256(r2_acc, r2);
    }

    for (size_t i = min_len; i < max_len; ++i) {
        const uint64_t c0 = (i < texts[0].size()) ? char_mask[static_cast<uint8_t>(p0[i])] : ~0ULL;
        const uint64_t c1 = (i < texts[1].size()) ? char_mask[static_cast<uint8_t>(p1[i])] : ~0ULL;
        const uint64_t c2 = (i < texts[2].size()) ? char_mask[static_cast<uint8_t>(p2[i])] : ~0ULL;
        const uint64_t c3 = (i < texts[3].size()) ? char_mask[static_cast<uint8_t>(p3[i])] : ~0ULL;
        const __m256i cm =
            _mm256_set_epi64x(static_cast<long long>(c3), static_cast<long long>(c2),
                              static_cast<long long>(c1), static_cast<long long>(c0));
        advanceWuManber4Avx2(cm, r0, r1, r2);

        const __m256i active =
            _mm256_cmpgt_epi64(lenVec, _mm256_set1_epi64x(static_cast<long long>(i)));
        const __m256i inactive = _mm256_andnot_si256(active, allOnes);
        r1_acc = _mm256_and_si256(r1_acc, _mm256_or_si256(r1, inactive));
        r2_acc = _mm256_and_si256(r2_acc, _mm256_or_si256(r2, inactive));
    }

    const int err1Mask = _mm256_movemask_pd(
        _mm256_castsi256_pd(_mm256_cmpeq_epi64(_mm256_and_si256(r1_acc, mmask), zero)));
    const int err2Mask = _mm256_movemask_pd(
        _mm256_castsi256_pd(_mm256_cmpeq_epi64(_mm256_and_si256(r2_acc, mmask), zero)));

    for (int k = 0; k < 4; ++k) {
        if (texts[k].size() + 2U >= pat_len) {
            if (err1Mask & (1 << k)) {
                scores[k] = 150;
            } else if (err2Mask & (1 << k)) {
                scores[k] = 80;
            }
        }
    }
}

__attribute__((target("avx2"))) int
countMatchesBatchAVX2(const std::string &lowerArena, const std::vector<uint32_t> &offs,
                      const std::vector<uint32_t> &lens, const std::array<uint64_t, 256> &char_mask,
                      uint64_t match_mask, uint64_t pat_len, size_t &out_idx)
{
    int matches = 0;
    const size_t n = offs.size();
    size_t i = 0;
    int scores[4];
    std::string_view texts[4];
    const char *base = lowerArena.data();
    for (; i + 4 <= n; i += 4) {
        texts[0] = std::string_view(base + offs[i], lens[i]);
        texts[1] = std::string_view(base + offs[i + 1], lens[i + 1]);
        texts[2] = std::string_view(base + offs[i + 2], lens[i + 2]);
        texts[3] = std::string_view(base + offs[i + 3], lens[i + 3]);
        score4ItemsAVX2(texts, char_mask, match_mask, pat_len, scores);
        matches += static_cast<int>(scores[0] >= 0) + static_cast<int>(scores[1] >= 0) +
                   static_cast<int>(scores[2] >= 0) + static_cast<int>(scores[3] >= 0);
    }
    out_idx = i;
    return matches;
}
#endif

} // namespace

namespace WallpaperTags {

const std::vector<TagDefinition> &allKnownTags()
{
    static const std::vector<TagDefinition> tags = {{Anime, "anime", "Anime"},
                                                    {Nature, "nature", "Nature"},
                                                    {Abstract, "abstract", "Abstract"},
                                                    {Cyberpunk, "cyberpunk", "Cyberpunk"},
                                                    {Minimal, "minimal", "Minimal"},
                                                    {SciFi, "scifi", "Sci-Fi"},
                                                    {Landscape, "landscape", "Landscape"},
                                                    {Architecture, "architecture", "Architecture"},
                                                    {Dark, "dark", "Dark"},
                                                    {Light, "light", "Light"},
                                                    {Retro, "retro", "Retro"},
                                                    {Fantasy, "fantasy", "Fantasy"},
                                                    {Vehicles, "vehicles", "Vehicles"},
                                                    {Animals, "animals", "Animals"},
                                                    {PixelArt, "pixelart", "Pixel Art"},
                                                    {Space, "space", "Space"},
                                                    {Urban, "urban", "Urban"},
                                                    {DigitalArt, "digitalart", "Digital Art"},
                                                    {Photography, "photography", "Photography"}};
    return tags;
}

uint64_t parseTagsFromString(std::string_view text)
{
    uint64_t mask = 0;
    std::string lower = toLowerString(text);
    for (const auto &tag : allKnownTags()) {
        if (lower.find(tag.name) != std::string::npos) {
            mask |= tag.mask;
        }
    }
    return mask;
}

} // namespace WallpaperTags

waywal_thumb_meta_t CatalogItem::toThumbMeta() const noexcept
{
    waywal_thumb_meta_t m{};
    m.id = id;
    uint32_t derivedFlags = flags;
    switch (badge) {
    case WallpaperBadge::Video:
        derivedFlags |= WAYWAL_THUMB_FLAG_VIDEO;
        break;
    case WallpaperBadge::Shader:
        derivedFlags |= WAYWAL_THUMB_FLAG_SHADER;
        break;
    case WallpaperBadge::Scene:
        derivedFlags |= WAYWAL_THUMB_FLAG_SCENE;
        break;
    case WallpaperBadge::Favorite:
        derivedFlags |= WAYWAL_THUMB_FLAG_FAVORITE;
        break;
    case WallpaperBadge::Image:
    case WallpaperBadge::GnomeXml:
        break;
    }
    m.flags = derivedFlags;
    m.tag_mask = tag_mask;
    copyUtf8Safe(m.title, sizeof(m.title), title);
    copyUtf8Safe(m.path, sizeof(m.path), path);
    m.aspect_ratio = aspect_ratio;
    m.dominant_color = dominant_color;
    return m;
}

float CatalogItem::computeHue() const noexcept
{
    float r = ((dominant_color >> 16) & 0xFF) / 255.0f;
    float g = ((dominant_color >> 8) & 0xFF) / 255.0f;
    float b = (dominant_color & 0xFF) / 255.0f;

    float max_c = std::max({r, g, b});
    float min_c = std::min({r, g, b});
    float delta = max_c - min_c;

    if (delta < 0.0001f) {
        return 0.0f; // Grayscale
    }

    float hue = 0.0f;
    if (max_c == r) {
        hue = 60.0f * std::fmod(((g - b) / delta), 6.0f);
    } else if (max_c == g) {
        hue = 60.0f * (((b - r) / delta) + 2.0f);
    } else {
        hue = 60.0f * (((r - g) / delta) + 4.0f);
    }

    if (hue < 0.0f) {
        hue += 360.0f;
    }
    return hue;
}

inline void cpuRelaxSpin() noexcept
{
#if WWAL_HAVE_X86_RUNTIME_AVX2
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

struct FuzzyCatalogIndex::FilterWorkerPool {
    using TaskFn = void (*)(void *, size_t, std::vector<std::pair<int, uint32_t>> &);

    size_t totalWorkers{1U};
    std::vector<std::vector<std::pair<int, uint32_t>>> chunkScored;
    std::vector<std::thread> workers;
    alignas(64) std::atomic<uint64_t> generation{0ULL};
    alignas(64) std::atomic<size_t> completedWorkers{0U};
    alignas(64) std::atomic<size_t> sleepingWorkers{0U};
    std::atomic<bool> stopping{false};
    std::mutex mtx;
    std::condition_variable cvWork;
    TaskFn taskFn{nullptr};
    void *taskCtx{nullptr};

    explicit FilterWorkerPool(size_t workerCount) : totalWorkers(workerCount)
    {
        chunkScored.resize(totalWorkers);
        for (auto &vec : chunkScored) {
            vec.reserve(8192U);
        }
        workers.reserve(totalWorkers - 1U);
        for (size_t id = 1U; id < totalWorkers; ++id) {
            workers.emplace_back([this, id]() { workerLoop(id); });
        }
    }

    ~FilterWorkerPool()
    {
        stopping.store(true, std::memory_order_release);
        generation.fetch_add(1ULL, std::memory_order_seq_cst);
        {
            std::lock_guard<std::mutex> lock(mtx);
        }
        cvWork.notify_all();
        for (auto &w : workers) {
            if (w.joinable()) {
                w.join();
            }
        }
    }

    void workerLoop(size_t id)
    {
        uint64_t seenGen = 0ULL;
#if defined(__SANITIZE_THREAD__)
        constexpr int kSpinIters = 16;
#else
        constexpr int kSpinIters = 32768;
#endif
        for (;;) {
            uint64_t curGen = generation.load(std::memory_order_acquire);
            for (int spin = 0; spin < kSpinIters; ++spin) {
                if (stopping.load(std::memory_order_acquire)) {
                    return;
                }
                curGen = generation.load(std::memory_order_acquire);
                if (curGen != seenGen) {
                    break;
                }
                cpuRelaxSpin();
            }
            if (stopping.load(std::memory_order_acquire)) {
                return;
            }
            if (curGen == seenGen) {
                std::unique_lock<std::mutex> lock(mtx);
                sleepingWorkers.fetch_add(1U, std::memory_order_seq_cst);
                cvWork.wait(lock, [&]() {
                    return stopping.load(std::memory_order_seq_cst) ||
                           generation.load(std::memory_order_seq_cst) != seenGen;
                });
                sleepingWorkers.fetch_sub(1U, std::memory_order_seq_cst);
                if (stopping.load(std::memory_order_acquire)) {
                    return;
                }
                curGen = generation.load(std::memory_order_acquire);
            }
            seenGen = curGen;
            TaskFn fn = taskFn;
            void *ctx = taskCtx;
            fn(ctx, id, chunkScored[id]);
            completedWorkers.fetch_add(1U, std::memory_order_acq_rel);
        }
    }

    void run(TaskFn fn, void *ctx)
    {
        for (auto &vec : chunkScored) {
            vec.clear();
        }
        taskFn = fn;
        taskCtx = ctx;
        completedWorkers.store(0U, std::memory_order_release);
        generation.fetch_add(1ULL, std::memory_order_seq_cst);
        if (sleepingWorkers.load(std::memory_order_seq_cst) > 0U) {
            {
                std::lock_guard<std::mutex> lock(mtx);
            }
            cvWork.notify_all();
        }
        fn(ctx, 0U, chunkScored[0]);
        const size_t expected = workers.size();
        while (completedWorkers.load(std::memory_order_acquire) < expected) {
#if defined(__SANITIZE_THREAD__)
            std::this_thread::yield();
#else
            cpuRelaxSpin();
#endif
        }
    }
};

FuzzyCatalogIndex::~FuzzyCatalogIndex() = default;
FuzzyCatalogIndex::FuzzyCatalogIndex(FuzzyCatalogIndex &&) noexcept = default;
FuzzyCatalogIndex &FuzzyCatalogIndex::operator=(FuzzyCatalogIndex &&) noexcept = default;

FuzzyCatalogIndex::FuzzyCatalogIndex()
{
    m_items.reserve(2048);
    m_itemHueQ16.reserve(2048);
    m_itemBadges.reserve(2048);
    m_itemTagMasks.reserve(2048);
    m_titleOff.reserve(2048);
    m_titleLen.reserve(2048);
    m_titleCharMasks.reserve(2048);
    m_titleCharMasks2.reserve(2048);
    m_titleBigramMasks0.reserve(2048);
    m_titleBigramMasks1.reserve(2048);
    m_titleBigramMasks2.reserve(2048);
    m_pathOff.reserve(2048);
    m_pathLen.reserve(2048);
    m_pathDirLen.reserve(2048);
    m_itemDirIds.reserve(2048);
    m_pathCharMasks.reserve(2048);
    m_fileCharMasks.reserve(2048);
    m_fileCharMasks2.reserve(2048);
    m_fileBigramMasks0.reserve(2048);
    m_fileBigramMasks1.reserve(2048);
    m_fileBigramMasks2.reserve(2048);
    m_filteredIndices.reserve(2048);
    resetFolderConfigToDefaults();
}

uint32_t FuzzyCatalogIndex::addItem(CatalogItem item)
{
    if (item.id == 0U) {
        item.id = m_nextAutoId++;
    } else if (item.id >= m_nextAutoId) {
        m_nextAutoId = item.id + 1U;
    }

    const uint16_t hueQ16 = quantizeHueQ16(item);
    const std::string lowerTitle = toLowerString(item.title);
    const std::string lowerPath = toLowerString(item.path);

    m_itemHueQ16.push_back(hueQ16);
    m_itemBadges.push_back(static_cast<uint32_t>(item.badge));
    m_itemTagMasks.push_back(item.tag_mask);

    m_titleOff.push_back(static_cast<uint32_t>(m_lowerTitleArena.size()));
    m_titleLen.push_back(static_cast<uint32_t>(lowerTitle.size()));
    m_lowerTitleArena.append(lowerTitle);
    const CharMaskPair titleMasks = computeAsciiCharMaskPair64(lowerTitle);
    m_titleCharMasks.push_back(titleMasks.m1);
    m_titleCharMasks2.push_back(titleMasks.m2);
    const BigramMask3 titleBigrams = computeBigramMask3(lowerTitle);
    m_titleBigramMasks0.push_back(titleBigrams.m0);
    m_titleBigramMasks1.push_back(titleBigrams.m1);
    m_titleBigramMasks2.push_back(titleBigrams.m2);

    const size_t slash = lowerPath.rfind('/');
    const uint32_t dirLen = (slash == std::string::npos) ? 0U : static_cast<uint32_t>(slash + 1U);
    const std::string_view dirSv(lowerPath.data(), dirLen);
    uint32_t dirId = m_lastAddDirId;
    if (dirId >= m_uniqueDirs.size() || m_uniqueDirs[dirId].lowerDir != dirSv) {
        std::string dirStr(dirSv);
        auto [it, inserted] = m_dirToId.emplace(dirStr, static_cast<uint32_t>(m_uniqueDirs.size()));
        if (inserted) {
            const CharMaskPair dm = computeAsciiCharMaskPair64(dirSv);
            m_uniqueDirs.push_back(DirMeta{std::move(dirStr), dm.m1, dm.m2});
        }
        dirId = it->second;
        m_lastAddDirId = dirId;
    }
    m_itemDirIds.push_back(dirId);

    m_pathOff.push_back(static_cast<uint32_t>(m_lowerPathArena.size()));
    m_pathLen.push_back(static_cast<uint32_t>(lowerPath.size()));
    m_pathDirLen.push_back(dirLen);
    m_lowerPathArena.append(lowerPath);
    m_pathCharMasks.push_back(computeAsciiCharMaskPair64(lowerPath).m1);
    const CharMaskPair fileMasks = computeAsciiCharMaskPair64(
        std::string_view(lowerPath.data() + dirLen, lowerPath.size() - dirLen));
    m_fileCharMasks.push_back(fileMasks.m1);
    m_fileCharMasks2.push_back(fileMasks.m2);
    const size_t bigramStart = (dirLen > 0U) ? static_cast<size_t>(dirLen - 1U) : 0U;
    const BigramMask3 fileBigrams = computeBigramMask3(
        std::string_view(lowerPath.data() + bigramStart, lowerPath.size() - bigramStart));
    m_fileBigramMasks0.push_back(fileBigrams.m0);
    m_fileBigramMasks1.push_back(fileBigrams.m1);
    m_fileBigramMasks2.push_back(fileBigrams.m2);

    m_items.push_back(std::move(item));
    m_filteredPosDirty = true;
    return m_items.back().id;
}

void FuzzyCatalogIndex::clear()
{
    m_items.clear();
    m_itemHueQ16.clear();
    m_itemBadges.clear();
    m_itemTagMasks.clear();
    m_titleOff.clear();
    m_titleLen.clear();
    m_lowerTitleArena.clear();
    m_titleCharMasks.clear();
    m_titleCharMasks2.clear();
    m_titleBigramMasks0.clear();
    m_titleBigramMasks1.clear();
    m_titleBigramMasks2.clear();
    m_pathOff.clear();
    m_pathLen.clear();
    m_pathDirLen.clear();
    m_itemDirIds.clear();
    m_uniqueDirs.clear();
    m_dirToId.clear();
    m_lastAddDirId = UINT32_MAX;
    m_lowerPathArena.clear();
    m_pathCharMasks.clear();
    m_fileCharMasks.clear();
    m_fileCharMasks2.clear();
    m_fileBigramMasks0.clear();
    m_fileBigramMasks1.clear();
    m_fileBigramMasks2.clear();
    m_filteredIndices.clear();
    m_idToFilteredPos.clear();
    m_filteredPosDirty = true;
    m_seenPaths.clear();
    m_sizeNameFirstPath.clear();
    m_seenContentDigests.clear();
    m_nextAutoId = 0U;
}

std::optional<size_t> FuzzyCatalogIndex::filteredPositionOfId(uint32_t item_id) const
{
    if (m_filteredPosDirty) {
        m_idToFilteredPos.clear();
        m_idToFilteredPos.reserve(m_filteredIndices.size());
        for (size_t pos = 0; pos < m_filteredIndices.size(); ++pos) {
            const uint32_t idx = m_filteredIndices[pos];
            if (idx < m_items.size()) {
                m_idToFilteredPos.emplace(m_items[idx].id, pos);
            }
        }
        m_filteredPosDirty = false;
    }
    if (auto it = m_idToFilteredPos.find(item_id); it != m_idToFilteredPos.end()) {
        return it->second;
    }
    return std::nullopt;
}

void FuzzyCatalogIndex::setSearchQuery(std::string_view query)
{
    if (m_activeQuery != query) {
        m_activeQuery = std::string(query);
        refilter();
    }
}

void FuzzyCatalogIndex::setTagMask(uint64_t tag_mask, bool match_all)
{
    if (m_activeTagMask != tag_mask || m_tagMatchAll != match_all) {
        m_activeTagMask = tag_mask;
        m_tagMatchAll = match_all;
        refilter();
    }
}

void FuzzyCatalogIndex::setBadgeFilter(uint32_t badge_mask)
{
    if (m_activeBadgeMask != badge_mask) {
        m_activeBadgeMask = badge_mask;
        refilter();
    }
}

void FuzzyCatalogIndex::setSorting(SortCriteria criteria)
{
    if (m_activeSorting != criteria) {
        m_activeSorting = criteria;
        sortFiltered();
    }
}

int FuzzyCatalogIndex::scoreItemFuzzy(std::string_view text, std::string_view pattern,
                                      const std::array<uint64_t, 256> &char_mask,
                                      uint64_t pattern_len)
{
    if (pattern.empty() || pattern_len == 0) {
        return 100;
    }
    const size_t text_len = text.size();
    if (pattern.size() > 64U) {
        return scoreLongQuerySemiGlobal(toLowerString(text), toLowerString(pattern));
    }
    if (text_len == 0 || text_len + 2 < pattern_len) {
        return -1;
    }

    const char *p = text.data();
    const uint64_t match_mask = 1ULL << (pattern_len - 1);

    if (text_len >= pattern_len) {
        bool is_prefix = true;
        for (size_t i = 0; i < pattern_len; ++i) {
            uint8_t c = static_cast<uint8_t>(p[i]);
            if (char_mask[c] & (1ULL << i)) {
                is_prefix = false;
                break;
            }
        }
        if (is_prefix) {
            // Clamp length penalty so >1000-byte paths still yield positive match scores (AUD-159).
            return 1000 - static_cast<int>(std::min<size_t>(text_len - pattern_len, 400U));
        }
    }

    // Pass 1: Exact Bitap with pigeonhole character-presence filter
    uint64_t r0 = ~0ULL;
    uint64_t seen_bits = ~0ULL;
    for (size_t i = 0; i < text_len; ++i) {
        uint64_t cm = char_mask[static_cast<uint8_t>(p[i])];
        r0 = (r0 << 1) | cm;
        seen_bits &= cm;
        if (text_len >= pattern_len && !(r0 & match_mask)) {
            bool word_boundary = false;
            if (i >= pattern_len) {
                word_boundary = isWordBoundaryChar(p[i - pattern_len]);
            }
            const int base = word_boundary ? 500 : 250;
            const size_t cap = word_boundary ? 200U : 90U;
            return base - static_cast<int>(std::min<size_t>(i, cap));
        }
    }

    // If >2 pattern positions never occurred anywhere in text, edit distance is >2
    const uint64_t pat_mask = (pattern_len >= 64) ? ~0ULL : ((1ULL << pattern_len) - 1ULL);
    if (__builtin_popcountll(seen_bits & pat_mask) > 2) {
        return -1;
    }

    // Pass 2: Active-0 Wu-Manber fuzzy Bitap (up to 2 edits including pattern-char deletions)
    r0 = ~0ULL;
    uint64_t r1 = ~1ULL;
    uint64_t r2 = ~3ULL;
    bool err1 = false;
    bool err2 = false;

    for (size_t i = 0; i < text_len; ++i) {
        uint8_t c = static_cast<uint8_t>(p[i]);
        uint64_t cm = char_mask[c];
        uint64_t r0_old = r0;
        uint64_t r1_old = r1;

        r0 = ((r0 << 1) | cm);
        r1 = ((r1 << 1) | cm) & (r0_old << 1) & r0_old & (r0 << 1);
        r2 = ((r2 << 1) | cm) & (r1_old << 1) & r1_old & (r1 << 1);

        if (!(r1 & match_mask)) {
            err1 = true;
            break;
        } else if (!(r2 & match_mask)) {
            err2 = true;
        }
    }

    if (err1)
        return 150;
    if (err2)
        return 80;

    return -1; // No match
}

struct FuzzyCatalogIndex::FilterImpl {
    struct DirEval {
        int dirScore{-1};
        uint32_t safeBackBytes{0U};
        uint64_t tailCharMask{0ULL};
        uint64_t tailCharMask2{0ULL};
        BigramMask3 tailBigrams{};
    };

    struct PathCanonSlot {
        uint64_t hash{0ULL};
        uint32_t sampleOff{0U};
        uint32_t sampleLen{0U};
        int32_t score{-1};
        bool valid{false};
    };

    struct SearchCtx {
        const FuzzyCatalogIndex *idx{nullptr};
        const char *titleArenaBase{nullptr};
        const char *pathArenaBase{nullptr};
        std::string lowerQuery;
        bool longQuery{false};
        bool queryHasDigit{false};
        bool filterBadge{false};
        bool filterTag{false};
        uint64_t patLen{0U};
        std::array<uint64_t, 256> charMask{};
        uint64_t queryCharBits{0ULL};
        uint64_t queryCharBits2{0ULL};
        QueryBigramPieces queryBigrams{};
        uint64_t matchMask{0ULL};
        uint64_t patMask{0ULL};
        const std::vector<DirEval> *dirEval{nullptr};
        bool useAvx2{false};
        size_t totalItems{0U};
        size_t chunkSize{0U};
    };

    static inline int scoreFallback(const SearchCtx &ctx, std::string_view textSv)
    {
        if (ctx.longQuery) {
            return scoreLongQuerySemiGlobal(textSv, ctx.lowerQuery);
        }
        return FuzzyCatalogIndex::scoreItemFuzzy(textSv, ctx.lowerQuery, ctx.charMask, ctx.patLen);
    }

    static inline bool resolvePathCanonOrHit(const SearchCtx &ctx, uint32_t idx, uint32_t seamOff,
                                             uint32_t seamLen, std::string_view seamSv,
                                             std::array<PathCanonSlot, 128> &pathCanonCache,
                                             size_t &canonHits, size_t &canonMisses,
                                             std::vector<std::pair<int, uint32_t>> &outScored)
    {
        if (ctx.queryHasDigit || (canonHits == 0U && canonMisses >= 64U)) {
            return false;
        }
        const uint64_t h = hashSeamCanonDigits(seamSv);
        PathCanonSlot &slot = pathCanonCache[h & 127ULL];
        if (slot.valid && slot.hash == h && slot.sampleLen == seamLen &&
            equalSeamCanonDigits(
                seamSv, std::string_view(ctx.pathArenaBase + slot.sampleOff, slot.sampleLen))) {
            ++canonHits;
            if (slot.score >= 0) {
                outScored.emplace_back(slot.score, idx);
            }
            return true;
        }
        ++canonMisses;
        const int raw = scoreFallback(ctx, seamSv);
        const int finalScore = (raw >= 0) ? std::max(1, raw - 50) : -1;
        slot = PathCanonSlot{h, seamOff, seamLen, finalScore, true};
        if (finalScore >= 0) {
            outScored.emplace_back(finalScore, idx);
        }
        return true;
    }

    static void runRangeScalar(const SearchCtx &ctx, size_t beginIdx, size_t endIdx,
                               std::vector<std::pair<int, uint32_t>> &outScored)
    {
        const FuzzyCatalogIndex &self = *ctx.idx;
        std::array<PathCanonSlot, 128> pathCanonCache{};
        size_t canonHits = 0U;
        size_t canonMisses = 0U;
        const auto &dirEval = *ctx.dirEval;

        auto queuePath = [&](uint32_t itemIdx) {
            const auto &de = dirEval[self.m_itemDirIds[itemIdx]];
            if (de.dirScore >= 0) {
                outScored.emplace_back(std::max(1, de.dirScore - 50), itemIdx);
                return;
            }
            const uint64_t seamMask = self.m_fileCharMasks[itemIdx] | de.tailCharMask;
            const uint64_t seamMask2 = self.m_fileCharMasks2[itemIdx] | de.tailCharMask2 |
                                       (self.m_fileCharMasks[itemIdx] & de.tailCharMask);
            if (ctx.patLen > 2U && (std::popcount(ctx.queryCharBits & ~seamMask) +
                                    std::popcount(ctx.queryCharBits2 & ~seamMask2)) > 2) {
                return;
            }
            if (!matchesAnyBigramPiece(ctx.queryBigrams,
                                       self.m_fileBigramMasks0[itemIdx] | de.tailBigrams.m0,
                                       self.m_fileBigramMasks1[itemIdx] | de.tailBigrams.m1,
                                       self.m_fileBigramMasks2[itemIdx] | de.tailBigrams.m2)) {
                return;
            }
            const uint32_t dirLen = self.m_pathDirLen[itemIdx];
            const size_t startInPath = dirLen - de.safeBackBytes;
            const uint32_t seamOff = self.m_pathOff[itemIdx] + static_cast<uint32_t>(startInPath);
            const uint32_t seamLen = self.m_pathLen[itemIdx] - static_cast<uint32_t>(startInPath);
            const std::string_view seamSv(ctx.pathArenaBase + seamOff, seamLen);
            if (resolvePathCanonOrHit(ctx, itemIdx, seamOff, seamLen, seamSv, pathCanonCache,
                                      canonHits, canonMisses, outScored)) {
                return;
            }
            const int ps = scoreFallback(ctx, seamSv);
            if (ps >= 0) {
                outScored.emplace_back(std::max(1, ps - 50), itemIdx);
            }
        };

        for (size_t i = beginIdx; i < endIdx; ++i) {
            if (ctx.filterBadge && (self.m_activeBadgeMask & self.m_itemBadges[i]) == 0U) {
                continue;
            }
            if (ctx.filterTag) {
                if (self.m_tagMatchAll) {
                    if ((self.m_itemTagMasks[i] & self.m_activeTagMask) != self.m_activeTagMask) {
                        continue;
                    }
                } else if ((self.m_itemTagMasks[i] & self.m_activeTagMask) == 0ULL) {
                    continue;
                }
            }
            const bool titlePossible =
                ((ctx.patLen <= 2U) ||
                 ((std::popcount(ctx.queryCharBits & ~self.m_titleCharMasks[i]) +
                   std::popcount(ctx.queryCharBits2 & ~self.m_titleCharMasks2[i])) <= 2)) &&
                matchesAnyBigramPiece(ctx.queryBigrams, self.m_titleBigramMasks0[i],
                                      self.m_titleBigramMasks1[i], self.m_titleBigramMasks2[i]);
            if (titlePossible) {
                const std::string_view titleSv(ctx.titleArenaBase + self.m_titleOff[i],
                                               self.m_titleLen[i]);
                const int s = scoreFallback(ctx, titleSv);
                if (s >= 0) {
                    outScored.emplace_back(s, static_cast<uint32_t>(i));
                } else {
                    queuePath(static_cast<uint32_t>(i));
                }
            } else {
                queuePath(static_cast<uint32_t>(i));
            }
        }
    }

#if WWAL_HAVE_X86_RUNTIME_AVX2
    struct Avx2BatchState {
        std::array<PathCanonSlot, 128> pathCanonCache{};
        size_t canonHits{0U};
        size_t canonMisses{0U};
        std::array<uint32_t, 4> titleIdx{};
        std::array<std::string_view, 4> titleTexts{};
        size_t titleCount{0U};
        std::array<uint32_t, 4> titleFuzzyIdx{};
        std::array<std::string_view, 4> titleFuzzyTexts{};
        size_t titleFuzzyCount{0U};
        std::array<uint32_t, 4> pathIdx{};
        std::array<std::string_view, 4> pathTexts{};
        size_t pathCount{0U};
        std::array<uint32_t, 4> pathFuzzyIdx{};
        std::array<std::string_view, 4> pathFuzzyTexts{};
        size_t pathFuzzyCount{0U};
    };

    [[gnu::target("avx2")]] static inline void
    flushPathFuzzyAvx2(const SearchCtx &ctx, Avx2BatchState &st,
                       std::vector<std::pair<int, uint32_t>> &outScored)
    {
        if (st.pathFuzzyCount == 0U) {
            return;
        }
        if (st.pathFuzzyCount == 4U) {
            int scores[4];
            score4ItemsWuManberAVX2(st.pathFuzzyTexts.data(), ctx.charMask, ctx.matchMask,
                                    static_cast<size_t>(ctx.patLen), scores);
            for (size_t k = 0; k < 4U; ++k) {
                if (scores[k] >= 0) {
                    outScored.emplace_back(std::max(1, scores[k] - 50), st.pathFuzzyIdx[k]);
                }
            }
        } else {
            for (size_t k = 0; k < st.pathFuzzyCount; ++k) {
                const int ps = FuzzyCatalogIndex::scoreItemFuzzy(
                    st.pathFuzzyTexts[k], ctx.lowerQuery, ctx.charMask, ctx.patLen);
                if (ps >= 0) {
                    outScored.emplace_back(std::max(1, ps - 50), st.pathFuzzyIdx[k]);
                }
            }
        }
        st.pathFuzzyCount = 0U;
    }

    [[gnu::target("avx2")]] static inline void
    flushPathAvx2(const SearchCtx &ctx, Avx2BatchState &st,
                  std::vector<std::pair<int, uint32_t>> &outScored)
    {
        if (st.pathCount == 0U) {
            return;
        }
        if (st.pathCount == 4U) {
            int scores[4];
            alignas(32) uint64_t seen[4];
            score4ItemsAVX2(st.pathTexts.data(), ctx.charMask, ctx.matchMask,
                            static_cast<size_t>(ctx.patLen), scores, seen);
            for (size_t k = 0; k < 4U; ++k) {
                if (scores[k] >= 0) {
                    outScored.emplace_back(std::max(1, scores[k] - 50), st.pathIdx[k]);
                } else if (std::popcount(seen[k] & ctx.patMask) <= 2) {
                    st.pathFuzzyIdx[st.pathFuzzyCount] = st.pathIdx[k];
                    st.pathFuzzyTexts[st.pathFuzzyCount] = st.pathTexts[k];
                    ++st.pathFuzzyCount;
                    if (st.pathFuzzyCount == 4U) {
                        flushPathFuzzyAvx2(ctx, st, outScored);
                    }
                }
            }
        } else {
            for (size_t k = 0; k < st.pathCount; ++k) {
                const int ps = FuzzyCatalogIndex::scoreItemFuzzy(st.pathTexts[k], ctx.lowerQuery,
                                                                 ctx.charMask, ctx.patLen);
                if (ps >= 0) {
                    outScored.emplace_back(std::max(1, ps - 50), st.pathIdx[k]);
                }
            }
        }
        st.pathCount = 0U;
    }

    [[gnu::target("avx2")]] static inline void
    queuePathFallbackAvx2(const SearchCtx &ctx, Avx2BatchState &st, uint32_t itemIdx,
                          std::vector<std::pair<int, uint32_t>> &outScored)
    {
        const FuzzyCatalogIndex &self = *ctx.idx;
        const auto &de = (*ctx.dirEval)[self.m_itemDirIds[itemIdx]];
        if (de.dirScore >= 0) {
            outScored.emplace_back(std::max(1, de.dirScore - 50), itemIdx);
            return;
        }
        const uint64_t seamMask = self.m_fileCharMasks[itemIdx] | de.tailCharMask;
        const uint64_t seamMask2 = self.m_fileCharMasks2[itemIdx] | de.tailCharMask2 |
                                   (self.m_fileCharMasks[itemIdx] & de.tailCharMask);
        if (ctx.patLen > 2U && (std::popcount(ctx.queryCharBits & ~seamMask) +
                                std::popcount(ctx.queryCharBits2 & ~seamMask2)) > 2) {
            return;
        }
        if (!matchesAnyBigramPiece(ctx.queryBigrams,
                                   self.m_fileBigramMasks0[itemIdx] | de.tailBigrams.m0,
                                   self.m_fileBigramMasks1[itemIdx] | de.tailBigrams.m1,
                                   self.m_fileBigramMasks2[itemIdx] | de.tailBigrams.m2)) {
            return;
        }
        const uint32_t dirLen = self.m_pathDirLen[itemIdx];
        const size_t startInPath = dirLen - de.safeBackBytes;
        const uint32_t seamOff = self.m_pathOff[itemIdx] + static_cast<uint32_t>(startInPath);
        const uint32_t seamLen = self.m_pathLen[itemIdx] - static_cast<uint32_t>(startInPath);
        const std::string_view seamSv(ctx.pathArenaBase + seamOff, seamLen);
        if (resolvePathCanonOrHit(ctx, itemIdx, seamOff, seamLen, seamSv, st.pathCanonCache,
                                  st.canonHits, st.canonMisses, outScored)) {
            return;
        }
        st.pathIdx[st.pathCount] = itemIdx;
        st.pathTexts[st.pathCount] = seamSv;
        ++st.pathCount;
        if (st.pathCount == 4U) {
            flushPathAvx2(ctx, st, outScored);
        }
    }

    [[gnu::target("avx2")]] static inline void
    flushTitleFuzzyAvx2(const SearchCtx &ctx, Avx2BatchState &st,
                        std::vector<std::pair<int, uint32_t>> &outScored)
    {
        if (st.titleFuzzyCount == 0U) {
            return;
        }
        if (st.titleFuzzyCount == 4U) {
            int scores[4];
            score4ItemsWuManberAVX2(st.titleFuzzyTexts.data(), ctx.charMask, ctx.matchMask,
                                    static_cast<size_t>(ctx.patLen), scores);
            for (size_t k = 0; k < 4U; ++k) {
                if (scores[k] >= 0) {
                    outScored.emplace_back(scores[k], st.titleFuzzyIdx[k]);
                } else {
                    queuePathFallbackAvx2(ctx, st, st.titleFuzzyIdx[k], outScored);
                }
            }
        } else {
            for (size_t k = 0; k < st.titleFuzzyCount; ++k) {
                const int s = FuzzyCatalogIndex::scoreItemFuzzy(
                    st.titleFuzzyTexts[k], ctx.lowerQuery, ctx.charMask, ctx.patLen);
                if (s >= 0) {
                    outScored.emplace_back(s, st.titleFuzzyIdx[k]);
                } else {
                    queuePathFallbackAvx2(ctx, st, st.titleFuzzyIdx[k], outScored);
                }
            }
        }
        st.titleFuzzyCount = 0U;
    }

    [[gnu::target("avx2")]] static inline void
    flushTitleAvx2(const SearchCtx &ctx, Avx2BatchState &st,
                   std::vector<std::pair<int, uint32_t>> &outScored)
    {
        if (st.titleCount == 0U) {
            return;
        }
        if (st.titleCount == 4U) {
            int scores[4];
            alignas(32) uint64_t seen[4];
            score4ItemsAVX2(st.titleTexts.data(), ctx.charMask, ctx.matchMask,
                            static_cast<size_t>(ctx.patLen), scores, seen);
            for (size_t k = 0; k < 4U; ++k) {
                if (scores[k] >= 0) {
                    outScored.emplace_back(scores[k], st.titleIdx[k]);
                } else if (std::popcount(seen[k] & ctx.patMask) <= 2) {
                    st.titleFuzzyIdx[st.titleFuzzyCount] = st.titleIdx[k];
                    st.titleFuzzyTexts[st.titleFuzzyCount] = st.titleTexts[k];
                    ++st.titleFuzzyCount;
                    if (st.titleFuzzyCount == 4U) {
                        flushTitleFuzzyAvx2(ctx, st, outScored);
                    }
                } else {
                    queuePathFallbackAvx2(ctx, st, st.titleIdx[k], outScored);
                }
            }
        } else {
            for (size_t k = 0; k < st.titleCount; ++k) {
                const int s = FuzzyCatalogIndex::scoreItemFuzzy(st.titleTexts[k], ctx.lowerQuery,
                                                                ctx.charMask, ctx.patLen);
                if (s >= 0) {
                    outScored.emplace_back(s, st.titleIdx[k]);
                } else {
                    queuePathFallbackAvx2(ctx, st, st.titleIdx[k], outScored);
                }
            }
        }
        st.titleCount = 0U;
    }

    // Keep the outer loop and all batch helpers under target("avx2") so score4ItemsAVX2 and
    // score4ItemsWuManberAVX2 inline directly without per-batch vzeroupper transitions.
    [[gnu::target("avx2")]] static void
    runRangeAvx2(const SearchCtx &ctx, size_t beginIdx, size_t endIdx,
                 std::vector<std::pair<int, uint32_t>> &outScored)
    {
        const FuzzyCatalogIndex &self = *ctx.idx;
        Avx2BatchState st{};

        for (size_t i = beginIdx; i < endIdx; ++i) {
            if (ctx.filterBadge && (self.m_activeBadgeMask & self.m_itemBadges[i]) == 0U) {
                continue;
            }
            if (ctx.filterTag) {
                if (self.m_tagMatchAll) {
                    if ((self.m_itemTagMasks[i] & self.m_activeTagMask) != self.m_activeTagMask) {
                        continue;
                    }
                } else if ((self.m_itemTagMasks[i] & self.m_activeTagMask) == 0ULL) {
                    continue;
                }
            }

            const bool titlePossible =
                ((ctx.patLen <= 2U) ||
                 ((std::popcount(ctx.queryCharBits & ~self.m_titleCharMasks[i]) +
                   std::popcount(ctx.queryCharBits2 & ~self.m_titleCharMasks2[i])) <= 2)) &&
                matchesAnyBigramPiece(ctx.queryBigrams, self.m_titleBigramMasks0[i],
                                      self.m_titleBigramMasks1[i], self.m_titleBigramMasks2[i]);
            if (titlePossible) {
                st.titleIdx[st.titleCount] = static_cast<uint32_t>(i);
                st.titleTexts[st.titleCount] =
                    std::string_view(ctx.titleArenaBase + self.m_titleOff[i], self.m_titleLen[i]);
                ++st.titleCount;
                if (st.titleCount == 4U) {
                    flushTitleAvx2(ctx, st, outScored);
                }
            } else {
                queuePathFallbackAvx2(ctx, st, static_cast<uint32_t>(i), outScored);
            }
        }

        flushTitleAvx2(ctx, st, outScored);
        flushTitleFuzzyAvx2(ctx, st, outScored);
        flushPathAvx2(ctx, st, outScored);
        flushPathFuzzyAvx2(ctx, st, outScored);
    }
#endif

    static void runWorker(void *rawCtx, size_t workerId,
                          std::vector<std::pair<int, uint32_t>> &outBuf)
    {
        const auto *ctx = static_cast<const SearchCtx *>(rawCtx);
        const size_t s = workerId * ctx->chunkSize;
        const size_t e = std::min(ctx->totalItems, s + ctx->chunkSize);
        if (s >= e) {
            return;
        }
#if WWAL_HAVE_X86_RUNTIME_AVX2
        if (ctx->useAvx2 && !ctx->longQuery && ctx->patLen >= 1U) {
            runRangeAvx2(*ctx, s, e, outBuf);
            return;
        }
#endif
        runRangeScalar(*ctx, s, e, outBuf);
    }
};

void FuzzyCatalogIndex::refilter()
{
    auto start_time = std::chrono::high_resolution_clock::now();

    m_filteredIndices.clear();
    m_filteredPosDirty = true;

    const size_t n = m_items.size();
    const bool filterBadge = (m_activeBadgeMask != 0xFFFFFFFFu);
    const bool filterTag = (m_activeTagMask != 0ULL);
    const bool has_query = !m_activeQuery.empty();

    if (!has_query) {
        if (!filterBadge && !filterTag) {
            m_filteredIndices.resize(n);
            std::iota(m_filteredIndices.begin(), m_filteredIndices.end(), 0U);
        } else {
            for (size_t i = 0; i < n; ++i) {
                if (filterBadge && (m_activeBadgeMask & m_itemBadges[i]) == 0U) {
                    continue;
                }
                if (filterTag) {
                    if (m_tagMatchAll) {
                        if ((m_itemTagMasks[i] & m_activeTagMask) != m_activeTagMask) {
                            continue;
                        }
                    } else if ((m_itemTagMasks[i] & m_activeTagMask) == 0ULL) {
                        continue;
                    }
                }
                m_filteredIndices.push_back(static_cast<uint32_t>(i));
            }
        }
        sortFiltered();
        auto end_time = std::chrono::high_resolution_clock::now();
        m_lastFilterDurationUs =
            std::chrono::duration<double, std::micro>(end_time - start_time).count();
        return;
    }

    FilterImpl::SearchCtx ctx{};
    ctx.idx = this;
    ctx.titleArenaBase = m_lowerTitleArena.data();
    ctx.pathArenaBase = m_lowerPathArena.data();
    ctx.lowerQuery = toLowerString(m_activeQuery);
    ctx.longQuery = ctx.lowerQuery.size() > 64U;
    ctx.filterBadge = filterBadge;
    ctx.filterTag = filterTag;
    ctx.patLen = std::min<uint64_t>(ctx.lowerQuery.size(), 64U);

    for (unsigned char ch : ctx.lowerQuery) {
        if (ch >= '0' && ch <= '9') {
            ctx.queryHasDigit = true;
            break;
        }
    }

    ctx.charMask.fill(~0ULL);
    for (uint64_t i = 0; i < ctx.patLen; ++i) {
        const uint8_t lo = static_cast<uint8_t>(ctx.lowerQuery[i]);
        const uint8_t up = (lo >= 'a' && lo <= 'z') ? static_cast<uint8_t>(lo - ('a' - 'A')) : lo;
        ctx.charMask[lo] &= ~(1ULL << i);
        ctx.charMask[up] &= ~(1ULL << i);
    }
    const CharMaskPair qChars = computeAsciiCharMaskPair64(
        std::string_view(ctx.lowerQuery.data(), static_cast<size_t>(ctx.patLen)));
    ctx.queryCharBits = qChars.m1;
    ctx.queryCharBits2 = qChars.m2;

    if (ctx.patLen >= 6U) {
        // Three strictly disjoint 2-grams guarantee at least 1 survives any <=2 edit distance.
        ctx.queryBigrams.active = true;
        for (size_t p = 0; p < 3U; ++p) {
            const unsigned char a = static_cast<unsigned char>(ctx.lowerQuery[p * 2U]);
            const unsigned char b = static_cast<unsigned char>(ctx.lowerQuery[p * 2U + 1U]);
            ctx.queryBigrams.q0[p] = bigramBitSlice(a, b, 0x9E3779B97F4A7C15ULL, 0ULL);
            ctx.queryBigrams.q1[p] = bigramBitSlice(a, b, 0xBF58476D1CE4E5B9ULL, 0x10000ULL);
            ctx.queryBigrams.q2[p] = bigramBitSlice(a, b, 0x94D049BB133111EBULL, 0x20000ULL);
            ctx.queryBigrams.q0_all |= ctx.queryBigrams.q0[p];
        }
    }
    ctx.matchMask = (ctx.patLen > 0U) ? (1ULL << (ctx.patLen - 1U)) : 0ULL;
    ctx.patMask = (ctx.patLen >= 64U) ? ~0ULL : ((1ULL << ctx.patLen) - 1ULL);
#if WWAL_HAVE_X86_RUNTIME_AVX2
    static const bool cpuAvx2 = __builtin_cpu_supports("avx2");
    ctx.useAvx2 = cpuAvx2;
#endif

    // Evaluate unique directory prefixes once per refilter rather than per item.
    std::vector<FilterImpl::DirEval> dirEval(m_uniqueDirs.size());
    const size_t qFull = ctx.lowerQuery.size();
    for (size_t d = 0; d < m_uniqueDirs.size(); ++d) {
        const auto &meta = m_uniqueDirs[d];
        auto &ev = dirEval[d];
        const size_t dirLen = meta.lowerDir.size();
        if (dirLen == 0U || (!ctx.longQuery && ctx.patLen > 2U &&
                             (std::popcount(ctx.queryCharBits & ~meta.charMask) +
                              std::popcount(ctx.queryCharBits2 & ~meta.charMask2)) > 2)) {
            ev.dirScore = -1;
        } else {
            ev.dirScore = FilterImpl::scoreFallback(ctx, meta.lowerDir);
        }
        ev.safeBackBytes = static_cast<uint32_t>(
            std::min<size_t>(dirLen, (qFull > 64U ? qFull : static_cast<size_t>(ctx.patLen)) + 2U));
        const size_t tailOff = dirLen - ev.safeBackBytes;
        const std::string_view tailView(meta.lowerDir.data() + tailOff, ev.safeBackBytes);
        const CharMaskPair tailMasks = computeAsciiCharMaskPair64(tailView);
        ev.tailCharMask = tailMasks.m1;
        ev.tailCharMask2 = tailMasks.m2;
        ev.tailBigrams = computeBigramMask3(tailView);
    }
    ctx.dirEval = &dirEval;
    ctx.totalItems = n;

    const unsigned hw = std::max(1U, std::thread::hardware_concurrency());
    const size_t numWorkers = (n >= 24576U && hw > 1U) ? std::min<size_t>(hw, 4U) : 1U;
    ctx.chunkSize = (n + numWorkers - 1U) / numWorkers;

    auto scatterSortedBuckets = [&](const std::vector<std::pair<int, uint32_t>> *slices,
                                    size_t sliceCount, size_t totalMatches) {
        std::array<uint32_t, 1002> bucketCounts{};
        for (size_t s = 0U; s < sliceCount; ++s) {
            for (const auto &entry : slices[s]) {
                const int clamped = std::clamp(entry.first, 0, 1000);
                bucketCounts[static_cast<size_t>(1000 - clamped)]++;
            }
        }
        uint32_t offset = 0U;
        for (size_t b = 0U; b <= 1000U; ++b) {
            const uint32_t c = bucketCounts[b];
            bucketCounts[b] = offset;
            offset += c;
        }
        m_filteredIndices.resize(totalMatches);
        for (size_t s = 0U; s < sliceCount; ++s) {
            for (const auto &entry : slices[s]) {
                const int clamped = std::clamp(entry.first, 0, 1000);
                m_filteredIndices[bucketCounts[static_cast<size_t>(1000 - clamped)]++] =
                    entry.second;
            }
        }
    };

    if (numWorkers == 1U) {
        m_scoredScratch.clear();
        if (m_scoredScratch.capacity() < n) {
            m_scoredScratch.reserve(n);
        }
        FilterImpl::runWorker(&ctx, 0U, m_scoredScratch);
        scatterSortedBuckets(&m_scoredScratch, 1U, m_scoredScratch.size());
    } else {
        if (!m_filterPool || m_filterPool->totalWorkers != numWorkers) {
            m_filterPool = std::make_unique<FilterWorkerPool>(numWorkers);
        }
        m_filterPool->run(&FilterImpl::runWorker, &ctx);

        size_t totalMatches = 0U;
        for (const auto &vec : m_filterPool->chunkScored) {
            totalMatches += vec.size();
        }
        scatterSortedBuckets(m_filterPool->chunkScored.data(), numWorkers, totalMatches);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    m_lastFilterDurationUs =
        std::chrono::duration<double, std::micro>(end_time - start_time).count();
}

void FuzzyCatalogIndex::sortFiltered()
{
    m_filteredPosDirty = true;
    if (m_filteredIndices.empty())
        return;

    switch (m_activeSorting) {
    case SortCriteria::NameAsc:
        std::stable_sort(
            m_filteredIndices.begin(), m_filteredIndices.end(),
            [this](uint32_t a, uint32_t b) { return m_items[a].title < m_items[b].title; });
        break;
    case SortCriteria::NameDesc:
        std::stable_sort(
            m_filteredIndices.begin(), m_filteredIndices.end(),
            [this](uint32_t a, uint32_t b) { return m_items[a].title > m_items[b].title; });
        break;
    case SortCriteria::DateNewest:
        std::stable_sort(m_filteredIndices.begin(), m_filteredIndices.end(),
                         [this](uint32_t a, uint32_t b) {
                             return m_items[a].date_modified > m_items[b].date_modified;
                         });
        break;
    case SortCriteria::DateOldest:
        std::stable_sort(m_filteredIndices.begin(), m_filteredIndices.end(),
                         [this](uint32_t a, uint32_t b) {
                             return m_items[a].date_modified < m_items[b].date_modified;
                         });
        break;
    case SortCriteria::SizeLargest:
        std::stable_sort(
            m_filteredIndices.begin(), m_filteredIndices.end(),
            [this](uint32_t a, uint32_t b) { return m_items[a].file_size > m_items[b].file_size; });
        break;
    case SortCriteria::SizeSmallest:
        std::stable_sort(
            m_filteredIndices.begin(), m_filteredIndices.end(),
            [this](uint32_t a, uint32_t b) { return m_items[a].file_size < m_items[b].file_size; });
        break;
    case SortCriteria::Type:
        std::stable_sort(m_filteredIndices.begin(), m_filteredIndices.end(),
                         [this](uint32_t a, uint32_t b) {
                             if (m_items[a].badge != m_items[b].badge) {
                                 return static_cast<uint32_t>(m_items[a].badge) <
                                        static_cast<uint32_t>(m_items[b].badge);
                             }
                             return m_items[a].title < m_items[b].title;
                         });
        break;
    case SortCriteria::DominantHue:
        m_radixKeysScratch.resize(m_filteredIndices.size());
        for (size_t i = 0; i < m_filteredIndices.size(); ++i) {
            const uint32_t idx = m_filteredIndices[i];
            m_radixKeysScratch[i] = SortKeyItem{idx, m_itemHueQ16[idx]};
        }
        radix_sort_oklab_hue(m_radixKeysScratch, m_radixTmpScratch);
        for (size_t i = 0; i < m_filteredIndices.size(); ++i) {
            m_filteredIndices[i] = m_radixKeysScratch[i].item_index;
        }
        break;
    }
}

double FuzzyCatalogIndex::benchmarkSearch(std::string_view query, size_t iterations) const
{
    if (m_items.empty() || iterations == 0)
        return 0.0;

    const std::string lower_query = toLowerString(query);
    const uint64_t pat_len = std::min<uint64_t>(lower_query.size(), 64);
    if (pat_len == 0) {
        return 0.0;
    }
    std::array<uint64_t, 256> char_mask{};
    char_mask.fill(~0ULL);
    for (uint64_t i = 0; i < pat_len; ++i) {
        uint8_t lo = static_cast<uint8_t>(toLowerAscii(lower_query[i]));
        uint8_t up = (lo >= 'a' && lo <= 'z') ? static_cast<uint8_t>(lo - ('a' - 'A')) : lo;
        char_mask[lo] &= ~(1ULL << i);
        char_mask[up] &= ~(1ULL << i);
    }

    auto start = std::chrono::high_resolution_clock::now();
    volatile int total_matches = 0;

    const uint64_t match_mask = 1ULL << (pat_len - 1);
#if WWAL_HAVE_X86_RUNTIME_AVX2
    const bool use_avx2 = __builtin_cpu_supports("avx2");
#endif

    for (size_t it = 0; it < iterations; ++it) {
        int matches = 0;
#if WWAL_HAVE_X86_RUNTIME_AVX2
        if (use_avx2 && lower_query.size() <= 64U) {
            size_t tail_idx = 0;
            matches = countMatchesBatchAVX2(m_lowerTitleArena, m_titleOff, m_titleLen, char_mask,
                                            match_mask, pat_len, tail_idx);
            const char *base = m_lowerTitleArena.data();
            for (size_t i = tail_idx; i < m_items.size(); ++i) {
                const std::string_view sv(base + m_titleOff[i], m_titleLen[i]);
                if (scoreItemFuzzy(sv, lower_query, char_mask, pat_len) >= 0)
                    ++matches;
            }
        } else {
            for (const auto &item : m_items) {
                if (scoreItemFuzzy(item.title, lower_query, char_mask, pat_len) >= 0)
                    ++matches;
            }
        }
#else
        (void)match_mask;
        for (const auto &item : m_items) {
            if (scoreItemFuzzy(item.title, lower_query, char_mask, pat_len) >= 0)
                ++matches;
        }
#endif
        total_matches += matches;
    }
    (void)total_matches;

    auto end = std::chrono::high_resolution_clock::now();
    double total_us = std::chrono::duration<double, std::micro>(end - start).count();
    return total_us / static_cast<double>(iterations);
}

std::string FuzzyCatalogIndex::expandUserPath(std::string_view raw_path)
{
    if (raw_path.empty())
        return {};
    std::string raw(raw_path);
    while (!raw.empty() &&
           (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n')) {
        raw.pop_back();
    }
    size_t start = 0;
    while (start < raw.size() && (raw[start] == ' ' || raw[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        raw.erase(0, start);
    }
    if (raw.empty())
        return {};

    char expanded[WAYWAL_PATH_MAX];
    if (waywal_expand_path(raw.c_str(), expanded, sizeof(expanded)) && expanded[0] != '\0') {
        std::string res(expanded);
        while (res.size() > 1 && res.back() == '/') {
            res.pop_back();
        }
        return res;
    }

    if (raw[0] == '~') {
        const char *home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            if (raw.size() == 1) {
                raw = home;
            } else if (raw[1] == '/') {
                raw = std::string(home) + raw.substr(1);
            }
        }
    }
    while (raw.size() > 1 && raw.back() == '/') {
        raw.pop_back();
    }
    return raw;
}

std::string FuzzyCatalogIndex::defaultConfigFilePath()
{
    const char *xdg_cfg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path cfg_dir;
    if (xdg_cfg && xdg_cfg[0] != '\0') {
        cfg_dir = std::filesystem::path(xdg_cfg) / "wwal";
    } else {
        const char *home = std::getenv("HOME");
        cfg_dir = std::filesystem::path(home ? home : "/tmp") / ".config" / "wwal";
    }
    return (cfg_dir / "studio_catalog.json").string();
}

void FuzzyCatalogIndex::resetFolderConfigToDefaults()
{
    m_includedDirs.clear();
    m_excludedDirs.clear();
    rebuildExcludedCache();
    m_scanSystemPaths = true;

    const char *home = std::getenv("HOME");
    std::string home_str = home ? home : "";

    if (!home_str.empty()) {
        std::error_code ec;
        std::filesystem::path pic_wall =
            std::filesystem::path(home_str) / "Pictures" / "Wallpapers";
        std::filesystem::path pic_wall_lower =
            std::filesystem::path(home_str) / "Pictures" / "wallpapers";
        std::filesystem::path home_wall = std::filesystem::path(home_str) / "Wallpapers";
        std::filesystem::path pic_root = std::filesystem::path(home_str) / "Pictures";

        // Smart default: if ~/Pictures/Wallpapers exists, ONLY scan ~/Pictures/Wallpapers
        // rather than all unrelated folders in ~/Pictures!
        if (std::filesystem::exists(pic_wall, ec) && std::filesystem::is_directory(pic_wall, ec)) {
            m_includedDirs.push_back(pic_wall.string());
        } else if (std::filesystem::exists(pic_wall_lower, ec) &&
                   std::filesystem::is_directory(pic_wall_lower, ec)) {
            m_includedDirs.push_back(pic_wall_lower.string());
        } else if (std::filesystem::exists(pic_root, ec) &&
                   std::filesystem::is_directory(pic_root, ec)) {
            m_includedDirs.push_back(pic_root.string());
        }

        if (std::filesystem::exists(home_wall, ec) &&
            std::filesystem::is_directory(home_wall, ec)) {
            m_includedDirs.push_back(home_wall.string());
        }
    }

    const char *xdg_data = std::getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0] != '\0') {
        std::error_code ec;
        std::filesystem::path xw = std::filesystem::path(xdg_data) / "wallpapers";
        if (std::filesystem::exists(xw, ec) && std::filesystem::is_directory(xw, ec)) {
            m_includedDirs.push_back(xw.string());
        }
    } else if (!home_str.empty()) {
        std::error_code ec;
        std::filesystem::path lw =
            std::filesystem::path(home_str) / ".local" / "share" / "wallpapers";
        if (std::filesystem::exists(lw, ec) && std::filesystem::is_directory(lw, ec)) {
            m_includedDirs.push_back(lw.string());
        }
    }
}

void FuzzyCatalogIndex::setIncludedDirectories(const std::vector<std::string> &dirs)
{
    m_includedDirs.clear();
    for (const auto &d : dirs) {
        std::string exp = expandUserPath(d);
        if (!exp.empty()) {
            m_includedDirs.push_back(exp);
        }
    }
    m_configLoaded = true;
}

void FuzzyCatalogIndex::rebuildExcludedCache()
{
    m_excludedNamesLower.clear();
    m_excludedNormPaths.clear();
    for (const auto &raw_ex : m_excludedDirs) {
        if (raw_ex.empty()) {
            continue;
        }
        if (raw_ex.find('/') == std::string::npos) {
            m_excludedNamesLower.push_back(toLowerString(raw_ex));
            continue;
        }
        std::string ex_norm =
            std::filesystem::path(expandUserPath(raw_ex)).lexically_normal().string();
        while (ex_norm.size() > 1 && ex_norm.back() == '/') {
            ex_norm.pop_back();
        }
        if (!ex_norm.empty()) {
            m_excludedNormPaths.push_back(std::move(ex_norm));
        }
    }
}

void FuzzyCatalogIndex::setExcludedDirectories(const std::vector<std::string> &dirs)
{
    m_excludedDirs.clear();
    for (const auto &d : dirs) {
        std::string exp = expandUserPath(d);
        if (!exp.empty()) {
            m_excludedDirs.push_back(exp);
        }
    }
    rebuildExcludedCache();
    m_configLoaded = true;
}

void FuzzyCatalogIndex::addExcludedDirectory(const std::string &dir)
{
    std::string exp = expandUserPath(dir);
    if (exp.empty())
        return;
    if (std::find(m_excludedDirs.begin(), m_excludedDirs.end(), exp) == m_excludedDirs.end()) {
        m_excludedDirs.push_back(exp);
        rebuildExcludedCache();
    }
    m_configLoaded = true;
}

void FuzzyCatalogIndex::setScanSystemAndWorkshopPaths(bool enabled)
{
    m_scanSystemPaths = enabled;
    m_configLoaded = true;
}

bool FuzzyCatalogIndex::isPathExcluded(const std::filesystem::path &p) const
{
    if (m_excludedDirs.empty()) {
        return false;
    }

    if (!m_excludedNamesLower.empty()) {
        const std::string fname_lower = toLowerString(p.filename().string());
        for (const auto &ex_lower : m_excludedNamesLower) {
            if (fname_lower == ex_lower) {
                return true;
            }
            for (const auto &part : p) {
                if (toLowerString(part.string()) == ex_lower) {
                    return true;
                }
            }
        }
    }

    if (!m_excludedNormPaths.empty()) {
        std::string p_str = p.lexically_normal().string();
        while (p_str.size() > 1 && p_str.back() == '/') {
            p_str.pop_back();
        }
        for (const auto &ex_norm : m_excludedNormPaths) {
            if (p_str == ex_norm) {
                return true;
            }
            if (p_str.size() > ex_norm.size() && p_str.compare(0, ex_norm.size(), ex_norm) == 0 &&
                p_str[ex_norm.size()] == '/') {
                return true;
            }
        }
    }
    return false;
}

bool FuzzyCatalogIndex::loadConfigFromFile(const std::string &file_path)
{
    std::string target = file_path.empty() ? defaultConfigFilePath() : file_path;
    QFile f(QString::fromStdString(target));
    if (!f.exists()) {
        m_configLoaded = true;
        return false;
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Avoid re-opening an unreadable config on every subsequent scanAll() (AUD-324).
        m_configLoaded = true;
        return false;
    }

    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        m_configLoaded = true;
        return false;
    }

    QJsonObject obj = doc.object();
    if (obj.contains(QStringLiteral("scan_system_paths"))) {
        m_scanSystemPaths = obj.value(QStringLiteral("scan_system_paths")).toBool(true);
    }

    if (obj.contains(QStringLiteral("include_dirs")) &&
        obj.value(QStringLiteral("include_dirs")).isArray()) {
        std::vector<std::string> inc;
        for (const auto &v : obj.value(QStringLiteral("include_dirs")).toArray()) {
            std::string s = expandUserPath(v.toString().toStdString());
            if (!s.empty())
                inc.push_back(s);
        }
        m_includedDirs = std::move(inc);
    }

    if (obj.contains(QStringLiteral("exclude_dirs")) &&
        obj.value(QStringLiteral("exclude_dirs")).isArray()) {
        std::vector<std::string> exc;
        for (const auto &v : obj.value(QStringLiteral("exclude_dirs")).toArray()) {
            std::string s = expandUserPath(v.toString().toStdString());
            if (!s.empty())
                exc.push_back(s);
        }
        m_excludedDirs = std::move(exc);
        rebuildExcludedCache();
    }

    m_configLoaded = true;
    return true;
}

bool FuzzyCatalogIndex::saveConfigToFile(const std::string &file_path) const
{
    std::string target = file_path.empty() ? defaultConfigFilePath() : file_path;
    QFileInfo fi(QString::fromStdString(target));
    QDir dir = fi.dir();
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("scan_system_paths"), m_scanSystemPaths);

    QJsonArray incArr;
    for (const auto &d : m_includedDirs) {
        incArr.append(QString::fromStdString(d));
    }
    root.insert(QStringLiteral("include_dirs"), incArr);

    QJsonArray excArr;
    for (const auto &d : m_excludedDirs) {
        excArr.append(QString::fromStdString(d));
    }
    root.insert(QStringLiteral("exclude_dirs"), excArr);

    QSaveFile f(QString::fromStdString(target));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (f.write(payload) != payload.size()) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

void FuzzyCatalogIndex::scanDirectory(const std::filesystem::path &dir_path, size_t max_depth)
{
    std::error_code ec;
    if (!std::filesystem::exists(dir_path, ec) || !std::filesystem::is_directory(dir_path, ec)) {
        return;
    }
    if (isPathExcluded(dir_path)) {
        return;
    }

    static const std::vector<std::string> image_exts = {".png",  ".jpg", ".jpeg", ".webp", ".jxl",
                                                        ".avif", ".bmp", ".gif",  ".svg",  ".qoi"};
    static const std::vector<std::string> video_exts = {".mp4", ".mkv", ".webm", ".avi", ".mov"};
    static const std::vector<std::string> shader_exts = {".frag", ".glsl", ".shadertoy", ".comp"};

    auto it_opts = std::filesystem::directory_options::skip_permission_denied;
    const std::filesystem::recursive_directory_iterator end;
    for (auto it = std::filesystem::recursive_directory_iterator(dir_path, it_opts, ec);
         it != end;) {

        if (ec) {
            ec.clear();
            it.increment(ec);
            continue;
        }

        if (static_cast<size_t>(it.depth()) > max_depth) {
            // pop(ec) already advances past the parent directory; avoid ++it on end (AUD-160).
            it.pop(ec);
            continue;
        }

        const auto &entry = *it;
        const auto &p = entry.path();
        const std::string filename = p.filename().string();
        if (filename.empty() || filename[0] == '.') {
            if (entry.is_directory(ec)) {
                it.disable_recursion_pending();
            }
            ec.clear();
            it.increment(ec);
            continue;
        }

        if (entry.is_directory(ec)) {
            if (static_cast<size_t>(it.depth()) >= max_depth || isPathExcluded(p)) {
                it.disable_recursion_pending();
            }
            ec.clear();
            it.increment(ec);
            continue;
        }

        if (!entry.is_regular_file(ec)) {
            ec.clear();
            it.increment(ec);
            continue;
        }

        if (isPathExcluded(p)) {
            it.increment(ec);
            continue;
        }

        std::filesystem::path norm_p = std::filesystem::weakly_canonical(p, ec);
        if (ec) {
            ec.clear();
            norm_p = p.lexically_normal();
        }
        std::string norm_str = norm_p.string();
        if (m_seenPaths.contains(norm_str)) {
            it.increment(ec);
            continue;
        }

        WallpaperBadge badge = WallpaperBadge::Image;
        uint32_t flags = WAYWAL_THUMB_FLAG_NONE;

        if (hasExtension(p, image_exts)) {
            badge = WallpaperBadge::Image;
        } else if (hasExtension(p, video_exts)) {
            badge = WallpaperBadge::Video;
            flags |= WAYWAL_THUMB_FLAG_VIDEO;
        } else if (hasExtension(p, shader_exts)) {
            badge = WallpaperBadge::Shader;
            flags |= WAYWAL_THUMB_FLAG_SHADER;
        } else if (p.extension() == ".xml") {
            std::ifstream f(p);
            std::string line;
            bool is_gnome = false;
            for (int l = 0; l < 15 && std::getline(f, line); ++l) {
                if (line.find("<wallpaper") != std::string::npos ||
                    line.find("<timed") != std::string::npos ||
                    line.find("<static") != std::string::npos) {
                    is_gnome = true;
                    break;
                }
            }
            if (is_gnome) {
                badge = WallpaperBadge::GnomeXml;
            } else {
                it.increment(ec);
                continue;
            }
        } else {
            it.increment(ec);
            continue;
        }

        uint64_t fsize = entry.file_size(ec);
        if (ec) {
            fsize = 0;
            ec.clear();
        }

        if (fsize > 0) {
            const std::string sizeNameKey = toLowerString(filename) + ":" + std::to_string(fsize);
            auto prevIt = m_sizeNameFirstPath.find(sizeNameKey);
            if (prevIt == m_sizeNameFirstPath.end()) {
                m_sizeNameFirstPath.emplace(sizeNameKey, norm_str);
            } else {
                if (!prevIt->second.empty()) {
                    const std::string firstDigest = computeFileSha256Hex(prevIt->second);
                    if (!firstDigest.empty()) {
                        m_seenContentDigests.insert(firstDigest);
                    }
                    prevIt->second.clear();
                }
                const std::string curDigest = computeFileSha256Hex(norm_p);
                if (!curDigest.empty() && !m_seenContentDigests.insert(curDigest).second) {
                    it.increment(ec);
                    continue;
                }
            }
        }
        m_seenPaths.insert(norm_str);

        CatalogItem item;
        item.badge = badge;
        item.flags = flags;
        item.path = p.string();
        item.title = p.stem().string();
        item.file_size = fsize;

        auto ftime = entry.last_write_time(ec);
        if (!ec) {
            item.date_modified =
                std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
        } else {
            item.date_modified = 0;
            ec.clear();
        }

        item.aspect_ratio = 16.0f / 9.0f;
        item.dominant_color = calculateSampleDominantColor(p);
        item.tag_mask = WallpaperTags::parseTagsFromString(p.string());

        addItem(std::move(item));
        it.increment(ec);
    }
}

void FuzzyCatalogIndex::scanSteamWorkshop()
{
    // 1. First attempt scan via libwaywal_core.a we_workshop_scan_all()
    we_project_t *ws_items = nullptr;
    size_t ws_count = 0;

    if (we_workshop_scan_all(&ws_items, &ws_count) && ws_items && ws_count > 0) {
        for (size_t i = 0; i < ws_count; ++i) {
            const we_project_t &proj = ws_items[i];
            if (isPathExcluded(proj.workshop_dir)) {
                continue;
            }
            std::string ws_path(proj.workshop_dir);
            if (!ws_path.empty() && m_seenPaths.contains(ws_path)) {
                continue;
            }
            if (!ws_path.empty()) {
                m_seenPaths.insert(ws_path);
            }

            CatalogItem item;
            item.badge = WallpaperBadge::Scene;
            item.flags = WAYWAL_THUMB_FLAG_SCENE;

            std::string ptype(proj.type);
            for (char &c : ptype)
                c = toLowerAscii(c);
            if (ptype == "video") {
                item.badge = WallpaperBadge::Video;
                item.flags = WAYWAL_THUMB_FLAG_VIDEO;
            }

            item.title = proj.title[0] != '\0' ? proj.title : proj.workshop_id;
            item.workshop_id = proj.workshop_id;
            item.path = proj.workshop_dir;

            // Resolve preview image
            char prev_path[1024];
            if (we_workshop_find_preview_file(proj.workshop_dir, prev_path, sizeof(prev_path))) {
                item.preview_path = prev_path;
            } else if (proj.preview[0] != '\0') {
                std::filesystem::path pp = std::filesystem::path(proj.workshop_dir) / proj.preview;
                item.preview_path = pp.string();
            }

            item.aspect_ratio = 16.0f / 9.0f;
            item.dominant_color = calculateSampleDominantColor(item.path);
            item.tag_mask = WallpaperTags::parseTagsFromString(item.title);
            item.tag_mask |= WallpaperTags::parseTagsFromString(item.path);

            addItem(std::move(item));
        }
        std::free(ws_items);
    }
}

size_t FuzzyCatalogIndex::scanAll(const std::vector<std::string> &extra_search_paths)
{
    if (!m_configLoaded) {
        loadConfigFromFile();
    }

    clear();

    // Honor WWAL_EXCLUDE_DIRS environment variable (colon-separated)
    if (const char *env_exc = std::getenv("WWAL_EXCLUDE_DIRS"); env_exc && env_exc[0] != '\0') {
        std::stringstream ss(env_exc);
        std::string token;
        while (std::getline(ss, token, ':')) {
            if (!token.empty()) {
                addExcludedDirectory(token);
            }
        }
    }

    // Honor WWAL_WALLPAPER_DIRS environment variable (colon-separated override)
    std::vector<std::string> env_inc_dirs;
    if (const char *env_inc = std::getenv("WWAL_WALLPAPER_DIRS"); env_inc && env_inc[0] != '\0') {
        std::stringstream ss(env_inc);
        std::string token;
        while (std::getline(ss, token, ':')) {
            std::string exp = expandUserPath(token);
            if (!exp.empty()) {
                env_inc_dirs.push_back(exp);
            }
        }
    }

    const auto &dirs_to_scan = !env_inc_dirs.empty() ? env_inc_dirs : m_includedDirs;

    // 1. Scan configured user wallpaper directories (defaults to ~/Pictures/Wallpapers if present)
    for (const auto &d : dirs_to_scan) {
        if (!d.empty()) {
            scanDirectory(expandUserPath(d));
        }
    }

    // 2. Scan system & Steam Workshop paths if enabled
    if (m_scanSystemPaths && env_inc_dirs.empty()) {
        scanDirectory("/usr/share/backgrounds", 2);
        scanDirectory("/usr/share/wallpapers", 2);
        scanSteamWorkshop();
    }

    // 3. Extra user-configured paths passed directly to scanAll
    for (const auto &p : extra_search_paths) {
        if (!p.empty()) {
            scanDirectory(expandUserPath(p));
        }
    }

    // Initial filter & sort
    refilter();
    return m_items.size();
}

} // namespace wwal::studio
