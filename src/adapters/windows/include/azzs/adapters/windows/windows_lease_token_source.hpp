#pragma once

#include <string>

#include "azzs/application/operation_occupancy.hpp"

namespace azzs::adapters::windows {

class WindowsLeaseTokenSource final : public application::LeaseTokenSource {
 public:
  [[nodiscard]] std::string next_token() override;
};

}  // namespace azzs::adapters::windows
