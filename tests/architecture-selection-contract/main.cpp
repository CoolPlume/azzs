#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/architecture_selection.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/platform_info.hpp"
#include "azzs/domain/architecture_selection.hpp"

namespace {

namespace domain_selection = azzs::domain::architecture_selection;
namespace app_selection = azzs::application::architecture_selection;
using azzs::application::CorrelationId;
using azzs::application::DiagnosticContext;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogReceipt;
using azzs::application::DiagnosticExportReceipt;
using azzs::domain::SystemArchitecture;

[[nodiscard]] bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "architecture selection contract failed: " << message << '\n';
  }
  return condition;
}

class SequencePlatformInfo final : public azzs::application::PlatformInfo {
 public:
  explicit SequencePlatformInfo(std::vector<SystemArchitecture> sequence)
      : sequence_(std::move(sequence)) {}

  [[nodiscard]] std::optional<azzs::domain::SystemVersion> windows_version()
      const override {
    return azzs::domain::SystemVersion{10, 0, 19045};
  }

  [[nodiscard]] SystemArchitecture windows_architecture() const override {
    auto const index = sequence_.empty()
                           ? 0U
                           : std::min(calls_, sequence_.size() - 1U);
    ++calls_;
    return sequence_.empty() ? SystemArchitecture::unknown : sequence_[index];
  }

  [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

 private:
  std::vector<SystemArchitecture> sequence_;
  mutable std::size_t calls_{0};
};

class RecordingLog final : public ExecutionLog {
 public:
  [[nodiscard]] CorrelationId begin_correlation() override {
    return CorrelationId{"architecture-contract-" + std::to_string(next_++)};
  }

  [[nodiscard]] ExecutionLogReceipt append(
      CorrelationId const&, ExecutionEvent const& event) override {
    events.push_back(event);
    return {.persisted = true, .segment = 1, .sequence = next_++};
  }

  [[nodiscard]] ExecutionLogClearReceipt clear() override {
    events.clear();
    return {.cleared = true};
  }

  [[nodiscard]] DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const&) override {
    return {.produced = true};
  }

  [[nodiscard]] bool has_stage(std::string_view stage) const {
    return std::ranges::any_of(events, [&](ExecutionEvent const& event) {
      return event.stage == stage;
    });
  }

  [[nodiscard]] bool has_stage_with_field(std::string_view stage,
                                          std::string_view key,
                                          std::string_view value) const {
    return std::ranges::any_of(events, [&](ExecutionEvent const& event) {
      return event.stage == stage &&
             std::ranges::any_of(event.fields, [&](auto const& field) {
               return field.key == key && field.value == value;
             });
    });
  }

  std::vector<ExecutionEvent> events;

 private:
  std::uint64_t next_{1};
};

[[nodiscard]] domain_selection::PackageCandidate package(
    std::string_view software_id, domain_selection::PackageArchitecture arch,
    std::string_view identity) {
  return {.software_id = std::string{software_id},
          .architecture = arch,
          .version = "1.0",
          .identity = std::string{identity}};
}

[[nodiscard]] bool verify_domain_decisions() {
  bool passed = true;
  std::vector candidates{
      package("native", domain_selection::PackageArchitecture::x64, "native-x64"),
      package("native", domain_selection::PackageArchitecture::arm64, "native-arm64"),
      package("fallback", domain_selection::PackageArchitecture::x64, "fallback-x64"),
      package("independent", domain_selection::PackageArchitecture::architecture_independent,
              "independent"),
      package("arm-only", domain_selection::PackageArchitecture::arm64, "arm-only"),
      package("unresolved", domain_selection::PackageArchitecture::unknown,
              "unresolved"),
  };

  auto find = [&](std::string_view id) {
    std::vector<domain_selection::PackageCandidate> result;
    for (auto const& candidate : candidates) {
      if (candidate.software_id == id) {
        result.push_back(candidate);
      }
    }
    return result;
  };

  auto native = find("native");
  auto const x64_native = domain_selection::select_package(
      {.system = SystemArchitecture::x64, .id = 1},
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback,
      "native", native);
  passed &= expect(x64_native.status == domain_selection::SelectionStatus::selected_native &&
                       x64_native.package->architecture ==
                           domain_selection::PackageArchitecture::x64,
                   "x64 must select only the x64 package");

  auto fallback = find("fallback");
  auto const prompt = domain_selection::select_package(
      {.system = SystemArchitecture::arm64, .id = 2},
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback,
      "fallback", fallback);
  passed &= expect(
      prompt.requires_confirmation() && prompt.package.has_value() &&
          prompt.package->architecture ==
              domain_selection::PackageArchitecture::x64,
      "ARM64 prompt preference must expose the x64 package for confirmation");
  auto const automatic = domain_selection::select_package(
      {.system = SystemArchitecture::arm64, .id = 3},
      domain_selection::ArchitecturePreference::prefer_arm64_auto_fallback,
      "fallback", fallback);
  passed &= expect(automatic.status ==
                       domain_selection::SelectionStatus::selected_compatibility_fallback,
                   "automatic preference must select x64 fallback");
  auto const x64_preferred = domain_selection::select_package(
      {.system = SystemArchitecture::arm64, .id = 31},
      domain_selection::ArchitecturePreference::prefer_x64, "native", native);
  passed &= expect(
      x64_preferred.status ==
          domain_selection::SelectionStatus::selected_compatibility_fallback &&
          x64_preferred.package->architecture ==
              domain_selection::PackageArchitecture::x64,
      "always prefer x64 must select x64 even when an ARM64 package exists");

  auto independent = find("independent");
  auto const unknown_independent = domain_selection::select_package(
      {.system = SystemArchitecture::unknown, .id = 4},
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback,
      "independent", independent);
  passed &= expect(
      unknown_independent.status ==
          domain_selection::SelectionStatus::selected_architecture_independent,
      "architecture-independent software must remain selectable when detection fails");

  auto arm_only = find("arm-only");
  auto const unknown_specific = domain_selection::select_package(
      {.system = SystemArchitecture::unknown, .id = 5},
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback,
      "arm-only", arm_only);
  passed &= expect(
      unknown_specific.status ==
          domain_selection::SelectionStatus::detection_failed_paused,
      "detection failure must pause only architecture-specific software");

  auto const x64_only = domain_selection::select_package(
      {.system = SystemArchitecture::x64, .id = 6},
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback,
      "arm-only", arm_only);
  passed &= expect(x64_only.status == domain_selection::SelectionStatus::incompatible,
                   "x64 must disable ARM64-only software");
  auto unresolved = find("unresolved");
  auto const unknown_package = domain_selection::select_package(
      {.system = SystemArchitecture::x64, .id = 7},
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback,
      "unresolved", unresolved);
  passed &= expect(
      unknown_package.status ==
          domain_selection::SelectionStatus::package_information_unavailable,
      "an unresolved package architecture must not be guessed as incompatible");
  return passed;
}

[[nodiscard]] bool verify_batch_rechecks_new_arm64_fallback() {
  bool passed = true;
  SequencePlatformInfo platform{
      {SystemArchitecture::x64, SystemArchitecture::arm64}};
  RecordingLog log;
  app_selection::ArchitectureSelectionLifecycle lifecycle{
      platform, log,
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback};
  auto const frozen = lifecycle.start().current;
  app_selection::BatchPackageSnapshot old_batch{
      .software_id = "editor",
      .package = package("editor", domain_selection::PackageArchitecture::x64,
                         "editor-x64"),
      .observation = frozen,
  };
  auto const recheck = lifecycle.recheck_batch(
      frozen,
      std::span<app_selection::BatchPackageSnapshot const>{&old_batch, 1});
  passed &= expect(
      recheck.changed && recheck.affected.size() == 1 &&
          recheck.affected.front().status ==
              domain_selection::SelectionStatus::fallback_confirmation_required &&
          recheck.affected.front().package == old_batch.package,
      "an x64 frozen package needs a new per-item confirmation after x64 to ARM64 change");
  return passed;
}

[[nodiscard]] bool verify_batch_recheck_stays_paused_after_uncertain_observation() {
  bool passed = true;
  SequencePlatformInfo platform{
      {SystemArchitecture::x64, SystemArchitecture::unknown,
       SystemArchitecture::x64}};
  RecordingLog log;
  app_selection::ArchitectureSelectionLifecycle lifecycle{
      platform, log,
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback};
  auto const frozen = lifecycle.start().current;
  app_selection::BatchPackageSnapshot old_batch{
      .software_id = "editor",
      .package = package("editor", domain_selection::PackageArchitecture::x64,
                         "editor-x64"),
      .observation = frozen,
  };
  auto const original_package = old_batch.package;
  auto const original_observation = old_batch.observation;

  auto const unavailable = lifecycle.recheck_batch(
      frozen,
      std::span<app_selection::BatchPackageSnapshot const>{&old_batch, 1});
  auto const recovered = lifecycle.recheck_batch(
      frozen,
      std::span<app_selection::BatchPackageSnapshot const>{&old_batch, 1});
  auto const recovered_requires_reassessment =
      recovered.changed && recovered.affected.size() == 1;

  passed &= expect(
      unavailable.changed && unavailable.affected.size() == 1 &&
          unavailable.affected.front().status ==
              domain_selection::SelectionStatus::detection_failed_paused,
      "an uncertain architecture observation must pause a frozen batch item");
  passed &= expect(
      recovered_requires_reassessment &&
          recovered.affected.front().status ==
              domain_selection::SelectionStatus::architecture_changed &&
          !recovered.affected.front().selected(),
      "x64 after an uncertain observation must require explicit batch reassessment instead of continuing automatically");
  passed &= expect(
      recovered_requires_reassessment &&
          recovered.affected.front().package == original_package &&
          old_batch.package == original_package &&
          old_batch.observation == original_observation,
      "architecture rechecks must not rewrite the frozen package or observation");
  passed &= expect(
      log.has_stage_with_field("detection-failed-paused", "software_id", "editor") &&
          log.has_stage_with_field("architecture-changed", "package_architecture",
                                   "x64"),
      "the paused batch and later reassessment requirement must be structured log events");
  return passed;
}

[[nodiscard]] bool verify_restore_uncertainty_invalidates_old_batch() {
  bool passed = true;
  SequencePlatformInfo platform{
      {SystemArchitecture::x64, SystemArchitecture::unknown,
       SystemArchitecture::x64}};
  RecordingLog log;
  app_selection::ArchitectureSelectionLifecycle lifecycle{
      platform, log,
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback};
  auto const frozen = lifecycle.start().current;
  app_selection::BatchPackageSnapshot old_batch{
      .software_id = "editor",
      .package = package("editor", domain_selection::PackageArchitecture::x64,
                         "editor-x64"),
      .observation = frozen,
  };
  auto const original_package = old_batch.package;
  auto const original_observation = old_batch.observation;

  auto const restored = lifecycle.restore();
  auto const recheck = lifecycle.recheck_batch(
      frozen,
      std::span<app_selection::BatchPackageSnapshot const>{&old_batch, 1});
  auto const requires_reassessment =
      recheck.changed && recheck.affected.size() == 1;

  passed &= expect(
      restored.current.system == SystemArchitecture::unknown,
      "restore must retain an unavailable architecture observation before batch recheck");
  passed &= expect(
      requires_reassessment &&
          recheck.affected.front().status ==
              domain_selection::SelectionStatus::architecture_changed &&
          !recheck.affected.front().selected(),
      "an old x64 batch must not continue automatically after restore observed unknown then x64");
  passed &= expect(
      requires_reassessment &&
          recheck.affected.front().package == original_package &&
          old_batch.package == original_package &&
          old_batch.observation == original_observation,
      "restore uncertainty must not rewrite the frozen package or installation snapshot");
  passed &= expect(
      log.has_stage_with_field("architecture-observation", "system_architecture",
                               "unknown") &&
          log.has_stage_with_field("architecture-changed", "software_id", "editor"),
      "restore uncertainty and required batch reassessment must be structured log events");
  return passed;
}

[[nodiscard]] bool verify_lifecycle_rechecks_and_scopes_refusal() {
  bool passed = true;
  SequencePlatformInfo platform{
      {SystemArchitecture::arm64, SystemArchitecture::arm64,
       SystemArchitecture::arm64, SystemArchitecture::arm64,
       SystemArchitecture::x64, SystemArchitecture::x64}};
  RecordingLog log;
  app_selection::ArchitectureSelectionLifecycle lifecycle{
      platform, log,
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback};

  auto const started = lifecycle.start();
  passed &= expect(started.current.system == SystemArchitecture::arm64,
                   "startup must observe the current architecture");
  app_selection::SoftwarePackageRequest request{
      .software_id = "editor",
      .candidates = {package("editor", domain_selection::PackageArchitecture::x64,
                              "editor-x64")},
  };
  auto pending = lifecycle.evaluate(request);
  passed &= expect(pending.requires_confirmation(),
                   "ARM64 fallback must be confirmed per software item");
  auto refused = lifecycle.refuse_fallback("editor");
  passed &= expect(refused.status == domain_selection::SelectionStatus::fallback_refused,
                   "refusal must skip only the current item");
  auto retry = lifecycle.retry(request, domain_selection::PackageArchitecture::x64);
  passed &= expect(retry.status ==
                       domain_selection::SelectionStatus::selected_compatibility_fallback,
                   "retry must allow a one-shot x64 choice after refusal");
  passed &= expect(platform.calls() == 3,
                   "retry must re-observe instead of reusing the old observation");

  auto confirmed_request = request;
  confirmed_request.software_id = "viewer";
  confirmed_request.candidates.front().software_id = "viewer";
  auto confirmation = lifecycle.evaluate(confirmed_request);
  passed &= expect(confirmation.requires_confirmation(),
                   "a second item must retain its own fallback confirmation");
  auto confirmed = lifecycle.confirm_fallback("viewer");
  passed &= expect(
      confirmed.status ==
          domain_selection::SelectionStatus::selected_compatibility_fallback,
      "confirming a fallback must select x64 only for that pending item");

  auto const recovered = lifecycle.restore();
  passed &= expect(recovered.changed &&
                       recovered.current.system == SystemArchitecture::x64,
                   "recovery must recheck and report architecture changes");
  passed &= expect(log.has_stage("architecture-observation") &&
                       log.has_stage("architecture-changed") &&
                       log.has_stage("fallback-refused"),
                   "architecture decisions and changes must be detailed in the log");

  app_selection::BatchPackageSnapshot old_batch{
      .software_id = "editor",
      .package = package("editor", domain_selection::PackageArchitecture::arm64,
                         "editor-arm64"),
      .observation = started.current,
  };
  auto const changed = lifecycle.recheck_batch(
      started.current,
      std::span<app_selection::BatchPackageSnapshot const>{&old_batch, 1});
  passed &= expect(changed.changed && changed.affected.size() == 1 &&
                       changed.affected.front().status ==
                           domain_selection::SelectionStatus::architecture_changed,
                   "an architecture change must pause the old pending item without rewriting it");
  return passed;
}

[[nodiscard]] bool verify_confirmation_rechecks_and_batch_pause() {
  bool passed = true;
  app_selection::SoftwarePackageRequest request{
      .software_id = "editor",
      .candidates = {package("editor", domain_selection::PackageArchitecture::x64,
                              "editor-x64")},
  };

  {
    SequencePlatformInfo platform{
        {SystemArchitecture::arm64, SystemArchitecture::unknown}};
    RecordingLog log;
    app_selection::ArchitectureSelectionLifecycle lifecycle{
        platform, log,
        domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback};
    auto const started = lifecycle.start();
    static_cast<void>(lifecycle.evaluate(request));
    auto const recovered = lifecycle.restore();
    passed &= expect(recovered.current.system == SystemArchitecture::unknown,
                     "confirmation must not reuse a stale architecture after recovery");
    auto const confirmed = lifecycle.confirm_fallback("editor");
    passed &= expect(
        confirmed.status ==
            domain_selection::SelectionStatus::detection_failed_paused,
        "confirming a stale fallback must pause when architecture detection fails");

    app_selection::BatchPackageSnapshot old_batch{
        .software_id = "editor",
        .package = package("editor", domain_selection::PackageArchitecture::arm64,
                           "editor-arm64"),
        .observation = started.current,
    };
    auto const batch = lifecycle.recheck_batch(
        started.current,
        std::span<app_selection::BatchPackageSnapshot const>{&old_batch, 1});
    passed &= expect(
        batch.affected.size() == 1 &&
            batch.affected.front().status ==
                domain_selection::SelectionStatus::detection_failed_paused,
        "batch recheck must pause architecture-specific items on detection failure");
  }

  {
    SequencePlatformInfo platform{
        {SystemArchitecture::arm64, SystemArchitecture::x64}};
    RecordingLog log;
    app_selection::ArchitectureSelectionLifecycle lifecycle{
        platform, log,
        domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback};
    static_cast<void>(lifecycle.start());
    static_cast<void>(lifecycle.evaluate(request));
    auto const refused = lifecycle.refuse_fallback("editor");
    passed &= expect(
        refused.status == domain_selection::SelectionStatus::fallback_refused &&
            !refused.package.has_value(),
        "refusing a stale fallback must skip only the current item even when a fresh observation is native");
  }

  return passed;
}

[[nodiscard]] bool verify_current_preference_and_one_shot_scope() {
  bool passed = true;
  SequencePlatformInfo platform{
      {SystemArchitecture::arm64, SystemArchitecture::arm64,
       SystemArchitecture::arm64}};
  RecordingLog log;
  app_selection::ArchitectureSelectionLifecycle lifecycle{
      platform, log,
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback};
  app_selection::SoftwarePackageRequest request{
      .software_id = "editor",
      .candidates = {package("editor", domain_selection::PackageArchitecture::x64,
                              "editor-x64")},
  };
  static_cast<void>(lifecycle.start());
  static_cast<void>(lifecycle.evaluate(request));
  lifecycle.set_preference(
      domain_selection::ArchitecturePreference::prefer_arm64_auto_fallback);
  auto const retry = lifecycle.retry(request);
  passed &= expect(
      retry.status ==
          domain_selection::SelectionStatus::selected_compatibility_fallback,
      "retry must use the current architecture preference");

  lifecycle.set_preference(
      domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback);
  auto const one_shot = lifecycle.retry(
      request, domain_selection::PackageArchitecture::x64);
  passed &= expect(
      one_shot.status ==
          domain_selection::SelectionStatus::selected_compatibility_fallback,
      "one-shot x64 selection must work during retry");
  auto const after_one_shot = lifecycle.evaluate(request);
  passed &= expect(after_one_shot.requires_confirmation(),
                   "one-shot retry selection must not change the global preference");
  return passed;
}

[[nodiscard]] bool verify_preference_change_refusal_skips_the_item() {
  bool passed = true;
  app_selection::SoftwarePackageRequest request{
      .software_id = "editor",
      .candidates = {package("editor", domain_selection::PackageArchitecture::x64,
                              "editor-x64")},
  };

  for (auto const preference : {
           domain_selection::ArchitecturePreference::prefer_arm64_auto_fallback,
           domain_selection::ArchitecturePreference::prefer_x64,
       }) {
    SequencePlatformInfo platform{
        {SystemArchitecture::arm64, SystemArchitecture::arm64}};
    RecordingLog log;
    app_selection::ArchitectureSelectionLifecycle lifecycle{
        platform, log,
        domain_selection::ArchitecturePreference::prefer_arm64_prompt_fallback};
    static_cast<void>(lifecycle.start());
    auto const pending = lifecycle.evaluate(request);
    lifecycle.set_preference(preference);
    auto const refused = lifecycle.refuse_fallback("editor");

    passed &= expect(
        pending.requires_confirmation() &&
            refused.status == domain_selection::SelectionStatus::fallback_refused &&
            !refused.package.has_value() && !refused.selected() &&
            refused.observation.system == SystemArchitecture::arm64,
        "refusing after a preference change must skip only the pending item without selecting x64");
    passed &= expect(
        log.has_stage_with_field(
            "fallback-refused", "architecture_preference",
            domain_selection::to_string(preference)),
        "fallback refusal must log the preference used for its fresh reassessment");
  }
  return passed;
}

}  // namespace

int main() {
  auto passed = verify_domain_decisions();
  passed &= verify_lifecycle_rechecks_and_scopes_refusal();
  passed &= verify_confirmation_rechecks_and_batch_pause();
  passed &= verify_current_preference_and_one_shot_scope();
  passed &= verify_batch_rechecks_new_arm64_fallback();
  passed &= verify_batch_recheck_stays_paused_after_uncertain_observation();
  passed &= verify_restore_uncertainty_invalidates_old_batch();
  passed &= verify_preference_change_refusal_skips_the_item();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "architecture selection contract passed\n";
  return EXIT_SUCCESS;
}
