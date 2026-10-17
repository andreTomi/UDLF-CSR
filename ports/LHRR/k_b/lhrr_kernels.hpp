/**
 * @file lhrr_kernels.hpp
 * @brief K1 (fillPos) and K2 (sort) kernels for LHRR sparse v1
 */

#ifndef LHRR_KERNELS_HPP
#define LHRR_KERNELS_HPP

#include "lhrr_types.hpp"
#include <vector>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <limits>

namespace lhrr {

struct WEntry {
    int col;
    uint16_t pos;
    uint16_t weight;
};
using WRow = std::vector<WEntry>;
using WType = std::vector<WRow>;

inline WType buildW(const uint32_t* rkLists, int n, int l) {
    if (l <= 0 || l >= 65536)
        throw std::invalid_argument("LHRR: require 0 < L < 65536 for uint16_t W fields");
    WType W(n);
    for (int rk = 0; rk < n; ++rk) {
        W[rk].reserve(l);
        for (int pos = 0; pos < l; ++pos) {
            int img = rkLists[(long)l * rk + pos];
            W[rk].push_back({img, (uint16_t)pos, (uint16_t)(l - pos)});
        }
        std::sort(W[rk].begin(), W[rk].end(), [](const WEntry& a, const WEntry& b) {
            if (a.col != b.col) return a.col < b.col;
            return a.pos < b.pos;
        });
    }
    return W;
}

inline uint16_t getWeight(const WRow& row, int img) {
    WEntry key{img, 0, 0};
    auto it = std::lower_bound(row.begin(), row.end(), key,
        [](const WEntry& a, const WEntry& b) { return a.col < b.col; });
    if (it != row.end() && it->col == img) return it->weight;
    return 0;
}

struct WColLess {
    bool operator()(const WEntry& entry, int value) const { return entry.col < value; }
    bool operator()(int value, const WEntry& entry) const { return value < entry.col; }
};

inline void k2_sort(float* a, uint32_t* list, int l, int self_id) {
    int i, j;
    float keyA;
    uint32_t keyR;
    for (j = 2; j < l; ++j) {
        keyA = a[j];
        keyR = list[j];
        i = j - 1;
        while (i > 0 && a[i] < keyA) {
            a[i + 1] = a[i];
            list[i + 1] = list[i];
            --i;
        }
        a[i + 1] = keyA;
        list[i + 1] = keyR;
    }
    i = 0;
    while (i < l && list[i] != (uint32_t)self_id) ++i;
    if (i > 0 && i < l) {
        std::swap(list[0], list[i]);
        std::swap(a[0], a[i]);
    }
}

// The original dense initial sort sums forward and reverse positional weights.
// W is sorted by col, so equal_range preserves repeated-ID contributions.
inline float initial_sort_score(const WType& W, int rk, int img) {
    float score = 0.0f;
    const auto forward = std::equal_range(W[rk].begin(), W[rk].end(), img, WColLess{});
    for (auto it = forward.first; it != forward.second; ++it)
        score += static_cast<float>(it->weight);
    const auto backward = std::equal_range(W[img].begin(), W[img].end(), rk, WColLess{});
    for (auto it = backward.first; it != backward.second; ++it)
        score += static_cast<float>(it->weight);
    return score;
}

inline void initial_sort_rankings(const WType& W, uint32_t* rkLists,
                                  int n, int l) {
    std::vector<float> scores(l);
    std::vector<uint32_t> list(l);
    for (int rk = 0; rk < n; ++rk) {
        for (int j = 0; j < l; ++j) {
            const int img = static_cast<int>(rkLists[(long)l * rk + j]);
            scores[j] = initial_sort_score(W, rk, img);
            list[j] = static_cast<uint32_t>(img);
        }
        k2_sort(scores.data(), list.data(), l, rk);
        for (int j = 0; j < l; ++j)
            rkLists[(long)l * rk + j] = list[j];
    }
}

} // namespace lhrr

#endif // LHRR_KERNELS_HPP
