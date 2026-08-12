#include "azzs/application/architecture_selection.hpp"

#include <string>
#include <utility>

namespace azzs::application::architecture_selection {
namespace {

[[nodiscard]] ExecutionResult event_result(
    selection_domain::SelectionStatus status) noexcept {
  switch (status) {
    case selection_domain::SelectionStatus::selected_native:
    case selection_domain::SelectionStatus::selected_architecture_independent:
    case selection_domain::SelectionStatus::selected_compatibility_fallback:
      return ExecutionResult::succeeded;
    case selection_domain::SelectionStatus::fallback_confirmation_required:
    case selection_domain::SelectionStatus::detection_failed_paused:
    case selection_domain::SelectionStatus::architecture_changed:
      return ExecutionResult::unknown;
    case selection_domain::SelectionStatus::fallback_refused:
      return ExecutionResult::cancelled;
    case selection_domain::SelectionStatus::incompatible:
    case selection_domain::SelectionStatus::package_information_unavailable:
      return ExecutionResult::failed;
  }
  return ExecutionResult::unknown;
}

[[nodiscard]] bool package_matches_system(
    selection_domain::PackageArchitecture package,
    domain::SystemArchitecture system) noexcept {
  return package == selection_domain::PackageArchitecture::architecture_independent ||
         (system == domain::SystemArchitecture::x64 &&
          package == selection_domain::PackageArchitecture::x64) ||
         (system == domain::SystemArchitecture::arm64 &&
          (package == selection_domain::PackageArchitecture::arm64 ||
           package == selection_domain::PackageArchitecture::x64));
}

[[nodiscard]] bool x64_fallback_requires_confirmation(
    selection_domain::PackageArchitecture package,
    domain::SystemArchitecture system,
    selection_domain::ArchitecturePreference preference) noexcept {
  return system == domain::SystemArchitecture::arm64 &&
         package == selection_domain::PackageArchitecture::x64 &&
         preference == selection_domain::ArchitecturePreference::
                           prefer_arm64_prompt_fallback;
}

}  // namespace

ArchitectureSelectionLifecycle::ArchitectureSelectionLifecycle(
    PlatformInfo const& platform, ExecutionLog& log,
    selection_domain::ArchitecturePreference preference) noexcept
    : platform_(platform), log_(log), preference_(preference) {}

void ArchitectureSelectionLifecycle::set_preference(
    selection_domain::ArchitecturePreference preference) noexcept {
  preference_ = preference;
}

ArchitectureRecheck ArchitectureSelectionLifecycle::start() {
  return observe("startup");
}

ArchitectureRecheck ArchitectureSelectionLifecycle::restore() {
  return observe("restore");
}

ArchitectureRecheck ArchitectureSelectionLifecycle::observe(
    std::string_view trigger) {
  auto const previous = current_.value_or(
      selection_domain::ArchitectureObservation{});
  auto const current = selection_domain::ArchitectureObservation{
      .system = platform_.windows_architecture(),
      .id = next_observation_id_++,
  };
  auto const changed = selection_domain::architecture_changed(previous, current);
  current_ = current;
  auto const observation_result = current.detected() ? ExecutionResult::succeeded
                                                      : ExecutionResult::failed;
  log_event("architecture-observation", observation_result, current, {},
            std::nullopt,
            std::string{trigger} +
                (current.detected() ? "; observation=succeeded"
                                     : "; observation=failed"));
  if (changed) {
    log_event("architecture-changed", ExecutionResult::unknown, current,
              {}, std::nullopt,
              std::string{selection_domain::to_string(previous.system)} +
                  "->" + selection_domain::to_string(current.system) +
                  "; trigger=" + std::string{trigger});
  }
  return {.previous = previous, .current = current, .changed = changed};
}

selection_domain::SelectionResult
ArchitectureSelectionLifecycle::evaluate(SoftwarePackageRequest const& request) {
  if (!current_.has_value()) {
    static_cast<void>(observe("implicit-evaluate"));
  }
  return evaluate_with_observation(request, *current_, std::nullopt,
                                   "evaluate");
}

selection_domain::SelectionResult
ArchitectureSelectionLifecycle::evaluate_with_observation(
    SoftwarePackageRequest const& request,
    selection_domain::ArchitectureObservation observation,
    std::optional<selection_domain::PackageArchitecture> one_shot_preference,
    std::string_view trigger) {
  auto result = selection_domain::select_package(
      observation, preference_, request.software_id, request.candidates,
      one_shot_preference);
  if (result.requires_confirmation()) {
    pending_fallbacks_.insert_or_assign(
        request.software_id,
        PendingFallback{.request = request, .observation = observation});
  } else {
    pending_fallbacks_.erase(request.software_id);
  }
  log_event(selection_domain::to_string(result.status), event_result(result.status),
            observation, request.software_id,
            result.package.has_value()
                ? std::optional{result.package->architecture}
                : std::nullopt,
            std::string{trigger} + "; " + result.reason);
  return result;
}

selection_domain::SelectionResult
ArchitectureSelectionLifecycle::confirm_fallback(std::string_view software_id) {
  auto found = pending_fallbacks_.find(software_id);
  if (found == pending_fallbacks_.end()) {
    return {.software_id = std::string{software_id},
            .status = selection_domain::SelectionStatus::incompatible,
            .observation = current_.value_or(selection_domain::ArchitectureObservation{}),
            .reason = "no matching fallback confirmation is pending"};
  }
  auto pending = std::move(found->second);
  pending_fallbacks_.erase(found);
  auto const recheck = observe("fallback-confirmed");
  auto result = selection_domain::select_package(
      recheck.current, preference_, pending.request.software_id,
      pending.request.candidates);
  if (result.requires_confirmation()) {
    result = selection_domain::select_package(
        recheck.current, preference_, pending.request.software_id,
        pending.request.candidates, selection_domain::PackageArchitecture::x64);
  }
  log_event(selection_domain::to_string(result.status), event_result(result.status),
            recheck.current, pending.request.software_id,
            result.package.has_value()
                ? std::optional{result.package->architecture}
                : std::nullopt,
            "fallback-confirmed; " + result.reason);
  return result;
}

selection_domain::SelectionResult
ArchitectureSelectionLifecycle::refuse_fallback(std::string_view software_id) {
  auto found = pending_fallbacks_.find(software_id);
  if (found == pending_fallbacks_.end()) {
    return {.software_id = std::string{software_id},
            .status = selection_domain::SelectionStatus::incompatible,
            .observation = current_.value_or(selection_domain::ArchitectureObservation{}),
            .reason = "no matching fallback confirmation is pending"};
  }
  auto pending = std::move(found->second);
  pending_fallbacks_.erase(found);
  auto const recheck = observe("fallback-refused");
  auto const reevaluated = selection_domain::select_package(
      recheck.current, preference_, pending.request.software_id,
      pending.request.candidates);
  auto result = selection_domain::SelectionResult{
      .software_id = pending.request.software_id,
      .status = selection_domain::SelectionStatus::fallback_refused,
      .observation = recheck.current,
      .reason = "the user refused the x64 compatibility fallback for this item; "
                "current reevaluation: " +
                reevaluated.reason,
  };
  log_event("fallback-refused", ExecutionResult::cancelled, recheck.current,
            software_id, std::nullopt, result.reason);
  return result;
}

selection_domain::SelectionResult ArchitectureSelectionLifecycle::retry(
    SoftwarePackageRequest const& request,
    std::optional<selection_domain::PackageArchitecture> one_shot_preference) {
  auto const recheck = observe("retry");
  return evaluate_with_observation(request, recheck.current, one_shot_preference,
                                   "retry");
}

BatchArchitectureRecheck ArchitectureSelectionLifecycle::recheck_batch(
    selection_domain::ArchitectureObservation frozen,
    std::span<BatchPackageSnapshot const> pending_items) {
  auto const recheck = observe("batch-recheck");
  auto const frozen_changed =
      selection_domain::architecture_changed(frozen, recheck.current);
  if (!frozen.detected() || !recheck.current.detected() || frozen_changed) {
    batches_requiring_reassessment_.insert(frozen);
  }
  auto const reassessment_required =
      batches_requiring_reassessment_.contains(frozen);
  BatchArchitectureRecheck result{
      .observation = recheck,
      .changed = reassessment_required,
  };
  for (auto const& item : pending_items) {
    if (item.package.architecture ==
        selection_domain::PackageArchitecture::architecture_independent) {
      continue;
    }
    auto status = selection_domain::SelectionStatus::architecture_changed;
    std::string reason;
    if (!recheck.current.detected()) {
      status = selection_domain::SelectionStatus::detection_failed_paused;
      reason = "current system architecture could not be detected for a pending architecture-specific item";
    } else if (item.package.architecture ==
               selection_domain::PackageArchitecture::unknown) {
      status = selection_domain::SelectionStatus::package_information_unavailable;
      reason = "the frozen package architecture could not be determined";
    } else if (x64_fallback_requires_confirmation(item.package.architecture,
                                                  recheck.current.system,
                                                  preference_)) {
      status = selection_domain::SelectionStatus::fallback_confirmation_required;
      reason = "the frozen x64 package requires confirmation on the current ARM64 system";
    } else if (reassessment_required) {
      reason = "the frozen batch requires explicit reassessment after an architecture change or unavailable observation";
    } else if (!package_matches_system(item.package.architecture,
                                       recheck.current.system)) {
      reason = "the frozen package architecture no longer matches the current system";
    } else {
      continue;
    }
    {
      selection_domain::SelectionResult affected{
          .software_id = item.software_id,
          .status = status,
          .observation = recheck.current,
          .package = item.package,
          .reason = std::move(reason),
      };
      log_event(selection_domain::to_string(affected.status),
                event_result(affected.status),
                recheck.current, item.software_id, item.package.architecture,
                affected.reason);
      result.affected.push_back(std::move(affected));
    }
  }
  return result;
}

void ArchitectureSelectionLifecycle::log_event(
    std::string_view stage, ExecutionResult result,
    selection_domain::ArchitectureObservation observation,
    std::string_view software_id,
    std::optional<selection_domain::PackageArchitecture> package,
    std::string_view detail) {
  auto correlation = log_.begin_correlation();
  if (correlation.value.empty()) {
    return;
  }
  std::vector<DiagnosticField> fields{
      {.key = "system_architecture",
       .value = selection_domain::to_string(observation.system),
       .disposition = DiagnosticValueDisposition::retain},
      {.key = "architecture_observation_id",
       .value = std::to_string(observation.id),
       .disposition = DiagnosticValueDisposition::retain},
      {.key = "architecture_preference",
       .value = selection_domain::to_string(preference_),
       .disposition = DiagnosticValueDisposition::retain},
  };
  if (!software_id.empty()) {
    fields.push_back({.key = "software_id",
                      .value = std::string{software_id},
                      .disposition = DiagnosticValueDisposition::retain});
  }
  if (package.has_value()) {
    fields.push_back({.key = "package_architecture",
                      .value = selection_domain::to_string(*package),
                      .disposition = DiagnosticValueDisposition::retain});
  }
  if (!detail.empty()) {
    fields.push_back({.key = "architecture_detail",
                      .value = std::string{detail},
                      .disposition = DiagnosticValueDisposition::retain});
  }
  static_cast<void>(log_.append(
      correlation,
      ExecutionEvent{.kind = ExecutionEventKind::state_transition,
                     .component = "architecture-selection",
                     .stage = std::string{stage},
                     .result = result,
                     .fields = std::move(fields)}));
}

}  // namespace azzs::application::architecture_selection
