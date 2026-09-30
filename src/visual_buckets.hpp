#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace omniocr::detail {
struct VisualCandidate {
    uint64_t ticket;
    size_t visual, prefill;
};

struct VisualBuckets {
    size_t capture_size = 1;
    std::vector<std::vector<VisualCandidate>> groups;
};

// Repartition only the bounded pending snapshot. Equal-frequency buckets
// adapt even when every request falls below the old first static edge.
// Capture sizes are hints for client admission, not vLLM batch guarantees.
inline VisualBuckets partition_visual_buckets(std::vector<VisualCandidate> pending,
    const std::vector<int>& captures, size_t capacity, size_t prefill_budget) {
    VisualBuckets result;
    if (pending.empty() || !capacity) return result;
    std::stable_sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) {
        return a.visual < b.visual;
    });
    capacity = std::min(capacity, pending.size());
    size_t anchor = 0;
    if (captures.empty()) result.capture_size = capacity;
    else {
        // Prefer the largest captured request count for which at least one
        // visually adjacent group fits the estimated prefill budget. Anchor
        // a bucket on that window, even when its start is not width-aligned.
        for (int capture : captures) {
            const size_t width = size_t(capture);
            if (width > capacity) break;
            size_t sum = 0;
            size_t best = 0, best_start = 0;
            bool feasible = false;
            for (size_t i = 0; i < pending.size(); ++i) {
                sum += pending[i].prefill;
                if (i >= width) sum -= pending[i - width].prefill;
                if (i + 1 >= width && sum <= prefill_budget && (!feasible || sum > best)) {
                    feasible = true;
                    best = sum;
                    best_start = i + 1 - width;
                }
            }
            if (feasible) {
                result.capture_size = width;
                anchor = best_start;
            }
        }
    }
    auto append_range = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; i += result.capture_size) {
            const size_t end = std::min(to, i + result.capture_size);
            result.groups.emplace_back(pending.begin() + i, pending.begin() + end);
        }
    };
    append_range(0, anchor);
    if (anchor + result.capture_size <= pending.size()) {
        const size_t end = anchor + result.capture_size;
        result.groups.emplace_back(pending.begin() + anchor, pending.begin() + end);
        append_range(end, pending.size());
    } else {
        append_range(anchor, pending.size());
    }
    return result;
}
} // namespace omniocr::detail
