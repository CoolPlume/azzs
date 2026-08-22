#pragma once

#include <compare>
#include <cstdint>

namespace azzs::domain {

struct SystemVersion final {
  std::uint32_t major{};
  std::uint32_t minor{};
  std::uint32_t build{};

  auto operator<=>(SystemVersion const&) const = default;
};

}  // namespace azzs::domain
