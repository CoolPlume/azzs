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

struct DiagnosticContext final {
  std::string workbench_build;
  std::string release_form;
  std::string process_architecture;
  std::string package_architecture;
  std::string windows_version;
  std::string language;
  std::string timezone;
  WallClockTime coverage_started_at;
  WallClockTime coverage_ended_at;
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
  std::vector<DiagnosticField> fields;
  // Known identity or credential fragments that may also occur inside raw
  // adapter error text. Values are consumed only by the redactor and are never
  // serialized.
  std::vector<std::string> sensitive_values;
};

struct ExecutionLogReceipt final {
  bool persisted{false};
  std::uint64_t segment{0};
  std::uint64_t sequence{0};
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
  std::size_t file_count{0};
  std::string file_name;
  std::string file_bytes;
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
  [[nodiscard]] virtual ExecutionLogClearReceipt clear() = 0;
  [[nodiscard]] virtual DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const& context) = 0;
};

}  // namespace azzs::application
