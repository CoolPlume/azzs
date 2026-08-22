#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "azzs/application/clock.hpp"

namespace azzs::application {

struct CorrelationId final {
  std::string value;

  friend bool operator==(CorrelationId const&, CorrelationId const&) = default;
};

enum class ExecutionEventKind {
  user_command,
  state_transition,
  adapter_result,
  coverage_gap,
};

enum class ExecutionResult {
  started,
  succeeded,
  failed,
  cancelled,
  unknown,
};

// Noncritical events are diagnostic detail only. State transitions and coverage
// boundaries remain critical even when a caller accidentally classifies them
// as noncritical.
enum class ExecutionLogCriticality {
  critical,
  noncritical,
};

enum class ExecutionLogCapacityState {
  available,
  space_exhausted,
};

struct ExecutionError final {
  std::string source;
  std::int64_t code{0};
  std::string message;
};

struct LastTrustedState final {
  std::uint64_t generation{0};
  std::string summary;
};

enum class CoverageGapKind {
  dropped,
  truncated,
  abnormal_exit,
  platform_unavailable,
  flush_failed,
  permission_denied,
  redaction_gap,
  unknown_after_last_persisted,
};

struct CoverageGap final {
  CoverageGapKind kind{CoverageGapKind::platform_unavailable};
  std::optional<std::uint64_t> first_missing_sequence;
  std::optional<std::uint64_t> last_missing_sequence;
  std::string reason;
};

enum class DiagnosticValueDisposition {
  retain,
  sensitive,
};

struct DiagnosticField final {
  std::string key;
  std::string value;
  // Callers must opt in to retaining a diagnostic value. This prevents a new
  // or misspelled field key from silently bypassing centralized redaction.
  DiagnosticValueDisposition disposition{DiagnosticValueDisposition::sensitive};
};

struct MissingDiagnosticFact final {
  std::string fact;
  std::string reason;
};

// Each owner supplies either a retained fact or an explicit unavailable
// reason. Values default to sensitive so a new fact cannot bypass redaction.
struct DiagnosticFact final {
  std::string value;
  std::string unavailable_reason;
  DiagnosticValueDisposition disposition{DiagnosticValueDisposition::sensitive};
};

struct DiagnosticContext final {
  std::string workbench_build;
  std::string release_form;
  std::string process_architecture;
  std::string package_architecture;
  std::string windows_version;
  std::string language;
  std::string timezone;
  std::optional<WallClockTime> coverage_started_at;
  std::optional<WallClockTime> coverage_ended_at;
  DiagnosticFact frozen_directory_identity;
  DiagnosticFact directory_application_association;
  DiagnosticFact directory_load_result;
  DiagnosticFact directory_release_result;
  DiagnosticFact batch_plan;
  DiagnosticFact debug_log_coverage;
  std::vector<DiagnosticField> fields;
  std::vector<MissingDiagnosticFact> missing_facts;
  std::vector<std::string> sensitive_values;
};

struct ExecutionEvent final {
  ExecutionEventKind kind{ExecutionEventKind::state_transition};
  std::string component;
  std::string stage;
  ExecutionResult result{ExecutionResult::unknown};
  std::optional<ExecutionError> error;
  std::optional<LastTrustedState> last_trusted_state;
  std::optional<CoverageGap> coverage_gap;
  ExecutionLogCriticality criticality{ExecutionLogCriticality::critical};
  std::vector<DiagnosticField> fields;
  // Known identity or credential fragments that may also occur inside raw
  // adapter error text. Values are consumed only by the redactor and are never
  // serialized.
  std::vector<std::string> sensitive_values;
};

struct ExecutionLogReceipt final {
  bool persisted{false};
  bool suppressed{false};
  std::uint64_t segment{0};
  std::uint64_t sequence{0};
  ExecutionLogCapacityState capacity_state{
      ExecutionLogCapacityState::available};
  std::uint64_t noncritical_dropped_count{0};
  std::string error;
};

// Logging remains centralized even when a caller requests the richer debug
// projection. Adapters that cannot change their diagnostic detail report that
// fact instead of letting a settings surface infer a successful request.
enum class ExecutionLogDebugModeStatus {
  applied,
  unavailable,
};

struct ExecutionLogDebugModeResult final {
  ExecutionLogDebugModeStatus status{ExecutionLogDebugModeStatus::unavailable};
  bool enabled{false};
  std::string error;
};

struct ExecutionLogDebugModeRead final {
  bool available{false};
  bool enabled{false};
  std::string error;
};

struct ExecutionLogClearReceipt final {
  bool cleared{false};
  std::uint64_t cutoff_segment{0};
  std::uint64_t cutoff_sequence{0};
  std::uint64_t active_segment{0};
  std::string error;
};

struct DiagnosticExportReceipt final {
  bool produced{false};
  // A produced file may still name observations that the host could not
  // obtain. Consumers must not present that as a complete diagnostic.
  bool complete{false};
  std::size_t file_count{0};
  std::size_t missing_fact_count{0};
  std::string file_name;
  std::string file_bytes;
  std::string error;
};

// A read-only, already-redacted projection. Consumers never receive the
// storage format or an unredacted event payload.
struct ExecutionLogFieldProjection final {
  std::string key;
  std::string value;
};

struct ExecutionLogEventProjection final {
  std::uint64_t segment{0};
  std::uint64_t sequence{0};
  CorrelationId correlation;
  std::int64_t recorded_at_milliseconds{0};
  ExecutionEventKind kind{ExecutionEventKind::state_transition};
  std::string component;
  std::string stage;
  ExecutionResult result{ExecutionResult::unknown};
  std::optional<ExecutionError> error;
  std::optional<LastTrustedState> last_trusted_state;
  std::optional<CoverageGap> coverage_gap;
  std::vector<ExecutionLogFieldProjection> fields;
};

// The cutoff record is durable, while the replacement segment has not yet
// been verified. Readers must present this as a committed clear in progress,
// not as unchanged old records or a confirmed new segment.
struct ExecutionLogPendingClearProjection final {
  std::uint64_t cutoff_segment{0};
  std::uint64_t cutoff_sequence{0};
};

struct ExecutionLogSnapshot final {
  bool available{false};
  std::uint64_t active_segment{0};
  std::uint64_t last_sequence{0};
  std::size_t durable_bytes{0};
  std::optional<WallClockTime> coverage_started_at;
  std::optional<WallClockTime> coverage_ended_at;
  std::size_t coverage_gap_count{0};
  ExecutionLogCapacityState capacity_state{
      ExecutionLogCapacityState::available};
  std::uint64_t noncritical_dropped_count{0};
  std::optional<ExecutionLogPendingClearProjection> pending_clear;
  std::vector<ExecutionLogEventProjection> events;
  std::string error;
};

// Application-owned structured logging seam. Implementations must redact all
// diagnostic text before any byte reaches durable storage.
class ExecutionLog {
 public:
  virtual ~ExecutionLog() = default;

  [[nodiscard]] virtual CorrelationId begin_correlation() = 0;
  [[nodiscard]] virtual ExecutionLogReceipt append(
      CorrelationId const& correlation, ExecutionEvent const& event) = 0;
  [[nodiscard]] virtual ExecutionLogDebugModeResult set_debug_mode(
      bool enabled) {
    return {.enabled = enabled,
            .error = "execution log debug-mode control is unavailable"};
  }
  [[nodiscard]] virtual ExecutionLogDebugModeRead debug_mode() const {
    return {.error = "execution log debug-mode state is unavailable"};
  }
  // Test fakes that only record writes may retain the empty default. Production
  // adapters override it with a parsed, centrally redacted projection.
  [[nodiscard]] virtual ExecutionLogSnapshot snapshot() { return {}; }
  [[nodiscard]] virtual ExecutionLogClearReceipt clear() = 0;
  [[nodiscard]] virtual DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const& context) = 0;
};

}  // namespace azzs::application
