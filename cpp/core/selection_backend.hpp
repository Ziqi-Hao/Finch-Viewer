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
  virtual std::vector<uint8_t> SelectInBox(const TractogramStore& tractogram,
                                           const Bounds& bounds) const = 0;
};

std::unique_ptr<SelectionBackend> CreateCpuSelectionBackend();

}  // namespace tracto
