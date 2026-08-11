#pragma once

#include <optional>

#include "azzs/domain/system_version.hpp"

namespace azzs::application {

class PlatformInfo {
 public:
  virtual ~PlatformInfo() = default;

  [[nodiscard]] virtual std::optional<domain::SystemVersion>
  windows_version() const = 0;
};

}  // namespace azzs::application
