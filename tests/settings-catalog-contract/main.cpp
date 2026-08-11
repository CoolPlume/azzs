#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/adapters/infrastructure/settings_catalog_file_adapter.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/settings_catalog/settings_catalog.hpp"
#include "azzs/settings_catalog/settings_catalog_lifecycle.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace catalog_app = azzs::application::settings_catalog;
namespace catalog_domain = azzs::domain::settings_catalog;

using azzs::adapters::infrastructure::SettingsCatalogFileAdapter;
using azzs::application::CorrelationId;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogReceipt;
using azzs::application::OperationIdentity;
using azzs::application::SharedOperationOccupancy;
using azzs::domain::AggregateId;
using azzs::domain::DeviceState;
using azzs::domain::StateBytes;
using azzs::domain::StateKey;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryOperationOccupancyStorage;
using azzs::testing::InMemoryStateFileSystem;
using azzs::testing::SequenceLeaseTokenSource;
using azzs::testing::StateFileOperation;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "settings catalog contract failed: " << message << '\n';
  }
  return condition;
}

class RecordingExecutionLog final : public ExecutionLog {
 public:
  [[nodiscard]] CorrelationId begin_correlation() override {
    return CorrelationId{"settings-catalog-correlation-" +
                         std::to_string(next_correlation_++)};
  }

  [[nodiscard]] ExecutionLogReceipt append(
      CorrelationId const&, ExecutionEvent const& event) override {
    events_.push_back(event);
    return {.persisted = true,
            .segment = 1,
            .sequence = events_.size()};
  }

  [[nodiscard]] ExecutionLogClearReceipt clear() override {
    events_.clear();
    return {.cleared = true};
  }

  [[nodiscard]] DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const&) override {
    return {.produced = true,
            .file_count = 1,
            .file_name = "settings-catalog-contract.azzsdiag"};
  }

  [[nodiscard]] bool contains_stage(std::string_view stage) const {
    return std::ranges::any_of(events_, [&](ExecutionEvent const& event) {
      return event.component == "settings-catalog" && event.stage == stage;
    });
  }

  [[nodiscard]] bool contains_import_event(
      std::string_view stage, azzs::application::ExecutionResult result,
      std::string_view source_path, std::string_view import_result) const {
    return std::ranges::any_of(events_, [&](ExecutionEvent const& event) {
      if (event.component != "settings-catalog" || event.stage != stage ||
          event.result != result) {
        return false;
      }
      auto has_field = [&](std::string_view key, std::string_view value) {
        return std::ranges::any_of(event.fields, [&](auto const& field) {
          return field.key == key && field.value == value;
        });
      };
      return has_field("source_type", "local-file") &&
             has_field("source_path", source_path) &&
             has_field("import_result", import_result);
    });
  }

  [[nodiscard]] bool contains_text(std::string_view text) const {
    return std::ranges::any_of(events_, [&](ExecutionEvent const& event) {
      if (event.error.has_value() &&
          event.error->message.find(text) != std::string::npos) {
        return true;
      }
      return std::ranges::any_of(event.fields, [&](auto const& field) {
        return field.value.find(text) != std::string::npos;
      });
    });
  }

 private:
  std::uint64_t next_correlation_{1};
  std::vector<ExecutionEvent> events_;
};

class MutableCatalogImportAuthorization final
    : public catalog_app::SettingsCatalogImportAuthorization {
 public:
  [[nodiscard]] bool debug_import_allowed() const noexcept override {
    return enabled_;
  }

  void set_enabled(bool enabled) noexcept { enabled_ = enabled; }

 private:
  bool enabled_{false};
};

[[nodiscard]] catalog_domain::SupportedCapabilities capabilities() {
  return {
      .apply = {"settings.apply-value"},
      .detect = {"settings.detect-value"},
      .recover = {"settings.restore-recorded-value"},
  };
}

[[nodiscard]] catalog_domain::SettingDefinition setting(
    std::string id, std::string name, std::string identity,
    bool default_selected) {
  return {
      .id = catalog_domain::StableId{std::move(id)},
      .display_name = std::move(name),
      .description = "A controlled settings catalog contract fixture.",
      .source_url = "https://example.invalid/settings-reference",
      .known_windows_range = {
          .minimum = catalog_domain::WindowsVersion{
              .generation = catalog_domain::WindowsGeneration::windows_10,
              .feature_update_year = 22,
              .feature_update_half = 2},
          .maximum = catalog_domain::WindowsVersion{
              .generation = catalog_domain::WindowsGeneration::windows_11,
              .feature_update_year = 25,
              .feature_update_half = 2},
      },
      .default_selected = default_selected,
      .risk = catalog_domain::SettingRiskLevel::low,
      .force_attempt_rule = catalog_domain::ForceAttemptRule::prohibited,
      .recovery_requirement =
          catalog_domain::RecoveryRequirement::restore_record_required,
      .restart_requirement =
          catalog_domain::RestartRequirement::explorer,
      .semantics = {
          .identity = std::move(identity),
          .apply_capability = "settings.apply-value",
          .detect_capability = "settings.detect-value",
          .recover_capability = "settings.restore-recorded-value",
      },
  };
}

[[nodiscard]] catalog_domain::OptimizationPlan plan(
    std::string second_setting,
    std::string plan_id = "plan.recommended") {
  return {
      .id = catalog_domain::StableId{std::move(plan_id)},
      .display_name = "Recommended optimization",
      .description = "A visible selection of existing settings.",
      .members = {
          {.setting_id = catalog_domain::StableId{"setting.alpha"},
           .order = 10,
           .default_selected = true},
          {.setting_id = catalog_domain::StableId{std::move(second_setting)},
           .order = 20,
           .default_selected = false},
      },
  };
}

[[nodiscard]] catalog_domain::SettingsCatalog base_catalog() {
  auto alpha = setting("setting.alpha", "Alpha setting",
                       "windows.target.alpha", true);
  auto beta = setting("setting.beta", "Beta setting", "windows.target.beta",
                      false);
  beta.depends_on = {catalog_domain::StableId{"setting.alpha"}};
  beta.risk = catalog_domain::SettingRiskLevel::elevated;
  beta.force_attempt_rule =
      catalog_domain::ForceAttemptRule::allowed_with_explicit_confirmation;
  return {
      .revision = 1,
      .settings = {std::move(alpha), std::move(beta)},
      .plans = {plan("setting.beta")},
  };
}

[[nodiscard]] catalog_domain::SettingsCatalog updated_catalog() {
  auto alpha = setting("setting.alpha", "Alpha setting updated",
                       "windows.target.alpha", true);
  auto gamma = setting("setting.gamma", "Gamma setting",
                       "windows.target.gamma", false);
  gamma.depends_on = {catalog_domain::StableId{"setting.alpha"}};
  gamma.risk = catalog_domain::SettingRiskLevel::elevated;
  gamma.force_attempt_rule =
      catalog_domain::ForceAttemptRule::allowed_with_explicit_confirmation;
  return {
      .revision = 2,
      .settings = {std::move(alpha), std::move(gamma)},
      .plans = {plan("setting.gamma", "plan.recommended-v2")},
  };
}

[[nodiscard]] catalog_domain::SettingsCatalog third_catalog() {
  auto catalog = updated_catalog();
  catalog.revision = 3;
  catalog.settings[1].display_name = "Gamma setting updated";
  return catalog;
}

[[nodiscard]] StateBytes bytes(std::string_view text) {
  StateBytes result;
  result.reserve(text.size());
  for (auto character : text) {
    result.push_back(static_cast<std::byte>(character));
  }
  return result;
}

[[nodiscard]] DeviceState unrelated_state(std::string_view payload) {
  return DeviceState{
      .value = {.schema = 2,
                .minimum_reader = 1,
                .minimum_writer = 2,
                .payload = bytes(payload)},
  };
}

[[nodiscard]] std::string dash(
    std::optional<std::string> const& value) {
  return value.value_or("-");
}

[[nodiscard]] std::string version_text(
    std::optional<catalog_domain::WindowsVersion> const& version) {
  if (!version.has_value()) {
    return "-";
  }
  auto const generation =
      version->generation == catalog_domain::WindowsGeneration::windows_10
          ? 10
          : 11;
  return std::to_string(generation) + "-" +
         std::to_string(version->feature_update_year) + "h" +
         std::to_string(version->feature_update_half);
}

[[nodiscard]] std::string package_text(
    catalog_domain::SettingsCatalog const& catalog,
    std::string_view extra_record = {}) {
  std::ostringstream output;
  output << "AZZS_SETTINGS_CATALOG\t2\t" << catalog.schema_version << '\t'
         << catalog.revision << '\n';
  if (!extra_record.empty()) {
    output << extra_record << '\n';
  }
  for (auto const& item : catalog.settings) {
    auto const recovery =
        item.recovery_requirement ==
                catalog_domain::RecoveryRequirement::restore_record_required
            ? "restore"
            : "none";
    auto restart = "none";
    if (item.restart_requirement ==
        catalog_domain::RestartRequirement::explorer) {
      restart = "explorer";
    } else if (item.restart_requirement ==
               catalog_domain::RestartRequirement::windows) {
      restart = "windows";
    }
    output << "SETTING\t" << item.id.value << '\t' << item.display_name
           << '\t' << item.description << '\t' << dash(item.source_url)
           << '\t' << version_text(item.known_windows_range.minimum) << '\t'
           << version_text(item.known_windows_range.maximum) << '\t'
           << (item.default_selected ? 1 : 0) << '\t' << recovery << '\t'
           << restart << '\t' << item.semantics.identity << '\t'
           << item.semantics.apply_capability << '\t'
           << item.semantics.detect_capability << '\t'
           << dash(item.semantics.recover_capability) << '\t';
    if (item.depends_on.empty()) {
      output << '-';
    } else {
      for (std::size_t index = 0; index < item.depends_on.size(); ++index) {
        if (index != 0) {
          output << ',';
        }
        output << item.depends_on[index].value;
      }
    }
    output << '\t'
           << (item.risk == catalog_domain::SettingRiskLevel::low
                   ? "low"
                   : "elevated")
           << '\t'
           << (item.force_attempt_rule ==
                       catalog_domain::ForceAttemptRule::prohibited
                   ? "prohibited"
                   : "confirm-if-recoverable")
           << '\n';
  }
  for (auto const& item : catalog.plans) {
    output << "PLAN\t" << item.id.value << '\t' << item.display_name << '\t'
           << item.description << '\n';
    for (auto const& member : item.members) {
      output << "MEMBER\t" << item.id.value << '\t'
             << member.setting_id.value << '\t' << member.order << '\t'
             << (member.default_selected ? 1 : 0) << '\n';
    }
  }
  return std::move(output).str();
}

class TemporaryCatalogFile final {
 public:
  TemporaryCatalogFile(std::string name, std::string contents)
      : path_(std::filesystem::current_path() / std::move(name)) {
    std::ofstream output{path_, std::ios::binary | std::ios::trunc};
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
  }

  ~TemporaryCatalogFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

struct Fixture final {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'508'800'000}}};
  azzs::application::DeviceStateStore states{files, clock};
  SettingsCatalogFileAdapter adapter{states};
  RecordingExecutionLog log;
  MutableCatalogImportAuthorization import_authorization;
  InMemoryOperationOccupancyStorage occupancy_storage;
  SequenceLeaseTokenSource lease_tokens{"settings-catalog-lease-"};
  SharedOperationOccupancy occupancy{occupancy_storage, lease_tokens};
  catalog_app::SettingsCatalogLifecycle lifecycle{
      adapter, adapter, log, occupancy, import_authorization, capabilities()};
};

[[nodiscard]] bool stable_identity_lifecycle_contract() {
  bool passed = true;

  Fixture setting_fixture;
  if (setting_fixture.lifecycle.initialize_builtin(base_catalog()).status !=
      catalog_app::InitializeStatus::initialized) {
    return expect(false, "stable identity fixture must initialize");
  }
  auto retired = base_catalog();
  retired.revision = 2;
  retired.settings.erase(retired.settings.begin() + 1);
  retired.plans.clear();
  auto retired_preview = setting_fixture.lifecycle.prepare_update(retired);
  if (!retired_preview.prepared.has_value() ||
      setting_fixture.lifecycle.confirm(
          retired_preview.prepared->confirmation_token)
              .status != catalog_app::ConfirmStatus::committed) {
    return expect(false, "stable identity fixture must retire catalog items");
  }

  auto reused_in_third_revision = retired;
  reused_in_third_revision.revision = 3;
  reused_in_third_revision.settings.push_back(setting(
      "setting.beta", "Reused beta setting", "windows.target.reused", false));
  passed &= expect(
      setting_fixture.lifecycle.prepare_update(reused_in_third_revision)
              .status == catalog_app::PrepareStatus::rejected,
      "a retired setting id must not be reused by the third revision");

  auto third_without_reuse = retired;
  third_without_reuse.revision = 3;
  third_without_reuse.settings[0].display_name = "Alpha third revision";
  auto third_preview =
      setting_fixture.lifecycle.prepare_update(third_without_reuse);
  if (!third_preview.prepared.has_value() ||
      setting_fixture.lifecycle.confirm(
          third_preview.prepared->confirmation_token)
              .status != catalog_app::ConfirmStatus::committed) {
    return expect(false, "stable identity fixture must advance past N-1");
  }
  auto reused_after_previous_expired = third_without_reuse;
  reused_after_previous_expired.revision = 4;
  reused_after_previous_expired.settings.push_back(setting(
      "setting.beta", "Late reused beta setting", "windows.target.late", false));
  SettingsCatalogFileAdapter identity_restarted_adapter{
      setting_fixture.states};
  catalog_app::SettingsCatalogLifecycle identity_restarted{
      identity_restarted_adapter, identity_restarted_adapter,
      setting_fixture.log, setting_fixture.occupancy,
      setting_fixture.import_authorization, capabilities()};
  passed &= expect(
      identity_restarted.prepare_update(reused_after_previous_expired)
              .status == catalog_app::PrepareStatus::rejected,
      "a retired setting id must remain unavailable after restart and N-1 expiry");

  Fixture plan_fixture;
  if (plan_fixture.lifecycle.initialize_builtin(base_catalog()).status !=
      catalog_app::InitializeStatus::initialized) {
    return expect(false, "plan identity fixture must initialize");
  }
  auto plan_retired = base_catalog();
  plan_retired.revision = 2;
  plan_retired.plans.clear();
  auto plan_retired_preview =
      plan_fixture.lifecycle.prepare_update(plan_retired);
  if (!plan_retired_preview.prepared.has_value() ||
      plan_fixture.lifecycle.confirm(
          plan_retired_preview.prepared->confirmation_token)
              .status != catalog_app::ConfirmStatus::committed) {
    return expect(false, "plan identity fixture must retire its plan");
  }
  auto plan_reused = plan_retired;
  plan_reused.revision = 3;
  auto reassigned_plan = plan("setting.beta");
  reassigned_plan.members[0].order = 30;
  reassigned_plan.members[1].order = 10;
  plan_reused.plans.push_back(std::move(reassigned_plan));
  passed &= expect(
      plan_fixture.lifecycle.prepare_update(plan_reused).status ==
          catalog_app::PrepareStatus::rejected,
      "a retired plan id must not be reused with different members or order");

  std::vector<catalog_domain::SettingsCatalog> changed_semantics;
  auto changed_apply = base_catalog();
  changed_apply.revision = 2;
  changed_apply.settings[0].semantics.apply_capability =
      "settings.apply-other";
  changed_semantics.push_back(std::move(changed_apply));
  auto changed_detect = base_catalog();
  changed_detect.revision = 2;
  changed_detect.settings[0].semantics.detect_capability =
      "settings.detect-other";
  changed_semantics.push_back(std::move(changed_detect));
  auto changed_recover = base_catalog();
  changed_recover.revision = 2;
  changed_recover.settings[0].semantics.recover_capability =
      "settings.recover-other";
  changed_semantics.push_back(std::move(changed_recover));
  auto changed_restart = base_catalog();
  changed_restart.revision = 2;
  changed_restart.settings[0].restart_requirement =
      catalog_domain::RestartRequirement::windows;
  changed_semantics.push_back(std::move(changed_restart));
  auto changed_dependency = base_catalog();
  changed_dependency.revision = 2;
  changed_dependency.settings[1].depends_on.clear();
  changed_semantics.push_back(std::move(changed_dependency));
  auto changed_risk = base_catalog();
  changed_risk.revision = 2;
  changed_risk.settings[1].risk = catalog_domain::SettingRiskLevel::low;
  changed_semantics.push_back(std::move(changed_risk));
  auto changed_force_attempt = base_catalog();
  changed_force_attempt.revision = 2;
  changed_force_attempt.settings[1].force_attempt_rule =
      catalog_domain::ForceAttemptRule::prohibited;
  changed_semantics.push_back(std::move(changed_force_attempt));
  auto semantic_capabilities = capabilities();
  semantic_capabilities.apply.push_back("settings.apply-other");
  semantic_capabilities.detect.push_back("settings.detect-other");
  semantic_capabilities.recover.push_back("settings.recover-other");
  Fixture semantic_capability_fixture;
  catalog_app::SettingsCatalogLifecycle semantic_lifecycle{
      semantic_capability_fixture.adapter, semantic_capability_fixture.adapter,
      semantic_capability_fixture.log, semantic_capability_fixture.occupancy,
      semantic_capability_fixture.import_authorization,
      semantic_capabilities};
  if (semantic_lifecycle.initialize_builtin(base_catalog()).status !=
      catalog_app::InitializeStatus::initialized) {
    return expect(false, "expanded semantic fixture must initialize");
  }
  for (auto& candidate : changed_semantics) {
    passed &= expect(
        semantic_lifecycle.prepare_update(std::move(candidate)).status ==
            catalog_app::PrepareStatus::rejected,
        "a stable setting id must freeze execution, dependency, risk, and recovery semantics");
  }
  return passed;
}

[[nodiscard]] bool domain_model_contract() {
  auto valid = catalog_domain::validate(base_catalog(), capabilities());
  bool passed = expect(valid.validated.has_value(),
                       "a complete supported catalog must validate");
  if (!valid.validated.has_value()) {
    return false;
  }
  passed &= expect(catalog_domain::find_setting(
                       *valid.validated,
                       catalog_domain::StableId{"setting.alpha"}) != nullptr,
                   "downstream consumers must resolve settings by stable id");
  auto const* beta = catalog_domain::find_setting(
      *valid.validated, catalog_domain::StableId{"setting.beta"});
  auto const windows_11_24h2 = catalog_domain::WindowsVersion{
      .generation = catalog_domain::WindowsGeneration::windows_11,
      .feature_update_year = 24,
      .feature_update_half = 2};
  passed &= expect(
      beta != nullptr && beta->depends_on.size() == 1 &&
          beta->depends_on[0] == catalog_domain::StableId{"setting.alpha"} &&
          beta->risk == catalog_domain::SettingRiskLevel::elevated &&
          beta->force_attempt_rule ==
              catalog_domain::ForceAttemptRule::
                  allowed_with_explicit_confirmation &&
          beta->known_windows_range.minimum <
              beta->known_windows_range.maximum &&
          beta->known_windows_range.contains(windows_11_24h2),
      "settings must expose dependencies, risk, force-attempt, and comparable Windows range facts");
  auto const* available = catalog_domain::find_plan_availability(
      *valid.validated, catalog_domain::StableId{"plan.recommended"});
  passed &= expect(available != nullptr && available->enabled,
                   "a valid stable-id-only plan must remain enabled");

  auto invalid_plan_catalog = base_catalog();
  invalid_plan_catalog.plans[0].members[1].setting_id =
      catalog_domain::StableId{"setting.missing"};
  auto invalid_plan = catalog_domain::validate(std::move(invalid_plan_catalog),
                                               capabilities());
  passed &= expect(invalid_plan.validated.has_value(),
                   "a broken plan reference must not reject valid settings");
  if (invalid_plan.validated.has_value()) {
    auto const* disabled = catalog_domain::find_plan_availability(
        *invalid_plan.validated,
        catalog_domain::StableId{"plan.recommended"});
    passed &= expect(disabled != nullptr && !disabled->enabled &&
                         invalid_plan.validated->catalog.settings.size() == 2,
                     "a broken reference must disable only its plan");
  }

  auto cyclic_catalog = base_catalog();
  cyclic_catalog.settings[0].depends_on = {
      catalog_domain::StableId{"setting.beta"}};
  auto cyclic = catalog_domain::validate(std::move(cyclic_catalog),
                                         capabilities());
  passed &= expect(cyclic.validated.has_value() &&
                       !cyclic.validated->plans[0].enabled,
                   "cyclic plan relationships must disable only the plan");

  auto unsafe_force_attempt = base_catalog();
  unsafe_force_attempt.settings[1].recovery_requirement =
      catalog_domain::RecoveryRequirement::unavailable;
  unsafe_force_attempt.settings[1].semantics.recover_capability.reset();
  passed &= expect(
      !catalog_domain::validate(std::move(unsafe_force_attempt), capabilities())
           .validated.has_value(),
      "force attempt must require explicit recovery semantics");

  auto unknown = base_catalog();
  unknown.settings[0].semantics.apply_capability =
      "settings.future-operation";
  auto rejected = catalog_domain::validate(std::move(unknown), capabilities());
  passed &= expect(!rejected.validated.has_value() &&
                       !rejected.problems.empty(),
                   "unknown execution semantics must reject the whole catalog");

  auto display_extension = base_catalog();
  display_extension.ignored_display_fields.push_back("DISPLAY_FUTURE_NOTE");
  passed &= expect(catalog_domain::validate(std::move(display_extension),
                                            capabilities())
                       .validated.has_value(),
                   "unknown display-only content must be ignorable");

  auto reused = updated_catalog();
  reused.settings[0].semantics.identity = "windows.target.other";
  auto reused_validated = catalog_domain::validate(std::move(reused),
                                                   capabilities());
  passed &= expect(reused_validated.validated.has_value() &&
                       !catalog_domain::validate_transition(
                            *valid.validated, *reused_validated.validated)
                            .empty(),
                   "a stable id must not be reassigned to another target");
  return passed;
}

[[nodiscard]] bool update_rollback_and_isolation_contract() {
  Fixture fixture;
  std::vector<StateKey> unrelated_keys{
      StateKey::machine(AggregateId{"software-driver-catalog"}),
      StateKey::machine(AggregateId{"software-optimization-catalog"}),
      StateKey::machine(AggregateId{"settings-recovery-records"}),
      StateKey::machine(AggregateId{"system-optimization-state"}),
  };
  for (std::size_t index = 0; index < unrelated_keys.size(); ++index) {
    auto seeded = fixture.states.initialize(
        unrelated_keys[index], unrelated_state("unrelated-" +
                                               std::to_string(index)));
    if (seeded.status != azzs::application::StateCommitStatus::committed) {
      return expect(false, "unrelated isolation fixture must initialize");
    }
  }
  std::vector<azzs::domain::DeviceStateSnapshot> unrelated_before;
  for (auto const& key : unrelated_keys) {
    unrelated_before.push_back(*fixture.states.inspect(key).snapshot);
  }

  auto initialized = fixture.lifecycle.initialize_builtin(base_catalog());
  bool passed = expect(initialized.status ==
                           catalog_app::InitializeStatus::initialized,
                       "the built-in catalog must initialize once");
  auto initial = fixture.lifecycle.snapshot();
  passed &= expect(initial.status == catalog_app::CatalogSnapshotStatus::available &&
                       initial.current.has_value() &&
                       initial.current->catalog.revision == 1 &&
                       !initial.previous.has_value(),
                   "the initialized catalog must be independently available");

  auto prepared = fixture.lifecycle.prepare_update(updated_catalog());
  passed &= expect(prepared.status == catalog_app::PrepareStatus::ready &&
                       prepared.prepared.has_value(),
                   "a valid newer catalog must produce a preview");
  if (!prepared.prepared.has_value()) {
    return false;
  }
  passed &= expect(prepared.prepared->changes.added.size() == 2 &&
                       prepared.prepared->changes.changed.size() == 1 &&
                       prepared.prepared->changes.retired.size() == 2,
                   "preview must separate added, changed, and retired items");
  passed &= expect(fixture.lifecycle.snapshot().current->catalog.revision == 1,
                   "preparing a preview must not load the candidate");
  passed &= expect(fixture.lifecycle.confirm("wrong-token").status ==
                       catalog_app::ConfirmStatus::stale_preview &&
                       fixture.lifecycle.snapshot().current->catalog.revision == 1,
                   "an unrelated confirmation must leave the current catalog intact");

  auto committed = fixture.lifecycle.confirm(
      prepared.prepared->confirmation_token);
  passed &= expect(committed.status == catalog_app::ConfirmStatus::committed &&
                       committed.active_revision == 2,
                   "the exact preview confirmation must load the candidate");
  auto updated = fixture.lifecycle.snapshot();
  passed &= expect(updated.current.has_value() &&
                       updated.current->catalog.revision == 2 &&
                       updated.previous.has_value() &&
                       updated.previous->catalog.revision == 1,
                   "a successful update must retain the previous usable version");

  SettingsCatalogFileAdapter restarted_adapter{fixture.states};
  catalog_app::SettingsCatalogLifecycle restarted{
      restarted_adapter, restarted_adapter, fixture.log, fixture.occupancy,
      fixture.import_authorization, capabilities()};
  passed &= expect(restarted.snapshot().current->catalog.revision == 2,
                   "the active and previous catalogs must survive restart");

  auto rollback = restarted.prepare_rollback();
  passed &= expect(rollback.status == catalog_app::PrepareStatus::ready &&
                       rollback.prepared.has_value() &&
                       rollback.prepared->changes.downgrade,
                   "rollback must also be an explicit downgrade preview");
  if (rollback.prepared.has_value()) {
    passed &= expect(restarted.confirm(
                         rollback.prepared->confirmation_token)
                         .status == catalog_app::ConfirmStatus::committed,
                     "confirmed rollback must swap the current and previous versions");
  }
  auto rolled_back = restarted.snapshot();
  passed &= expect(rolled_back.current->catalog.revision == 1 &&
                       rolled_back.previous->catalog.revision == 2,
                   "rollback must preserve the replaced version for another recovery");

  for (std::size_t index = 0; index < unrelated_keys.size(); ++index) {
    auto after = fixture.states.inspect(unrelated_keys[index]);
    passed &= expect(after.snapshot.has_value() &&
                         *after.snapshot == unrelated_before[index],
                     "catalog lifecycle must not mutate other catalogs or recovery records");
  }
  passed &= expect(fixture.log.contains_stage("preview-update") &&
                       fixture.log.contains_stage("preview-rollback") &&
                       fixture.log.contains_stage("commit"),
                   "preview and authoritative changes must use the existing log seam");
  return passed;
}

[[nodiscard]] bool debug_import_contract() {
  Fixture fixture;
  auto initialized = fixture.lifecycle.initialize_builtin(updated_catalog());
  if (initialized.status != catalog_app::InitializeStatus::initialized) {
    return expect(false, "debug import fixture must initialize");
  }

  bool passed = expect(
      fixture.lifecycle.prepare_update(base_catalog()).status ==
          catalog_app::PrepareStatus::downgrade_requires_debug_import,
      "ordinary update must not silently downgrade the catalog");
  passed &= expect(fixture.lifecycle.prepare_debug_import(
                       "does-not-exist.azcat")
                       .status ==
                       catalog_app::PrepareStatus::debug_mode_required,
                   "manual import must be absent outside debug mode");
  fixture.import_authorization.set_enabled(true);
  std::string const missing_path =
      "/Users/private-account/catalog-input/does-not-exist.azcat";
  auto missing = fixture.lifecycle.prepare_debug_import(missing_path);
  passed &= expect(
      missing.status == catalog_app::PrepareStatus::rejected &&
          missing.import_source.has_value() &&
          missing.import_source->type ==
              catalog_app::CatalogImportSourceType::local_file &&
          missing.import_source->redacted_path == "does-not-exist.azcat" &&
          missing.detail.find("/Users/private-account") == std::string::npos,
      "a failed import must retain only a diagnosable redacted source");
  passed &= expect(
      fixture.log.contains_import_event(
          "debug-import-load", azzs::application::ExecutionResult::failed,
          "does-not-exist.azcat", "not-found"),
      "an import load failure must write a structured audit event");
  passed &= expect(!fixture.log.contains_text("/Users/private-account"),
                   "import audit must not retain a sensitive absolute path");

  TemporaryCatalogFile unknown_file{
      "settings-catalog-unknown.azcat",
      package_text(base_catalog(), "FUTURE_EXECUTION\tunsafe")};
  auto unknown =
      fixture.lifecycle.prepare_debug_import(unknown_file.path());
  passed &= expect(unknown.status == catalog_app::PrepareStatus::rejected &&
                       fixture.lifecycle.snapshot().current->catalog.revision == 2,
                   "unknown imported execution semantics must preserve the current catalog");
  passed &= expect(
      unknown.import_source.has_value() &&
          unknown.import_source->redacted_path ==
              "settings-catalog-unknown.azcat" &&
          fixture.log.contains_import_event(
              "debug-import-validation",
              azzs::application::ExecutionResult::failed,
              "settings-catalog-unknown.azcat", "rejected"),
      "an import validation rejection must retain and audit a redacted source");

  TemporaryCatalogFile import_file{
      "settings-catalog-valid.azcat",
      package_text(base_catalog(), "DISPLAY_FUTURE_NOTE\tignored")};
  auto imported =
      fixture.lifecycle.prepare_debug_import(import_file.path());
  passed &= expect(imported.status == catalog_app::PrepareStatus::ready &&
                       imported.prepared.has_value() &&
                       imported.prepared->changes.downgrade &&
                       imported.prepared->import_source.has_value() &&
                       imported.prepared->import_source->redacted_path ==
                           "settings-catalog-valid.azcat" &&
                       imported.prepared->setting_count == 2 &&
                       imported.prepared->plan_count == 1,
                   "debug downgrade preview must show path, version, and item counts");
  passed &= expect(fixture.lifecycle.snapshot().current->catalog.revision == 2,
                   "debug import must wait for exact confirmation");
  if (imported.prepared.has_value()) {
    auto confirmed_import = fixture.lifecycle.confirm(
        imported.prepared->confirmation_token);
    passed &= expect(confirmed_import.status ==
                             catalog_app::ConfirmStatus::committed &&
                         confirmed_import.import_source.has_value() &&
                         confirmed_import.import_source->redacted_path ==
                             "settings-catalog-valid.azcat",
                     "a confirmed, validated debug downgrade must load");
  }
  passed &= expect(
      fixture.log.contains_import_event(
          "debug-import-confirm", azzs::application::ExecutionResult::started,
          "settings-catalog-valid.azcat", "confirmation-started") &&
          fixture.log.contains_import_event(
              "debug-import-confirm",
              azzs::application::ExecutionResult::succeeded,
              "settings-catalog-valid.azcat", "committed"),
      "import confirmation and success must write structured audit events");
  passed &= expect(!fixture.log.contains_text(import_file.path()),
                   "successful import audit must not retain an absolute path");
  passed &= expect(fixture.lifecycle.snapshot().current->catalog.revision == 1,
                   "debug downgrade must become the independent current catalog");
  auto imported_snapshot = fixture.lifecycle.snapshot();
  auto const* imported_beta = catalog_domain::find_setting(
      *imported_snapshot.current,
      catalog_domain::StableId{"setting.beta"});
  passed &= expect(
      imported_beta != nullptr && imported_beta->depends_on.size() == 1 &&
          imported_beta->risk == catalog_domain::SettingRiskLevel::elevated &&
          imported_beta->force_attempt_rule ==
              catalog_domain::ForceAttemptRule::
                  allowed_with_explicit_confirmation &&
          imported_beta->known_windows_range.contains(
              catalog_domain::WindowsVersion{
                  .generation =
                      catalog_domain::WindowsGeneration::windows_11,
                  .feature_update_year = 24,
                  .feature_update_half = 2}),
      "the file adapter must parse and persist downstream settings facts");
  return passed;
}

[[nodiscard]] bool occupancy_failure_and_recovery_contract() {
  Fixture fixture;
  if (fixture.lifecycle.initialize_builtin(base_catalog()).status !=
      catalog_app::InitializeStatus::initialized) {
    return expect(false, "occupancy fixture must initialize");
  }
  auto prepared = fixture.lifecycle.prepare_update(updated_catalog());
  if (!prepared.prepared.has_value()) {
    return expect(false, "occupancy fixture must prepare an update");
  }

  SequenceLeaseTokenSource blocker_tokens{"blocker-"};
  SharedOperationOccupancy blocker{fixture.occupancy_storage, blocker_tokens};
  auto blocked_lease = blocker.try_acquire(OperationIdentity{
      .kind = "consumer-operation",
      .operation_id = "another-authoritative-operation",
      .correlation_id = "other-correlation",
  });
  bool passed = expect(blocked_lease.lease.has_value(),
                       "another operation must acquire the shared seam");
  passed &= expect(fixture.lifecycle.confirm(
                       prepared.prepared->confirmation_token)
                       .status == catalog_app::ConfirmStatus::occupied &&
                       fixture.lifecycle.snapshot().current->catalog.revision == 1,
                   "occupied state must preserve the old catalog");
  if (blocked_lease.lease.has_value()) {
    static_cast<void>(blocker.release(*blocked_lease.lease));
  }
  passed &= expect(fixture.lifecycle.confirm(
                       prepared.prepared->confirmation_token)
                       .status == catalog_app::ConfirmStatus::committed,
                   "the same confirmation may retry after occupancy clears");

  auto third = fixture.lifecycle.prepare_update(third_catalog());
  if (!third.prepared.has_value()) {
    return expect(false, "failure fixture must prepare a third catalog");
  }
  fixture.files.fail_next(StateFileOperation::write,
                          azzs::application::StateFileSlot::candidate,
                          "injected settings catalog commit failure");
  passed &= expect(fixture.lifecycle.confirm(
                       third.prepared->confirmation_token)
                       .status == catalog_app::ConfirmStatus::failed &&
                       fixture.lifecycle.snapshot().current->catalog.revision == 2,
                   "a failed authoritative commit must preserve the active catalog");
  passed &= expect(fixture.lifecycle.confirm(
                       third.prepared->confirmation_token)
                       .status == catalog_app::ConfirmStatus::committed &&
                       fixture.lifecycle.snapshot().current->catalog.revision == 3,
                   "a directed retry may commit after the storage fault clears");

  auto const key = StateKey::machine(AggregateId{"settings-catalog"});
  fixture.files.corrupt(key, azzs::application::StateFileSlot::current);
  auto recovered = fixture.lifecycle.snapshot();
  passed &= expect(
      recovered.status ==
              catalog_app::CatalogSnapshotStatus::recovered_read_only &&
          recovered.current.has_value() &&
          recovered.current->catalog.revision == 2,
      "a damaged current generation may expose only the last trusted catalog read-only");
  passed &= expect(fixture.lifecycle.prepare_update(third_catalog()).status ==
                       catalog_app::PrepareStatus::read_only,
                   "recovered N-1 catalog state must not accept new writes");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= stable_identity_lifecycle_contract();
  passed &= domain_model_contract();
  passed &= update_rollback_and_isolation_contract();
  passed &= debug_import_contract();
  passed &= occupancy_failure_and_recovery_contract();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "settings catalog contract passed\n";
  return EXIT_SUCCESS;
}
