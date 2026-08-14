#pragma once

#include <array>
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

inline constexpr std::array<DebugLogFilterField, 11>
    kRequiredDebugLogFilterFields{
        DebugLogFilterField::command,
        DebugLogFilterField::state_transition,
        DebugLogFilterField::catalog,
        DebugLogFilterField::source,
        DebugLogFilterField::download,
        DebugLogFilterField::installer,
        DebugLogFilterField::recovery,
        DebugLogFilterField::adapter_result,
        DebugLogFilterField::duration,
        DebugLogFilterField::retry,
        DebugLogFilterField::correlation_id,
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

inline constexpr std::array<DebugLogCoverage, 9>
    kRequiredDebugLogCoverage{
        DebugLogCoverage::catalog_validation,
        DebugLogCoverage::source_resolution,
        DebugLogCoverage::dependency_planning,
        DebugLogCoverage::download,
        DebugLogCoverage::installation,
        DebugLogCoverage::result_detection,
        DebugLogCoverage::recovery,
        DebugLogCoverage::state_transition,
        DebugLogCoverage::adapter_result,
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

// Stable injection seam for history and diagnostic consumers. Implementations
// expose policy facts only and cannot mutate settings or retained log records.
class DebugLogPolicySnapshotSource {
 public:
  virtual ~DebugLogPolicySnapshotSource() = default;
  [[nodiscard]] virtual DebugLogPolicySnapshot snapshot() const = 0;
};

// Stable string tokens for adding one already-read snapshot to a diagnostic
// context without re-reading the provider or duplicating enum mappings.
struct DebugLogPolicyContext final {
  bool facts_available{false};
  std::string debug_mode;
  std::string granularity;
  std::vector<std::string> filterable_fields;
  std::string locating_semantics;
  std::vector<std::string> coverage;
  std::string existing_log_retention;
  std::string not_obtained_reason;
};

[[nodiscard]] DebugLogPolicyContext make_debug_log_policy_context(
    DebugLogPolicySnapshot const& snapshot);

class DebugLogPolicyReader final : public DebugLogPolicySnapshotSource {
 public:
  DebugLogPolicyReader() = default;
  explicit DebugLogPolicyReader(
      std::shared_ptr<DebugLogPolicyProvider const> provider) noexcept;

  [[nodiscard]] DebugLogPolicySnapshot snapshot() const override;

 private:
  std::shared_ptr<DebugLogPolicyProvider const> provider_;
};

}  // namespace azzs::application
