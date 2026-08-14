#include "azzs/application/debug_log_policy/debug_log_policy.hpp"

#include <algorithm>
#include <optional>
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

template <typename Value, std::size_t Size>
[[nodiscard]] std::optional<Value> first_missing(
    std::vector<Value> const& values,
    std::array<Value, Size> const& required) {
  for (auto const value : required) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
      return value;
    }
  }
  return std::nullopt;
}

[[nodiscard]] char const* name(DebugModeState value) noexcept {
  switch (value) {
    case DebugModeState::unavailable:
      return "unavailable";
    case DebugModeState::disabled:
      return "disabled";
    case DebugModeState::enabled:
      return "enabled";
  }
  return "unavailable";
}

[[nodiscard]] char const* name(DebugLogGranularity value) noexcept {
  switch (value) {
    case DebugLogGranularity::unavailable:
      return "unavailable";
    case DebugLogGranularity::normal:
      return "normal";
    case DebugLogGranularity::maximum:
      return "maximum";
  }
  return "unavailable";
}

[[nodiscard]] char const* name(DebugLogFilterField value) noexcept {
  switch (value) {
    case DebugLogFilterField::command:
      return "command";
    case DebugLogFilterField::state_transition:
      return "state_transition";
    case DebugLogFilterField::catalog:
      return "catalog";
    case DebugLogFilterField::source:
      return "source";
    case DebugLogFilterField::download:
      return "download";
    case DebugLogFilterField::installer:
      return "installer";
    case DebugLogFilterField::recovery:
      return "recovery";
    case DebugLogFilterField::adapter_result:
      return "adapter_result";
    case DebugLogFilterField::duration:
      return "duration";
    case DebugLogFilterField::retry:
      return "retry";
    case DebugLogFilterField::correlation_id:
      return "correlation_id";
  }
  return "unknown";
}

[[nodiscard]] char const* name(DebugLogLocationSemantics value) noexcept {
  switch (value) {
    case DebugLogLocationSemantics::unavailable:
      return "unavailable";
    case DebugLogLocationSemantics::event_sequence_and_correlation_id:
      return "event_sequence_and_correlation_id";
  }
  return "unavailable";
}

[[nodiscard]] char const* name(DebugLogCoverage value) noexcept {
  switch (value) {
    case DebugLogCoverage::catalog_validation:
      return "catalog_validation";
    case DebugLogCoverage::source_resolution:
      return "source_resolution";
    case DebugLogCoverage::dependency_planning:
      return "dependency_planning";
    case DebugLogCoverage::download:
      return "download";
    case DebugLogCoverage::installation:
      return "installation";
    case DebugLogCoverage::result_detection:
      return "result_detection";
    case DebugLogCoverage::recovery:
      return "recovery";
    case DebugLogCoverage::state_transition:
      return "state_transition";
    case DebugLogCoverage::adapter_result:
      return "adapter_result";
  }
  return "unknown";
}

[[nodiscard]] char const* name(DebugLogRetention value) noexcept {
  switch (value) {
    case DebugLogRetention::unavailable:
      return "unavailable";
    case DebugLogRetention::preserves_existing_records:
      return "preserves_existing_records";
  }
  return "unavailable";
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
  if (read.debug_enabled && read.granularity != DebugLogGranularity::maximum) {
    return unavailable("debug mode requires maximum log granularity");
  }
  if (!read.debug_enabled && read.granularity != DebugLogGranularity::normal) {
    return unavailable("disabled debug mode requires normal log granularity");
  }

  normalize(read.filterable_fields);
  normalize(read.coverage);
  if (auto const missing = first_missing(
          read.filterable_fields, kRequiredDebugLogFilterFields)) {
    return unavailable(std::string{
                           "debug log policy provider omitted required filter "
                           "field: "} +
                       name(*missing));
  }
  if (read.filterable_fields.size() !=
      kRequiredDebugLogFilterFields.size()) {
    return unavailable(
        "debug log policy provider supplied an unsupported filter field");
  }
  if (auto const missing =
          first_missing(read.coverage, kRequiredDebugLogCoverage)) {
    return unavailable(std::string{
                           "debug log policy provider omitted required "
                           "coverage: "} +
                       name(*missing));
  }
  if (read.coverage.size() != kRequiredDebugLogCoverage.size()) {
    return unavailable(
        "debug log policy provider supplied unsupported coverage");
  }
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

DebugLogPolicyContext make_debug_log_policy_context(
    DebugLogPolicySnapshot const& snapshot) {
  DebugLogPolicyContext context{
      .facts_available = snapshot.facts_available,
      .debug_mode = name(snapshot.debug_mode),
      .granularity = name(snapshot.granularity),
      .filterable_fields = {},
      .locating_semantics = name(snapshot.locating_semantics),
      .coverage = {},
      .existing_log_retention = name(snapshot.existing_log_retention),
      .not_obtained_reason = snapshot.not_obtained_reason,
  };
  context.filterable_fields.reserve(snapshot.filterable_fields.size());
  for (auto const field : snapshot.filterable_fields) {
    context.filterable_fields.emplace_back(name(field));
  }
  context.coverage.reserve(snapshot.coverage.size());
  for (auto const coverage : snapshot.coverage) {
    context.coverage.emplace_back(name(coverage));
  }
  return context;
}

}  // namespace azzs::application
