#pragma once

// Real symmetric spherical-harmonic (SH) basis — Descoteaux07 convention.
//
// This mirrors dmri-explorer's Slicer::SH::DescoteauxBasis
// (dmri-explorer/Engine/src/spherical_harmonic.cpp) bit-for-bit so a fODF
// .nii.gz authored for that viewer (or by DIPY's legacy `descoteaux07` basis)
// reconstructs identically here. It is a pure-CPU, header-declared interface;
// the .cpp implements it. No GL, no Qt.
//
// CONVENTION (must be matched exactly by the implementation):
//
//  * Real basis. Output is real-valued, built from complex SH Y_l^m:
//      for m <= 0:  value = Real(Y_l^|m|) = r * cos(m*phi)
//      for m  > 0:  value = Imag(Y_l^|m|) = r * sin(m*phi)
//    (spherical_harmonic.cpp:70-85)
//
//  * r = scaling(l, |m|) * P_l^|m|(cos theta), where the associated Legendre
//    polynomial is
//      P_l^m(x) = (-1)^m * std::assoc_legendre(l, |m|, x),
//    with the m<0 correction P_l^m(x) *= (-1)^m * (l-m)!/(l+m)!
//    (spherical_harmonic.cpp:14-22, 102-106). std::polar(r, m*phi) is the
//    complex form whose real/imag part is taken above.
//
//  * Normalization (scaling) constant (spherical_harmonic.cpp:49-67):
//      base = sqrt( (2l+1)/(4*pi) * (l-m)!/(l+m)! )
//      scaling = base                 for m == 0
//      scaling = sqrt(2) * base       for m != 0
//
//  * Symmetric basis: only even orders l = 0, 2, 4, ... are present.
//    Flattened coefficient index  J(l, m) = l*(l+1)/2 + m,  iterating l outer
//    (step 2), then m from -l to +l (spherical_harmonic.cpp:31-37, 92-94).
//    Coefficient count nCoeffs = (l_max+1)*(l_max+2)/2.
//
//  * Full basis (all orders l = 0, 1, 2, ...): J(l, m) = l*(l+1) + m,
//    nCoeffs = (l_max+1)^2. Supported for completeness; fODF data is symmetric.
//
//  * Order from coeff count (spherical_harmonic.cpp:123-146): try symmetric
//    l_max = (-3 + sqrt(1 + 8*nCoeffs)) / 2 first; if it is an integer the basis
//    is symmetric. Else try full l_max = sqrt(nCoeffs) - 1; integer => full.
//    Neither integer => not a valid SH coefficient count.

#include "odf_types.hpp"  // tracto::odf::Vec3

#include <cstddef>
#include <vector>

namespace tracto {
namespace odf {

// Which family of SH coefficients a volume carries. fODF data is Symmetric.
enum class ShBasisKind {
  Symmetric,  // even orders only:  J(l,m) = l*(l+1)/2 + m,  nCoeffs=(L+1)(L+2)/2
  Full,       // all orders:        J(l,m) = l*(l+1) + m,    nCoeffs=(L+1)^2
};

// Result of inferring the basis layout from a raw coefficient count.
struct ShOrder {
  int lMax = 0;                            // maximum band l present
  ShBasisKind kind = ShBasisKind::Symmetric;
  std::size_t nCoeffs = 0;                 // echo of the input count (validated)
};

// nCoeffs for a given lMax under a basis kind:
//   Symmetric: (lMax+1)*(lMax+2)/2     Full: (lMax+1)^2
// (spherical_harmonic.cpp:40-46)
std::size_t ShNumCoeffs(int lMax, ShBasisKind kind);

// Flattened coefficient index J(l, m). Symmetric requires l even.
//   Symmetric: l*(l+1)/2 + m           Full: l*(l+1) + m
// (spherical_harmonic.cpp:31-37)
std::size_t ShIndex(int l, int m, ShBasisKind kind);

// Infer (lMax, kind) from a coefficient count. Tries the symmetric formula
// first, then the full one (spherical_harmonic.cpp:123-146).
// Throws std::runtime_error if nCoeffs is not a valid SH count under either.
ShOrder InferShOrder(std::size_t nCoeffs);

// Evaluate one real SH basis function at a direction given in spherical coords
// (theta = polar/colatitude in [0, pi] from +Z; phi = azimuth in [0, 2pi)).
// Implements the m<=0 Real / m>0 Imag rule with the scaling + Legendre terms
// above (spherical_harmonic.cpp:70-85, 102-106). `l` must be valid for `kind`
// (even when Symmetric); `m` in [-l, l].
float EvalRealSh(int l, int m, float theta, float phi, ShBasisKind kind);

// Convert a unit (or arbitrary, will be normalized) Cartesian direction to the
// (theta, phi) spherical convention used by EvalRealSh:
//   theta = acos(z), phi = atan2(y, x). Matches the icosphere's own conversion
//   (sphere.cpp:106-152) so directions and basis agree.
void DirectionToSpherical(Vec3 dir, float& theta, float& phi);

// A precomputed basis matrix: row-major B[d * nCoeffs + c] = value of the c-th
// SH basis function at direction d. Holding it explicitly lets a glyph be
// reconstructed for ALL its sample directions with one dense (nDir x nCoeffs)
// * (nCoeffs) matvec per voxel — the CPU analogue of dmri-explorer's per-vertex
// shader evaluation. `nDir` rows, `order.nCoeffs` columns.
struct ShBasisMatrix {
  ShOrder order;
  std::size_t nDir = 0;       // number of sample directions (icosphere vertices)
  std::size_t nCoeffs = 0;    // == order.nCoeffs (cached for stride math)
  std::vector<float> values;  // size nDir * nCoeffs, row-major (dir outer)

  // Pointer to the start of row d (its nCoeffs basis values). Caller guarantees
  // d < nDir.
  const float* row(std::size_t d) const { return values.data() + d * nCoeffs; }
};

// Build the basis matrix for a set of sample directions (typically the unit
// icosphere vertices). `order` fixes lMax/kind/nCoeffs so the matrix matches a
// given volume's coefficient layout. The c-th column iterates (l, m) in the
// exact J ordering (l outer with the kind's step, m = -l..l), so column c lines
// up with coefficient c of the volume. Directions need not be pre-normalized.
ShBasisMatrix BuildShBasisMatrix(const std::vector<Vec3>& directions,
                                 const ShOrder& order);

// Convenience: infer the order from a coefficient count, then build the matrix.
// Equivalent to BuildShBasisMatrix(directions, InferShOrder(nCoeffs)).
ShBasisMatrix BuildShBasisMatrix(const std::vector<Vec3>& directions,
                                 std::size_t nCoeffs);

}  // namespace odf
}  // namespace tracto
