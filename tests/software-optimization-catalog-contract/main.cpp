#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "azzs/adapters/infrastructure/local_software_optimization_catalog_file.hpp"
#include "azzs/application/software_optimization_catalog_lifecycle.hpp"
#include "azzs/domain/software_optimization_catalog.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace catalog = azzs::domain::software_optimization_catalog;

using azzs::application::CorrelationId;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogReceipt;
using azzs::application::OperationIdentity;
using azzs::application::SharedOperationOccupancy;
using azzs::application::SoftwareOptimizationCatalogFile;
using azzs::application::SoftwareOptimizationCatalogFileRead;
using azzs::application::SoftwareOptimizationCatalogLifecycle;
using azzs::application::SoftwareOptimizationCatalogLifecycleCode;
using azzs::application::SoftwareOptimizationCatalogStateMode;
using azzs::adapters::infrastructure::LocalSoftwareOptimizationCatalogFile;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryOperationOccupancyStorage;
using azzs::testing::InMemoryStateFileSystem;
using azzs::testing::SequenceLeaseTokenSource;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "software optimization catalog contract failed: " << message
              << '\n';
  }
  return condition;
}

class RecordingLog final : public ExecutionLog {
 public:
  [[nodiscard]] CorrelationId begin_correlation() override {
    return CorrelationId{"catalog-correlation-" +
                         std::to_string(next_correlation_++)};
  }

  [[nodiscard]] ExecutionLogReceipt append(
      CorrelationId const& correlation, ExecutionEvent const& event) override {
    correlations.push_back(correlation);
    events.push_back(event);
    if (fail_next_append_) {
      fail_next_append_ = false;
      return {.error = "injected catalog log failure"};
    }
    return {.persisted = true,
            .segment = 1,
            .sequence = static_cast<std::uint64_t>(events.size())};
  }

  [[nodiscard]] ExecutionLogClearReceipt clear() override {
    events.clear();
    correlations.clear();
    return {.cleared = true, .active_segment = 2};
  }

  [[nodiscard]] DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const&) override {
    return {.produced = true,
            .file_count = 1,
            .file_name = "catalog-diagnostic.txt"};
  }

  void fail_next_append() noexcept { fail_next_append_ = true; }

  std::vector<CorrelationId> correlations;
  std::vector<ExecutionEvent> events;

 private:
  std::uint64_t next_correlation_{1};
  bool fail_next_append_{false};
};

class MemoryCatalogFiles final : public SoftwareOptimizationCatalogFile {
 public:
  [[nodiscard]] SoftwareOptimizationCatalogFileRead read(
      std::string_view path) override {
    auto const found = files.find(std::string{path});
    if (found == files.end()) {
      return {.error = "candidate file does not exist"};
    }
    return {.succeeded = true, .source = found->second};
  }

  std::unordered_map<std::string, std::string> files;
};

[[nodiscard]] std::vector<catalog::BuiltInRuleDefinition>
built_in_rule_definitions() {
  using catalog::RulePurpose;
  return {
      {{"sogou.detect.installed"}, RulePurpose::install_detection},
      {{"sogou.detect.version"}, RulePurpose::version_detection},
      {{"sogou.statusbar.hide"}, RulePurpose::option_execution},
      {{"sogou.statusbar.hidden"}, RulePurpose::option_state_detection},
      {{"sogou.reusable.apply"}, RulePurpose::option_execution},
      {{"sogou.reusable.apply-a"}, RulePurpose::option_execution},
      {{"sogou.reusable.apply-b"}, RulePurpose::option_execution},
      {{"sogou.reusable.detect"}, RulePurpose::option_state_detection},
      {{"sogou.extra.apply"}, RulePurpose::option_execution},
      {{"sogou.extra.detect"}, RulePurpose::option_state_detection},
      {{"sogou.broken.detect"}, RulePurpose::option_state_detection},
  };
}

struct Fixture final {
  InMemoryStateFileSystem state_files;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'400'000}}};
  azzs::application::DeviceStateStore states{state_files, clock};
  RecordingLog log;
  InMemoryOperationOccupancyStorage occupancy_storage;
  SequenceLeaseTokenSource tokens{"catalog-lease-"};
  SharedOperationOccupancy occupancy{occupancy_storage, tokens};
  MemoryCatalogFiles catalog_files;
  std::vector<catalog::BuiltInRuleDefinition> built_in_rules{
      built_in_rule_definitions()};
  SoftwareOptimizationCatalogLifecycle lifecycle{states, log, occupancy,
                                                  catalog_files,
                                                  built_in_rules};
};

[[nodiscard]] std::string catalog_source(
    std::uint64_t revision, bool include_extra_scheme = false,
    bool include_reusable_option = false,
    std::string reusable_execution = "sogou.reusable.apply",
    bool break_primary_rule = false,
    std::string extra_top_level = {},
    std::string release_state = "release",
    bool include_broken_scheme = false) {
  std::ostringstream source;
  source << "schema_version = 1\n"
         << "semantics_version = 1\n"
         << "catalog_id = \"software-optimization\"\n"
         << "revision = " << revision << "\n"
         << "release_state = \"" << release_state << "\"\n"
         << "default_locale = \"zh-CN\"\n";
  if (!extra_top_level.empty()) {
    source << extra_top_level << '\n';
  }
  source
      << "\n[[targets]]\n"
      << "id = \"sogou-input-target\"\n"
      << "identity_anchor = \"vendor.sogou.input.windows\"\n"
      << "required_first_release = true\n"
      << "support_mode = \"supported\"\n"
      << "version_min = \"14.0\"\n"
      << "version_max = \"15.9\"\n"
      << "install_detection_kind = \"built_in_definition\"\n"
      << "install_detection_definition = \"sogou.detect.installed\"\n"
      << "version_detection_kind = \"built_in_definition\"\n"
      << "version_detection_definition = \"sogou.detect.version\"\n"
      << "installation_item_id = \"sogou-input\"\n"
      << "explanation_source = \"https://shurufa.sogou.com/windows\"\n"
      << "\n[[schemes]]\n"
      << "id = \"sogou.recommended\"\n"
      << "target_id = \"sogou-input-target\"\n"
      << "required_first_release = true\n"
      << "automation = \"controlled\"\n"
      << "version_min = \"14.0\"\n"
      << "version_max = \"15.9\"\n"
      << "impact = \"调整搜狗输入法自身行为\"\n"
      << "risk = \"medium\"\n"
      << "exit_requirement = \"graceful_exit\"\n"
      << "restart_requirement = \"none\"\n"
      << "required_scheme_ids = []\n"
      << "conflicting_scheme_ids = []\n"
      << "explanation_source = \"https://coolplume.github.io/posts/f4654180.html\"\n"
      << "manual_emergency_explanation = \"受控执行失败时打开官方设置核对\"\n"
      << "\n[[options]]\n"
      << "id = \"sogou.hide-statusbar\"\n"
      << "scheme_id = \"sogou.recommended\"\n"
      << "version_min = \"14.0\"\n"
      << "version_max = \"15.9\"\n"
      << "impact = \"隐藏状态栏\"\n"
      << "default_selected = false\n"
      << "required = false\n"
      << "automation = \"controlled\"\n"
      << "execution_kind = \"built_in_definition\"\n"
      << "execution_definition = \""
      << (break_primary_rule ? "invalid rule" : "sogou.statusbar.hide")
      << "\"\n"
      << "state_detection_kind = \"built_in_definition\"\n"
      << "state_detection_definition = \"sogou.statusbar.hidden\"\n"
      << "required_option_ids = []\n"
      << "conflicting_option_ids = []\n"
      << "allowed_values = []\n"
      << "explanation_source = \"https://coolplume.github.io/posts/f4654180.html\"\n";
  if (include_reusable_option) {
    source
        << "\n[[options]]\n"
        << "id = \"sogou.reusable-option\"\n"
        << "scheme_id = \"sogou.recommended\"\n"
        << "version_min = \"14.0\"\n"
        << "version_max = \"15.9\"\n"
        << "impact = \"稳定标识复用测试\"\n"
        << "default_selected = false\n"
        << "required = false\n"
        << "automation = \"controlled\"\n"
        << "execution_kind = \"built_in_definition\"\n"
        << "execution_definition = \"" << reusable_execution << "\"\n"
        << "state_detection_kind = \"built_in_definition\"\n"
        << "state_detection_definition = \"sogou.reusable.detect\"\n"
        << "required_option_ids = []\n"
        << "conflicting_option_ids = []\n"
        << "allowed_values = []\n"
        << "explanation_source = \"https://example.invalid/reusable\"\n";
  }
  if (include_extra_scheme) {
    source
        << "\n[[schemes]]\n"
        << "id = \"sogou.extra\"\n"
        << "target_id = \"sogou-input-target\"\n"
        << "required_first_release = false\n"
        << "automation = \"controlled\"\n"
        << "version_min = \"14.0\"\n"
        << "version_max = \"15.9\"\n"
        << "impact = \"额外声明式优化\"\n"
        << "risk = \"low\"\n"
        << "exit_requirement = \"none\"\n"
        << "restart_requirement = \"explorer\"\n"
        << "required_scheme_ids = []\n"
        << "conflicting_scheme_ids = []\n"
        << "explanation_source = \"https://example.invalid/extra\"\n"
        << "manual_emergency_explanation = \"\"\n"
        << "\n[[options]]\n"
        << "id = \"sogou.extra.option\"\n"
        << "scheme_id = \"sogou.extra\"\n"
        << "version_min = \"14.0\"\n"
        << "version_max = \"15.9\"\n"
        << "impact = \"额外声明式选项\"\n"
        << "default_selected = true\n"
        << "required = false\n"
        << "automation = \"controlled\"\n"
        << "execution_kind = \"built_in_definition\"\n"
        << "execution_definition = \"sogou.extra.apply\"\n"
        << "state_detection_kind = \"built_in_definition\"\n"
        << "state_detection_definition = \"sogou.extra.detect\"\n"
        << "required_option_ids = []\n"
        << "conflicting_option_ids = []\n"
        << "allowed_values = []\n"
        << "explanation_source = \"https://example.invalid/extra\"\n";
  }
  if (include_broken_scheme) {
    source
        << "\n[[schemes]]\n"
        << "id = \"sogou.broken\"\n"
        << "target_id = \"sogou-input-target\"\n"
        << "required_first_release = false\n"
        << "automation = \"controlled\"\n"
        << "version_min = \"14.0\"\n"
        << "version_max = \"15.9\"\n"
        << "impact = \"局部配置错误测试\"\n"
        << "risk = \"low\"\n"
        << "exit_requirement = \"none\"\n"
        << "restart_requirement = \"none\"\n"
        << "required_scheme_ids = []\n"
        << "conflicting_scheme_ids = []\n"
        << "explanation_source = \"https://example.invalid/broken\"\n"
        << "manual_emergency_explanation = \"\"\n"
        << "\n[[options]]\n"
        << "id = \"sogou.broken.option\"\n"
        << "scheme_id = \"sogou.broken\"\n"
        << "version_min = \"14.0\"\n"
        << "version_max = \"15.9\"\n"
        << "impact = \"未注册内置定义引用\"\n"
        << "default_selected = false\n"
        << "required = false\n"
        << "automation = \"controlled\"\n"
        << "execution_kind = \"built_in_definition\"\n"
        << "execution_definition = \"sogou.unregistered.apply\"\n"
        << "state_detection_kind = \"built_in_definition\"\n"
        << "state_detection_definition = \"sogou.broken.detect\"\n"
        << "required_option_ids = []\n"
        << "conflicting_option_ids = []\n"
        << "allowed_values = []\n"
        << "explanation_source = \"https://example.invalid/broken\"\n";
  }
  source
      << "\n[[compatibility_baselines]]\n"
      << "id = \"sogou.stable.baseline\"\n"
      << "target_id = \"sogou-input-target\"\n"
      << "software_item_id = \"sogou-input\"\n"
      << "installer_baseline_id = \"sogou-stable-windows\"\n"
      << "installed_version_min = \"14.2\"\n"
      << "installed_version_max = \"15.0\"\n";
  return source.str();
}

[[nodiscard]] bool has_issue(
    std::vector<catalog::CatalogIssue> const& issues,
    catalog::CatalogIssueCode code) {
  return std::ranges::any_of(issues, [code](catalog::CatalogIssue const& issue) {
    return issue.code == code;
  });
}

[[nodiscard]] bool has_id(std::vector<catalog::StableId> const& ids,
                          std::string_view id) {
  return std::ranges::any_of(ids, [id](catalog::StableId const& value) {
    return value.value == id;
  });
}

[[nodiscard]] bool verify_domain_validation_and_compatibility() {
  auto const rules = built_in_rule_definitions();
  auto valid = catalog::load_catalog(catalog_source(1, true), rules);
  bool passed = expect(valid.accepted(), "a valid declarative catalog must load");
  if (!valid.catalog.has_value()) {
    return false;
  }
  auto summary = catalog::summarize(*valid.catalog);
  passed &= expect(summary.target_count == 1 && summary.scheme_count == 2 &&
                       summary.option_count == 2 &&
                       summary.disabled_scheme_count == 0 &&
                       summary.intrinsic_release_eligible,
                   "valid catalog counts and release gate must be deterministic");

  auto with_display = catalog::load_catalog(
      catalog_source(1, false, false, "sogou.reusable.apply", false,
                     "display_future_note = \"safe forward display\""),
      rules);
  passed &= expect(with_display.accepted(),
                   "unknown display-only fields must remain forward compatible");

  auto with_unknown_semantics = catalog::load_catalog(
      catalog_source(2, false, false, "sogou.reusable.apply", false,
                     "execution_future = \"third-party-plugin\""),
      rules);
  passed &= expect(!with_unknown_semantics.accepted() &&
                       has_issue(with_unknown_semantics.package_issues,
                                 catalog::CatalogIssueCode::
                                     unknown_execution_semantics),
                   "unknown execution semantics must reject the whole package");

  auto unknown_schema_source = catalog_source(2);
  unknown_schema_source.replace(unknown_schema_source.find("schema_version = 1"),
                                std::string_view{"schema_version = 1"}.size(),
                                "schema_version = 2");
  auto unknown_schema = catalog::load_catalog(unknown_schema_source, rules);
  passed &= expect(!unknown_schema.accepted() &&
                       has_issue(unknown_schema.package_issues,
                                 catalog::CatalogIssueCode::unknown_schema),
                   "unknown schema versions must reject the whole package");

  auto duplicate_id_source = catalog_source(2);
  auto const baseline_id = std::string{"id = \"sogou.stable.baseline\""};
  duplicate_id_source.replace(duplicate_id_source.find(baseline_id),
                              baseline_id.size(),
                              "id = \"sogou.hide-statusbar\"");
  auto duplicate_id = catalog::load_catalog(duplicate_id_source, rules);
  passed &= expect(!duplicate_id.accepted() &&
                       has_issue(duplicate_id.package_issues,
                                 catalog::CatalogIssueCode::duplicate_stable_id),
                   "stable id collisions must reject the whole package");

  auto isolated = catalog::load_catalog(
      catalog_source(2, true, false, "sogou.reusable.apply", true), rules);
  passed &= expect(isolated.accepted(),
                   "a known-rule local configuration error must not reject the package");
  if (isolated.catalog.has_value()) {
    auto const* broken = isolated.catalog->find_scheme("sogou.recommended");
    auto const* healthy = isolated.catalog->find_scheme("sogou.extra");
    passed &= expect(
        broken != nullptr &&
            broken->availability == catalog::SchemeAvailability::configuration_error &&
            healthy != nullptr &&
            healthy->availability == catalog::SchemeAvailability::available,
        "local rule errors must disable only the related scheme");
  }

  std::array matching_baseline{
      catalog::SoftwareCatalogInstallerBaseline{
          .software_item_id = catalog::StableId{"sogou-input"},
          .installer_baseline_id = catalog::StableId{"sogou-stable-windows"},
          .installed_versions = {"14.5", "14.9"},
      }};
  auto compatible =
      catalog::assess_release_compatibility(*valid.catalog, matching_baseline);
  passed &= expect(compatible.compatible,
                   "intersecting Sogou installer declarations must pass");
  std::array mismatching_baseline{
      catalog::SoftwareCatalogInstallerBaseline{
          .software_item_id = catalog::StableId{"sogou-input"},
          .installer_baseline_id = catalog::StableId{"sogou-stable-windows"},
          .installed_versions = {"16.0", "16.9"},
      }};
  auto incompatible = catalog::assess_release_compatibility(
      *valid.catalog, mismatching_baseline);
  passed &= expect(!incompatible.compatible &&
                       has_issue(incompatible.issues,
                                 catalog::CatalogIssueCode::
                                     compatibility_baseline_mismatch),
                   "non-intersecting Sogou baselines must block release");
  return passed;
}

[[nodiscard]] bool verify_lifecycle_import_downgrade_and_rollback() {
  Fixture fixture;
  auto const unrelated_key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"unrelated-history"});
  auto unrelated = fixture.states.initialize(
      unrelated_key,
      azzs::domain::DeviceState{
          .value = {.schema = 2,
                    .minimum_reader = 1,
                    .minimum_writer = 2,
                    .payload = {std::byte{'o'}, std::byte{'k'}}},
      });
  bool passed = expect(
      unrelated.status == azzs::application::StateCommitStatus::committed,
      "unrelated history fixture must initialize");

  auto built_in = fixture.lifecycle.ensure_builtin(catalog_source(1), "builtin-1");
  passed &= expect(built_in.code ==
                           SoftwareOptimizationCatalogLifecycleCode::applied &&
                       built_in.state_changed && built_in.active.has_value() &&
                       built_in.active->revision == 1,
                   "builtin catalog must establish the first active version");
  auto first_snapshot = fixture.lifecycle.snapshot();
  passed &= expect(first_snapshot.mode ==
                           SoftwareOptimizationCatalogStateMode::available &&
                       first_snapshot.current.has_value() &&
                       !first_snapshot.previous_available,
                   "first active catalog must persist without a fake previous version");
  auto frozen = catalog::freeze_scheme(*first_snapshot.current,
                                       "sogou.recommended");
  passed &= expect(frozen.has_value() && frozen->catalog_revision == 1,
                   "catalog consumers must be able to freeze an immutable scheme");

  fixture.catalog_files.files["update-v2.toml"] = catalog_source(2, true);
  auto updated = fixture.lifecycle.apply_update("update-v2.toml", "update-2");
  passed &= expect(updated.code ==
                           SoftwareOptimizationCatalogLifecycleCode::applied &&
                       updated.active.has_value() &&
                       updated.active->revision == 2,
                   "a higher candidate revision must apply atomically");
  auto second_snapshot = fixture.lifecycle.snapshot();
  passed &= expect(second_snapshot.current.has_value() &&
                       second_snapshot.current->revision == 2 &&
                       second_snapshot.previous_available,
                   "successful update must retain the replaced version for rollback");
  passed &= expect(frozen.has_value() && frozen->catalog_revision == 1 &&
                       frozen->scheme.options.size() == 1,
                   "catalog update must not mutate an existing batch snapshot");

  fixture.catalog_files.files["write-failure-v3.toml"] =
      catalog_source(3, true);
  fixture.state_files.fail_next(
      azzs::testing::StateFileOperation::write,
      azzs::application::StateFileSlot::candidate);
  auto write_failed = fixture.lifecycle.apply_update("write-failure-v3.toml",
                                                     "update-write-failure");
  passed &= expect(
      write_failed.code ==
              SoftwareOptimizationCatalogLifecycleCode::persistence_failed &&
          fixture.lifecycle.snapshot().current->revision == 2,
      "persistence failure must retain the previous authoritative catalog");

  fixture.catalog_files.files["lower-v1.toml"] = catalog_source(1);
  auto normal_downgrade =
      fixture.lifecycle.apply_update("lower-v1.toml", "update-lower");
  passed &= expect(normal_downgrade.code ==
                           SoftwareOptimizationCatalogLifecycleCode::rejected &&
                       fixture.lifecycle.snapshot().current->revision == 2,
                   "normal update must reject downgrade and retain the active version");

  auto denied_preview =
      fixture.lifecycle.preview_manual_import("lower-v1.toml", false);
  passed &= expect(denied_preview.code ==
                       SoftwareOptimizationCatalogLifecycleCode::debug_mode_required,
                   "manual import preview must be debug-mode only");
  auto preview = fixture.lifecycle.preview_manual_import("lower-v1.toml", true);
  passed &= expect(preview.code ==
                           SoftwareOptimizationCatalogLifecycleCode::preview_ready &&
                       preview.path == "lower-v1.toml" &&
                       preview.candidate.has_value() &&
                       preview.candidate->revision == 1 && preview.downgrade &&
                       has_id(preview.lost_or_changed_schemes, "sogou.extra"),
                   "downgrade preview must show path, revision, counts, and lost schemes");
  auto unconfirmed = fixture.lifecycle.apply_manual_import(
      "lower-v1.toml", preview.preview_token, false, true,
      "import-unconfirmed");
  passed &= expect(unconfirmed.code ==
                       SoftwareOptimizationCatalogLifecycleCode::confirmation_required,
                   "manual import must not apply before explicit confirmation");
  auto stale = fixture.lifecycle.apply_manual_import(
      "lower-v1.toml", "0000000000000000", true, true, "import-stale");
  passed &= expect(stale.code ==
                       SoftwareOptimizationCatalogLifecycleCode::preview_stale,
                   "manual import must re-read and bind confirmation to preview bytes");
  auto downgraded = fixture.lifecycle.apply_manual_import(
      "lower-v1.toml", preview.preview_token, true, true,
      "import-downgrade");
  passed &= expect(downgraded.code ==
                           SoftwareOptimizationCatalogLifecycleCode::downgraded &&
                       downgraded.state_changed &&
                       fixture.lifecycle.snapshot().current->revision == 1,
                   "confirmed debug import may install a lower valid revision");
  auto rolled_back = fixture.lifecycle.rollback("rollback-2");
  passed &= expect(rolled_back.code ==
                           SoftwareOptimizationCatalogLifecycleCode::rolled_back &&
                       rolled_back.active.has_value() &&
                       rolled_back.active->revision == 2 &&
                       fixture.lifecycle.snapshot().current->revision == 2,
                   "rollback must atomically swap to the retained valid version");

  SoftwareOptimizationCatalogLifecycle restarted{
      fixture.states, fixture.log, fixture.occupancy, fixture.catalog_files,
      fixture.built_in_rules};
  auto restarted_snapshot = restarted.snapshot();
  passed &= expect(restarted_snapshot.current.has_value() &&
                       restarted_snapshot.current->revision == 2,
                   "active and previous catalogs must survive lifecycle reconstruction");
  auto unrelated_after = fixture.states.inspect(unrelated_key);
  passed &= expect(unrelated_after.snapshot.has_value() &&
                       unrelated_after.snapshot->state.value.payload ==
                           std::vector<std::byte>{std::byte{'o'}, std::byte{'k'}},
                   "catalog lifecycle must not rewrite history or other aggregates");
  passed &= expect(!fixture.log.events.empty() &&
                       std::ranges::any_of(
                           fixture.log.events, [](ExecutionEvent const& event) {
                             return event.stage == "preview-manual-import";
                           }),
                   "preview, apply, downgrade, and rollback outcomes must use the log seam");
  return passed;
}

[[nodiscard]] bool verify_rejection_identity_history_and_occupancy() {
  Fixture fixture;
  bool passed = true;
  auto initial = fixture.lifecycle.ensure_builtin(
      catalog_source(1, false, true, "sogou.reusable.apply-a"),
      "identity-initial");
  passed &= expect(initial.state_changed,
                   "identity history fixture must establish revision one");

  fixture.catalog_files.files["identity-v2.toml"] = catalog_source(2);
  auto removed =
      fixture.lifecycle.apply_update("identity-v2.toml", "identity-remove");
  passed &= expect(removed.state_changed && removed.active->revision == 2,
                   "a later version may remove a stable item");
  fixture.catalog_files.files["identity-v3.toml"] =
      catalog_source(3, false, true, "sogou.reusable.apply-b");
  auto reused =
      fixture.lifecycle.apply_update("identity-v3.toml", "identity-reuse");
  passed &= expect(reused.code ==
                           SoftwareOptimizationCatalogLifecycleCode::rejected &&
                       has_issue(reused.issues,
                                 catalog::CatalogIssueCode::stable_id_reuse) &&
                       fixture.lifecycle.snapshot().current->revision == 2,
                   "removed stable ids must remain reserved against semantic reuse");

  fixture.catalog_files.files["unknown-v3.toml"] = catalog_source(
      3, false, false, "sogou.reusable.apply", false,
      "execution_plugin = \"load-library\"");
  auto unknown =
      fixture.lifecycle.apply_update("unknown-v3.toml", "unknown-semantics");
  passed &= expect(unknown.code ==
                           SoftwareOptimizationCatalogLifecycleCode::rejected &&
                       fixture.lifecycle.snapshot().current->revision == 2,
                   "whole-package rejection must retain the last valid catalog");

  fixture.catalog_files.files["isolated-v3.toml"] =
      catalog_source(3, true, false, "sogou.reusable.apply", false, {},
                     "release", true);
  auto isolated =
      fixture.lifecycle.apply_update("isolated-v3.toml", "isolated-error");
  auto isolated_snapshot = fixture.lifecycle.snapshot();
  passed &= expect(isolated.state_changed &&
                       isolated_snapshot.current->revision == 3 &&
                       isolated_snapshot.current->find_scheme("sogou.broken")
                               ->availability ==
                           catalog::SchemeAvailability::configuration_error &&
                       isolated_snapshot.current->find_scheme("sogou.extra")
                               ->availability ==
                           catalog::SchemeAvailability::available,
                   "local configuration errors must switch with the package without partial old definitions");

  auto blocker = fixture.occupancy.try_acquire(OperationIdentity{
      .kind = "other-device-operation",
      .operation_id = "block-catalog-update",
      .correlation_id = "blocker-correlation",
  });
  passed &= expect(blocker.lease.has_value(),
                   "occupancy blocker fixture must acquire a lease");
  fixture.catalog_files.files["blocked-v4.toml"] = catalog_source(4, true);
  auto blocked =
      fixture.lifecycle.apply_update("blocked-v4.toml", "blocked-update");
  passed &= expect(blocked.code ==
                           SoftwareOptimizationCatalogLifecycleCode::occupied &&
                       fixture.lifecycle.snapshot().current->revision == 3,
                   "catalog mutation must consume the shared device occupancy seam");
  if (blocker.lease.has_value()) {
    passed &= expect(fixture.occupancy.release(*blocker.lease).code ==
                         azzs::application::OccupancyResultCode::released,
                     "occupancy blocker fixture must release its lease");
  }
  return passed;
}

[[nodiscard]] bool verify_unavailable_and_logging_fail_closed() {
  Fixture fixture;
  auto empty = fixture.lifecycle.snapshot();
  bool passed = expect(
      empty.mode == SoftwareOptimizationCatalogStateMode::unavailable &&
          !empty.current.has_value(),
      "missing catalog must disable only new software optimization work");

  fixture.log.fail_next_append();
  auto failed = fixture.lifecycle.ensure_builtin(catalog_source(1),
                                                 "log-fail-closed");
  passed &= expect(failed.code ==
                           SoftwareOptimizationCatalogLifecycleCode::logging_failed &&
                       !fixture.lifecycle.snapshot().current.has_value(),
                   "a mutation must not start when its required audit event cannot persist");
  auto applied = fixture.lifecycle.ensure_builtin(catalog_source(1),
                                                  "log-retry");
  passed &= expect(applied.state_changed,
                   "catalog may load after the logging seam recovers");
  return passed;
}

class TemporaryCatalogFile final {
 public:
  explicit TemporaryCatalogFile(std::string bytes) {
    static std::uint64_t next{1};
    path = std::filesystem::temp_directory_path() /
           ("azzs-software-optimization-catalog-contract-" +
            std::to_string(next++) + ".toml");
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  ~TemporaryCatalogFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  TemporaryCatalogFile(TemporaryCatalogFile const&) = delete;
  TemporaryCatalogFile& operator=(TemporaryCatalogFile const&) = delete;

  std::filesystem::path path;
};

[[nodiscard]] bool verify_local_file_adapter_contract() {
  LocalSoftwareOptimizationCatalogFile adapter;
  auto const source = catalog_source(1);
  TemporaryCatalogFile valid{source};
  auto read = adapter.read(valid.path.string());
  bool passed = expect(read.succeeded && read.source == source,
                       "local file adapter must return exact candidate bytes");
  auto missing = adapter.read(valid.path.string() + ".missing");
  passed &= expect(!missing.succeeded && !missing.error.empty(),
                   "local file adapter must type a missing-file failure");
  TemporaryCatalogFile binary{std::string{"schema", 6} + '\0' + "invalid"};
  auto binary_read = adapter.read(binary.path.string());
  passed &= expect(!binary_read.succeeded &&
                       binary_read.error.find("binary null") !=
                           std::string::npos,
                   "local file adapter must reject binary candidate content");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_domain_validation_and_compatibility();
  passed &= verify_lifecycle_import_downgrade_and_rollback();
  passed &= verify_rejection_identity_history_and_occupancy();
  passed &= verify_unavailable_and_logging_fail_closed();
  passed &= verify_local_file_adapter_contract();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "software optimization catalog contract passed\n";
  return EXIT_SUCCESS;
}
