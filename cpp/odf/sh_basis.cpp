// Real symmetric spherical-harmonic basis — Descoteaux07 convention.
//
// Bit-for-bit port of dmri-explorer's Slicer::SH::DescoteauxBasis
// (dmri-explorer/Engine/src/spherical_harmonic.cpp). The point of matching it
// exactly is reconstruction parity: an fODF .nii.gz authored for that viewer
// (or by DIPY's legacy `descoteaux07` basis) must reproduce here. Where the
// reference and a "cleaner" formula disagree, the reference wins.
//
// Pure CPU, C++17 stdlib only. No GL/Qt/zlib here (zlib is only the NIfTI
// loader's concern).

#include "sh_basis.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace tracto {
namespace odf {

namespace {

// factorial(n) = n! via tgamma(n+1), matching utils.hpp:120-127 exactly.
// WHY tgamma rather than an integer product: the reference uses it, and at the
// orders involved (l <= ~12, so arguments up to ~24!) the double result is
// identical to within the float rounding that the scaling cast applies anyway.
// Using the same primitive guarantees we never diverge by a last-bit difference.
double Factorial(int n) {
  if (n < 0) {
    throw std::runtime_error("Invalid value for factorial.");
  }
  return std::tgamma(static_cast<double>(n) + 1.0);
}

// std::assoc_legendre(l, m, x) for m >= 0, computed by the standard upward
// recurrence. WHY hand-rolled: this is a C++17 <cmath> special function present
// in libstdc++ (the reference's compiler) but ABSENT in Apple's libc++, and the
// whole module must build on macOS. We reproduce the SAME function the standard
// defines — crucially WITHOUT the Condon-Shortley (-1)^m phase, matching the ISO
// C++ definition that libstdc++'s std::assoc_legendre also uses. The reference's
// legendre() then reintroduces (-1)^m itself; see Legendre() below.
//
// Recurrence (no CS phase):
//   P_m^m(x) = (2m-1)!! * (1-x^2)^(m/2)        [seed]
//   P_{m+1}^m(x) = x * (2m+1) * P_m^m(x)       [one step up in l]
//   (l-m) P_l^m = x(2l-1) P_{l-1}^m - (l+m-1) P_{l-2}^m   [general]
double StdAssocLegendre(int l, int m, double x) {
  // Seed P_m^m. (2m-1)!! = 1*3*5*...*(2m-1); sintheta^m = (1-x^2)^(m/2).
  double pmm = 1.0;
  if (m > 0) {
    const double somx2 = std::sqrt((1.0 - x) * (1.0 + x));  // |sin theta|
    double fact = 1.0;
    for (int i = 1; i <= m; ++i) {
      pmm *= fact * somx2;
      fact += 2.0;
    }
  }
  if (l == m) {
    return pmm;
  }
  double pmmp1 = x * (2.0 * m + 1.0) * pmm;  // P_{m+1}^m
  if (l == m + 1) {
    return pmmp1;
  }
  double pll = 0.0;
  for (int ll = m + 2; ll <= l; ++ll) {
    pll = (x * (2.0 * ll - 1.0) * pmmp1 - (ll + m - 1.0) * pmm) / (ll - m);
    pmm = pmmp1;
    pmmp1 = pll;
  }
  return pll;
}

// Associated Legendre polynomial P_l^m(x), reproducing spherical_harmonic.cpp:14-22.
//   P_l^m(x) = (-1)^m * assoc_legendre(l, |m|, x)
// The reference also has an m<0 correction branch, but in the actual evaluation
// path legendre() is *only ever called with m = |m| >= 0* (computeSHFunc passes
// abs(m), spherical_harmonic.cpp:104), so that branch is dead. We mirror the
// live path: callers here always pass m >= 0. The explicit (-1)^m here is the
// Condon-Shortley phase the reference applies on top of the (CS-phase-free)
// std::assoc_legendre — net result is the standard P_l^m sign the Descoteaux
// normalization expects.
double Legendre(int l, int m, double x) {
  return std::pow(-1.0, m) * StdAssocLegendre(l, m, x);
}

// Normalization (scaling) constant for band (l, m), spherical_harmonic.cpp:49-67:
//   base    = sqrt( (2l+1)/(4*pi) * (l-m)!/(l+m)! )
//   scaling = base            for m == 0
//   scaling = sqrt(2) * base  for m != 0
// The sqrt(2) for m!=0 is the real-basis condensation factor: a pair of complex
// modes Y_l^{+|m|}, Y_l^{-|m|} collapses into one real cos/sin mode, and energy
// conservation pulls in the sqrt(2). Computed in double, stored as float, just
// like the reference (sqrtf on a double expression, then a float multiply).
float Scaling(int l, int m) {
  const float base = std::sqrt(static_cast<float>(
      (2.0 * l + 1.0) / 4.0 / M_PI * Factorial(l - m) / Factorial(l + m)));
  if (m != 0) {
    return base * std::sqrt(2.0f);
  }
  return base;
}

}  // namespace

std::size_t ShNumCoeffs(int lMax, ShBasisKind kind) {
  // Symmetric counts only even bands: (L+1)(L+2)/2. Full counts all: (L+1)^2.
  // (spherical_harmonic.cpp:40-46)
  if (kind == ShBasisKind::Full) {
    return static_cast<std::size_t>(lMax + 1) * static_cast<std::size_t>(lMax + 1);
  }
  return static_cast<std::size_t>(lMax + 1) * static_cast<std::size_t>(lMax + 2) / 2;
}

std::size_t ShIndex(int l, int m, ShBasisKind kind) {
  // Flattened coefficient index J(l, m), spherical_harmonic.cpp:31-37.
  //   Full:      l*(l+1) + m
  //   Symmetric: l*(l+1)/2 + m   (only even l contribute; the /2 packs out the
  //              skipped odd bands so indices stay contiguous)
  if (kind == ShBasisKind::Full) {
    return static_cast<std::size_t>(l * (l + 1) + m);
  }
  return static_cast<std::size_t>(l * (l + 1) / 2 + m);
}

ShOrder InferShOrder(std::size_t nCoeffs) {
  // Mirror getOrderFromNbCoeffs (spherical_harmonic.cpp:123-146): try the
  // symmetric inversion first, then the full one. The "is it an integer?" test
  // compares trunc(x) against x within one float epsilon, exactly as the
  // reference — guarding against, e.g., 5.9999998 that should round to 6.
  const float floatEpsilon = std::numeric_limits<float>::epsilon();

  // Symmetric: nCoeffs = (L+1)(L+2)/2  =>  L = (-3 + sqrt(1 + 8*nCoeffs)) / 2.
  const float symOrder = static_cast<float>(
      (-3.0 + std::sqrt(1.0 + 8.0 * static_cast<double>(nCoeffs))) / 2.0);
  if (std::trunc(symOrder) >= symOrder - floatEpsilon &&
      std::trunc(symOrder) <= symOrder + floatEpsilon) {
    ShOrder order;
    order.lMax = static_cast<int>(symOrder);
    order.kind = ShBasisKind::Symmetric;
    order.nCoeffs = nCoeffs;
    return order;
  }

  // Full: nCoeffs = (L+1)^2  =>  L = sqrt(nCoeffs) - 1.
  const float fullOrder = std::sqrt(static_cast<float>(nCoeffs)) - 1.0f;
  if (std::trunc(fullOrder) >= fullOrder - floatEpsilon &&
      std::trunc(fullOrder) <= fullOrder + floatEpsilon) {
    ShOrder order;
    order.lMax = static_cast<int>(fullOrder);
    order.kind = ShBasisKind::Full;
    order.nCoeffs = nCoeffs;
    return order;
  }

  throw std::runtime_error(
      "Invalid number of SH coefficients: " + std::to_string(nCoeffs));
}

float EvalRealSh(int l, int m, float theta, float phi, ShBasisKind kind) {
  // Validity check matching at() (spherical_harmonic.cpp:72-74): in a symmetric
  // basis odd bands do not exist.
  if (l < 0 || std::abs(m) > l || (kind == ShBasisKind::Symmetric && l % 2 != 0)) {
    throw std::runtime_error("Invalid (l, m) for SH basis.");
  }

  // computeSHFunc (spherical_harmonic.cpp:102-106) builds the complex SH at the
  // *magnitude* |m|, then at() takes real (m<=0) or imag (m>0). We collapse that
  // to the equivalent real arithmetic: with std::polar(r, m*phi),
  //   real = r*cos(m*phi), imag = r*sin(m*phi).
  const int am = std::abs(m);
  const float r = static_cast<float>(
      static_cast<double>(Scaling(l, am)) * Legendre(l, am, std::cos(theta)));

  // The reference forms std::polar(r, |m|*phi) and takes its real part (m<=0)
  // or imaginary part (m>0). That is exactly r*cos(|m|*phi) and r*sin(|m|*phi).
  // We compute those directly rather than via std::polar: libc++ requires the
  // magnitude passed to std::polar to be NON-NEGATIVE, and r is routinely
  // negative (odd-parity Legendre values), which makes libc++'s std::polar
  // return NaN. libstdc++ (the reference's compiler) tolerates a negative rho,
  // so the reference "works" only by luck of its toolchain. The closed-form
  // real/imag below is both portable (matters: macOS uses libc++) and clearer.
  const float angle = static_cast<float>(am) * phi;
  return m <= 0 ? r * std::cos(angle) : r * std::sin(angle);
}

void DirectionToSpherical(Vec3 dir, float& theta, float& phi) {
  // Normalize so an arbitrary-length direction still maps onto the unit sphere
  // (icosphere vertices are unit, but callers may pass raw vectors). theta is
  // the colatitude from +Z, phi the azimuth in the XY plane — the same
  // convention the icosphere uses (sphere.cpp:106-152), so basis and directions
  // agree.
  const Vec3 u = Normalize(dir);
  // Guard acos against tiny float overshoot of [-1, 1] from the normalize.
  const float z = u.z < -1.0f ? -1.0f : (u.z > 1.0f ? 1.0f : u.z);
  theta = std::acos(z);
  phi = std::atan2(u.y, u.x);
}

ShBasisMatrix BuildShBasisMatrix(const std::vector<Vec3>& directions,
                                 const ShOrder& order) {
  ShBasisMatrix matrix;
  matrix.order = order;
  matrix.nDir = directions.size();
  matrix.nCoeffs = order.nCoeffs;
  matrix.values.resize(matrix.nDir * matrix.nCoeffs);

  // Iterate l outer (step 2 for Symmetric, 1 for Full), then m = -l..l, so the
  // c-th column produced lines up with coefficient c of the volume — identical
  // to the reference's at(theta, phi) push_back order (spherical_harmonic.cpp:92-94).
  // We assert that derived column counter against nCoeffs as a cheap invariant.
  const int step = order.kind == ShBasisKind::Full ? 1 : 2;

  for (std::size_t d = 0; d < matrix.nDir; ++d) {
    float theta = 0.0f;
    float phi = 0.0f;
    DirectionToSpherical(directions[d], theta, phi);

    float* rowPtr = matrix.values.data() + d * matrix.nCoeffs;
    std::size_t c = 0;
    for (int l = 0; l <= order.lMax; l += step) {
      for (int m = -l; m <= l; ++m) {
        // Column index from J(l,m) is equivalent to the running counter c for
        // this canonical ordering; writing via c keeps it branch-free and lets
        // the loop double as the validation below.
        rowPtr[c++] = EvalRealSh(l, m, theta, phi, order.kind);
      }
    }
    if (c != matrix.nCoeffs) {
      throw std::runtime_error(
          "SH basis column count mismatch: emitted " + std::to_string(c) +
          " for nCoeffs " + std::to_string(matrix.nCoeffs));
    }
  }
  return matrix;
}

ShBasisMatrix BuildShBasisMatrix(const std::vector<Vec3>& directions,
                                 std::size_t nCoeffs) {
  return BuildShBasisMatrix(directions, InferShOrder(nCoeffs));
}

}  // namespace odf
}  // namespace tracto
