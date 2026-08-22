#include "azzs/application/hardware_overview.hpp"

#include <array>
#include <mutex>
#include <string_view>
#include <utility>

namespace azzs::application {

bool HardwareObservation::usable() const noexcept {
  return has_confirmed_physical_hardware();
}

bool HardwareObservation::has_confirmed_physical_hardware() const noexcept {
  for (auto const& device : devices) {
    if (device.confirmed_physical()) {
      return true;
    }
  }
  return false;
}

std::string HardwareObservation::model_fingerprint() const {
  // Keep the cache key limited to non-unique model text. Never add serial
  // numbers, MAC/IP addresses, computer names, or other device identifiers.
  std::array<std::string_view, 6> const fields{
      cpu,
      gpu,
      motherboard,
      network_adapter,
      oem_model,
      to_string(oem_vendor),
  };
  std::string result;
  for (auto const field : fields) {
    result.append(std::to_string(field.size()));
    result.push_back(':');
    result.append(field);
    result.push_back('|');
  }
  for (auto const& device : devices) {
    auto const append_field = [&result](std::string_view field) {
      result.append(std::to_string(field.size()));
      result.push_back(':');
      result.append(field);
      result.push_back('|');
    };
    append_field(to_string(device.kind));
    append_field(device.name);
    append_field(to_string(device.vendor));
    append_field(to_string(device.status));
    append_field(to_string(device.physicality));
  }
  return result;
}

char const* to_string(HardwareDeviceKind value) noexcept {
  switch (value) {
    case HardwareDeviceKind::cpu: return "cpu";
    case HardwareDeviceKind::gpu: return "gpu";
    case HardwareDeviceKind::motherboard: return "motherboard";
    case HardwareDeviceKind::network_adapter: return "network-adapter";
  }
  return "unknown";
}

char const* to_string(HardwareDevicePhysicality value) noexcept {
  switch (value) {
    case HardwareDevicePhysicality::confirmed_physical:
      return "confirmed-physical";
    case HardwareDevicePhysicality::virtual_device: return "virtual";
    case HardwareDevicePhysicality::software: return "software";
    case HardwareDevicePhysicality::vpn: return "vpn";
    case HardwareDevicePhysicality::loopback: return "loopback";
    case HardwareDevicePhysicality::unknown: return "unknown";
    case HardwareDevicePhysicality::conflicting: return "conflicting";
  }
  return "unknown";
}

char const* to_string(HardwareObservationSource value) noexcept {
  switch (value) {
    case HardwareObservationSource::wmi: return "wmi";
    case HardwareObservationSource::setup_api: return "setup-api";
    case HardwareObservationSource::unknown: return "unknown";
  }
  return "unknown";
}

char const* to_string(HardwareObservationConfidence value) noexcept {
  switch (value) {
    case HardwareObservationConfidence::confirmed: return "confirmed";
    case HardwareObservationConfidence::inferred: return "inferred";
    case HardwareObservationConfidence::unknown: return "unknown";
  }
  return "unknown";
}

char const* to_string(HardwareDeviceStatus value) noexcept {
  switch (value) {
    case HardwareDeviceStatus::enabled: return "enabled";
    case HardwareDeviceStatus::disabled: return "disabled";
    case HardwareDeviceStatus::no_driver: return "no-driver";
    case HardwareDeviceStatus::error: return "error";
    case HardwareDeviceStatus::unknown: return "unknown";
  }
  return "unknown";
}

char const* to_string(HardwareVendor value) noexcept {
  switch (value) {
    case HardwareVendor::unknown: return "unknown";
    case HardwareVendor::amd: return "amd";
    case HardwareVendor::intel: return "intel";
    case HardwareVendor::nvidia: return "nvidia";
    case HardwareVendor::dell: return "dell";
    case HardwareVendor::hp: return "hp";
    case HardwareVendor::lenovo: return "lenovo";
    case HardwareVendor::asus: return "asus";
  }
  return "unknown";
}

HardwareOverviewService::HardwareOverviewService(HardwareObserver& observer,
                                                 Clock const& clock) noexcept
    : observer_(observer), clock_(clock) {}

HardwareOverviewSnapshot HardwareOverviewService::snapshot() const {
  std::scoped_lock lock{mutex_};
  return snapshot_;
}

bool HardwareOverviewService::cache_valid(WallClockTime now) const noexcept {
  if (!cached_observation_.has_value() || !cached_at_.has_value()) {
    return false;
  }
  return now >= *cached_at_ && now - *cached_at_ < kSessionCacheLifetime;
}

bool HardwareOverviewService::should_observe(HardwareOverviewTrigger trigger,
                                             WallClockTime now) const noexcept {
  if (trigger == HardwareOverviewTrigger::user_refresh ||
      trigger == HardwareOverviewTrigger::hardware_changed ||
      trigger == HardwareOverviewTrigger::cache_expired) {
    return true;
  }
  return !cache_valid(now);
}

bool HardwareOverviewService::fingerprint_changed(
    std::stop_token cancellation) {
  if (cancellation.stop_requested()) {
    return false;
  }
  std::string cached_fingerprint;
  {
    std::scoped_lock lock{mutex_};
    cached_fingerprint = cached_fingerprint_;
  }
  if (cached_fingerprint.empty()) {
    return false;
  }
  try {
    auto const current = observer_.current_model_observation(cancellation);
    return current.has_value() && current->usable() &&
           current->model_fingerprint() != cached_fingerprint;
  } catch (...) {
    return false;
  }
}

HardwareOverviewSnapshot HardwareOverviewService::unrecognized(
    HardwareObservationCode code, std::string error, bool stale) const {
  HardwareOverviewSnapshot result{
      .state = HardwareOverviewState::unrecognized,
      .observation = std::nullopt,
      .observed_at = std::nullopt,
      .error = std::move(error),
      .cache_hit = false,
      .stale = stale,
  };
  if (result.error.empty()) {
    switch (code) {
      case HardwareObservationCode::permission_denied:
        result.error = "hardware observation permission denied";
        break;
      case HardwareObservationCode::cancelled:
        result.error = "hardware observation cancelled";
        break;
      case HardwareObservationCode::failed:
        result.error = "hardware observation failed";
        break;
      case HardwareObservationCode::timed_out:
        result.error = "hardware observation timed out";
        break;
      case HardwareObservationCode::partial:
      case HardwareObservationCode::succeeded:
        result.error = "hardware observation produced no usable facts";
        break;
    }
  }
  return result;
}

HardwareOverviewSnapshot HardwareOverviewService::observe(
    HardwareOverviewTrigger trigger, std::stop_token cancellation) {
  std::scoped_lock operation_lock{operation_mutex_};

  if (cancellation.stop_requested()) {
    std::scoped_lock lock{mutex_};
    cached_observation_.reset();
    cached_at_.reset();
    cached_fingerprint_.clear();
    snapshot_ = unrecognized(HardwareObservationCode::cancelled,
                             "hardware observation cancelled", false);
    return snapshot_;
  }

  auto const now = clock_.now();
  bool use_cache = false;
  {
    std::scoped_lock lock{mutex_};
    use_cache = !should_observe(trigger, now) &&
                cached_observation_.has_value() && cached_at_.has_value();
  }

  // A cheap fingerprint probe lets a page revisit invalidate the session
  // cache after a device change without requiring a full observation first.
  if (use_cache && fingerprint_changed(cancellation)) {
    use_cache = false;
  }
  if (cancellation.stop_requested()) {
    std::scoped_lock lock{mutex_};
    cached_observation_.reset();
    cached_at_.reset();
    cached_fingerprint_.clear();
    snapshot_ = unrecognized(HardwareObservationCode::cancelled,
                             "hardware observation cancelled", false);
    return snapshot_;
  }
  if (use_cache) {
    std::scoped_lock lock{mutex_};
    snapshot_ = HardwareOverviewSnapshot{
        .state = HardwareOverviewState::ready,
        .observation = cached_observation_,
        .observed_at = cached_at_,
        .error = {},
        .cache_hit = true,
        .stale = false,
    };
    return snapshot_;
  }

  {
    std::scoped_lock lock{mutex_};
    snapshot_.state = HardwareOverviewState::observing;
    snapshot_.cache_hit = false;
    snapshot_.stale = cached_observation_.has_value();
  }

  HardwareObservationResult result;
  try {
    result = observer_.observe(cancellation);
  } catch (...) {
    result = {.code = HardwareObservationCode::failed,
              .observation = std::nullopt,
              .error = "hardware observation failed unexpectedly"};
  }

  if (cancellation.stop_requested()) {
    result = {.code = HardwareObservationCode::cancelled,
              .observation = std::nullopt,
              .error = "hardware observation cancelled"};
  }

  std::scoped_lock lock{mutex_};
  if (result.succeeded() && result.observation->usable()) {
    auto observation = std::move(*result.observation);
    cached_fingerprint_ = observation.model_fingerprint();
    cached_observation_ = std::move(observation);
    cached_at_ = clock_.now();
    snapshot_ = HardwareOverviewSnapshot{
        .state = HardwareOverviewState::ready,
        .observation = cached_observation_,
        .observed_at = cached_at_,
        .error = {},
        .cache_hit = false,
        .stale = false,
    };
    return snapshot_;
  }

  cached_observation_.reset();
  cached_at_.reset();
  cached_fingerprint_.clear();
  snapshot_ = unrecognized(result.code, std::move(result.error), false);
  return snapshot_;
}

}  // namespace azzs::application
