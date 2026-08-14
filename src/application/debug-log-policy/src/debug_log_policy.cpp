#include "azzs/application/debug_log_policy/debug_log_policy.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace azzs::application {
namespace {

template <typename Value>
void normalize(std::vector<Value>& values) {
  std::sort(values.begin(), values.end(), [](Value left, Value right) {
    return static_cast<int>(left) < static_cast<int>(right);
  });
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

[[nodiscard]] std::string not_obtained_reason(std::string reason) {
  constexpr std::string_view kPrefix{"NOT_OBTAINED:"};
  if (reason.empty()) {
    return "NOT_OBTAINED: debug log policy facts are unavailable";
  }
  if (reason.starts_with(kPrefix)) {
    return reason;
  }
  return "NOT_OBTAINED: " + reason;
}

[[nodiscard]] DebugLogPolicySnapshot unavailable(std::string reason) {
  return {
      .debug_mode = DebugModeState::unavailable,
      .granularity = DebugLogGranularity::unavailable,
      .filterable_fields = {},
      .locating_semantics = DebugLogLocationSemantics::unavailable,
      .coverage = {},
      .existing_log_retention = DebugLogRetention::unavailable,
      .facts_available = false,
      .not_obtained_reason = not_obtained_reason(std::move(reason)),
  };
}

}  // namespace

DebugLogPolicyReader::DebugLogPolicyReader(
    std::shared_ptr<DebugLogPolicyProvider const> provider) noexcept
    : provider_(std::move(provider)) {}

DebugLogPolicySnapshot DebugLogPolicyReader::snapshot() const {
  if (!provider_) {
    return unavailable("debug log policy provider is not configured");
  }

  DebugLogPolicyRead read;
  try {
    read = provider_->read_debug_log_policy();
  } catch (...) {
    return unavailable("debug log policy provider failed");
  }

  if (!read.available) {
    return unavailable(std::move(read.not_obtained_reason));
  }
  if (read.granularity == DebugLogGranularity::unavailable) {
    return unavailable("debug log policy provider omitted log granularity");
  }
  if (read.locating_semantics == DebugLogLocationSemantics::unavailable) {
    return unavailable("debug log policy provider omitted locating semantics");
  }
  if (read.filterable_fields.empty()) {
    return unavailable("debug log policy provider omitted filterable fields");
  }
  if (read.coverage.empty()) {
    return unavailable("debug log policy provider omitted coverage scope");
  }
  if (read.debug_enabled && read.granularity != DebugLogGranularity::maximum) {
    return unavailable("debug mode requires maximum log granularity");
  }
  if (!read.debug_enabled && read.granularity != DebugLogGranularity::normal) {
    return unavailable("disabled debug mode requires normal log granularity");
  }

  normalize(read.filterable_fields);
  normalize(read.coverage);
  return {
      .debug_mode = read.debug_enabled ? DebugModeState::enabled
                                       : DebugModeState::disabled,
      .granularity = read.granularity,
      .filterable_fields = std::move(read.filterable_fields),
      .locating_semantics = read.locating_semantics,
      .coverage = std::move(read.coverage),
      // Closing debug mode only changes the future granularity; existing
      // structured records remain owned by the log aggregate.
      .existing_log_retention =
          DebugLogRetention::preserves_existing_records,
      .facts_available = true,
      .not_obtained_reason = {},
  };
}

}  // namespace azzs::application
