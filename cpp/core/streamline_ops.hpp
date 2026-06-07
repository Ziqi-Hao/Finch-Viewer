#pragma once

#include "bounds.hpp"
#include "trk_io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tracto {

bool StreamlineInBounds(const Streamline& sl, const Bounds& b);
std::array<unsigned char, 3> DirectionRgbFromPoints(const float* points,
                                                    std::size_t pointCount,
                                                    std::size_t pointIndex);
std::array<unsigned char, 3> DirectionRgb(const Streamline& sl, int32_t i);
std::vector<int> MakeDisplayIndices(int fullCount, int displayN, uint64_t seed);
std::vector<int> MakeDisplayIndicesFromAlive(const std::vector<uint8_t>& alive,
                                             int displayN,
                                             uint64_t seed);

}  // namespace tracto
