#pragma once

#include "bounds.hpp"
#include "tractogram_store.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tracto {

class SelectionBackend {
 public:
  virtual ~SelectionBackend() = default;

  virtual std::string Name() const = 0;

  // Prepare any spatial index for this tractogram. Call once after a load (and
  // again on reload). Default: no-op (the linear scan needs no index).
  // `bounds`, if given, is the tractogram's RAS AABB (identical to what the
  // index would compute itself); pass the already-cached RasBounds to skip a
  // redundant full x/y/z minmax sweep. Ignored by index-free backends.
  virtual void Build(const TractogramStore& tractogram,
                     const Bounds* bounds = nullptr) {
    (void)tractogram;
    (void)bounds;
  }

  // True if a spatial index is active after Build(). An index-free backend, or a
  // grid whose cell count tripped the guard cap (degrading queries to a full
  // linear scan), returns false — the caller may warn the user.
  virtual bool IndexBuilt() const { return false; }

  // Per-streamline flags: 1 if ANY point of the streamline lies in the box.
  virtual std::vector<uint8_t> SelectInBox(const TractogramStore& tractogram,
                                           const Bounds& bounds) const = 0;
};

// Linear: one vectorized pass over all points. No index, O(all points).
std::unique_ptr<SelectionBackend> CreateCpuSelectionBackend();

// Grid: uniform-voxel CSR index; visits only buckets overlapping the box, so a
// localized query is O(box) instead of O(all points) — fast enough to drive the
// live in-box highlight on the full set. Call Build() after each load.
std::unique_ptr<SelectionBackend> CreateGridSelectionBackend();

}  // namespace tracto
