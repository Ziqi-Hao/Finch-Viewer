#pragma once

// Embedded fixed spheres for DISCRETE-SPHERE (SF) ODFs — per-voxel amplitudes sampled
// on a known sphere (e.g. RUMBA's output), as opposed to SH coefficients.
//
// WHY THIS EXISTS: a sphere-sampled ODF NIfTI stores only the amplitudes (N per voxel);
// the directions each amplitude belongs to are NOT in the file — they are an external
// convention (the tool's sphere). Without them the data is unrenderable. dipy's
// `symmetric362` is the standard 362-direction sphere these files use, so we embed it
// (vertices + triangle faces, dumped from dipy) and render by deforming that mesh:
// radius[i] = amplitude[i]. Returned as an Icosphere (vertices + index list) so the
// glyph builder treats it exactly like the SH icosphere — only the radius source differs.

#include "icosphere.hpp"  // Icosphere (vertices + indices)

namespace tracto {
namespace odf {

// dipy get_sphere('symmetric362'): 362 unit vertices + 720 faces. Built once, shared.
const Icosphere& Symmetric362();

// The embedded sphere a discrete-sphere ODF with `nDirections` amplitudes is defined on,
// or nullptr if we don't have it (then the file can't be glyph-rendered). Only 362
// (symmetric362) is embedded today; add more counts here as needed.
const Icosphere* DiscreteSphereForCount(int nDirections);

}  // namespace odf
}  // namespace tracto
