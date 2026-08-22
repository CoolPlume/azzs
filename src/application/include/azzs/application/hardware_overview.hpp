#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "azzs/application/clock.hpp"

namespace azzs::application {

// Hardware records are deliberately typed at the portable core boundary.
// The Windows adapter owns the classification decision; consumers must not
// infer physicality from display names.
enum class HardwareDeviceKind {
  cpu,
  gpu,
  motherboard,
  network_adapter,
};

enum class HardwareDevicePhysicality {
  confirmed_physical,
  virtual_device,
  software,
  vpn,
  loopback,
  unknown,
  conflicting,
};

enum class HardwareObservationSource {
  wmi,
  setup_api,
  unknown,
};

enum class HardwareObservationConfidence {
  confirmed,
  inferred,
  unknown,
};

enum class HardwareDeviceStatus {
  enabled,
  disabled,
  no_driver,
  error,
  unknown,
};

enum class HardwareVendor {
  unknown,
  amd,
  intel,
  nvidia,
  dell,
  hp,
  lenovo,
  asus,
};

struct HardwareDeviceRecord final {
  HardwareDeviceKind kind{HardwareDeviceKind::cpu};
  std::string name;
  HardwareDevicePhysicality physicality{HardwareDevicePhysicality::unknown};
  HardwareObservationSource source{HardwareObservationSource::unknown};
  HardwareObservationConfidence confidence{
      HardwareObservationConfidence::unknown};
  HardwareDeviceStatus status{HardwareDeviceStatus::unknown};
  HardwareVendor vendor{HardwareVendor::unknown};
  bool physically_present{false};
  std::string filter_reason;

  [[nodiscard]] bool confirmed_physical() const noexcept {
    return physically_present &&
           physicality == HardwareDevicePhysicality::confirmed_physical &&
           confidence == HardwareObservationConfidence::confirmed &&
           source != HardwareObservationSource::unknown && !name.empty();
  }
};

// The adapter returns facts only. It never starts a driver, downloads a
// package, benchmarks hardware, or changes system state.
struct HardwareObservation final {
  std::string cpu;
  std::string gpu;
  std::string motherboard;
  std::string network_adapter;
  std::string oem_model;
  HardwareVendor oem_vendor{HardwareVendor::unknown};
  std::vector<HardwareDeviceRecord> devices;

  [[nodiscard]] bool usable() const noexcept;
  [[nodiscard]] bool has_confirmed_physical_hardware() const noexcept;
  // Produces a cache-only key from non-unique model text. It intentionally
  // excludes serial numbers, MAC/IP addresses, computer names, and all other
  // device identifiers.
  [[nodiscard]] std::string model_fingerprint() const;
};

enum class HardwareObservationCode {
  succeeded,
  partial,
  failed,
  permission_denied,
  cancelled,
  timed_out,
};

struct HardwareObservationResult final {
  HardwareObservationCode code{HardwareObservationCode::failed};
  std::optional<HardwareObservation> observation;
  std::string error;

  [[nodiscard]] bool succeeded() const noexcept {
    return (code == HardwareObservationCode::succeeded ||
            code == HardwareObservationCode::partial) &&
           observation.has_value();
  }
};

class HardwareObserver {
 public:
  virtual ~HardwareObserver() = default;

  [[nodiscard]] virtual HardwareObservationResult observe(
      std::stop_token cancellation) = 0;

  // Optional model-only probe used when a valid session cache is revisited.
  // It must not return serial numbers, MAC/IP addresses, computer names, or
  // any other device identifier. A missing value keeps the cache authoritative
  // until expiry or an explicit refresh/change trigger.
  [[nodiscard]] virtual std::optional<HardwareObservation>
  current_model_observation(
      std::stop_token) {
    return std::nullopt;
  }
};

enum class HardwareOverviewState {
  not_observed,
  observing,
  ready,
  unrecognized,
};

struct HardwareOverviewSnapshot final {
  HardwareOverviewState state{HardwareOverviewState::not_observed};
  std::optional<HardwareObservation> observation;
  std::optional<WallClockTime> observed_at;
  std::string error;
  bool cache_hit{false};
  bool stale{false};
};

enum class HardwareOverviewTrigger {
  page_entered,
  user_refresh,
  hardware_changed,
  cache_expired,
};

[[nodiscard]] char const* to_string(HardwareDeviceKind value) noexcept;
[[nodiscard]] char const* to_string(HardwareDevicePhysicality value) noexcept;
[[nodiscard]] char const* to_string(HardwareObservationSource value) noexcept;
[[nodiscard]] char const* to_string(HardwareObservationConfidence value) noexcept;
[[nodiscard]] char const* to_string(HardwareDeviceStatus value) noexcept;
[[nodiscard]] char const* to_string(HardwareVendor value) noexcept;

// Owns the session-only ten-minute cache and the user-visible hardware state.
// Calls are synchronous and must be made by the application/use-case layer;
// the UI only consumes the returned immutable snapshot.
class HardwareOverviewService final {
 public:
  static constexpr std::chrono::minutes kSessionCacheLifetime{10};

  HardwareOverviewService(HardwareObserver& observer,
                          Clock const& clock) noexcept;

  [[nodiscard]] HardwareOverviewSnapshot snapshot() const;
  [[nodiscard]] HardwareOverviewSnapshot observe(
      HardwareOverviewTrigger trigger,
      std::stop_token cancellation = {});
  [[nodiscard]] HardwareOverviewSnapshot refresh(
      std::stop_token cancellation = {}) {
    return observe(HardwareOverviewTrigger::user_refresh, cancellation);
  }

 private:
  [[nodiscard]] bool cache_valid(WallClockTime now) const noexcept;
  [[nodiscard]] bool should_observe(HardwareOverviewTrigger trigger,
                                    WallClockTime now) const noexcept;
  [[nodiscard]] bool fingerprint_changed(std::stop_token cancellation);
  [[nodiscard]] HardwareOverviewSnapshot unrecognized(
      HardwareObservationCode code, std::string error,
      bool stale) const;

  HardwareObserver& observer_;
  Clock const& clock_;
  mutable std::mutex mutex_;
  mutable std::mutex operation_mutex_;
  HardwareOverviewSnapshot snapshot_;
  std::optional<HardwareObservation> cached_observation_;
  std::optional<WallClockTime> cached_at_;
  std::string cached_fingerprint_;
};

}  // namespace azzs::application
