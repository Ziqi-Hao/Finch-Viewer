#pragma once

#include "bounds.hpp"
#include "trk_io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tracto {

// Direction RGB for a polyline point (|tangent| components), from a packed
// [x,y,z]* buffer. The display builders colour their decimated lines with this.
std::array<unsigned char, 3> DirectionRgbFromPoints(const float* points,
                                                    std::size_t pointCount,
                                                    std::size_t pointIndex);
std::vector<int> MakeDisplayIndices(int fullCount, int displayN, uint64_t seed);
std::vector<int> MakeDisplayIndicesFromAlive(const std::vector<uint8_t>& alive,
                                             int displayN,
                                             uint64_t seed);

}  // namespace tracto
