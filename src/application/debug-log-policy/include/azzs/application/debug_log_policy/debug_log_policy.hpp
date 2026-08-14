#pragma once

#include <memory>
#include <string>
#include <vector>

namespace azzs::application {

enum class DebugModeState {
  unavailable,
  disabled,
  enabled,
};

enum class DebugLogGranularity {
  unavailable,
  normal,
  maximum,
};

enum class DebugLogFilterField {
  command,
  state_transition,
  catalog,
  source,
  download,
  installer,
  recovery,
  adapter_result,
  duration,
  retry,
  correlation_id,
};

enum class DebugLogLocationSemantics {
  unavailable,
  event_sequence_and_correlation_id,
};

enum class DebugLogCoverage {
  catalog_validation,
  source_resolution,
  dependency_planning,
  download,
  installation,
  result_detection,
  recovery,
  state_transition,
  adapter_result,
};

enum class DebugLogRetention {
  unavailable,
  preserves_existing_records,
};

// Facts returned by the settings/status owner. This module deliberately does
// not own, persist, or cache any debug-mode or logging state.
struct DebugLogPolicyRead final {
  bool available{false};
  bool debug_enabled{false};
  DebugLogGranularity granularity{DebugLogGranularity::unavailable};
  std::vector<DebugLogFilterField> filterable_fields;
  DebugLogLocationSemantics locating_semantics{
      DebugLogLocationSemantics::unavailable};
  std::vector<DebugLogCoverage> coverage;
  std::string not_obtained_reason;
};

// Implemented by the authoritative settings/status owner. Issue 32 can add
// its provider without giving readers a second writable state source.
class DebugLogPolicyProvider {
 public:
  virtual ~DebugLogPolicyProvider() = default;
  [[nodiscard]] virtual DebugLogPolicyRead read_debug_log_policy() const = 0;
};

// Stable read-only projection consumed by history and diagnostic views.
struct DebugLogPolicySnapshot final {
  DebugModeState debug_mode{DebugModeState::unavailable};
  DebugLogGranularity granularity{DebugLogGranularity::unavailable};
  std::vector<DebugLogFilterField> filterable_fields;
  DebugLogLocationSemantics locating_semantics{
      DebugLogLocationSemantics::unavailable};
  std::vector<DebugLogCoverage> coverage;
  DebugLogRetention existing_log_retention{DebugLogRetention::unavailable};
  bool facts_available{false};
  std::string not_obtained_reason;
};

class DebugLogPolicyReader final {
 public:
  DebugLogPolicyReader() = default;
  explicit DebugLogPolicyReader(
      std::shared_ptr<DebugLogPolicyProvider const> provider) noexcept;

  [[nodiscard]] DebugLogPolicySnapshot snapshot() const;

 private:
  std::shared_ptr<DebugLogPolicyProvider const> provider_;
};

}  // namespace azzs::application
