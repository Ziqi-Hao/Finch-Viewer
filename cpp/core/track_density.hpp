#pragma once

// Track-density image (TDI): an isotropic RAS-mm grid over the streamlines'
// extent where each voxel counts how many (alive) streamlines pass through it.
// Returned as a `Volume` so it renders through the existing slice/texture path.
// Used to show a density "map" of a bundle in the 2-D slice views instead of the
// full streamline geometry (much cheaper and clearer for slice navigation).

#include "nifti_io.hpp"          // Volume
#include "tractogram_store.hpp"  // TractogramStore

#include <cstdint>
#include <vector>

namespace tracto {

// `voxelSizeMm` sets the target resolution; the grid is capped to a safe maximum
// dimension (the voxel size is enlarged if the bundle is very large). `alive`
// (per streamline) selects which streamlines contribute; pass an all-ones mask
// for the full set. `valueMax` is set to a robust (high-percentile) count so a
// few very dense voxels don't wash the map out. Returns an empty Volume if the
// store has no points.
Volume BuildTrackDensity(const TractogramStore& store,
                         const std::vector<uint8_t>& alive,
                         float voxelSizeMm);

}  // namespace tracto
