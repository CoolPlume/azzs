#pragma once

#include <chrono>

namespace azzs::application {

using WallClockTime = std::chrono::sys_time<std::chrono::milliseconds>;

class Clock {
 public:
  virtual ~Clock() = default;

  [[nodiscard]] virtual WallClockTime now() const noexcept = 0;
};

}  // namespace azzs::application
