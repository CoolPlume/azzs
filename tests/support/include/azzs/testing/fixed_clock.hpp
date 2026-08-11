#pragma once

#include <chrono>

#include "azzs/application/clock.hpp"

namespace azzs::testing {

class FixedClock final : public application::Clock {
 public:
  explicit FixedClock(application::WallClockTime current) noexcept
      : current_(current) {}

  [[nodiscard]] application::WallClockTime now() const noexcept override {
    return current_;
  }

  void set(application::WallClockTime current) noexcept { current_ = current; }

  void advance(std::chrono::milliseconds elapsed) noexcept {
    current_ += elapsed;
  }

 private:
  application::WallClockTime current_;
};

}  // namespace azzs::testing
