#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace prof {

// Exact latency statistics over raw nanosecond samples. Samples are stored
// (a million of them is 8 MB) and sorted once, so every percentile reported
// is the true order statistic -- no bucketed approximation, no interpolation.
class LatencyStats {
public:
    void reserve(std::size_t n) { samples_.reserve(n); }
    void add(std::uint64_t ns) { samples_.push_back(ns); }
    std::size_t count() const { return samples_.size(); }

    void finalize() {
        std::sort(samples_.begin(), samples_.end());
        double sum = 0.0;
        for (auto s : samples_) sum += static_cast<double>(s);
        mean_ = samples_.empty() ? 0.0 : sum / static_cast<double>(samples_.size());
    }

    // Nearest-rank percentile: the smallest sample at or above the p-th
    // fraction of the distribution. p in (0, 1].
    std::uint64_t percentile(double p) const {
        if (samples_.empty()) return 0;
        std::size_t rank = static_cast<std::size_t>(std::ceil(p * samples_.size()));
        if (rank == 0) rank = 1;
        if (rank > samples_.size()) rank = samples_.size();
        return samples_[rank - 1];
    }

    std::uint64_t p50() const { return percentile(0.50); }
    std::uint64_t p99() const { return percentile(0.99); }
    std::uint64_t p999() const { return percentile(0.999); }
    std::uint64_t max() const { return samples_.empty() ? 0 : samples_.back(); }
    std::uint64_t min() const { return samples_.empty() ? 0 : samples_.front(); }
    double mean() const { return mean_; }

    const std::vector<std::uint64_t>& samples() const { return samples_; }

private:
    std::vector<std::uint64_t> samples_;
    double mean_ = 0.0;
};

}  // namespace prof
