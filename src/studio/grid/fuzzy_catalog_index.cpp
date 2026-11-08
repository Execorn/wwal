// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuzzy_catalog_index.hpp"

#include <waywal/path.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

namespace wwal::studio {

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace {

inline char toLowerAscii(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c + ('a' - 'A'));
    }
    return c;
}

inline std::string toLowerString(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (char c : sv) {
        result.push_back(toLowerAscii(c));
    }
    return result;
}

uint32_t calculateSampleDominantColor(const std::filesystem::path &path) {
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

bool hasExtension(const std::filesystem::path &path, const std::vector<std::string> &exts) {
    std::string ext = path.extension().string();
    for (char &c : ext) c = toLowerAscii(c);
    for (const auto &target : exts) {
        if (ext == target) return true;
    }
    return false;
}

#if defined(__AVX2__)
inline void score4ItemsAVX2(const std::string_view texts[4],
                            const std::array<uint64_t, 256> &char_mask,
                            uint64_t match_mask, size_t pat_len, int scores[4]) {
    for (int k = 0; k < 4; ++k) scores[k] = -1;

    size_t max_len = 0;
    for (int k = 0; k < 4; ++k) {
        if (texts[k].size() > max_len) max_len = texts[k].size();
    }
    if (max_len < pat_len) return;

    __m256i r = _mm256_set1_epi64x(~0ULL);
    __m256i mmask = _mm256_set1_epi64x(match_mask);

    for (size_t i = 0; i < max_len; ++i) {
        uint64_t c0 = (i < texts[0].size()) ? char_mask[static_cast<uint8_t>(texts[0][i])] : ~0ULL;
        uint64_t c1 = (i < texts[1].size()) ? char_mask[static_cast<uint8_t>(texts[1][i])] : ~0ULL;
        uint64_t c2 = (i < texts[2].size()) ? char_mask[static_cast<uint8_t>(texts[2][i])] : ~0ULL;
        uint64_t c3 = (i < texts[3].size()) ? char_mask[static_cast<uint8_t>(texts[3][i])] : ~0ULL;

        __m256i cm = _mm256_set_epi64x(c3, c2, c1, c0);
        r = _mm256_or_si256(_mm256_slli_epi64(r, 1), cm);

        __m256i matched = _mm256_cmpeq_epi64(_mm256_and_si256(r, mmask), _mm256_setzero_si256());
        int mask = _mm256_movemask_pd(_mm256_castsi256_pd(matched));
        if (mask) {
            if ((mask & 1) && scores[0] < 0) scores[0] = 250 - static_cast<int>(i);
            if ((mask & 2) && scores[1] < 0) scores[1] = 250 - static_cast<int>(i);
            if ((mask & 4) && scores[2] < 0) scores[2] = 250 - static_cast<int>(i);
            if ((mask & 8) && scores[3] < 0) scores[3] = 250 - static_cast<int>(i);
            if (scores[0] >= 0 && scores[1] >= 0 && scores[2] >= 0 && scores[3] >= 0) break;
        }
    }
}
#endif

} // namespace

namespace WallpaperTags {

const std::vector<TagDefinition> &allKnownTags() {
    static const std::vector<TagDefinition> tags = {
        { Anime,        "anime",        "Anime" },
        { Nature,       "nature",       "Nature" },
        { Abstract,     "abstract",     "Abstract" },
        { Cyberpunk,    "cyberpunk",    "Cyberpunk" },
        { Minimal,      "minimal",      "Minimal" },
        { SciFi,        "scifi",        "Sci-Fi" },
        { Landscape,    "landscape",    "Landscape" },
        { Architecture, "architecture", "Architecture" },
        { Dark,         "dark",         "Dark" },
        { Light,        "light",        "Light" },
        { Retro,        "retro",        "Retro" },
        { Fantasy,      "fantasy",      "Fantasy" },
        { Vehicles,     "vehicles",     "Vehicles" },
        { Animals,      "animals",      "Animals" },
        { PixelArt,     "pixelart",     "Pixel Art" },
        { Space,        "space",        "Space" },
        { Urban,        "urban",        "Urban" },
        { DigitalArt,   "digitalart",   "Digital Art" },
        { Photography,  "photography",  "Photography" }
    };
    return tags;
}

uint64_t parseTagsFromString(std::string_view text) {
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

waywal_thumb_meta_t CatalogItem::toThumbMeta() const noexcept {
    waywal_thumb_meta_t m{};
    m.id = id;
    m.flags = flags;
    m.tag_mask = tag_mask;
    std::strncpy(m.title, title.c_str(), sizeof(m.title) - 1);
    m.title[sizeof(m.title) - 1] = '\0';
    std::strncpy(m.path, path.c_str(), sizeof(m.path) - 1);
    m.path[sizeof(m.path) - 1] = '\0';
    m.aspect_ratio = aspect_ratio;
    m.dominant_color = dominant_color;
    return m;
}

float CatalogItem::computeHue() const noexcept {
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

FuzzyCatalogIndex::FuzzyCatalogIndex() {
    m_items.reserve(2048);
    m_filteredIndices.reserve(2048);
    resetFolderConfigToDefaults();
}

uint32_t FuzzyCatalogIndex::addItem(CatalogItem item) {
    item.id = static_cast<uint32_t>(m_items.size());
    m_items.push_back(std::move(item));
    return item.id;
}

void FuzzyCatalogIndex::clear() {
    m_items.clear();
    m_filteredIndices.clear();
    m_seenPaths.clear();
}

void FuzzyCatalogIndex::setSearchQuery(std::string_view query) {
    if (m_activeQuery != query) {
        m_activeQuery = std::string(query);
        refilter();
    }
}

void FuzzyCatalogIndex::setTagMask(uint64_t tag_mask, bool match_all) {
    if (m_activeTagMask != tag_mask || m_tagMatchAll != match_all) {
        m_activeTagMask = tag_mask;
        m_tagMatchAll = match_all;
        refilter();
    }
}

void FuzzyCatalogIndex::setBadgeFilter(uint32_t badge_mask) {
    if (m_activeBadgeMask != badge_mask) {
        m_activeBadgeMask = badge_mask;
        refilter();
    }
}

void FuzzyCatalogIndex::setSorting(SortCriteria criteria) {
    if (m_activeSorting != criteria) {
        m_activeSorting = criteria;
        sortFiltered();
    }
}

int FuzzyCatalogIndex::scoreItemFuzzy(std::string_view text, std::string_view pattern,
                                      const std::array<uint64_t, 256> &char_mask,
                                      uint64_t pattern_len) {
    if (pattern.empty()) {
        return 100;
    }
    const size_t text_len = text.size();
    if (text_len < pattern_len) {
        return -1;
    }

    const char *p = text.data();
    const uint64_t match_mask = 1ULL << (pattern_len - 1);

    // Fast exact prefix check using precomputed char_mask
    bool is_prefix = true;
    for (size_t i = 0; i < pattern_len; ++i) {
        uint8_t c = static_cast<uint8_t>(p[i]);
        if (char_mask[c] & (1ULL << i)) {
            is_prefix = false;
            break;
        }
    }
    if (is_prefix) {
        return 1000 - static_cast<int>(text_len - pattern_len);
    }

    // Pass 1: Ultra-fast exact Bitap with pigeonhole character-presence filter
    uint64_t r0 = ~0ULL;
    uint64_t seen_bits = ~0ULL;
    for (size_t i = 0; i < text_len; ++i) {
        uint64_t cm = char_mask[static_cast<uint8_t>(p[i])];
        r0 = (r0 << 1) | cm;
        seen_bits &= cm;
        if (!(r0 & match_mask)) {
            bool word_boundary = false;
            if (i >= pattern_len) {
                char prev = p[i - pattern_len];
                if (prev == ' ' || prev == '_' || prev == '-' || prev == '/' || prev == '.') {
                    word_boundary = true;
                }
            }
            return (word_boundary ? 500 : 250) - static_cast<int>(i);
        }
    }

    // If >2 pattern positions never occurred anywhere in text, edit distance is >2
    const uint64_t pat_mask = (pattern_len >= 64) ? ~0ULL : ((1ULL << pattern_len) - 1ULL);
    if (__builtin_popcountll(seen_bits & pat_mask) > 2) {
        return -1;
    }

    // Pass 2: Fuzzy Bitap (up to 2 errors) - evaluated ONLY if exact match was not found
    r0 = ~0ULL;
    uint64_t r1 = ~0ULL;
    uint64_t r2 = ~0ULL;
    bool err1 = false;
    bool err2 = false;

    for (size_t i = 0; i < text_len; ++i) {
        uint8_t c = static_cast<uint8_t>(p[i]);
        uint64_t cm = char_mask[c];
        uint64_t r0_old = r0;
        uint64_t r1_old = r1;

        r0 = ((r0 << 1) | cm);
        r1 = ((r1 << 1) | cm) & (r0_old << 1) & r0_old & r0;
        r2 = ((r2 << 1) | cm) & (r1_old << 1) & r1_old & r1;

        if (!(r1 & match_mask)) {
            err1 = true;
            break;
        } else if (!(r2 & match_mask)) {
            err2 = true;
        }
    }

    if (err1) return 150;
    if (err2) return 80;

    return -1; // No match
}

void FuzzyCatalogIndex::refilter() {
    auto start_time = std::chrono::high_resolution_clock::now();

    m_filteredIndices.clear();

    const bool has_query = !m_activeQuery.empty();
    const std::string lower_query = toLowerString(m_activeQuery);
    const uint64_t pat_len = std::min<uint64_t>(lower_query.size(), 64);

    std::array<uint64_t, 256> char_mask{};
    char_mask.fill(~0ULL);
    if (has_query) {
        for (uint64_t i = 0; i < pat_len; ++i) {
            uint8_t lo = static_cast<uint8_t>(toLowerAscii(lower_query[i]));
            uint8_t up = (lo >= 'a' && lo <= 'z') ? static_cast<uint8_t>(lo - ('a' - 'A')) : lo;
            char_mask[lo] &= ~(1ULL << i);
            char_mask[up] &= ~(1ULL << i);
        }
    }

    std::vector<std::pair<int, uint32_t>> scored_items;
    scored_items.reserve(m_items.size());

    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto &item = m_items[i];

        // 1. Badge Mask Filtering
        uint32_t badge_bit = 1u << static_cast<uint32_t>(item.badge);
        if ((m_activeBadgeMask & badge_bit) == 0 && m_activeBadgeMask != 0xFFFFFFFFu) {
            continue;
        }

        // 2. Tag Mask Filtering
        if (m_activeTagMask != 0) {
            if (m_tagMatchAll) {
                if ((item.tag_mask & m_activeTagMask) != m_activeTagMask) {
                    continue;
                }
            } else {
                if ((item.tag_mask & m_activeTagMask) == 0) {
                    continue;
                }
            }
        }

        // 3. Search Query Myers/Bitap Scoring
        if (has_query) {
            int score = scoreItemFuzzy(item.title, lower_query, char_mask, pat_len);
            if (score < 0) {
                // If title doesn't match, check path as fallback
                score = scoreItemFuzzy(item.path, lower_query, char_mask, pat_len);
                if (score >= 0) {
                    score -= 50; // Slight penalty for path-only match
                }
            }
            if (score >= 0) {
                scored_items.emplace_back(score, static_cast<uint32_t>(i));
            }
        } else {
            m_filteredIndices.push_back(static_cast<uint32_t>(i));
        }
    }

    if (has_query) {
        // If sorting by relevance when searching
        std::stable_sort(scored_items.begin(), scored_items.end(),
                         [](const auto &a, const auto &b) {
                             return a.first > b.first;
                         });
        m_filteredIndices.resize(scored_items.size());
        for (size_t j = 0; j < scored_items.size(); ++j) {
            m_filteredIndices[j] = scored_items[j].second;
        }
    } else {
        sortFiltered();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    m_lastFilterDurationUs = std::chrono::duration<double, std::micro>(end_time - start_time).count();
}

void FuzzyCatalogIndex::sortFiltered() {
    if (m_filteredIndices.empty()) return;

    switch (m_activeSorting) {
    case SortCriteria::NameAsc:
        std::stable_sort(m_filteredIndices.begin(), m_filteredIndices.end(),
                         [this](uint32_t a, uint32_t b) {
                             return m_items[a].title < m_items[b].title;
                         });
        break;
    case SortCriteria::NameDesc:
        std::stable_sort(m_filteredIndices.begin(), m_filteredIndices.end(),
                         [this](uint32_t a, uint32_t b) {
                             return m_items[a].title > m_items[b].title;
                         });
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
        std::stable_sort(m_filteredIndices.begin(), m_filteredIndices.end(),
                         [this](uint32_t a, uint32_t b) {
                             return m_items[a].file_size > m_items[b].file_size;
                         });
        break;
    case SortCriteria::SizeSmallest:
        std::stable_sort(m_filteredIndices.begin(), m_filteredIndices.end(),
                         [this](uint32_t a, uint32_t b) {
                             return m_items[a].file_size < m_items[b].file_size;
                         });
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
        std::stable_sort(m_filteredIndices.begin(), m_filteredIndices.end(),
                         [this](uint32_t a, uint32_t b) {
                             return m_items[a].computeHue() < m_items[b].computeHue();
                         });
        break;
    }
}

double FuzzyCatalogIndex::benchmarkSearch(std::string_view query, size_t iterations) const {
    if (m_items.empty() || iterations == 0) return 0.0;

    const std::string lower_query = toLowerString(query);
    const uint64_t pat_len = std::min<uint64_t>(lower_query.size(), 64);
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

#if defined(__AVX2__)
    const uint64_t match_mask = 1ULL << (pat_len - 1);
    const size_t n = m_items.size();
#endif

    for (size_t it = 0; it < iterations; ++it) {
        int matches = 0;
#if defined(__AVX2__)
        size_t i = 0;
        int scores[4];
        std::string_view texts[4];
        for (; i + 4 <= n; i += 4) {
            texts[0] = m_items[i].title;
            texts[1] = m_items[i + 1].title;
            texts[2] = m_items[i + 2].title;
            texts[3] = m_items[i + 3].title;
            score4ItemsAVX2(texts, char_mask, match_mask, pat_len, scores);
            if (scores[0] >= 0) ++matches;
            if (scores[1] >= 0) ++matches;
            if (scores[2] >= 0) ++matches;
            if (scores[3] >= 0) ++matches;
        }
        for (; i < n; ++i) {
            int s = scoreItemFuzzy(m_items[i].title, lower_query, char_mask, pat_len);
            if (s >= 0) ++matches;
        }
#else
        for (const auto &item : m_items) {
            int s = scoreItemFuzzy(item.title, lower_query, char_mask, pat_len);
            if (s >= 0) ++matches;
        }
#endif
        total_matches += matches;
    }
    (void)total_matches;

    auto end = std::chrono::high_resolution_clock::now();
    double total_us = std::chrono::duration<double, std::micro>(end - start).count();
    return total_us / static_cast<double>(iterations);
}

std::string FuzzyCatalogIndex::expandUserPath(std::string_view raw_path) {
    if (raw_path.empty()) return {};
    std::string raw(raw_path);
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n')) {
        raw.pop_back();
    }
    size_t start = 0;
    while (start < raw.size() && (raw[start] == ' ' || raw[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        raw.erase(0, start);
    }
    if (raw.empty()) return {};

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

std::string FuzzyCatalogIndex::defaultConfigFilePath() {
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

void FuzzyCatalogIndex::resetFolderConfigToDefaults() {
    m_includedDirs.clear();
    m_excludedDirs.clear();
    m_scanSystemPaths = true;

    const char *home = std::getenv("HOME");
    std::string home_str = home ? home : "";

    if (!home_str.empty()) {
        std::error_code ec;
        std::filesystem::path pic_wall = std::filesystem::path(home_str) / "Pictures" / "Wallpapers";
        std::filesystem::path pic_wall_lower = std::filesystem::path(home_str) / "Pictures" / "wallpapers";
        std::filesystem::path home_wall = std::filesystem::path(home_str) / "Wallpapers";
        std::filesystem::path pic_root = std::filesystem::path(home_str) / "Pictures";

        // Smart default: if ~/Pictures/Wallpapers exists, ONLY scan ~/Pictures/Wallpapers
        // rather than all unrelated folders in ~/Pictures!
        if (std::filesystem::exists(pic_wall, ec) && std::filesystem::is_directory(pic_wall, ec)) {
            m_includedDirs.push_back(pic_wall.string());
        } else if (std::filesystem::exists(pic_wall_lower, ec) && std::filesystem::is_directory(pic_wall_lower, ec)) {
            m_includedDirs.push_back(pic_wall_lower.string());
        } else if (std::filesystem::exists(pic_root, ec) && std::filesystem::is_directory(pic_root, ec)) {
            m_includedDirs.push_back(pic_root.string());
        }

        if (std::filesystem::exists(home_wall, ec) && std::filesystem::is_directory(home_wall, ec)) {
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
        std::filesystem::path lw = std::filesystem::path(home_str) / ".local" / "share" / "wallpapers";
        if (std::filesystem::exists(lw, ec) && std::filesystem::is_directory(lw, ec)) {
            m_includedDirs.push_back(lw.string());
        }
    }
}

void FuzzyCatalogIndex::setIncludedDirectories(const std::vector<std::string> &dirs) {
    m_includedDirs.clear();
    for (const auto &d : dirs) {
        std::string exp = expandUserPath(d);
        if (!exp.empty()) {
            m_includedDirs.push_back(exp);
        }
    }
    m_configLoaded = true;
}

void FuzzyCatalogIndex::setExcludedDirectories(const std::vector<std::string> &dirs) {
    m_excludedDirs.clear();
    for (const auto &d : dirs) {
        std::string exp = expandUserPath(d);
        if (!exp.empty()) {
            m_excludedDirs.push_back(exp);
        }
    }
    m_configLoaded = true;
}

void FuzzyCatalogIndex::addExcludedDirectory(const std::string &dir) {
    std::string exp = expandUserPath(dir);
    if (exp.empty()) return;
    if (std::find(m_excludedDirs.begin(), m_excludedDirs.end(), exp) == m_excludedDirs.end()) {
        m_excludedDirs.push_back(exp);
    }
    m_configLoaded = true;
}

void FuzzyCatalogIndex::setScanSystemAndWorkshopPaths(bool enabled) {
    m_scanSystemPaths = enabled;
    m_configLoaded = true;
}

bool FuzzyCatalogIndex::isPathExcluded(const std::filesystem::path &p) const {
    if (m_excludedDirs.empty()) return false;

    std::string p_str = p.lexically_normal().string();
    while (p_str.size() > 1 && p_str.back() == '/') {
        p_str.pop_back();
    }
    std::string fname_lower = toLowerString(p.filename().string());

    for (const auto &raw_ex : m_excludedDirs) {
        if (raw_ex.empty()) continue;

        // If exclusion rule is a bare folder name (no slash), match against directory/file name or path component
        if (raw_ex.find('/') == std::string::npos) {
            std::string ex_lower = toLowerString(raw_ex);
            if (fname_lower == ex_lower) {
                return true;
            }
            for (const auto &part : p) {
                if (toLowerString(part.string()) == ex_lower) {
                    return true;
                }
            }
            continue;
        }

        std::string ex_norm = std::filesystem::path(expandUserPath(raw_ex)).lexically_normal().string();
        while (ex_norm.size() > 1 && ex_norm.back() == '/') {
            ex_norm.pop_back();
        }
        if (ex_norm.empty()) continue;

        if (p_str == ex_norm) {
            return true;
        }
        if (p_str.size() > ex_norm.size() &&
            p_str.compare(0, ex_norm.size(), ex_norm) == 0 &&
            p_str[ex_norm.size()] == '/') {
            return true;
        }
    }
    return false;
}

bool FuzzyCatalogIndex::loadConfigFromFile(const std::string &file_path) {
    std::string target = file_path.empty() ? defaultConfigFilePath() : file_path;
    QFile f(QString::fromStdString(target));
    if (!f.exists()) {
        m_configLoaded = true;
        return false;
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    QJsonObject obj = doc.object();
    if (obj.contains(QStringLiteral("scan_system_paths"))) {
        m_scanSystemPaths = obj.value(QStringLiteral("scan_system_paths")).toBool(true);
    }

    if (obj.contains(QStringLiteral("include_dirs")) && obj.value(QStringLiteral("include_dirs")).isArray()) {
        std::vector<std::string> inc;
        for (const auto &v : obj.value(QStringLiteral("include_dirs")).toArray()) {
            std::string s = expandUserPath(v.toString().toStdString());
            if (!s.empty()) inc.push_back(s);
        }
        m_includedDirs = std::move(inc);
    }

    if (obj.contains(QStringLiteral("exclude_dirs")) && obj.value(QStringLiteral("exclude_dirs")).isArray()) {
        std::vector<std::string> exc;
        for (const auto &v : obj.value(QStringLiteral("exclude_dirs")).toArray()) {
            std::string s = expandUserPath(v.toString().toStdString());
            if (!s.empty()) exc.push_back(s);
        }
        m_excludedDirs = std::move(exc);
    }

    m_configLoaded = true;
    return true;
}

bool FuzzyCatalogIndex::saveConfigToFile(const std::string &file_path) const {
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

    QFile f(QString::fromStdString(target));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

void FuzzyCatalogIndex::scanDirectory(const std::filesystem::path &dir_path, size_t max_depth) {
    std::error_code ec;
    if (!std::filesystem::exists(dir_path, ec) || !std::filesystem::is_directory(dir_path, ec)) {
        return;
    }
    if (isPathExcluded(dir_path)) {
        return;
    }

    static const std::vector<std::string> image_exts = {
        ".png", ".jpg", ".jpeg", ".webp", ".jxl", ".avif", ".bmp", ".gif", ".svg", ".qoi"
    };
    static const std::vector<std::string> video_exts = {
        ".mp4", ".mkv", ".webm", ".avi", ".mov"
    };
    static const std::vector<std::string> shader_exts = {
        ".frag", ".glsl", ".shadertoy"
    };

    auto it_opts = std::filesystem::directory_options::skip_permission_denied;
    for (auto it = std::filesystem::recursive_directory_iterator(dir_path, it_opts, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {

        if (ec) {
            ec.clear();
            continue;
        }

        if (static_cast<size_t>(it.depth()) > max_depth) {
            it.pop();
            continue;
        }

        const auto &entry = *it;
        const auto &p = entry.path();
        std::string filename = p.filename().string();
        if (filename.empty() || filename[0] == '.') {
            if (entry.is_directory(ec)) {
                it.disable_recursion_pending();
            }
            ec.clear();
            continue; // Skip hidden files and hidden directories
        }

        if (entry.is_directory(ec)) {
            if (isPathExcluded(p)) {
                it.disable_recursion_pending();
            }
            ec.clear();
            continue;
        }

        if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
        }

        if (isPathExcluded(p)) {
            continue;
        }

        // Deduplicate by canonical / normalized path
        std::filesystem::path norm_p = std::filesystem::weakly_canonical(p, ec);
        if (ec) {
            ec.clear();
            norm_p = p.lexically_normal();
        }
        std::string norm_str = norm_p.string();
        if (m_seenPaths.contains(norm_str)) {
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
            // Check if Gnome timed wallpaper
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
                continue;
            }
        } else {
            continue; // Unsupported
        }

        uint64_t fsize = entry.file_size(ec);
        if (ec) { fsize = 0; ec.clear(); }

        std::string content_sig;
        if (fsize > 0) {
            content_sig = "sig:" + toLowerString(filename) + ":" + std::to_string(fsize);
            if (m_seenPaths.contains(content_sig)) {
                continue;
            }
            m_seenPaths.insert(content_sig);
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
            item.date_modified = std::chrono::duration_cast<std::chrono::seconds>(
                ftime.time_since_epoch()).count();
        } else {
            item.date_modified = 0;
            ec.clear();
        }

        item.aspect_ratio = 16.0f / 9.0f;
        item.dominant_color = calculateSampleDominantColor(p);
        item.tag_mask = WallpaperTags::parseTagsFromString(p.string());

        addItem(std::move(item));
    }
}

void FuzzyCatalogIndex::scanSteamWorkshop() {
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
            for (char &c : ptype) c = toLowerAscii(c);
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

size_t FuzzyCatalogIndex::scanAll(const std::vector<std::string> &extra_search_paths) {
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
