#include "statistics.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace tracto {
namespace {

NumericSummary Summarize(std::vector<double> values) {
  NumericSummary summary;
  if (values.empty()) {
    return summary;
  }

  summary.valid = true;
  summary.min = values.front();
  summary.max = values.front();

  long double sum = 0.0L;
  for (double value : values) {
    sum += value;
    summary.min = std::min(summary.min, value);
    summary.max = std::max(summary.max, value);
  }
  summary.mean = static_cast<double>(sum / static_cast<long double>(values.size()));

  long double squaredError = 0.0L;
  for (double value : values) {
    const long double d = static_cast<long double>(value) - summary.mean;
    squaredError += d * d;
  }
  summary.stddev =
      std::sqrt(static_cast<double>(squaredError / static_cast<long double>(values.size())));

  const std::size_t mid = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
  summary.median = values[mid];
  if (values.size() % 2 == 0) {
    const auto lowerIt =
        std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid));
    summary.median = 0.5 * (*lowerIt + summary.median);
  }

  return summary;
}

}  // namespace

BasicStats ComputeBasicStats(const TractogramStore& store,
                             const std::vector<uint8_t>& aliveFull) {
  if (aliveFull.size() != store.StreamlineCount()) {
    throw std::runtime_error("alive mask size does not match tractogram");
  }
  if (store.lengthsMm.size() != store.StreamlineCount() ||
      store.pointCounts.size() != store.StreamlineCount()) {
    throw std::runtime_error("tractogram statistics arrays are not initialized");
  }

  BasicStats stats;
  stats.fullCount = store.StreamlineCount();

  std::vector<double> lengths;
  std::vector<double> pointCounts;
  lengths.reserve(stats.fullCount);
  pointCounts.reserve(stats.fullCount);

  for (std::size_t i = 0; i < stats.fullCount; ++i) {
    if (!aliveFull[i]) {
      continue;
    }
    ++stats.aliveCount;
    lengths.push_back(store.lengthsMm[i]);
    pointCounts.push_back(static_cast<double>(store.pointCounts[i]));
  }

  stats.deadCount = stats.fullCount - stats.aliveCount;
  if (stats.fullCount > 0) {
    stats.deletedPercent =
        100.0 * static_cast<double>(stats.deadCount) / static_cast<double>(stats.fullCount);
  }

  stats.lengthMm = Summarize(std::move(lengths));
  stats.pointsPerLine = Summarize(std::move(pointCounts));
  return stats;
}

}  // namespace tracto
