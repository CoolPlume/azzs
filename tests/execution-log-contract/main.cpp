#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "azzs/adapters/infrastructure/local_file_log_storage.hpp"
#include "azzs/adapters/infrastructure/structured_execution_log.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "in_memory_log_storage.hpp"

#ifdef _WIN32
#include <windows.h>

#include "azzs/adapters/windows/windows_device_data_environment.hpp"
#endif

namespace {

using azzs::adapters::infrastructure::StructuredExecutionLog;
using azzs::adapters::infrastructure::LocalFileLogStorage;
#ifdef _WIN32
using azzs::adapters::infrastructure::classify_windows_log_storage_write_failure;
#endif
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionEventKind;
using azzs::application::ExecutionError;
using azzs::application::ExecutionResult;
using azzs::application::CorrelationId;
using azzs::application::CoverageGap;
using azzs::application::CoverageGapKind;
using azzs::application::DiagnosticFact;
using azzs::application::DiagnosticField;
using azzs::application::DiagnosticContext;
using azzs::application::MissingDiagnosticFact;
using azzs::application::DiagnosticValueDisposition;
using azzs::application::ExecutionLogCapacityState;
using azzs::application::ExecutionLogCriticality;
using azzs::application::LastTrustedState;
using azzs::application::WallClockTime;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryLogStorage;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "execution log contract failed: " << message << '\n';
  }
  return condition;
}

void seed_storage(InMemoryLogStorage& storage, std::string bytes) {
  auto transaction = storage.begin_transaction();
  auto const seeded = transaction->replace(std::move(bytes));
  if (!seeded.committed) {
    std::abort();
  }
}

void replace_once(std::string& bytes, std::string_view original,
                  std::string_view replacement) {
  auto const position = bytes.find(original);
  if (position == std::string::npos) {
    std::abort();
  }
  bytes.replace(position, original.size(), replacement);
}

[[nodiscard]] bool verify_unknown_and_corrupt_formats_are_preserved() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'399'000}}};
  constexpr std::string_view future_bytes{
      "AZZS-EXECUTION-LOG\t2\nCOUNTERS\t7\t9\t4\nFUTURE\topaque\n"};
  constexpr std::string_view corrupt_bytes{
      "AZZS-EXECUTION-LOG\t1\nCOUNTERS\tbroken\nopaque-original-bytes\n"};
  constexpr std::string_view corrupt_record_bytes{
      "AZZS-EXECUTION-LOG\t1\nCOUNTERS\t1\t1\t1\nBROKEN\topaque-record\n"};

  InMemoryLogStorage future_for_correlation;
  seed_storage(future_for_correlation, std::string{future_bytes});
  StructuredExecutionLog future_correlation_log{future_for_correlation, clock};
  auto const correlation = future_correlation_log.begin_correlation();

  InMemoryLogStorage future_for_append;
  seed_storage(future_for_append, std::string{future_bytes});
  StructuredExecutionLog future_append_log{future_for_append, clock};
  auto const future_append = future_append_log.append(
      CorrelationId{"correlation-1"},
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "must-not-be-written",
          .stage = "future-format",
          .result = ExecutionResult::failed,
      });

  InMemoryLogStorage future_for_clear;
  seed_storage(future_for_clear, std::string{future_bytes});
  StructuredExecutionLog future_clear_log{future_for_clear, clock};
  auto const future_clear = future_clear_log.clear();

  InMemoryLogStorage corrupt_for_append;
  seed_storage(corrupt_for_append, std::string{corrupt_bytes});
  StructuredExecutionLog corrupt_append_log{corrupt_for_append, clock};
  auto const corrupt_append = corrupt_append_log.append(
      CorrelationId{"correlation-1"},
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "must-not-be-written",
          .stage = "corrupt-format",
          .result = ExecutionResult::failed,
      });

  InMemoryLogStorage corrupt_record_for_append;
  seed_storage(corrupt_record_for_append, std::string{corrupt_record_bytes});
  StructuredExecutionLog corrupt_record_log{corrupt_record_for_append, clock};
  auto const corrupt_record_append = corrupt_record_log.append(
      CorrelationId{"correlation-1"},
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "must-not-be-written",
          .stage = "corrupt-record",
          .result = ExecutionResult::failed,
      });

  return expect(correlation.value.empty(),
                "a future log format must refuse correlation allocation") &&
         expect(future_for_correlation.bytes() == future_bytes,
                "correlation allocation must preserve future log bytes") &&
         expect(!future_append.persisted && !future_append.error.empty(),
                "append must explicitly reject a future log format") &&
         expect(future_for_append.bytes() == future_bytes,
                "append must preserve future log bytes exactly") &&
         expect(!future_clear.cleared && !future_clear.error.empty(),
                "clear must explicitly reject a future log format") &&
         expect(future_for_clear.bytes() == future_bytes,
                "clear must preserve future log bytes exactly") &&
         expect(!corrupt_append.persisted && !corrupt_append.error.empty(),
                "append must explicitly reject corrupt counters") &&
         expect(corrupt_for_append.bytes() == corrupt_bytes,
                "append must preserve corrupt log bytes exactly") &&
         expect(!corrupt_record_append.persisted &&
                    !corrupt_record_append.error.empty(),
                "append must explicitly reject corrupt record framing") &&
         expect(corrupt_record_for_append.bytes() == corrupt_record_bytes,
                "append must preserve corrupt record bytes exactly");
}

[[nodiscard]] bool verify_diagnostic_export_is_read_only_during_gaps() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'399'500}}};
  constexpr std::string_view future_bytes{
      "AZZS-EXECUTION-LOG\t2\nCOUNTERS\t7\t9\t4\n"
      "FUTURE\tultra-secret-form-content\n"};
  constexpr std::string_view corrupt_bytes{
      "AZZS-EXECUTION-LOG\t1\nCOUNTERS\tbroken\n"
      "ultra-secret-corrupt-content\n"};

  InMemoryLogStorage future_storage;
  seed_storage(future_storage, std::string{future_bytes});
  StructuredExecutionLog future_log{future_storage, clock};
  auto const future_export = future_log.export_diagnostic(DiagnosticContext{
      .coverage_started_at = clock.now(),
      .coverage_ended_at = clock.now(),
  });

  InMemoryLogStorage corrupt_storage;
  seed_storage(corrupt_storage, std::string{corrupt_bytes});
  StructuredExecutionLog corrupt_log{corrupt_storage, clock};
  auto const corrupt_export = corrupt_log.export_diagnostic(DiagnosticContext{
      .coverage_started_at = clock.now(),
      .coverage_ended_at = clock.now(),
  });

  InMemoryLogStorage pending_clear_storage;
  StructuredExecutionLog pending_clear_log{pending_clear_storage, clock};
  auto const correlation = pending_clear_log.begin_correlation();
  auto const old_event = pending_clear_log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "old-private-event",
          .stage = "persist",
          .result = ExecutionResult::succeeded,
      });
  pending_clear_storage.fail_replace_on(
      2, "injected pending clear transition failure");
  auto const interrupted_clear = pending_clear_log.clear();
  auto const bytes_before_export = pending_clear_storage.bytes();
  auto const pending_export = pending_clear_log.export_diagnostic(
      DiagnosticContext{
          .coverage_started_at = clock.now(),
          .coverage_ended_at = clock.now(),
      });

  return expect(future_export.produced && future_export.file_count == 1,
                "a future source format must still produce one diagnostic") &&
         expect(future_storage.bytes() == future_bytes,
                "future source bytes must remain unchanged by export") &&
         expect(future_export.file_bytes.find(
                    "SOURCE_LOG_STATUS\tunsupported_execution_log_format") !=
                    std::string_view::npos &&
                    future_export.file_bytes.find("SOURCE_LOG_BYTES\t") !=
                        std::string_view::npos &&
                    future_export.file_bytes.find(
                        "SOURCE_LOG_FINGERPRINT_FNV1A64\t") !=
                        std::string_view::npos,
                "a future source must be summarized as an explicit gap") &&
         expect(future_export.file_bytes.find("ultra-secret-form-content") ==
                    std::string_view::npos,
                "untrusted future bytes must never be embedded in an export") &&
         expect(corrupt_export.produced && corrupt_export.file_count == 1 &&
                    corrupt_storage.bytes() == corrupt_bytes,
                "a corrupt source must export without changing original bytes") &&
         expect(corrupt_export.file_bytes.find(
                    "SOURCE_LOG_STATUS\tcorrupt_execution_log_counters") !=
                    std::string_view::npos &&
                    corrupt_export.file_bytes.find(
                        "ultra-secret-corrupt-content") ==
                        std::string_view::npos,
                "a corrupt source export must describe the gap without raw bytes") &&
         expect(old_event.persisted && !interrupted_clear.cleared &&
                    pending_export.produced,
                "an interrupted clear must remain diagnosable") &&
         expect(pending_clear_storage.bytes() == bytes_before_export,
                "diagnostic export must not complete a pending clear") &&
         expect(pending_export.file_bytes.find(
                    "SOURCE_LOG_STATUS\tpending_clear") !=
                    std::string_view::npos &&
                    pending_export.file_bytes.find("old-private-event") ==
                        std::string_view::npos,
                "a pending clear export must retain the boundary but omit cleared events");
}

[[nodiscard]] bool verify_first_event_is_persisted() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'400'000}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};

  auto const correlation = log.begin_correlation();
  auto const receipt = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::user_command,
          .component = "catalog",
          .stage = "validate",
          .result = ExecutionResult::succeeded,
      });

  auto const bytes = storage.bytes();
  return expect(receipt.persisted, "the first event must be persisted") &&
         expect(receipt.segment == 1, "the first segment must be stable") &&
         expect(receipt.sequence == 1,
                "the first event sequence must be stable") &&
         expect(!correlation.value.empty(),
                "the adapter must allocate a correlation identifier") &&
         expect(bytes.find("catalog") != std::string_view::npos,
                "the persisted event must retain its component") &&
         expect(bytes.find("validate") != std::string_view::npos,
                "the persisted event must retain its stage");
}

[[nodiscard]] bool verify_semantic_order_and_signed_error_contract() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'400'250}}};
  InMemoryLogStorage valid_storage;
  StructuredExecutionLog valid_log{valid_storage, clock};
  auto const correlation = valid_log.begin_correlation();
  auto const negative_error = valid_log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "windows-adapter",
          .stage = "invoke",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .source = "hresult",
              .code = -2'147'024'891,
              .message = "access denied",
          },
      });
  auto const after_negative = valid_log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "windows-adapter",
          .stage = "finish",
          .result = ExecutionResult::failed,
      });

  auto orphan_segment_bytes = valid_storage.bytes();
  replace_once(orphan_segment_bytes, "COUNTERS\t1\t2\t1",
               "COUNTERS\t2\t2\t1");
  replace_once(orphan_segment_bytes, "EVENT\t1\t1", "EVENT\t2\t1");
  replace_once(orphan_segment_bytes, "EVENT\t1\t2", "EVENT\t2\t2");
  InMemoryLogStorage orphan_segment_storage;
  seed_storage(orphan_segment_storage, orphan_segment_bytes);
  StructuredExecutionLog orphan_segment_log{orphan_segment_storage, clock};
  auto const orphan_segment_append = orphan_segment_log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "must-not-write",
          .stage = "orphan-segment",
          .result = ExecutionResult::failed,
      });

  auto duplicate_sequence_bytes = valid_storage.bytes();
  replace_once(duplicate_sequence_bytes, "EVENT\t1\t2", "EVENT\t1\t1");
  InMemoryLogStorage duplicate_sequence_storage;
  seed_storage(duplicate_sequence_storage, duplicate_sequence_bytes);
  StructuredExecutionLog duplicate_sequence_log{duplicate_sequence_storage,
                                                  clock};
  auto const duplicate_sequence_append = duplicate_sequence_log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "must-not-write",
          .stage = "duplicate-sequence",
          .result = ExecutionResult::failed,
      });

  auto const before_unallocated = valid_storage.bytes();
  auto const unallocated = valid_log.append(
      CorrelationId{"correlation-999"},
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "must-not-write",
          .stage = "unallocated-correlation",
          .result = ExecutionResult::failed,
      });
  auto const noncanonical = valid_log.append(
      CorrelationId{"correlation-0001"},
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "must-not-write",
          .stage = "noncanonical-correlation",
          .result = ExecutionResult::failed,
      });

  return expect(negative_error.persisted && after_negative.persisted &&
                    after_negative.sequence == 2 &&
                    valid_storage.bytes().find("-2147024891") !=
                        std::string_view::npos,
                "a signed raw error code must remain readable on the next append") &&
         expect(!orphan_segment_append.persisted &&
                    !orphan_segment_append.error.empty() &&
                    orphan_segment_storage.bytes() == orphan_segment_bytes,
                "a non-initial segment without a clear boundary must be rejected") &&
         expect(!duplicate_sequence_append.persisted &&
                    !duplicate_sequence_append.error.empty() &&
                    duplicate_sequence_storage.bytes() ==
                        duplicate_sequence_bytes,
                "a duplicate durable event sequence must be rejected") &&
         expect(!unallocated.persisted && !unallocated.error.empty() &&
                    !noncanonical.persisted && !noncanonical.error.empty() &&
                    valid_storage.bytes() == before_unallocated,
                "unallocated and noncanonical correlations must be rejected");
}

[[nodiscard]] bool verify_failure_keeps_raw_error_and_trusted_state() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'401'000}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};

  auto const correlation = log.begin_correlation();
  auto const receipt = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "state-store",
          .stage = "replace",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .source = "filesystem",
              .code = 112,
              .message = "disk full while replacing authority",
          },
          .last_trusted_state = LastTrustedState{
              .generation = 41,
              .summary = "generation 41 passed full validation",
          },
      });

  auto const bytes = storage.bytes();
  return expect(receipt.persisted,
                "a failure event with diagnostic facts must persist") &&
         expect(bytes.find("filesystem") != std::string_view::npos,
                "the raw error source must be retained") &&
         expect(bytes.find("112") != std::string_view::npos,
                "the raw error code must be retained") &&
         expect(bytes.find("disk full while replacing authority") !=
                    std::string_view::npos,
                "the raw error text must be retained") &&
         expect(bytes.find("generation 41 passed full validation") !=
                    std::string_view::npos,
                "the last trusted state must be retained");
}

[[nodiscard]] bool verify_coverage_gaps_are_explicit() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'402'000}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();

  auto const dropped = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::coverage_gap,
          .component = "execution-log",
          .stage = "buffer",
          .result = ExecutionResult::failed,
          .coverage_gap = CoverageGap{
              .kind = CoverageGapKind::dropped,
              .first_missing_sequence = 7,
              .last_missing_sequence = 9,
              .reason = "memory pressure",
          },
      });
  auto const unknown = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::coverage_gap,
          .component = "execution-log",
          .stage = "startup-recovery",
          .result = ExecutionResult::unknown,
          .coverage_gap = CoverageGap{
              .kind = CoverageGapKind::unknown_after_last_persisted,
              .reason = "previous process ended abnormally",
          },
      });
  auto const abnormal_exit = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::coverage_gap,
          .component = "process-lifecycle",
          .stage = "startup",
          .result = ExecutionResult::unknown,
          .coverage_gap = CoverageGap{
              .kind = CoverageGapKind::abnormal_exit,
              .reason = "previous run had no normal-end marker",
          },
      });

  auto const bytes = storage.bytes();
  return expect(dropped.persisted && unknown.persisted &&
                    abnormal_exit.persisted,
                "known and unknown coverage gaps must persist") &&
         expect(bytes.find("dropped") != std::string_view::npos,
                "a dropped-event gap must be distinguishable") &&
         expect(bytes.find("7") != std::string_view::npos &&
                    bytes.find("9") != std::string_view::npos,
                "a known missing sequence interval must be retained") &&
         expect(bytes.find("unknown_after_last_persisted") !=
                    std::string_view::npos,
                "the unknowable interval after the last persisted event must "
                "be explicit") &&
         expect(bytes.find("previous process ended abnormally") !=
                    std::string_view::npos,
                "a coverage-gap reason must remain diagnosable") &&
         expect(bytes.find("abnormal_exit") != std::string_view::npos,
                "an abnormal prior process end must be distinguishable");
}

[[nodiscard]] bool verify_clear_commits_cutoff_before_new_segment() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'403'000}}};
  InMemoryLogStorage storage;
  storage.set_unrelated_aggregate("device-state-generation-41");
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();
  auto const before_clear = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "must-be-cleared",
          .stage = "completed",
          .result = ExecutionResult::succeeded,
      });
  auto const clear = log.clear();
  auto const after_clear = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::user_command,
          .component = "new-segment-event",
          .stage = "start",
          .result = ExecutionResult::started,
      });

  auto const revisions = storage.revisions();
  auto const final_bytes = storage.bytes();
  bool cutoff_was_committed_first = false;
  for (auto const& revision : revisions) {
    if (revision.find("must-be-cleared") != std::string::npos &&
        revision.find("CLEAR_CUTOFF") != std::string::npos) {
      cutoff_was_committed_first = true;
      break;
    }
  }

  return expect(before_clear.persisted && clear.cleared &&
                    after_clear.persisted,
                "append, clear, and post-clear append must persist") &&
         expect(cutoff_was_committed_first,
                "clear must durably commit its cutoff before switching") &&
         expect(final_bytes.find("CLEAR_CUTOFF") != std::string::npos,
                "the final log must retain the clear boundary") &&
         expect(final_bytes.find("must-be-cleared") == std::string::npos,
                "events before the clear cutoff must be removed") &&
         expect(final_bytes.find("new-segment-event") != std::string::npos,
                "events after clear must enter the new segment") &&
         expect(after_clear.segment == 2 && after_clear.sequence == 1,
                "clear must start a new segment with a fresh event sequence") &&
         expect(storage.unrelated_aggregate() ==
                    "device-state-generation-41",
                "clearing logs must not mutate another aggregate");
}

[[nodiscard]] bool verify_interrupted_clear_recovers_before_next_event() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'403'500}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();
  auto const old_event = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "old-segment-private-event",
          .stage = "complete",
          .result = ExecutionResult::succeeded,
      });
  storage.fail_replace_on(2, "injected segment transition failure");
  auto const interrupted = log.clear();
  auto const cutoff_bytes = storage.bytes();
  auto const pending_projection = log.snapshot();
  auto const recovered = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::coverage_gap,
          .component = "new-segment-after-recovery",
          .stage = "clear-recovery",
          .result = ExecutionResult::succeeded,
      });
  auto const final_bytes = storage.bytes();

  return expect(old_event.persisted && !interrupted.cleared &&
                    interrupted.cutoff_segment == 1 &&
                    interrupted.cutoff_sequence == 2,
                "an interrupted clear must report its committed cutoff") &&
         expect(cutoff_bytes.find("CLEAR_CUTOFF\t1\t2") !=
                    std::string_view::npos,
                "the clear cutoff must survive a failed segment switch") &&
         expect(pending_projection.available &&
                    pending_projection.pending_clear.has_value() &&
                    pending_projection.pending_clear->cutoff_segment == 1 &&
                    pending_projection.pending_clear->cutoff_sequence == 2 &&
                    pending_projection.events.empty(),
                "a committed cutoff with an unconfirmed replacement must project as pending clear") &&
         expect(recovered.persisted && recovered.segment == 2 &&
                    recovered.sequence == 1,
                "the next operation must finish the pending clear before append") &&
         expect(final_bytes.find("old-segment-private-event") ==
                    std::string_view::npos,
                "pending clear recovery must not revive pre-cutoff events") &&
         expect(final_bytes.find("SEGMENT_START\t2") !=
                    std::string_view::npos,
                "pending clear recovery must retain an explicit new boundary");
}

[[nodiscard]] bool verify_sensitive_values_never_reach_persistent_bytes() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'404'000}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();

  std::vector<std::string> const forbidden{
      "CorrectHorseBatteryStaple!",
      "token-very-secret-123",
      "session=private-cookie",
      "WORKSTATION-ALPHA",
      "alice",
      "device-unique-987654",
      "AA:BB:CC:DD:EE:FF",
      "192.168.44.9",
      "PrivateHomeWifi",
      R"(C:\Users\alice\Documents\private\trace.txt)",
      "https://alice:secret@example.invalid/download?token=hidden#private",
  };

  auto const receipt = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "download",
          .stage = "request",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .source = "http",
              .code = 401,
              .message =
                  "request from WORKSTATION-ALPHA/alice at 192.168.44.9 "
                  "used token-very-secret-123 and "
                  "https://alice:secret@example.invalid/download?token="
                  "hidden#private",
          },
          .fields = {
              DiagnosticField{"password", "CorrectHorseBatteryStaple!",
                              DiagnosticValueDisposition::sensitive},
              DiagnosticField{"token", "token-very-secret-123",
                              DiagnosticValueDisposition::sensitive},
              DiagnosticField{"cookie", "session=private-cookie",
                              DiagnosticValueDisposition::sensitive},
              DiagnosticField{"computer_name", "WORKSTATION-ALPHA",
                              DiagnosticValueDisposition::sensitive},
              DiagnosticField{"windows_username", "alice",
                              DiagnosticValueDisposition::sensitive},
              DiagnosticField{"device_id", "device-unique-987654",
                              DiagnosticValueDisposition::sensitive},
              DiagnosticField{"mac_address", "AA:BB:CC:DD:EE:FF",
                              DiagnosticValueDisposition::sensitive},
              DiagnosticField{"ip_address", "192.168.44.9",
                              DiagnosticValueDisposition::sensitive},
              DiagnosticField{"ssid", "PrivateHomeWifi",
                              DiagnosticValueDisposition::sensitive},
              DiagnosticField{"user_path",
                              R"(C:\Users\alice\Documents\private\trace.txt)",
                              DiagnosticValueDisposition::sensitive},
              DiagnosticField{
                  "sensitive_url",
                  "https://alice:secret@example.invalid/download?token="
                  "hidden#private",
                  DiagnosticValueDisposition::sensitive},
              DiagnosticField{"hardware_model", "ThinkPad P14s Gen 5",
                              DiagnosticValueDisposition::retain},
              DiagnosticField{"driver_version", "32.0.15.6094",
                              DiagnosticValueDisposition::retain},
              DiagnosticField{"network_state", "connected",
                              DiagnosticValueDisposition::retain},
              DiagnosticField{"normalized_path", "%TEMP%/azzs/trace.log",
                              DiagnosticValueDisposition::retain},
          },
          .sensitive_values = forbidden,
      });

  bool forbidden_bytes_found = false;
  for (auto const& revision : storage.revisions()) {
    for (auto const& value : forbidden) {
      if (revision.find(value) != std::string::npos) {
        forbidden_bytes_found = true;
      }
    }
  }

  auto const bytes = storage.bytes();
  return expect(receipt.persisted,
                "an event containing sensitive input must still persist") &&
         expect(!forbidden_bytes_found,
                "no sensitive value may appear in any persistent revision") &&
         expect(bytes.find("ThinkPad P14s Gen 5") != std::string_view::npos,
                "an allowed non-unique hardware model must remain usable") &&
         expect(bytes.find("32.0.15.6094") != std::string_view::npos,
                "an allowed driver version must remain usable") &&
         expect(bytes.find("connected") != std::string_view::npos,
                "an allowed network state must remain usable") &&
         expect(bytes.find("TEMP") != std::string_view::npos,
                "an allowed normalized path must remain usable");
}

[[nodiscard]] bool verify_raw_identity_labels_are_redacted() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'404'125}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();
  std::vector<std::string> const forbidden{
      "Private Wifi With Spaces",
      "a1b2c3d4e5f6",
      "HOST-PRIVATE-42",
      "Alice Smith",
      "device-unique-987654",
  };

  auto const receipt = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "network-adapter",
          .stage = "raw-identity-error",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .source = "win32",
              .code = 5,
              .message =
                  "SSID = Private Wifi With Spaces; BSSID: a1b2c3d4e5f6; "
                  "Computer Name=HOST-PRIVATE-42; User Name = Alice Smith; "
                  "Device Unique Identifier: device-unique-987654; "
                  "ordinary-detail-kept",
          },
      });
  auto const projection = log.snapshot();
  auto const exported = log.export_diagnostic(DiagnosticContext{
      .workbench_build = "1.0.0",
      .release_form = "portable",
      .process_architecture = "x64",
      .package_architecture = "x64",
      .windows_version = "10.0.26200",
      .language = "zh-CN",
      .timezone = "Asia/Shanghai",
      .coverage_started_at = clock.now(),
      .coverage_ended_at = clock.now(),
      .frozen_directory_identity = DiagnosticFact{
          .value = "catalog-identity-41",
          .disposition = DiagnosticValueDisposition::retain,
      },
      .directory_application_association = DiagnosticFact{
          .value = "application-association-41",
          .disposition = DiagnosticValueDisposition::retain,
      },
      .directory_load_result = DiagnosticFact{
          .value = "loaded",
          .disposition = DiagnosticValueDisposition::retain,
      },
      .directory_release_result = DiagnosticFact{
          .value = "release-approved",
          .disposition = DiagnosticValueDisposition::retain,
      },
      .batch_plan = DiagnosticFact{
          .value = "batch-plan-41",
          .disposition = DiagnosticValueDisposition::retain,
      },
      .debug_log_coverage = DiagnosticFact{
          .value = "normal",
          .disposition = DiagnosticValueDisposition::retain,
      },
  });

  bool leaked = false;
  for (auto const& value : forbidden) {
    leaked |= storage.bytes().find(value) != std::string::npos;
    leaked |= !projection.events.empty() &&
              projection.events.front().error.has_value() &&
              projection.events.front().error->message.find(value) !=
                  std::string::npos;
    leaked |= exported.file_bytes.find(value) != std::string::npos;
  }
  return expect(receipt.persisted && projection.available &&
                    exported.produced && exported.complete,
                "a complete diagnostic with raw identity labels must export") &&
         expect(!leaked,
                "central redaction must remove SSID, BSSID, computer name, username, and device identifiers from raw errors") &&
         expect(exported.file_bytes.find("ordinary-detail-kept") !=
                    std::string_view::npos,
                "redaction must retain non-identifying error context");
}

[[nodiscard]] bool verify_read_errors_fail_closed_and_export_gaps() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'404'175}}};
  InMemoryLogStorage storage;
  std::vector<std::string> const forbidden{
      "Private Wifi With Spaces",
      "HOST-PRIVATE-42",
  };
  storage.set_read_error(
      "log directory security validation failed: SSID=Private Wifi With "
      "Spaces; Computer Name=HOST-PRIVATE-42");
  StructuredExecutionLog log{storage, clock};

  auto const projection = log.snapshot();
  auto const exported = log.export_diagnostic(DiagnosticContext{
      .workbench_build = "1.0.0",
      .release_form = "portable",
      .process_architecture = "x64",
      .package_architecture = "x64",
      .windows_version = "10.0.26200",
      .language = "zh-CN",
      .timezone = "Asia/Shanghai",
      .coverage_started_at = clock.now(),
      .coverage_ended_at = clock.now(),
  });

  bool leaked = false;
  for (auto const& value : forbidden) {
    leaked |= exported.file_bytes.find(value) != std::string::npos;
  }
  return expect(!projection.available &&
                    projection.error.starts_with(
                        "log directory security validation failed:"),
                "storage read and security failures must make the snapshot unavailable") &&
         expect(exported.produced && !exported.complete &&
                    exported.missing_fact_count >= 1,
                "an export with an unreadable source must remain explicitly incomplete") &&
         expect(exported.file_bytes.find("SOURCE_LOG_BYTES\tNOT_OBTAINED") !=
                    std::string_view::npos &&
                    exported.file_bytes.find("NOT_OBTAINED\texecution_log\t") !=
                        std::string_view::npos,
                "an unreadable log must be represented as NOT_OBTAINED in its export") &&
         expect(!leaked &&
                    exported.file_bytes.find("SOURCE_LOG_STATUS\t") !=
                        std::string_view::npos,
                "read-error status and missing-fact records must retain centralized redaction");
}

[[nodiscard]] bool verify_user_paths_with_spaces_are_redacted() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'404'250}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();

  auto const windows = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "filesystem",
          .stage = "write",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .source = "filesystem",
              .code = 5,
              .message =
                  R"(write failed at C:\Users\Alice Smith\Private Files\trace.txt; retry-kept)",
          },
      });
  auto const posix = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "filesystem",
          .stage = "export",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .source = "filesystem",
              .code = 13,
              .message =
                  "export failed at /Users/Bob Jones/Private Data/report.txt; "
                  "export-kept",
          },
      });

  auto const bytes = storage.bytes();
  return expect(windows.persisted && posix.persisted,
                "events with user paths containing spaces must persist") &&
         expect(bytes.find("Alice Smith") == std::string_view::npos &&
                    bytes.find("Smith") == std::string_view::npos &&
                    bytes.find("Bob Jones") == std::string_view::npos &&
                    bytes.find("Jones") == std::string_view::npos,
                "central redaction must normalize user directory names containing spaces") &&
         expect(bytes.find("USERPROFILE") != std::string_view::npos &&
                    bytes.find("Private Files") != std::string_view::npos &&
                    bytes.find("Private Data") != std::string_view::npos &&
                    bytes.find("retry-kept") != std::string_view::npos &&
                    bytes.find("export-kept") != std::string_view::npos,
                "redaction must retain normalized relative paths and following diagnostics");
}

[[nodiscard]] bool verify_central_redaction_and_correlation_validation() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'404'500}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();
  auto const before_invalid = storage.bytes();
  auto const invalid = log.append(
      CorrelationId{"Authorization: Bearer correlation-secret"},
      ExecutionEvent{.kind = ExecutionEventKind::adapter_result,
                     .component = "must-not-persist",
                     .stage = "invalid-correlation",
                     .result = ExecutionResult::failed});
  auto const receipt = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "network",
          .stage = "diagnose",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .source = "winhttp",
              .code = 12029,
              .message =
                  "Authorization: Bearer bearer-secret C:/Users/alice/private "
                  "2001:db8::1 AA-BB-CC-DD-EE-FF control\x01tail",
          },
      });
  auto const bytes = storage.bytes();
  return expect(!invalid.persisted && !invalid.error.empty() &&
                    before_invalid == storage.revisions().at(0),
                "an unallocated correlation identifier must be rejected") &&
         expect(receipt.persisted,
                "a record with recognizable sensitive syntax must persist redacted") &&
         expect(bytes.find("correlation-secret") == std::string_view::npos &&
                    bytes.find("bearer-secret") == std::string_view::npos &&
                    bytes.find("C:/Users/alice/private") ==
                        std::string_view::npos &&
                    bytes.find("2001:db8::1") == std::string_view::npos &&
                    bytes.find("AA-BB-CC-DD-EE-FF") ==
                        std::string_view::npos,
                "central redaction must remove credentials, user paths, IPv6, and MAC addresses") &&
         expect(bytes.find("%01") != std::string_view::npos,
                "control bytes must be encoded before persistence");
}

[[nodiscard]] bool verify_redaction_defaults_and_stable_event_tokens() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'404'750}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();
  auto const safe = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "network-adapter",
          .stage = "connect",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .source = "winhttp",
              .code = 12'029,
              .message = "connection to ::abcd:1234 failed",
          },
          .fields = {
              DiagnosticField{"opaque", "bare-user-name"},
              DiagnosticField{"hardware_model", "ThinkPad P14s Gen 5",
                              DiagnosticValueDisposition::retain},
          },
      });
  auto const sensitive_token = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "private-component",
          .stage = "connect",
          .result = ExecutionResult::failed,
          .sensitive_values = {"private-component"},
      });
  auto const after_sensitive_token = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "network-adapter",
          .stage = "finish",
          .result = ExecutionResult::failed,
      });
  auto const before_invalid = storage.bytes();
  auto const invalid_component = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = R"(C:\Users\alice\private)",
          .stage = "persist",
          .result = ExecutionResult::failed,
      });
  auto const invalid_stage = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "network-adapter",
          .stage = "form content",
          .result = ExecutionResult::failed,
      });
  auto const bytes = storage.bytes();

  return expect(safe.persisted,
                "a structured event with safe stable tokens must persist") &&
         expect(sensitive_token.persisted && after_sensitive_token.persisted &&
                    bytes.find("private-component") == std::string_view::npos,
                "redacting a token must preserve a parseable stable token") &&
         expect(bytes.find("::abcd:1234") == std::string_view::npos,
                "a leading-compression IPv6 address must be redacted") &&
         expect(bytes.find("bare-user-name") == std::string_view::npos,
                "an unclassified field must be sensitive by default") &&
         expect(bytes.find("ThinkPad P14s Gen 5") != std::string_view::npos,
                "an explicitly retained non-unique model must remain useful") &&
         expect(!invalid_component.persisted && !invalid_stage.persisted &&
                    storage.bytes() == before_invalid,
                "component and stage must reject paths and free-form text before writing");
}

[[nodiscard]] bool diagnostic_file_is_self_contained(
    std::string_view file, std::string_view correlation) {
  auto const command = file.find("user_command");
  auto const failure = file.find("adapter_result");
  auto const first_correlation = file.find(correlation);
  auto const second_correlation =
      first_correlation == std::string_view::npos
          ? std::string_view::npos
          : file.find(correlation, first_correlation + correlation.size());
  return file.starts_with("AZZS-DIAGNOSTIC\t1\n") &&
         file.find("1.0.0+build.abc123") != std::string_view::npos &&
         file.find("portable") != std::string_view::npos &&
         file.find("arm64") != std::string_view::npos &&
         file.find("Windows 11 25H2 build 26200") !=
             std::string_view::npos &&
         file.find("zh-CN") != std::string_view::npos &&
         file.find("Asia/Shanghai") != std::string_view::npos &&
         file.find("ThinkPad P14s Gen 5") != std::string_view::npos &&
         file.find("catalog-schema-3") != std::string_view::npos &&
         file.find("local-trial") != std::string_view::npos &&
         command != std::string_view::npos &&
         failure != std::string_view::npos && command < failure &&
         first_correlation != std::string_view::npos &&
         second_correlation != std::string_view::npos &&
         file.find("installer") != std::string_view::npos &&
         file.find("launch") != std::string_view::npos &&
         file.find("win32") != std::string_view::npos &&
         file.find("740") != std::string_view::npos &&
         file.find("elevation was required") != std::string_view::npos &&
         file.find("generation 41 fully validated") !=
             std::string_view::npos &&
         file.find("LAST_PERSISTED_POINT\t1\t2") !=
             std::string_view::npos &&
         file.find("NOT_OBTAINED") != std::string_view::npos &&
         file.find("platform did not expose module stack") !=
             std::string_view::npos;
}

[[nodiscard]] bool verify_single_file_export_is_self_contained() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'405'000}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();

  auto const command = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::user_command,
          .component = "install-batch",
          .stage = "start",
          .result = ExecutionResult::started,
      });
  clock.advance(std::chrono::milliseconds{25});
  auto const failure = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "installer",
          .stage = "launch",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .source = "win32",
              .code = 740,
              .message = "elevation was required",
          },
          .last_trusted_state = LastTrustedState{
              .generation = 41,
              .summary = "generation 41 fully validated",
          },
      });

  auto const exported = log.export_diagnostic(
      DiagnosticContext{
          .workbench_build = "1.0.0+build.abc123",
          .release_form = "portable",
          .process_architecture = "arm64",
          .package_architecture = "arm64",
          .windows_version = "Windows 11 25H2 build 26200",
          .language = "zh-CN",
          .timezone = "Asia/Shanghai",
          .coverage_started_at =
              WallClockTime{std::chrono::milliseconds{1'786'422'400'000}},
          .coverage_ended_at = clock.now(),
          .fields = {
              DiagnosticField{"hardware_model", "ThinkPad P14s Gen 5",
                              DiagnosticValueDisposition::retain},
              DiagnosticField{"catalog_schema", "catalog-schema-3",
                              DiagnosticValueDisposition::retain},
              DiagnosticField{"catalog_revision", "2026.08.11",
                              DiagnosticValueDisposition::retain},
              DiagnosticField{"catalog_release_state", "draft",
                              DiagnosticValueDisposition::retain},
              DiagnosticField{"catalog_identity", "local-trial",
                              DiagnosticValueDisposition::retain},
              DiagnosticField{"network_state", "connected",
                              DiagnosticValueDisposition::retain},
              DiagnosticField{"computer_name", "EXPORT-HOST-PRIVATE"},
          },
          .missing_facts = {
              MissingDiagnosticFact{
                  .fact = "module_stack",
                  .reason = "platform did not expose module stack",
              },
          },
          .sensitive_values = {"EXPORT-HOST-PRIVATE"},
      });

  return expect(command.persisted && failure.persisted && exported.produced,
                "the event chain and diagnostic export must succeed") &&
         expect(exported.file_count == 1,
                "one diagnostic request must produce exactly one file") &&
         expect(!exported.file_name.empty() && storage.exports().size() == 1 &&
                    storage.exports().front() == exported.file_bytes,
                "diagnostic export must write exactly one storage artifact") &&
         expect(exported.file_bytes.find("EXPORT-HOST-PRIVATE") ==
                    std::string::npos,
                "the export context must pass through the same redactor") &&
         expect(diagnostic_file_is_self_contained(exported.file_bytes,
                                                  correlation.value),
                "the exported file alone must reconstruct build, environment, "
                "event chain, failure, error, and last trusted state");
}

[[nodiscard]] bool verify_diagnostic_export_failure_is_explicit() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'405'500}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  storage.fail_next_export("injected export failure");
  auto const exported = log.export_diagnostic(DiagnosticContext{
      .coverage_started_at = clock.now(),
      .coverage_ended_at = clock.now(),
  });
  return expect(!exported.produced && exported.file_count == 0 &&
                    exported.file_bytes.empty() &&
                    exported.error == "injected export failure",
                "diagnostic export storage failures must be explicit");
}

[[nodiscard]] bool verify_local_file_storage_and_single_file_export() {
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto const root = std::filesystem::temp_directory_path() /
                    ("azzs-execution-log-contract-" + unique);
  struct Cleanup final {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{root};

  auto root_utf8 = [&] {
    auto const value = root.u8string();
    return std::string{reinterpret_cast<char const*>(value.data()),
                       value.size()};
  }();
  std::string subject_id{"S-1-5-21-4242"};
#ifdef _WIN32
  auto environment =
      azzs::adapters::windows::WindowsDeviceDataEnvironment::prepare(
          azzs::adapters::windows::DeviceDataEnvironmentOptions{
              .root_override_utf8 = root_utf8});
  if (!environment) {
    return expect(false,
                  "the Windows local log contract requires a prepared ACL root");
  }
  root_utf8 = environment.environment->root_utf8;
  subject_id = environment.environment->subject_id;
#endif

  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'405'750}}};
  LocalFileLogStorage first_storage{root_utf8, subject_id};
  LocalFileLogStorage second_storage{root_utf8, subject_id};
  StructuredExecutionLog first{first_storage, clock};
  StructuredExecutionLog second{second_storage, clock};
  auto const correlation = first.begin_correlation();
  auto const first_event = first.append(
      correlation,
      ExecutionEvent{.kind = ExecutionEventKind::user_command,
                     .component = "local-first",
                     .stage = "persist",
                     .result = ExecutionResult::succeeded});
  auto const second_event = second.append(
      correlation,
      ExecutionEvent{.kind = ExecutionEventKind::adapter_result,
                     .component = "local-second",
                     .stage = "persist",
                     .result = ExecutionResult::succeeded});
  auto const exported = second.export_diagnostic(DiagnosticContext{
      .workbench_build = "contract-build",
      .coverage_started_at = clock.now(),
      .coverage_ended_at = clock.now(),
  });

  std::ifstream stream{exported.file_name, std::ios::binary};
  std::string persisted{std::istreambuf_iterator<char>{stream}, {}};
  auto const log_path =
      root / "subjects" / subject_id / "logs" / "execution.log";
  std::ifstream log_stream{log_path, std::ios::binary};
  std::string log_bytes{std::istreambuf_iterator<char>{log_stream}, {}};
  std::error_code directory_error;
  auto const export_directory = root / "subjects" / subject_id / "exports";
  auto const export_count = static_cast<std::size_t>(std::distance(
      std::filesystem::directory_iterator{export_directory, directory_error},
      std::filesystem::directory_iterator{}));
  return expect(first_event.persisted && second_event.persisted &&
                    second_event.sequence == 2,
                "two local storage instances must share one durable order") &&
         expect(exported.produced && exported.file_count == 1 &&
                    !directory_error && export_count == 1,
                "local diagnostic export must create exactly one file") &&
         expect(persisted == exported.file_bytes &&
                    persisted.find("AZZS-DIAGNOSTIC\t1") !=
                        std::string_view::npos,
                "the returned diagnostic bytes must match the reread file") &&
         expect(log_bytes.find("local-first") != std::string_view::npos &&
                    log_bytes.find("local-second") != std::string_view::npos,
                "the local execution log must retain both instance events");
}

[[nodiscard]] bool verify_concurrent_instances_share_a_stable_total_order() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'406'000}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog first{storage, clock};
  StructuredExecutionLog second{storage, clock};

  constexpr std::size_t event_count = 32;
  std::mutex results_mutex;
  std::vector<azzs::application::ExecutionLogReceipt> receipts;
  std::vector<std::string> correlations;
  receipts.reserve(event_count);
  correlations.reserve(event_count);
  {
    std::vector<std::jthread> writers;
    writers.reserve(event_count);
    for (std::size_t index = 0; index < event_count; ++index) {
      writers.emplace_back([&, index] {
        auto& log = index % 2 == 0 ? first : second;
        auto const correlation = log.begin_correlation();
        auto const receipt = log.append(
            correlation,
            ExecutionEvent{
                .kind = ExecutionEventKind::state_transition,
                .component = "concurrent-writer",
                .stage = "event-" + std::to_string(index),
                .result = ExecutionResult::succeeded,
            });
        std::scoped_lock lock{results_mutex};
        correlations.push_back(correlation.value);
        receipts.push_back(receipt);
      });
    }
  }

  std::ranges::sort(receipts, {},
                    &azzs::application::ExecutionLogReceipt::sequence);
  std::set<std::string> const unique_correlations{correlations.begin(),
                                                   correlations.end()};
  bool ordered = receipts.size() == event_count;
  for (std::size_t index = 0; index < receipts.size(); ++index) {
    ordered &= receipts[index].persisted && receipts[index].segment == 1 &&
               receipts[index].sequence == index + 1;
  }

  return expect(ordered,
                "concurrent instances must allocate one gap-free event order") &&
         expect(unique_correlations.size() == event_count,
                "concurrent instances must allocate unique stable "
                "correlation identifiers");
}

[[nodiscard]] bool verify_unverified_publication_is_never_reported_as_durable() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'406'500}}};
  InMemoryLogStorage log_storage;
  StructuredExecutionLog log{log_storage, clock};
  auto const correlation = log.begin_correlation();
  log_storage.publish_unverified_on_next_replace(
      "published log could not be durably verified");
  auto const append = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "durability-check",
          .stage = "publish",
          .result = ExecutionResult::failed,
      });

  InMemoryLogStorage export_storage;
  StructuredExecutionLog export_log{export_storage, clock};
  export_storage.publish_unverified_on_next_export(
      "published diagnostic could not be durably verified");
  auto const exported = export_log.export_diagnostic(DiagnosticContext{
      .coverage_started_at = clock.now(),
      .coverage_ended_at = clock.now(),
  });

  return expect(!append.persisted && append.segment == 0 &&
                    append.sequence == 0 &&
                    append.error ==
                        "published log could not be durably verified",
                "a published but unverified log revision is not durable") &&
         expect(log_storage.bytes().find("durability-check") !=
                    std::string_view::npos,
                "the contract must cover a publication that became visible") &&
         expect(!exported.produced && exported.file_count == 0 &&
                    exported.file_name.empty() && exported.file_bytes.empty() &&
                    exported.error ==
                        "published diagnostic could not be durably verified",
                "a published but unverified diagnostic is not produced") &&
         expect(export_storage.exports().size() == 1,
                "the export contract must cover an unverified visible artifact");
}

[[nodiscard]] bool
verify_capacity_exhaustion_suppresses_noncritical_events_and_recovers() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'406'750}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();
  auto const before_exhaustion = storage.bytes();

  storage.set_capacity_exhausted(true, "simulated log volume exhausted");
  auto const first_noncritical = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "debug-detail-one",
          .stage = "trace",
          .result = ExecutionResult::succeeded,
          .criticality = ExecutionLogCriticality::noncritical,
      });
  auto const second_noncritical = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "debug-detail-two",
          .stage = "trace",
          .result = ExecutionResult::succeeded,
          .criticality = ExecutionLogCriticality::noncritical,
      });
  auto const unavailable_critical = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "install-batch",
          .stage = "durable-transition",
          .result = ExecutionResult::started,
          .criticality = ExecutionLogCriticality::noncritical,
      });
  auto const during_exhaustion = log.snapshot();
  auto const during_exhaustion_export = log.export_diagnostic(DiagnosticContext{
      .coverage_started_at = clock.now(),
      .coverage_ended_at = clock.now(),
  });
  auto const revisions_after_suppression = storage.revisions().size();
  auto const bytes_after_critical_failure = storage.bytes();

  storage.set_capacity_exhausted(false);
  storage.fail_next_replace_due_to_capacity(
      "recovery annotation did not fit before the critical transition");
  auto const recovered_critical = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "install-batch",
          .stage = "durable-transition",
          .result = ExecutionResult::succeeded,
          .criticality = ExecutionLogCriticality::noncritical,
      });
  auto const resumed_noncritical = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "debug-detail-after-recovery",
          .stage = "trace",
          .result = ExecutionResult::succeeded,
          .criticality = ExecutionLogCriticality::noncritical,
      });
  auto const after_recovery = log.snapshot();
  auto const bytes = storage.bytes();

  return expect(first_noncritical.suppressed && !first_noncritical.persisted &&
                    first_noncritical.capacity_state ==
                        ExecutionLogCapacityState::space_exhausted &&
                    first_noncritical.noncritical_dropped_count == 1,
                "space exhaustion must suppress the first noncritical event") &&
         expect(second_noncritical.suppressed &&
                    second_noncritical.noncritical_dropped_count == 2 &&
                    revisions_after_suppression == 1,
                "space exhaustion must keep noncritical detail out of durable bytes") &&
         expect(!unavailable_critical.suppressed &&
                    !unavailable_critical.persisted &&
                    unavailable_critical.capacity_state ==
                        ExecutionLogCapacityState::space_exhausted &&
                    bytes_after_critical_failure == before_exhaustion,
                "critical transitions must remain write-through and never be silently dropped") &&
         expect(during_exhaustion.available &&
                    during_exhaustion.capacity_state ==
                        ExecutionLogCapacityState::space_exhausted &&
                    during_exhaustion.noncritical_dropped_count == 2 &&
                    during_exhaustion.coverage_gap_count == 0,
                "the snapshot must expose unresolved capacity loss") &&
         expect(during_exhaustion_export.produced &&
                    !during_exhaustion_export.complete &&
                    during_exhaustion_export.file_bytes.find(
                        "LOG_CAPACITY_STATE\tspace_exhausted") !=
                        std::string::npos &&
                    during_exhaustion_export.file_bytes.find(
                        "NONCRITICAL_DROPPED_COUNT\t2") !=
                        std::string::npos &&
                    during_exhaustion_export.file_bytes.find(
                        "NOT_OBTAINED\texecution_log_coverage\t2 noncritical events were suppressed while storage capacity was exhausted") !=
                        std::string::npos,
                "an incomplete export must carry the unresolved capacity coverage gap") &&
         expect(recovered_critical.persisted && !recovered_critical.suppressed &&
                    recovered_critical.capacity_state ==
                        ExecutionLogCapacityState::space_exhausted &&
                    recovered_critical.noncritical_dropped_count == 2 &&
                    recovered_critical.error ==
                        "recovery annotation did not fit before the critical transition" &&
                    resumed_noncritical.persisted &&
                    after_recovery.capacity_state ==
                        ExecutionLogCapacityState::available &&
                    after_recovery.noncritical_dropped_count == 2 &&
                    after_recovery.coverage_gap_count == 1,
                "the first recovered write must durably record the dropped coverage before resuming detail") &&
         expect(bytes.find("debug-detail-one") == std::string::npos &&
                    bytes.find("debug-detail-two") == std::string::npos &&
                    bytes.find("capacity-recovery") != std::string::npos &&
                    bytes.find("noncritical_dropped_count=2") !=
                        std::string::npos,
                "the durable recovery record must carry exact drop statistics");
}

#ifdef _WIN32
[[nodiscard]] bool
verify_windows_quota_exhaustion_is_classified_as_capacity_pressure() {
  return expect(
             classify_windows_log_storage_write_failure(
                 ERROR_DISK_QUOTA_EXCEEDED) ==
                 azzs::adapters::infrastructure::LogStorageWriteFailure::
                     capacity_exhausted,
             "Windows quota exhaustion must enter the noncritical suppression path") &&
         expect(
             classify_windows_log_storage_write_failure(ERROR_FILE_NOT_FOUND) ==
                 azzs::adapters::infrastructure::LogStorageWriteFailure::none,
             "unrelated Windows failures must not be misclassified as capacity pressure");
}
#endif

[[nodiscard]] bool
verify_diagnostic_context_facts_mark_absence_and_redact_dynamic_text() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'406'875}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};

  auto const exported = log.export_diagnostic(DiagnosticContext{
      .workbench_build = "1.0.0+facts",
      .release_form = "portable",
      .process_architecture = "x64",
      .package_architecture = "x64",
      .windows_version = "10.0.26200",
      .language = "zh-CN",
      .timezone = "Asia/Shanghai",
      .coverage_started_at = clock.now(),
      .coverage_ended_at = clock.now(),
      .frozen_directory_identity = DiagnosticFact{
          .value = "frozen-catalog-2026.08.15",
          .disposition = DiagnosticValueDisposition::retain,
      },
      .directory_application_association = DiagnosticFact{
          .unavailable_reason =
              "association source omitted SSID=Private Wifi With Spaces",
      },
      .directory_load_result = DiagnosticFact{
          .value = "loaded-with-warnings",
          .disposition = DiagnosticValueDisposition::retain,
      },
      .batch_plan = DiagnosticFact{
          .value = "software-batch-42",
          .disposition = DiagnosticValueDisposition::retain,
      },
      .debug_log_coverage = DiagnosticFact{
          .value = "Computer Name=HOST-DIAGNOSTIC-PRIVATE",
          .disposition = DiagnosticValueDisposition::retain,
      },
  });

  return expect(exported.produced && !exported.complete &&
                    exported.missing_fact_count >= 2,
                "missing owned diagnostic facts must make an export incomplete") &&
         expect(exported.file_bytes.find(
                    "FROZEN_DIRECTORY_IDENTITY\tfrozen-catalog-2026.08.15") !=
                        std::string::npos &&
                    exported.file_bytes.find(
                        "DIRECTORY_LOAD_RESULT\tloaded-with-warnings") !=
                        std::string::npos &&
                    exported.file_bytes.find("BATCH_PLAN\tsoftware-batch-42") !=
                        std::string::npos,
                "the export must carry retained directory and batch facts") &&
         expect(exported.file_bytes.find(
                    "NOT_OBTAINED\tdirectory_application_association\t") !=
                        std::string::npos &&
                    exported.file_bytes.find(
                        "NOT_OBTAINED\tdirectory_release_result\tthe directory release result owner did not provide it") !=
                        std::string::npos,
                "unobtained facts must use explicit stable diagnostic records") &&
         expect(exported.file_bytes.find("Private Wifi With Spaces") ==
                        std::string::npos &&
                    exported.file_bytes.find("HOST-DIAGNOSTIC-PRIVATE") ==
                        std::string::npos &&
                    exported.file_bytes.find("[redacted]") != std::string::npos,
                "diagnostic facts and unavailable reasons must share centralized redaction");
}

[[nodiscard]] bool verify_storage_failure_is_explicit_and_non_destructive() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'407'000}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const correlation = log.begin_correlation();
  auto const before = storage.bytes();
  storage.fail_next_replace("simulated disk full");

  auto const failed = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "download",
          .stage = "persist-result",
          .result = ExecutionResult::failed,
      });
  auto const after_failure = storage.bytes();
  auto const recovered = log.append(
      correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::coverage_gap,
          .component = "execution-log",
          .stage = "flush",
          .result = ExecutionResult::failed,
          .coverage_gap = CoverageGap{
              .kind = CoverageGapKind::flush_failed,
              .reason = "prior event could not be persisted: simulated disk "
                        "full",
          },
      });

  return expect(!failed.persisted &&
                    failed.error == "simulated disk full",
                "a storage failure must be returned explicitly") &&
         expect(after_failure == before,
                "a failed log write must not partially change durable bytes") &&
         expect(recovered.persisted && recovered.sequence == 1,
                "the next successful write must retain a gap-free durable "
                "sequence") &&
         expect(storage.bytes().find("flush_failed") != std::string::npos,
                "a detectable prior flush failure must be recordable as a "
                "coverage gap");
}

[[nodiscard]] bool verify_read_projection_is_redacted_and_segmented() {
  FixedClock clock{WallClockTime{std::chrono::milliseconds{1'786'422'399'000}}};
  InMemoryLogStorage storage;
  StructuredExecutionLog log{storage, clock};
  auto const initial_correlation = log.begin_correlation();
  auto const initial = log.append(
      initial_correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "installer",
          .stage = "completion",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .source = "executor",
              .code = 23,
              .message = "Authorization: Bearer projection-secret",
          },
          .fields = {
              DiagnosticField{"safe_context", "controlled-profile",
                              DiagnosticValueDisposition::retain},
              DiagnosticField{"password", "projection-secret",
                              DiagnosticValueDisposition::retain},
          },
          .sensitive_values = {"projection-secret"},
      });
  auto const code_only = log.append(
      initial_correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::adapter_result,
          .component = "installer",
          .stage = "error-code-only",
          .result = ExecutionResult::failed,
          .error = ExecutionError{
              .code = -2'147'024'891,
          },
      });
  auto const before_clear = log.snapshot();
  auto const clear = log.clear();
  auto const next_correlation = log.begin_correlation();
  auto const next = log.append(
      next_correlation,
      ExecutionEvent{
          .kind = ExecutionEventKind::state_transition,
          .component = "batch",
          .stage = "new-segment",
          .result = ExecutionResult::started,
      });
  auto const after_clear = log.snapshot();

  return expect(initial.persisted && before_clear.available &&
                    before_clear.active_segment == 1 &&
                    before_clear.events.size() == 2,
                "the read projection must expose the active durable event") &&
         expect(before_clear.events.front().error.has_value() &&
                    before_clear.events.front().error->source == "executor" &&
                    before_clear.events.front().error->code == 23 &&
                    before_clear.events.front().error->message.find(
                        "projection-secret") == std::string::npos &&
                    before_clear.events.front().fields.size() == 2 &&
                    before_clear.events.front().fields[1].value == "[redacted]",
                "the read projection must retain redacted source, code, and fields") &&
         expect(code_only.persisted && before_clear.events[1].error.has_value() &&
                    before_clear.events[1].error->source.empty() &&
                    before_clear.events[1].error->code == -2'147'024'891 &&
                    before_clear.events[1].error->message.empty(),
                "the read projection must preserve an error code even without text") &&
         expect(clear.cleared && next.persisted && after_clear.available &&
                    after_clear.active_segment == 2 &&
                    after_clear.events.size() == 1 &&
                    after_clear.events.front().sequence == 1 &&
                    after_clear.events.front().stage == "new-segment",
                "clearing must expose only the new log segment to readers");
}

}  // namespace

int main() {
  if (!verify_unknown_and_corrupt_formats_are_preserved() ||
      !verify_diagnostic_export_is_read_only_during_gaps() ||
      !verify_first_event_is_persisted() ||
      !verify_semantic_order_and_signed_error_contract() ||
      !verify_failure_keeps_raw_error_and_trusted_state() ||
      !verify_coverage_gaps_are_explicit() ||
      !verify_clear_commits_cutoff_before_new_segment() ||
      !verify_interrupted_clear_recovers_before_next_event() ||
      !verify_sensitive_values_never_reach_persistent_bytes() ||
      !verify_raw_identity_labels_are_redacted() ||
      !verify_read_errors_fail_closed_and_export_gaps() ||
      !verify_user_paths_with_spaces_are_redacted() ||
      !verify_central_redaction_and_correlation_validation() ||
      !verify_redaction_defaults_and_stable_event_tokens() ||
      !verify_single_file_export_is_self_contained() ||
      !verify_diagnostic_export_failure_is_explicit() ||
      !verify_capacity_exhaustion_suppresses_noncritical_events_and_recovers() ||
#ifdef _WIN32
      !verify_windows_quota_exhaustion_is_classified_as_capacity_pressure() ||
#endif
      !verify_diagnostic_context_facts_mark_absence_and_redact_dynamic_text() ||
      !verify_local_file_storage_and_single_file_export() ||
      !verify_concurrent_instances_share_a_stable_total_order() ||
      !verify_unverified_publication_is_never_reported_as_durable() ||
      !verify_storage_failure_is_explicit_and_non_destructive() ||
      !verify_read_projection_is_redacted_and_segmented()) {
    return EXIT_FAILURE;
  }

  std::cout << "execution log contract passed\n";
  return EXIT_SUCCESS;
}
