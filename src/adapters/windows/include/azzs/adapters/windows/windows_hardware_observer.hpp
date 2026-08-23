#pragma once

#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <cstdint>
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

// CPU-set topology is optional: older Windows builds or restricted sessions
// may not expose efficiency classes. The observer must then keep the ordinary
// WMI core/thread counts and label the P/E split as unavailable.
struct WindowsCpuTopology final {
  std::uint32_t performance_cores{0};
  std::uint32_t efficiency_cores{0};
  std::uint32_t logical_processors{0};
  bool split_known{false};
};

// DXGI reports dedicated and shared memory without the 32-bit truncation that
// can affect Win32_VideoController.AdapterRAM. The observer uses a value only
// when the WMI and DXGI model names match exactly after benign punctuation
// normalization.
struct WindowsGpuMemory final {
  std::string model_name;
  std::uint64_t dedicated_video_memory{0};
  std::uint64_t shared_system_memory{0};
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

  [[nodiscard]] virtual std::optional<WindowsCpuTopology> cpu_topology(
      std::stop_token) {
    return std::nullopt;
  }

  [[nodiscard]] virtual std::vector<WindowsGpuMemory> gpu_memory(
      std::stop_token) {
    return {};
  }
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
