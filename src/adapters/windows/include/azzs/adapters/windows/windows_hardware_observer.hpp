#pragma once

#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/hardware_overview.hpp"

namespace azzs::adapters::windows {

enum class WindowsHardwareQueryCode {
  succeeded,
  permission_denied,
  cancelled,
  timed_out,
  failed,
};

struct WindowsHardwareQueryResult final {
  WindowsHardwareQueryCode code{WindowsHardwareQueryCode::failed};
  // Each row contains values in the same order as the requested properties.
  std::vector<std::vector<std::string>> rows;
  std::string error;
};

// The production implementation executes read-only WMI queries. Tests may
// provide a deterministic implementation without touching a Windows host.
class WindowsHardwareQueryExecutor {
 public:
  virtual ~WindowsHardwareQueryExecutor() = default;

  [[nodiscard]] virtual WindowsHardwareQueryResult query(
      std::string_view class_name,
      std::span<std::string_view const> properties,
      std::stop_token cancellation) = 0;
};

class WindowsHardwareObserver final : public application::HardwareObserver {
 public:
  WindowsHardwareObserver();
  explicit WindowsHardwareObserver(
      std::unique_ptr<WindowsHardwareQueryExecutor> executor);

  [[nodiscard]] application::HardwareObservationResult observe(
      std::stop_token cancellation) override;
  [[nodiscard]] std::optional<application::HardwareObservation>
  current_model_observation(
      std::stop_token cancellation) override;

 private:
  std::unique_ptr<WindowsHardwareQueryExecutor> executor_;
};

}  // namespace azzs::adapters::windows
