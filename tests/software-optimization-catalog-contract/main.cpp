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
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "azzs/adapters/infrastructure/local_software_optimization_catalog_file.hpp"
#include "azzs/application/sogou_optimization.hpp"
#include "azzs/application/software_optimization_catalog_lifecycle.hpp"
#include "azzs/domain/controlled_install_profiles.hpp"
#include "azzs/domain/software_optimization_catalog.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace catalog = azzs::domain::software_optimization_catalog;
namespace sogou = azzs::application::sogou_optimization;
namespace install_catalog = azzs::domain::software_catalog;

using azzs::application::CorrelationId;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogReceipt;
using azzs::application::OperationIdentity;
using azzs::application::SharedOperationOccupancy;
using azzs::application::SoftwareOptimizationCatalogDebugAuthorization;
using azzs::application::SoftwareOptimizationCatalogLocalImportFile;
using azzs::application::SoftwareOptimizationCatalogLocalImportRead;
using azzs::application::SoftwareOptimizationCatalogLifecycle;
using azzs::application::SoftwareOptimizationCatalogLifecycleCode;
using azzs::application::SoftwareOptimizationCatalogSourceKind;
using azzs::application::SoftwareOptimizationCatalogStateMode;
using azzs::application::TrustedSoftwareOptimizationCatalogUpdate;
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

class MemoryCatalogFiles final
    : public SoftwareOptimizationCatalogLocalImportFile {
 public:
  [[nodiscard]] SoftwareOptimizationCatalogLocalImportRead read(
      std::string_view path) override {
    auto const found = files.find(std::string{path});
    if (found == files.end()) {
      return {.error = "candidate file does not exist"};
    }
    return {.succeeded = true, .source = found->second};
  }

  std::unordered_map<std::string, std::string> files;
};

class FixedCatalogDebugAuthorization final
    : public SoftwareOptimizationCatalogDebugAuthorization {
 public:
  explicit FixedCatalogDebugAuthorization(bool allowed) : allowed_(allowed) {}

  [[nodiscard]] bool local_import_allowed() const noexcept override {
    return allowed_;
  }

 private:
  bool allowed_{false};
};

class RecordingSogouAdapter final
    : public sogou::SogouOptimizationPlatformAdapter {
 public:
  [[nodiscard]] sogou::SogouTargetDetection detect_target() override {
    ++target_detection_calls;
    return {.status = sogou::SogouOptimizationStatus::succeeded,
            .installed_version = "16.7",
            .detail = "test adapter"};
  }

  [[nodiscard]] sogou::SogouOptionDetection detect_option(
      sogou::SogouOptimizationAction action,
      std::optional<sogou::SogouCandidateCount> expected_value) override {
    ++option_detection_calls;
    last_action = action;
    last_value = expected_value;
    return {.status = next_status, .candidate_count = expected_value,
            .detail = "test adapter"};
  }

  [[nodiscard]] sogou::SogouOptimizationExecution execute(
      sogou::SogouOptimizationAction action,
      std::optional<sogou::SogouCandidateCount> value) override {
    ++execution_calls;
    last_action = action;
    last_value = value;
    return {.status = next_status, .detail = "test adapter"};
  }

  sogou::SogouOptimizationStatus next_status{
      sogou::SogouOptimizationStatus::succeeded};
  std::size_t target_detection_calls{0};
  std::size_t option_detection_calls{0};
  std::size_t execution_calls{0};
  std::optional<sogou::SogouOptimizationAction> last_action;
  std::optional<sogou::SogouCandidateCount> last_value;
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

[[nodiscard]] std::vector<catalog::SoftwareCatalogInstallerBaseline>
matching_installer_baselines() {
  return {{
      .software_item_id = catalog::StableId{"sogou-input"},
      .installer_baseline_id = catalog::StableId{"sogou-stable-windows"},
      .installed_versions = {"14.5", "14.9"},
  }};
}

[[nodiscard]] bool has_issue(
    std::vector<catalog::CatalogIssue> const& issues,
    catalog::CatalogIssueCode code);

[[nodiscard]] bool verify_sogou_catalog_and_capability_contract() {
  LocalSoftwareOptimizationCatalogFile file_adapter;
  auto const file = file_adapter.read(AZZS_SOFTWARE_OPTIMIZATION_CATALOG_PATH);
  bool passed = expect(
      file.succeeded,
      "the first Sogou optimization catalog must be readable offline");
  if (!file.succeeded) {
    return false;
  }

  auto loaded = catalog::load_catalog(
      file.source, sogou::built_in_rule_definitions());
  passed &= expect(loaded.accepted(),
                   "the first Sogou optimization catalog must load formally");
  if (!loaded.catalog.has_value()) {
    return false;
  }

  auto const& optimized = *loaded.catalog;
  passed &= expect(
      optimized.revision == 1 &&
          optimized.publication_state == catalog::PublicationState::release &&
          optimized.targets.size() == 1 && optimized.schemes.size() == 1 &&
          optimized.compatibility_baselines.size() == 1,
      "the first Sogou catalog must contain one released target, scheme, and baseline");
  auto const* target = optimized.find_target("sogou-input-target-v1");
  auto const* scheme = optimized.find_scheme("sogou-input-recommended-v1");
  passed &= expect(
      target != nullptr && target->identity_anchor.value ==
                               "vendor.sogou.input.windows" &&
          target->installation_item_id.has_value() &&
          target->installation_item_id->value == "sogou-input" &&
          target->supported_versions.minimum == "16.7" &&
          target->supported_versions.maximum == "16.7" &&
          target->install_detection.kind == catalog::RuleKind::built_in_definition &&
          target->version_detection.kind == catalog::RuleKind::built_in_definition,
      "the Sogou target must bind the software item and exact 16.7 baseline");
  passed &= expect(
      scheme != nullptr && scheme->automation == catalog::AutomationSupport::controlled &&
          scheme->required_first_release && scheme->options.size() == 25,
      "the first Sogou scheme must expose all 25 controlled options");
  if (target == nullptr || scheme == nullptr) {
    return false;
  }

  auto const& baseline = optimized.compatibility_baselines.front();
  passed &= expect(
      baseline.software_item_id.value == "sogou-input" &&
          baseline.installer_baseline_id.value == "sogou-input-windows-16.7" &&
          baseline.installed_versions.minimum == "16.7" &&
          baseline.installed_versions.maximum == "16.7",
      "the optimization baseline must exactly match the 16.7 installer output");
  std::array const installer_baseline{
      catalog::SoftwareCatalogInstallerBaseline{
          .software_item_id = catalog::StableId{"sogou-input"},
          .installer_baseline_id = catalog::StableId{"sogou-input-windows-16.7"},
          .installed_versions = {"16.7", "16.7"},
      }};
  auto const compatibility =
      catalog::assess_release_compatibility(optimized, installer_baseline);
  passed &= expect(
      compatibility.compatible,
      "the first Sogou catalog must pass its exact installer compatibility gate");

  std::array<bool, 25> seen_actions{};
  std::size_t default_selected_count = 0;
  std::array<std::string_view, 6> expected_defaults{
      "sogou-input-disable-startup-v1",
      "sogou-input-disable-skin-recommendation-v1",
      "sogou-input-disable-skin-popup-recommendation-v1",
      "sogou-input-disable-desktop-recommendation-v1",
      "sogou-input-disable-search-recommendation-v1",
      "sogou-input-disable-ai-startup-v1",
  };
  for (auto const& option : scheme->options) {
    passed &= expect(
        option.automation == catalog::AutomationSupport::controlled &&
            option.execution.kind == catalog::RuleKind::built_in_definition &&
            option.state_detection.kind == catalog::RuleKind::built_in_definition &&
            option.supported_versions.minimum == "16.7" &&
            option.supported_versions.maximum == "16.7",
        "every Sogou option must declare controlled execution and detection for 16.7");
    auto const execution = sogou::map_execution_rule(
        option.execution.definition.value);
    auto const detection = sogou::map_detection_rule(
        option.state_detection.definition.value);
    passed &= expect(execution.has_value() && detection.has_value() &&
                         execution == detection,
                     "every Sogou option must use a registered execution/detection pair");
    if (execution.has_value()) {
      auto const index = static_cast<std::size_t>(*execution);
      passed &= expect(index < seen_actions.size() && !seen_actions[index],
                       "Sogou option actions must form a closed unique set");
      if (index < seen_actions.size()) {
        seen_actions[index] = true;
      }
    }
    if (option.default_selected) {
      ++default_selected_count;
      passed &= expect(
          std::ranges::find(expected_defaults, option.id.value) !=
              expected_defaults.end(),
          "only the confirmed low-impact Sogou options may be selected by default");
    } else {
      passed &= expect(
          std::ranges::find(expected_defaults, option.id.value) ==
              expected_defaults.end(),
          "confirmed low-impact Sogou defaults must be selected in the catalog");
    }
    if (option.id.value == "sogou-input-candidate-count-v1") {
      passed &= expect(
          option.allowed_values == std::vector<std::string>{
              "three", "four", "five", "six", "seven", "eight", "nine"} &&
              option.default_value.has_value() &&
              *option.default_value == "five",
          "candidate count must use the seven-value closed set with five as default");
    } else {
      passed &= expect(option.allowed_values.empty() &&
                           !option.default_value.has_value(),
                       "non-valued Sogou options must not carry free-form parameters");
    }
  }
  passed &= expect(default_selected_count == expected_defaults.size() &&
                       std::ranges::all_of(seen_actions, [](bool seen) { return seen; }),
                   "all 25 Sogou actions and exactly six defaults must be represented");
  passed &= expect(sogou::built_in_rule_definitions().size() == 52,
                   "the built-in registry must include target rules and 25 rule pairs");
  passed &= expect(!sogou::parse_candidate_count("ten").has_value(),
                   "candidate count parser must reject values outside the enum");

  for (auto const& field : std::array<std::string_view, 6>{
           "command", "script", "registry_path", "config_path", "selector",
           "ui_steps"}) {
    auto candidate = file.source;
    candidate.append("\n");
    candidate.append(field);
    candidate.append(field == "ui_steps" ? " = [\"click\"]" :
                                                " = \"arbitrary\"");
    auto rejected = catalog::load_catalog(
        candidate, sogou::built_in_rule_definitions());
    passed &= expect(!rejected.accepted() &&
                         has_issue(rejected.package_issues,
                                   catalog::CatalogIssueCode::unknown_execution_semantics),
                     "arbitrary command, script, path, selector, and UI fields must reject the whole package");
  }

  auto install_preferences =
      install_catalog::initial_controlled_install_profiles();
  auto const install_profile = std::ranges::find(
      install_preferences, "sogou-input-defaults-v1",
      &install_catalog::ControlledInstallProfile::id);
  passed &= expect(install_profile != install_preferences.end(),
                   "the installation profile must remain separately declared");
  if (install_profile != install_preferences.end()) {
    for (auto const& preference : install_profile->preferences) {
      passed &= expect(
          std::ranges::none_of(scheme->options, [&](auto const& option) {
            return option.id.value == preference.id;
          }),
          "installation-stage Sogou preference ids must not be duplicated in optimization options");
    }
  }

  RecordingSogouAdapter adapter;
  sogou::SogouOptimizationService service{adapter, adapter};
  passed &= expect(service.detect_target().status ==
                       sogou::SogouOptimizationStatus::succeeded &&
                       adapter.target_detection_calls == 1,
                   "the application service must expose typed target detection");
  auto const ordinary = std::ranges::find_if(
      scheme->options, [](auto const& option) {
        return option.id.value == "sogou-input-hide-status-bar-v1";
      });
  auto const candidate = std::ranges::find_if(
      scheme->options, [](auto const& option) {
        return option.id.value == "sogou-input-candidate-count-v1";
      });
  passed &= expect(ordinary != scheme->options.end() &&
                       candidate != scheme->options.end(),
                   "contract must find representative ordinary and valued options");
  if (ordinary == scheme->options.end() || candidate == scheme->options.end()) {
    return false;
  }
  passed &= expect(
      service.execute_option(*ordinary).status ==
          sogou::SogouOptimizationStatus::succeeded &&
          adapter.last_action == sogou::SogouOptimizationAction::hide_status_bar &&
          !adapter.last_value.has_value(),
      "ordinary options must reach the typed executor without extra parameters");
  passed &= expect(
      service.execute_option(*candidate).status ==
          sogou::SogouOptimizationStatus::succeeded &&
          adapter.last_action == sogou::SogouOptimizationAction::set_candidate_count &&
          adapter.last_value == sogou::SogouCandidateCount::five,
      "candidate count must use the catalog default through the typed seam");
  auto const calls_before_invalid = adapter.execution_calls;
  passed &= expect(
      service.execute_option(*candidate, "ten").status ==
          sogou::SogouOptimizationStatus::invalid_request &&
          adapter.execution_calls == calls_before_invalid,
      "candidate count ten must be rejected before reaching an executor");
  auto invalid_kind = *ordinary;
  invalid_kind.execution.kind = catalog::RuleKind::none;
  passed &= expect(
      service.detect_option(invalid_kind).status ==
          sogou::SogouOptimizationStatus::invalid_request,
      "an option without a built-in execution definition must be rejected");
  passed &= expect(
      service.detect_option(*ordinary).status ==
          sogou::SogouOptimizationStatus::succeeded &&
          adapter.option_detection_calls == 1,
      "valid option detection must reach the typed detector");
  return passed;
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
  FixedCatalogDebugAuthorization debug_authorization;
  std::vector<catalog::SoftwareCatalogInstallerBaseline> installer_baselines;
  SoftwareOptimizationCatalogLifecycle lifecycle;

  explicit Fixture(
      bool debug_import_allowed = true,
      std::vector<catalog::SoftwareCatalogInstallerBaseline> baselines =
          matching_installer_baselines())
      : debug_authorization(debug_import_allowed),
        installer_baselines(std::move(baselines)),
        lifecycle(states, log, occupancy, catalog_files, debug_authorization,
                  built_in_rules, installer_baselines) {}
};

static_assert(
    !std::is_invocable_v<decltype(&SoftwareOptimizationCatalogLifecycle::apply_update),
                         SoftwareOptimizationCatalogLifecycle&,
                         std::string_view, std::string>,
    "trusted catalog updates must not accept arbitrary local paths");

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

[[nodiscard]] TrustedSoftwareOptimizationCatalogUpdate trusted_update(
    std::string source, std::string reference) {
  return {
      .source = std::move(source),
      .source_reference = std::move(reference),
  };
}

[[nodiscard]] bool has_issue(
    std::vector<catalog::CatalogIssue> const& issues,
    catalog::CatalogIssueCode code) {
  return std::ranges::any_of(issues, [code](catalog::CatalogIssue const& issue) {
    return issue.code == code;
  });
}

[[nodiscard]] bool has_issue_for(
    std::vector<catalog::CatalogIssue> const& issues,
    catalog::CatalogIssueCode code, std::string_view entity_id) {
  return std::ranges::any_of(
      issues, [code, entity_id](catalog::CatalogIssue const& issue) {
        return issue.code == code && issue.entity_id == entity_id;
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

  auto with_structured_display = catalog::load_catalog(
      catalog_source(
          1, false, false, "sogou.reusable.apply", false,
          "display_future_metadata = { title = \"preview\", rank = 2 }\n"
          "description_future_scores = [1, 2, 3]"),
      rules);
  passed &= expect(
      with_structured_display.accepted(),
      "unknown display fields must tolerate valid TOML value shapes outside the execution parser");

  auto with_unknown_semantics = catalog::load_catalog(
      catalog_source(2, false, false, "sogou.reusable.apply", false,
                     "execution_future = \"third-party-plugin\""),
      rules);
  passed &= expect(!with_unknown_semantics.accepted() &&
                       has_issue(with_unknown_semantics.package_issues,
                                 catalog::CatalogIssueCode::
                                     unknown_execution_semantics),
                   "unknown execution semantics must reject the whole package");

  auto with_structured_unknown_semantics = catalog::load_catalog(
      catalog_source(
          2, false, false, "sogou.reusable.apply", false,
          "execution_future = { plugin = \"third-party\", modes = [1, 2] }"),
      rules);
  passed &= expect(
      !with_structured_unknown_semantics.accepted() &&
          has_issue(with_structured_unknown_semantics.package_issues,
                    catalog::CatalogIssueCode::unknown_execution_semantics),
      "unknown execution fields must remain whole-package semantic rejections regardless of value shape");

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

  auto narrow_target = *valid.catalog;
  narrow_target.targets.front().supported_versions.minimum = "14.6";
  auto target_gap =
      catalog::assess_release_compatibility(narrow_target, matching_baseline);
  passed &= expect(
      !target_gap.compatible &&
          has_issue_for(target_gap.issues,
                        catalog::CatalogIssueCode::
                            compatibility_baseline_mismatch,
                        "sogou-input-target"),
      "a first-release target must contain the complete installer output range");

  auto narrow_scheme = *valid.catalog;
  narrow_scheme.schemes.front().supported_versions.minimum = "14.6";
  auto scheme_gap =
      catalog::assess_release_compatibility(narrow_scheme, matching_baseline);
  passed &= expect(
      !scheme_gap.compatible &&
          has_issue_for(scheme_gap.issues,
                        catalog::CatalogIssueCode::
                            compatibility_baseline_mismatch,
                        "sogou.recommended"),
      "each first-release scheme must contain the complete installer output range");

  auto narrow_option = *valid.catalog;
  narrow_option.schemes.front().options.front().supported_versions.maximum =
      "14.8";
  auto option_gap =
      catalog::assess_release_compatibility(narrow_option, matching_baseline);
  passed &= expect(
      !option_gap.compatible &&
          has_issue_for(option_gap.issues,
                        catalog::CatalogIssueCode::
                            compatibility_baseline_mismatch,
                        "sogou.hide-statusbar"),
      "each option in a first-release scheme must contain the complete installer output range");

  auto const identity_history = catalog::stable_identities(*valid.catalog);
  auto rejects_scheme_behavior_change = [&](auto mutate,
                                             char const* message) {
    auto changed = *valid.catalog;
    mutate(changed.schemes.front());
    passed &= expect(
        has_issue(catalog::validate_stable_identity_history(
                      changed, identity_history),
                  catalog::CatalogIssueCode::stable_id_reuse),
        message);
  };
  rejects_scheme_behavior_change(
      [](catalog::SoftwareOptimizationScheme& scheme) {
        scheme.supported_versions.maximum = "15.8";
      },
      "a scheme id must not be reused with a different supported version range");
  rejects_scheme_behavior_change(
      [](catalog::SoftwareOptimizationScheme& scheme) {
        scheme.exit_requirement = catalog::ExitRequirement::none;
      },
      "a scheme id must not be reused with a different exit requirement");
  rejects_scheme_behavior_change(
      [](catalog::SoftwareOptimizationScheme& scheme) {
        scheme.restart_requirement = catalog::RestartRequirement::windows;
      },
      "a scheme id must not be reused with a different restart requirement");
  rejects_scheme_behavior_change(
      [](catalog::SoftwareOptimizationScheme& scheme) {
        scheme.required_scheme_ids.push_back(catalog::StableId{"sogou.extra"});
      },
      "a scheme id must not be reused with different dependencies");
  rejects_scheme_behavior_change(
      [](catalog::SoftwareOptimizationScheme& scheme) {
        scheme.options.front().default_selected =
            !scheme.options.front().default_selected;
      },
      "a scheme id must not be reused with different option behavior");
  rejects_scheme_behavior_change(
      [](catalog::SoftwareOptimizationScheme& scheme) {
        scheme.risk = catalog::RiskLevel::high;
      },
      "a scheme id must not be reused with a different risk");
  rejects_scheme_behavior_change(
      [](catalog::SoftwareOptimizationScheme& scheme) {
        scheme.target_id = catalog::StableId{"another-target"};
      },
      "a scheme id must not be reused for a different target");
  rejects_scheme_behavior_change(
      [](catalog::SoftwareOptimizationScheme& scheme) {
        scheme.automation = catalog::AutomationSupport::manual_only;
      },
      "a scheme id must not be reused with a different automation mode");

  auto display_only_change = *valid.catalog;
  auto& display_scheme = display_only_change.schemes.front();
  display_scheme.impact = "更新后的展示影响";
  display_scheme.explanation_source = "https://example.invalid/new-source";
  display_scheme.manual_emergency_explanation = "更新后的手动应急说明";
  display_scheme.options.front().impact = "更新后的选项展示影响";
  display_scheme.options.front().explanation_source =
      "https://example.invalid/new-option-source";
  passed &= expect(
      !has_issue(catalog::validate_stable_identity_history(
                     display_only_change, identity_history),
                 catalog::CatalogIssueCode::stable_id_reuse),
      "explicit display-only scheme fields may change without reusing behavior identity");
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

  auto updated = fixture.lifecycle.apply_update(
      trusted_update(catalog_source(2, true), "published-v2"), "update-2");
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

  fixture.state_files.fail_next(
      azzs::testing::StateFileOperation::write,
      azzs::application::StateFileSlot::candidate);
  auto write_failed = fixture.lifecycle.apply_update(
      trusted_update(catalog_source(3, true), "published-write-failure-v3"),
      "update-write-failure");
  passed &= expect(
      write_failed.code ==
              SoftwareOptimizationCatalogLifecycleCode::persistence_failed &&
          fixture.lifecycle.snapshot().current->revision == 2,
      "persistence failure must retain the previous authoritative catalog");

  fixture.catalog_files.files["lower-v1.toml"] = catalog_source(1);
  auto normal_downgrade = fixture.lifecycle.apply_update(
      trusted_update(catalog_source(1), "published-lower-v1"),
      "update-lower");
  passed &= expect(normal_downgrade.code ==
                           SoftwareOptimizationCatalogLifecycleCode::rejected &&
                       fixture.lifecycle.snapshot().current->revision == 2,
                   "normal update must reject downgrade and retain the active version");

  Fixture denied_fixture{false};
  denied_fixture.catalog_files.files["lower-v1.toml"] = catalog_source(1);
  auto denied_preview =
      denied_fixture.lifecycle.preview_manual_import("lower-v1.toml");
  passed &= expect(denied_preview.code ==
                       SoftwareOptimizationCatalogLifecycleCode::debug_mode_required,
                   "manual import preview must be debug-mode only");
  auto preview = fixture.lifecycle.preview_manual_import("lower-v1.toml");
  passed &= expect(preview.code ==
                           SoftwareOptimizationCatalogLifecycleCode::preview_ready &&
                       preview.path == "lower-v1.toml" &&
                       preview.candidate.has_value() &&
                       preview.candidate->revision == 1 && preview.downgrade &&
                       has_id(preview.lost_or_changed_schemes, "sogou.extra"),
                   "downgrade preview must show path, revision, counts, and lost schemes");
  auto unconfirmed = fixture.lifecycle.apply_manual_import(
      "lower-v1.toml", preview.preview_token, false, "import-unconfirmed");
  passed &= expect(unconfirmed.code ==
                       SoftwareOptimizationCatalogLifecycleCode::confirmation_required,
                   "manual import must not apply before explicit confirmation");
  auto stale = fixture.lifecycle.apply_manual_import(
      "lower-v1.toml", "0000000000000000", true, "import-stale");
  passed &= expect(stale.code ==
                       SoftwareOptimizationCatalogLifecycleCode::preview_stale,
                   "manual import must re-read and bind confirmation to preview bytes");
  auto downgraded = fixture.lifecycle.apply_manual_import(
      "lower-v1.toml", preview.preview_token, true, "import-downgrade");
  passed &= expect(downgraded.code ==
                           SoftwareOptimizationCatalogLifecycleCode::downgraded &&
                       downgraded.state_changed &&
                       fixture.lifecycle.snapshot().current->revision == 1,
                   "confirmed debug import may install a lower valid revision");
  auto rollback_preview = fixture.lifecycle.preview_rollback();
  passed &= expect(
      rollback_preview.code ==
              SoftwareOptimizationCatalogLifecycleCode::preview_ready &&
          rollback_preview.candidate.has_value() &&
          rollback_preview.candidate->revision == 2,
      "rollback must preview the retained candidate before confirmation");
  auto rolled_back = fixture.lifecycle.rollback(
      rollback_preview.preview_token, true, "rollback-2");
  passed &= expect(rolled_back.code ==
                           SoftwareOptimizationCatalogLifecycleCode::rolled_back &&
                       rolled_back.active.has_value() &&
                       rolled_back.active->revision == 2 &&
                       fixture.lifecycle.snapshot().current->revision == 2,
                   "rollback must atomically swap to the retained valid version");

  SoftwareOptimizationCatalogLifecycle restarted{
      fixture.states, fixture.log, fixture.occupancy, fixture.catalog_files,
      fixture.debug_authorization, fixture.built_in_rules,
      fixture.installer_baselines};
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

[[nodiscard]] bool verify_downgrade_disclosure_rejects_field_boundary_collision() {
  auto loaded =
      catalog::load_catalog(catalog_source(2), built_in_rule_definitions());
  bool passed = expect(loaded.catalog.has_value(),
                       "collision disclosure fixture must load");
  if (!loaded.catalog.has_value()) {
    return false;
  }

  auto current = *loaded.catalog;
  auto candidate = current;
  candidate.revision = 1;
  current.schemes.front().explanation_source = "https://x/a|b";
  current.schemes.front().manual_emergency_explanation = "c";
  candidate.schemes.front().explanation_source = "https://x/a";
  candidate.schemes.front().manual_emergency_explanation = "b|c";

  passed &= expect(
      has_id(catalog::schemes_lost_or_changed(current, candidate),
             "sogou.recommended"),
      "downgrade disclosure must not hide changes behind field-boundary collisions");
  return passed;
}

[[nodiscard]] bool verify_identity_history_capacity() {
  constexpr std::size_t kMaximumIdentityHistory = 100'000;
  std::vector<catalog::StableIdentityRecord> history;
  history.reserve(kMaximumIdentityHistory);
  for (std::size_t index = 0; index < kMaximumIdentityHistory; ++index) {
    history.push_back(catalog::StableIdentityRecord{
        .id = catalog::StableId{"history-" + std::to_string(index)},
        .kind = catalog::StableEntityKind::target,
        .semantic_fingerprint = "reserved",
    });
  }
  catalog::SoftwareOptimizationCatalog candidate;
  candidate.targets.push_back(catalog::TargetSoftware{
      .id = catalog::StableId{"new-target"},
      .identity_anchor = catalog::StableId{"new-target-anchor"},
  });
  auto below_capacity = history;
  below_capacity.pop_back();
  auto boundary = catalog::merge_stable_identity_history(
      below_capacity, candidate, kMaximumIdentityHistory);
  bool passed = expect(
      boundary.has_value() && boundary->size() == kMaximumIdentityHistory,
      "identity history may grow exactly to the persisted limit");
  auto overflow = catalog::merge_stable_identity_history(
      history, candidate, kMaximumIdentityHistory);
  passed &= expect(
      !overflow.has_value(),
      "identity history growth beyond the persisted limit must fail before commit");
  return passed;
}

[[nodiscard]] bool verify_manual_import_preview_rejects_concurrent_change() {
  Fixture fixture;
  bool passed = expect(
      fixture.lifecycle.ensure_builtin(catalog_source(1), "preview-base")
          .state_changed,
      "preview concurrency fixture must establish revision one");

  fixture.catalog_files.files["manual-v2.toml"] = catalog_source(2);
  auto preview =
      fixture.lifecycle.preview_manual_import("manual-v2.toml");
  passed &= expect(
      preview.code == SoftwareOptimizationCatalogLifecycleCode::preview_ready,
      "first instance must produce a manual import preview");

  SoftwareOptimizationCatalogLifecycle second_instance{
      fixture.states, fixture.log, fixture.occupancy, fixture.catalog_files,
      fixture.debug_authorization, fixture.built_in_rules,
      fixture.installer_baselines};
  auto concurrent = second_instance.apply_update(
      trusted_update(catalog_source(3), "published-concurrent-v3"),
      "preview-concurrent");
  passed &= expect(concurrent.state_changed && concurrent.active.has_value() &&
                       concurrent.active->revision == 3,
                   "second instance must commit a newer catalog");

  auto stale = fixture.lifecycle.apply_manual_import(
      "manual-v2.toml", preview.preview_token, true,
      "preview-stale-after-concurrent-change");
  auto snapshot = fixture.lifecycle.snapshot();
  passed &= expect(
      stale.code == SoftwareOptimizationCatalogLifecycleCode::preview_stale &&
          !stale.state_changed && snapshot.current.has_value() &&
          snapshot.current->revision == 3,
      "a preview from an older current revision must not overwrite another instance");

  Fixture same_candidate_fixture;
  passed &= expect(
      same_candidate_fixture.lifecycle
          .ensure_builtin(catalog_source(1), "same-preview-base")
          .state_changed,
      "same-candidate concurrency fixture must establish revision one");
  same_candidate_fixture.catalog_files.files["same-manual-v2.toml"] =
      catalog_source(2);
  auto same_preview = same_candidate_fixture.lifecycle.preview_manual_import(
      "same-manual-v2.toml");
  SoftwareOptimizationCatalogLifecycle same_second_instance{
      same_candidate_fixture.states, same_candidate_fixture.log,
      same_candidate_fixture.occupancy, same_candidate_fixture.catalog_files,
      same_candidate_fixture.debug_authorization,
      same_candidate_fixture.built_in_rules,
      same_candidate_fixture.installer_baselines};
  auto same_concurrent = same_second_instance.apply_update(
      trusted_update(catalog_source(2), "published-same-v2"),
      "same-preview-concurrent");
  passed &= expect(same_concurrent.state_changed,
                   "second instance must commit the previewed revision");
  auto same_stale = same_candidate_fixture.lifecycle.apply_manual_import(
      "same-manual-v2.toml", same_preview.preview_token, true,
      "same-preview-stale");
  passed &= expect(
      same_stale.code == SoftwareOptimizationCatalogLifecycleCode::preview_stale,
      "even an identical candidate requires a new preview after current state changes");
  return passed;
}

[[nodiscard]] bool verify_rollback_preview_rejects_concurrent_change() {
  Fixture fixture;
  bool passed = expect(
      fixture.lifecycle.ensure_builtin(catalog_source(1), "rollback-base")
          .state_changed,
      "rollback concurrency fixture must establish revision one");
  auto revision_two = fixture.lifecycle.apply_update(
      trusted_update(catalog_source(2), "published-rollback-v2"),
      "rollback-v2");
  passed &= expect(revision_two.state_changed,
                   "rollback concurrency fixture must establish revision two");
  auto preview = fixture.lifecycle.preview_rollback();
  passed &= expect(
      preview.code == SoftwareOptimizationCatalogLifecycleCode::preview_ready &&
          preview.candidate.has_value() && preview.candidate->revision == 1,
      "first instance must preview rollback to revision one");

  SoftwareOptimizationCatalogLifecycle second_instance{
      fixture.states, fixture.log, fixture.occupancy, fixture.catalog_files,
      fixture.debug_authorization, fixture.built_in_rules,
      fixture.installer_baselines};
  auto concurrent = second_instance.apply_update(
      trusted_update(catalog_source(3), "published-rollback-v3"),
      "rollback-concurrent-v3");
  passed &= expect(concurrent.state_changed,
                   "second instance must establish revision three");

  auto stale = fixture.lifecycle.rollback(
      preview.preview_token, true,
      "rollback-stale-after-concurrent-change");
  auto snapshot = fixture.lifecycle.snapshot();
  passed &= expect(
      stale.code == SoftwareOptimizationCatalogLifecycleCode::preview_stale &&
          !stale.state_changed && snapshot.current.has_value() &&
          snapshot.current->revision == 3,
      "an old rollback preview must not roll back another instance's commit");
  return passed;
}

[[nodiscard]] bool verify_debug_authorization_provenance_and_release_gate() {
  Fixture fixture;
  auto const local_path =
      std::string{"C:\\Users\\Alice\\private-software-optimization.toml"};
  fixture.catalog_files.files[local_path] = catalog_source(
      1, false, false, "sogou.reusable.apply", false, {}, "draft");
  auto preview = fixture.lifecycle.preview_manual_import(local_path);
  bool passed = expect(
      preview.code == SoftwareOptimizationCatalogLifecycleCode::preview_ready,
      "authorized local debug import may preview a runtime-valid draft");
  auto imported = fixture.lifecycle.apply_manual_import(
      local_path, preview.preview_token, true, "provenance-local-import");
  auto local_snapshot = fixture.lifecycle.snapshot();
  passed &= expect(
      imported.state_changed && local_snapshot.current_provenance.has_value() &&
          local_snapshot.current_provenance->kind ==
              SoftwareOptimizationCatalogSourceKind::local_debug_import &&
          local_snapshot.current_provenance->local_trial &&
          local_snapshot.current_provenance->redacted_source.find("Alice") ==
              std::string::npos,
      "local debug imports must persist a redacted local-trial identity");

  SoftwareOptimizationCatalogLifecycle restarted{
      fixture.states, fixture.log, fixture.occupancy, fixture.catalog_files,
      fixture.debug_authorization, fixture.built_in_rules,
      fixture.installer_baselines};
  auto restarted_snapshot = restarted.snapshot();
  passed &= expect(
      restarted_snapshot.current_provenance ==
          local_snapshot.current_provenance,
      "local-trial provenance must survive lifecycle reconstruction");

  auto draft_update = restarted.apply_update(
      trusted_update(catalog_source(2, false, false, "sogou.reusable.apply",
                                    false, {}, "draft"),
                     "draft-update-v2"),
      "provenance-draft-update");
  passed &= expect(
      draft_update.code == SoftwareOptimizationCatalogLifecycleCode::rejected &&
          restarted.snapshot().current->revision == 1,
      "trusted updates must pass the formal release gate");

  auto const trusted_reference =
      std::string{"https://updates.example.invalid/catalog-v2?token=secret"};
  auto formal_update = restarted.apply_update(
      trusted_update(catalog_source(2), trusted_reference),
      "provenance-formal-update");
  auto formal_snapshot = restarted.snapshot();
  passed &= expect(
      formal_update.state_changed &&
          formal_snapshot.current_provenance.has_value() &&
          formal_snapshot.current_provenance->kind ==
              SoftwareOptimizationCatalogSourceKind::trusted_update &&
          !formal_snapshot.current_provenance->local_trial &&
          formal_snapshot.current_provenance->redacted_source.find("secret") ==
              std::string::npos,
      "trusted updates must replace local-trial identity with redacted formal provenance");

  auto rollback_preview = restarted.preview_rollback();
  passed &= expect(
      rollback_preview.candidate_provenance.has_value() &&
          rollback_preview.candidate_provenance->local_trial,
      "rollback preview must expose that the retained candidate is a local trial");
  auto rolled_back = restarted.rollback(
      rollback_preview.preview_token, true, "provenance-rollback");
  auto rolled_back_snapshot = restarted.snapshot();
  passed &= expect(
      rolled_back.state_changed &&
          rolled_back_snapshot.current_provenance.has_value() &&
          rolled_back_snapshot.current_provenance->local_trial,
      "rollback must restore the retained source identity together with its catalog");

  std::vector incompatible_baseline{
      catalog::SoftwareCatalogInstallerBaseline{
          .software_item_id = catalog::StableId{"sogou-input"},
          .installer_baseline_id = catalog::StableId{"sogou-stable-windows"},
          .installed_versions = {"16.0", "16.9"},
      }};
  Fixture incompatible_fixture{true, std::move(incompatible_baseline)};
  auto formal_rejected = incompatible_fixture.lifecycle.ensure_builtin(
      catalog_source(1), "incompatible-formal-builtin");
  passed &= expect(
      formal_rejected.code == SoftwareOptimizationCatalogLifecycleCode::rejected &&
          !incompatible_fixture.lifecycle.snapshot().current.has_value(),
      "formal built-in catalogs must consume the installer compatibility gate");
  incompatible_fixture.catalog_files.files["local-incompatible.toml"] =
      catalog_source(1);
  auto local_incompatible_preview =
      incompatible_fixture.lifecycle.preview_manual_import(
          "local-incompatible.toml");
  auto local_incompatible = incompatible_fixture.lifecycle.apply_manual_import(
      "local-incompatible.toml", local_incompatible_preview.preview_token, true,
      "incompatible-local-trial");
  passed &= expect(
      local_incompatible.state_changed &&
          incompatible_fixture.lifecycle.snapshot()
              .current_provenance->local_trial,
      "runtime-valid local trials must remain distinct from formal release approval");
  return passed;
}

[[nodiscard]] bool verify_rejection_identity_history_and_occupancy() {
  Fixture fixture;
  bool passed = true;
  auto initial = fixture.lifecycle.ensure_builtin(
      catalog_source(1, true), "identity-initial");
  passed &= expect(initial.state_changed,
                   "identity history fixture must establish revision one");

  auto removed = fixture.lifecycle.apply_update(
      trusted_update(catalog_source(2), "published-identity-v2"),
      "identity-remove");
  passed &= expect(removed.state_changed && removed.active->revision == 2,
                   "a later version may remove a stable item");
  auto reused_source = catalog_source(3, true);
  auto const original_rule = std::string{"sogou.extra.apply"};
  reused_source.replace(reused_source.find(original_rule), original_rule.size(),
                        "sogou.reusable.apply-a");
  auto reused = fixture.lifecycle.apply_update(
      trusted_update(std::move(reused_source), "published-identity-v3"),
      "identity-reuse");
  passed &= expect(reused.code ==
                           SoftwareOptimizationCatalogLifecycleCode::rejected &&
                       has_issue(reused.issues,
                                 catalog::CatalogIssueCode::stable_id_reuse) &&
                       fixture.lifecycle.snapshot().current->revision == 2,
                   "removed stable ids must remain reserved against semantic reuse");

  auto unknown = fixture.lifecycle.apply_update(
      trusted_update(
          catalog_source(3, false, false, "sogou.reusable.apply", false,
                         "execution_plugin = \"load-library\""),
          "published-unknown-v3"),
      "unknown-semantics");
  passed &= expect(unknown.code ==
                           SoftwareOptimizationCatalogLifecycleCode::rejected &&
                       fixture.lifecycle.snapshot().current->revision == 2,
                   "whole-package rejection must retain the last valid catalog");

  auto isolated = fixture.lifecycle.apply_update(
      trusted_update(
          catalog_source(3, true, false, "sogou.reusable.apply", false, {},
                         "release", true),
          "published-isolated-v3"),
      "isolated-error");
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
  auto blocked = fixture.lifecycle.apply_update(
      trusted_update(catalog_source(4, true), "published-blocked-v4"),
      "blocked-update");
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
  passed &= verify_sogou_catalog_and_capability_contract();
  passed &= verify_domain_validation_and_compatibility();
  passed &= verify_lifecycle_import_downgrade_and_rollback();
  passed &= verify_downgrade_disclosure_rejects_field_boundary_collision();
  passed &= verify_identity_history_capacity();
  passed &= verify_manual_import_preview_rejects_concurrent_change();
  passed &= verify_rollback_preview_rejects_concurrent_change();
  passed &= verify_debug_authorization_provenance_and_release_gate();
  passed &= verify_rejection_identity_history_and_occupancy();
  passed &= verify_unavailable_and_logging_fail_closed();
  passed &= verify_local_file_adapter_contract();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "software optimization catalog contract passed\n";
  return EXIT_SUCCESS;
}
