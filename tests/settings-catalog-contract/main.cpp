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

 private:
  std::uint64_t next_correlation_{1};
  std::vector<ExecutionEvent> events_;
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
      .known_windows_range = {.minimum = "10-22h2",
                              .maximum = "11-25h2"},
      .default_selected = default_selected,
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
    std::string second_setting) {
  return {
      .id = catalog_domain::StableId{"plan.recommended"},
      .display_name = "Recommended optimization",
      .description = "A visible selection of existing settings.",
      .members = {
          {.setting_id = catalog_domain::StableId{"setting.alpha"},
           .order = 10,
           .default_selected = true},
          {.setting_id = catalog_domain::StableId{std::move(second_setting)},
           .order = 20,
           .default_selected = false,
           .depends_on = {catalog_domain::StableId{"setting.alpha"}}},
      },
  };
}

[[nodiscard]] catalog_domain::SettingsCatalog base_catalog() {
  return {
      .revision = 1,
      .settings = {
          setting("setting.alpha", "Alpha setting",
                  "windows.target.alpha", true),
          setting("setting.beta", "Beta setting", "windows.target.beta",
                  false),
      },
      .plans = {plan("setting.beta")},
  };
}

[[nodiscard]] catalog_domain::SettingsCatalog updated_catalog() {
  return {
      .revision = 2,
      .settings = {
          setting("setting.alpha", "Alpha setting updated",
                  "windows.target.alpha", true),
          setting("setting.gamma", "Gamma setting", "windows.target.gamma",
                  false),
      },
      .plans = {plan("setting.gamma")},
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

[[nodiscard]] std::string package_text(
    catalog_domain::SettingsCatalog const& catalog,
    std::string_view extra_record = {}) {
  std::ostringstream output;
  output << "AZZS_SETTINGS_CATALOG\t1\t" << catalog.schema_version << '\t'
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
           << '\t' << dash(item.known_windows_range.minimum) << '\t'
           << dash(item.known_windows_range.maximum) << '\t'
           << (item.default_selected ? 1 : 0) << '\t' << recovery << '\t'
           << restart << '\t' << item.semantics.identity << '\t'
           << item.semantics.apply_capability << '\t'
           << item.semantics.detect_capability << '\t'
           << dash(item.semantics.recover_capability) << '\n';
  }
  for (auto const& item : catalog.plans) {
    output << "PLAN\t" << item.id.value << '\t' << item.display_name << '\t'
           << item.description << '\n';
    for (auto const& member : item.members) {
      output << "MEMBER\t" << item.id.value << '\t'
             << member.setting_id.value << '\t' << member.order << '\t'
             << (member.default_selected ? 1 : 0) << '\t';
      if (member.depends_on.empty()) {
        output << '-';
      } else {
        for (std::size_t index = 0; index < member.depends_on.size(); ++index) {
          if (index != 0) {
            output << ',';
          }
          output << member.depends_on[index].value;
        }
      }
      output << '\n';
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
  InMemoryOperationOccupancyStorage occupancy_storage;
  SequenceLeaseTokenSource lease_tokens{"settings-catalog-lease-"};
  SharedOperationOccupancy occupancy{occupancy_storage, lease_tokens};
  catalog_app::SettingsCatalogLifecycle lifecycle{
      adapter, adapter, log, occupancy, capabilities()};
};

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
  cyclic_catalog.plans[0].members[0].depends_on = {
      catalog_domain::StableId{"setting.beta"}};
  auto cyclic = catalog_domain::validate(std::move(cyclic_catalog),
                                         capabilities());
  passed &= expect(cyclic.validated.has_value() &&
                       !cyclic.validated->plans[0].enabled,
                   "cyclic plan relationships must disable only the plan");

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
  passed &= expect(prepared.prepared->changes.added.size() == 1 &&
                       prepared.prepared->changes.changed.size() == 2 &&
                       prepared.prepared->changes.retired.size() == 1,
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
      capabilities()};
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
                       "does-not-exist.azcat", false)
                       .status ==
                       catalog_app::PrepareStatus::debug_mode_required,
                   "manual import must be absent outside debug mode");

  TemporaryCatalogFile unknown_file{
      "settings-catalog-unknown.azcat",
      package_text(base_catalog(), "FUTURE_EXECUTION\tunsafe")};
  auto unknown =
      fixture.lifecycle.prepare_debug_import(unknown_file.path(), true);
  passed &= expect(unknown.status == catalog_app::PrepareStatus::rejected &&
                       fixture.lifecycle.snapshot().current->catalog.revision == 2,
                   "unknown imported execution semantics must preserve the current catalog");

  TemporaryCatalogFile import_file{
      "settings-catalog-valid.azcat",
      package_text(base_catalog(), "DISPLAY_FUTURE_NOTE\tignored")};
  auto imported =
      fixture.lifecycle.prepare_debug_import(import_file.path(), true);
  passed &= expect(imported.status == catalog_app::PrepareStatus::ready &&
                       imported.prepared.has_value() &&
                       imported.prepared->changes.downgrade &&
                       imported.prepared->source_path == import_file.path() &&
                       imported.prepared->setting_count == 2 &&
                       imported.prepared->plan_count == 1,
                   "debug downgrade preview must show path, version, and item counts");
  passed &= expect(fixture.lifecycle.snapshot().current->catalog.revision == 2,
                   "debug import must wait for exact confirmation");
  if (imported.prepared.has_value()) {
    passed &= expect(fixture.lifecycle.confirm(
                         imported.prepared->confirmation_token)
                         .status == catalog_app::ConfirmStatus::committed,
                     "a confirmed, validated debug downgrade must load");
  }
  passed &= expect(fixture.lifecycle.snapshot().current->catalog.revision == 1,
                   "debug downgrade must become the independent current catalog");
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
