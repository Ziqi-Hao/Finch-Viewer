#include "utils.hpp"

namespace tracto {

std::string FormatCount(std::size_t value) {
  std::string s = std::to_string(value);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
    s.insert(static_cast<std::size_t>(i), ",");
  }
  return s;
}

}  // namespace tracto
