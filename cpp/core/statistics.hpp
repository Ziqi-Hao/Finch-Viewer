#pragma once

#include "tractogram_store.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tracto {

struct NumericSummary {
  bool valid = false;
  double mean = 0.0;
  double median = 0.0;
  double stddev = 0.0;
  double min = 0.0;
  double max = 0.0;
};

struct BasicStats {
  std::size_t fullCount = 0;
  std::size_t aliveCount = 0;
  std::size_t deadCount = 0;
  double deletedPercent = 0.0;
  NumericSummary lengthMm;
  NumericSummary pointsPerLine;
};

BasicStats ComputeBasicStats(const TractogramStore& store,
                             const std::vector<uint8_t>& aliveFull);

}  // namespace tracto
