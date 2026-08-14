#include <cstdlib>
#include <iostream>
#include <string>

#include "azzs/application/history_and_logs.hpp"

namespace {

using azzs::application::HistoryAndLogsFilter;
using azzs::application::HistoryEntryKind;
using azzs::application::HistoryFactDisposition;
using azzs::application::HistoryFactProjection;
using azzs::application::HistoryTimelineKind;
using azzs::application::HistoryTimelineProjection;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "history and logs contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] bool stable_history_taxonomy_is_available() {
  bool passed = true;
  passed &= expect(
      std::string{azzs::application::to_string(
          HistoryEntryKind::installation_batch)} == "installation-batch",
      "installation batches must retain their stable history kind");
  passed &= expect(
      std::string{azzs::application::to_string(
          HistoryEntryKind::system_setting_apply)} == "system-setting-apply",
      "system setting operations must be independently addressable");
  passed &= expect(
      std::string{azzs::application::to_string(
          HistoryEntryKind::restart_resume)} == "restart-resume",
      "restart handoffs must be independently addressable");
  passed &= expect(
      std::string{azzs::application::to_string(
          HistoryEntryKind::application_update)} == "application-update",
      "workbench update observations must be independently addressable");
  return passed;
}

[[nodiscard]] bool missing_facts_and_timelines_are_explicit() {
  HistoryFactProjection missing{
      .key = "source.version",
      .disposition = HistoryFactDisposition::not_obtained,
      .reason = "the frozen source snapshot had no version",
  };
  HistoryTimelineProjection timeline{
      .kind = HistoryTimelineKind::coverage_gap,
      .state = "unknown",
      .detail = "durable coverage ended before this observation",
      .facts = {missing},
  };
  return expect(
      std::string{azzs::application::to_string(missing.disposition)} ==
          "not-obtained" &&
          std::string{azzs::application::to_string(timeline.kind)} ==
              "coverage-gap" &&
          timeline.facts.size() == 1 && timeline.facts.front().value.empty() &&
          !timeline.facts.front().reason.empty(),
      "a missing fact must remain distinct from an empty or guessed value");
}

[[nodiscard]] bool filters_keep_history_and_log_axes_separate() {
  HistoryAndLogsFilter filter{
      .query = "batch-42",
      .history_kind = HistoryEntryKind::software_optimization_batch,
      .event_kind = azzs::application::ExecutionEventKind::state_transition,
      .event_result = azzs::application::ExecutionResult::unknown,
      .correlation_id = "correlation-42",
  };
  return expect(
      filter.query == "batch-42" && filter.history_kind.has_value() &&
          filter.event_kind.has_value() && filter.event_result.has_value() &&
          filter.correlation_id == "correlation-42",
      "a history/log filter must retain independent history and event selectors");
}

}  // namespace

int main() {
  bool passed = true;
  passed &= stable_history_taxonomy_is_available();
  passed &= missing_facts_and_timelines_are_explicit();
  passed &= filters_keep_history_and_log_axes_separate();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
