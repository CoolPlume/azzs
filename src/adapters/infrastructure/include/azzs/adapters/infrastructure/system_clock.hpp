#pragma once

#include <chrono>

#include "azzs/application/clock.hpp"

namespace azzs::adapters::infrastructure {

class SystemClock final : public application::Clock {
 public:
  [[nodiscard]] application::WallClockTime now() const noexcept override {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
  }
};

}  // namespace azzs::adapters::infrastructure
