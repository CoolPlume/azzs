#pragma once

#include <compare>

namespace azzs::domain {

// The architecture Windows is currently running for the purpose of selecting
// software packages. Unknown is a real observation result, not a fallback.
enum class SystemArchitecture {
  x64,
  arm64,
  unknown,
};

[[nodiscard]] constexpr auto operator<=>(SystemArchitecture left,
                                         SystemArchitecture right) noexcept {
  return static_cast<int>(left) <=> static_cast<int>(right);
}

}  // namespace azzs::domain
