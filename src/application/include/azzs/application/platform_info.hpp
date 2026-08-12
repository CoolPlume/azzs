#pragma once

#include <optional>

#include "azzs/domain/system_architecture.hpp"
#include "azzs/domain/system_version.hpp"

namespace azzs::application {

class PlatformInfo {
 public:
  virtual ~PlatformInfo() = default;

  [[nodiscard]] virtual std::optional<domain::SystemVersion>
  windows_version() const = 0;

  // Each call observes the current Windows architecture. Unknown represents a
  // failed observation and must not be treated as a compatible architecture.
  [[nodiscard]] virtual domain::SystemArchitecture windows_architecture()
      const = 0;
};

}  // namespace azzs::application
