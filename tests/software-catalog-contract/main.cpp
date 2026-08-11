#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/adapters/infrastructure/software_catalog_file.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

using namespace std::chrono_literals;
namespace catalog = azzs::domain::software_catalog;
namespace lifecycle = azzs::application::software_catalog;
using azzs::adapters::infrastructure::LocalSoftwareCatalogFileReader;
using azzs::adapters::infrastructure::TomlSoftwareCatalogCodec;
using azzs::application::CorrelationId;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogReceipt;
using azzs::application::SharedOperationOccupancy;
using azzs::application::WallClockTime;
using azzs::application::DeviceStateStore;
using azzs::domain::StateSubject;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryOperationOccupancyStorage;
using azzs::testing::InMemoryStateFileSystem;
using azzs::testing::SequenceLeaseTokenSource;
using azzs::testing::StateFileOperation;

[[nodiscard]] bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "software catalog contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] bool has_issue(
    std::vector<catalog::CatalogIssue> const& issues,
    catalog::CatalogIssueCode code,
    std::optional<catalog::CatalogIssueScope> scope = std::nullopt) {
  return std::ranges::any_of(issues, [&](catalog::CatalogIssue const& issue) {
    return issue.code == code && (!scope.has_value() || issue.scope == *scope);
  });
}

[[nodiscard]] bool contains(std::vector<std::string> const& values,
                            std::string_view value) {
  return std::ranges::find(values, value) != values.end();
}

[[nodiscard]] bool has_impact(
    lifecycle::CatalogSelectionImpact const& impact, std::string_view id,
    lifecycle::CatalogSelectionImpactReason reason) {
  return std::ranges::any_of(
      impact.items, [&](lifecycle::CatalogSelectionImpactItem const& item) {
        return item.id == id && item.reason == reason;
      });
}

[[nodiscard]] std::string header(std::uint64_t revision,
                                 std::string_view release_state) {
  return "schema_version = 1\n"
         "catalog_id = \"software\"\n"
         "revision = " + std::to_string(revision) + "\n"
         "release_state = \"" + std::string{release_state} + "\"\n"
         "default_locale = \"zh-CN\"\n\n"
         "[[categories]]\n"
         "id = \"tools\"\n"
         "name = \"工具\"\n";
}

[[nodiscard]] std::string software_entry(
    std::string_view id, std::string_view name,
    std::string_view dependencies = "[]", bool enabled = true,
    std::string_view extra_fields = {}, std::string_view notice = {}) {
  std::string value = "\n[[software]]\nid = \"" + std::string{id} +
                      "\"\nenabled = " +
                      (enabled ? "true\n" : "false\n");
  if (!enabled) {
    return value;
  }
  value += "name = \"" + std::string{name} + "\"\n"
           "tier = \"normal\"\n"
           "category_id = \"tools\"\n"
           "branch = \"Windows stable\"\n"
           "version_policy = \"latest_stable\"\n"
           "dependencies = " + std::string{dependencies} + "\n"
           "bundled_editions = []\n"
           "notice = \"" + std::string{notice} + "\"\n";
  value += extra_fields;
  value += "\n[[software.sources]]\n"
           "purpose = \"primary\"\n"
           "address = \"https://example.test/" + std::string{id} + "\"\n";
  return value;
}

[[nodiscard]] std::string driver_entry(std::string_view id,
                                       std::string_view name) {
  return "\n[[drivers]]\n"
         "id = \"" + std::string{id} + "\"\n"
         "enabled = true\n"
         "name = \"" + std::string{name} + "\"\n"
         "entry_type = \"vendor_page\"\n"
         "hardware_kinds = [\"gpu\"]\n"
         "branch = \"vendor portal\"\n"
         "version_policy = \"maintainer_provided\"\n"
         "notice = \"\"\n\n"
         "[[drivers.sources]]\n"
         "purpose = \"primary\"\n"
         "address = \"https://example.test/" + std::string{id} + "\"\n";
}

[[nodiscard]] std::string one_item_catalog(
    std::uint64_t revision, std::string_view release_state,
    std::string_view name = "Core", std::string_view dependencies = "[]",
    std::string_view extra_fields = {}, std::string_view notice = {}) {
  return header(revision, release_state) +
         software_entry("core", name, dependencies, true, extra_fields,
                        notice);
}

[[nodiscard]] std::string replace_once(std::string value,
                                       std::string_view from,
                                       std::string_view to) {
  auto const position = value.find(from);
  if (position == std::string::npos) {
    std::abort();
  }
  value.replace(position, from.size(), to);
  return value;
}

[[nodiscard]] catalog::SoftwareCatalogPolicy default_policy() {
  return catalog::SoftwareCatalogPolicy{
      .supported_schema_version = 1,
      .install_profiles = {{.id = "profile-v1",
                            .software_ids = {"core"},
                            .runtime_status =
                                catalog::InstallProfileRuntimeStatus::available,
                            .release_ready = true}},
      .supported_driver_hardware_kinds = {"gpu"},
  };
}

class RecordingExecutionLog final : public ExecutionLog {
 public:
  [[nodiscard]] CorrelationId begin_correlation() override {
    return CorrelationId{"catalog-test-" + std::to_string(next_++)};
  }

  [[nodiscard]] ExecutionLogReceipt append(
      CorrelationId const& correlation, ExecutionEvent const& event) override {
    events.push_back(event);
    correlations.push_back(correlation.value);
    if (append_failure_.has_value()) {
      if (append_failure_->successful_appends_before_failure == 0) {
        auto error = std::move(append_failure_->error);
        append_failure_.reset();
        return {.persisted = false, .error = std::move(error)};
      }
      --append_failure_->successful_appends_before_failure;
    }
    return {.persisted = true, .segment = 1, .sequence = next_++};
  }

  void fail_next_append(std::string error) {
    fail_after_appends(0, std::move(error));
  }

  void fail_after_appends(std::size_t successful_appends,
                          std::string error) {
    append_failure_ = PendingAppendFailure{
        .successful_appends_before_failure = successful_appends,
        .error = std::move(error),
    };
  }

  [[nodiscard]] ExecutionLogClearReceipt clear() override {
    events.clear();
    correlations.clear();
    return {.cleared = true};
  }

  [[nodiscard]] DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const&) override {
    return {.produced = true};
  }

  std::vector<ExecutionEvent> events;
  std::vector<std::string> correlations;

 private:
  struct PendingAppendFailure final {
    std::size_t successful_appends_before_failure{};
    std::string error;
  };

  std::optional<PendingAppendFailure> append_failure_;
  std::uint64_t next_{1};
};

class MemoryCatalogFiles final : public lifecycle::SoftwareCatalogFileReader {
 public:
  [[nodiscard]] lifecycle::CatalogFileRead read(
      std::string const& path) const override {
    auto const found = files.find(path);
    if (found == files.end()) {
      return {.path = path, .error = "fixture file is absent"};
    }
    return {.succeeded = true, .path = path, .bytes = found->second};
  }

  std::map<std::string, std::string> files;
};

struct LifecycleFixture final {
  LifecycleFixture()
      : states(state_files, clock),
        tokens("catalog-lease-"),
        occupancy(occupancy_storage, tokens),
        policy(default_policy()),
        catalog_lifecycle(states, log, occupancy, files, codec, policy,
                          StateSubject{"catalog-test-user"}) {}

  InMemoryStateFileSystem state_files;
  FixedClock clock{WallClockTime{1234ms}};
  DeviceStateStore states;
  InMemoryOperationOccupancyStorage occupancy_storage;
  SequenceLeaseTokenSource tokens;
  SharedOperationOccupancy occupancy;
  RecordingExecutionLog log;
  MemoryCatalogFiles files;
  TomlSoftwareCatalogCodec codec;
  catalog::SoftwareCatalogPolicy policy;
  lifecycle::SoftwareCatalogLifecycle catalog_lifecycle;
};

[[nodiscard]] bool content_identity_is_sha256() {
  bool passed = true;
  passed &= expect(
      catalog::content_identity("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "empty catalog identity must match the SHA-256 standard vector");
  passed &= expect(
      catalog::content_identity("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "catalog identity must match the SHA-256 abc standard vector");
  return passed;
}

[[nodiscard]] bool authoritative_catalog_loads_offline() {
  LocalSoftwareCatalogFileReader reader;
  TomlSoftwareCatalogCodec codec;
  auto const file = reader.read(AZZS_SOFTWARE_CATALOG_PATH);
  bool passed = true;
  passed &= expect(file.succeeded,
                   "the authoritative TOML must be readable without network");
  if (!file.succeeded) {
    return false;
  }
  auto decoded = codec.decode(file.bytes);
  passed &= expect(decoded.document.has_value() && decoded.issues.empty(),
                   "the authoritative TOML must satisfy the codec schema");
  if (!decoded.document.has_value() || !decoded.issues.empty()) {
    return false;
  }
  auto policy = default_policy();
  policy.install_profiles.push_back(
      {.id = "sogou-input-defaults-v1",
       .software_ids = {"sogou-input"},
       .runtime_status = catalog::InstallProfileRuntimeStatus::available,
       .release_ready = false});
  policy.required_install_profiles.push_back(
      {.software_id = "sogou-input",
       .profile_id = "sogou-input-defaults-v1"});
  auto runtime = catalog::validate_for_runtime(*decoded.document, policy);
  auto gate = catalog::evaluate_release_gate(
      *decoded.document, runtime, policy,
      catalog::content_identity(file.bytes));
  passed &= expect(runtime.accepted() && runtime.catalog.has_value(),
                   "the draft authoritative catalog must runtime-load");
  passed &= expect(runtime.catalog.has_value() &&
                       runtime.catalog->software.size() == 2 &&
                       runtime.catalog->drivers.empty(),
                   "disabled maintenance entries must stay out of runtime data");
  passed &= expect(!gate.passed() &&
                       has_issue(gate.issues,
                                 catalog::CatalogIssueCode::draft_release_state,
                                 catalog::CatalogIssueScope::release),
                   "runtime success must not turn a draft into a release");

  auto const encoded = codec.encode(*decoded.document);
  auto round_trip = codec.decode(encoded);
  passed &= expect(round_trip.issues.empty() &&
                       round_trip.document == decoded.document,
                   "the authoritative domain document must round-trip");
  passed &= expect(!reader.read(std::string{AZZS_SOFTWARE_CATALOG_PATH} +
                                ".missing")
                        .succeeded,
                   "file adapter failures must be explicit");
  return passed;
}

[[nodiscard]] bool toml_shape_round_trips_software_and_drivers() {
  constexpr std::string_view fixture = R"toml(schema_version = 1
catalog_id = "software"
revision = 7
release_state = "release"
default_locale = "zh-CN"
x_display_banner = "预览"

[[categories]]
id = "tools"
name = "工具"
display_hint = ["推荐", "稳定"]

[categories.localizations."en-US"]
name = "Tools"

[[software]]
id = "unicode-tool"
enabled = true
name = "工具\n套件"
tier = "normal"
category_id = "tools"
branch = "Windows 个人版"
version_policy = "latest_stable_with_history"
dependencies = []
bundled_editions = ["large_offline"]
notice = 'literal # text'
display_badge = "推荐"

[[software.sources]]
purpose = "primary"
address = "https://example.test/tool"

[[software.sources.history]]
version = "1.0"
address = "https://example.test/tool/1.0"
reason = "兼容旧系统"
visible = false

[software.education]
address = "https://example.test/education"
description = "教育版说明"

[software.localizations."en-US"]
name = "Unicode Tool"
notice = "Notice"

[[drivers]]
id = "vendor-gpu"
enabled = true
name = "显卡厂商页"
entry_type = "vendor_page"
hardware_kinds = ["gpu"]
branch = "official portal"
version_policy = "maintainer_provided"
notice = "由厂商页面处理"

[[drivers.sources]]
purpose = "primary"
address = "https://example.test/gpu"

[drivers.localizations."en-US"]
name = "GPU vendor page"
)toml";

  TomlSoftwareCatalogCodec codec;
  auto decoded = codec.decode(fixture);
  bool passed = true;
  passed &= expect(decoded.issues.empty() && decoded.document.has_value(),
                   "software and driver TOML must decode together");
  if (!decoded.document.has_value()) {
    return false;
  }
  auto runtime = catalog::validate_for_runtime(*decoded.document,
                                                default_policy());
  passed &= expect(runtime.accepted() && runtime.catalog.has_value() &&
                       runtime.catalog->software.size() == 1 &&
                       runtime.catalog->drivers.size() == 1,
                   "both entry kinds must enter one runtime package");
  passed &= expect(decoded.document->software.front().name == "工具\n套件" &&
                       !decoded.document->software.front()
                            .sources.front()
                            .history.front()
                            .visible &&
                       !decoded.document->display_extensions.empty(),
                   "Unicode, escapes, history and display extensions must survive");
  auto encoded = codec.encode(*decoded.document);
  auto again = codec.decode(encoded);
  passed &= expect(again.issues.empty() && again.document == decoded.document,
                   "canonical TOML must preserve domain semantics exactly");
  return passed;
}

[[nodiscard]] bool package_errors_and_disabled_minimum_are_separate() {
  TomlSoftwareCatalogCodec codec;
  auto policy = default_policy();
  bool passed = true;

  auto future = codec.decode(replace_once(one_item_catalog(1, "release"),
                                          "schema_version = 1",
                                          "schema_version = 2"));
  auto future_runtime = catalog::validate_for_runtime(*future.document, policy);
  passed &= expect(!future_runtime.accepted() &&
                       has_issue(future_runtime.issues,
                                 catalog::CatalogIssueCode::unsupported_schema),
                   "unknown schema versions must reject the whole package");

  auto unknown = codec.decode(replace_once(
      one_item_catalog(1, "release"), "notice = \"\"",
      "executor_command = \"run anything\"\nnotice = \"\""));
  passed &= expect(has_issue(
                       unknown.issues,
                       catalog::CatalogIssueCode::unknown_execution_semantics),
                   "unknown operational fields must fail closed");

  auto unknown_table = codec.decode(one_item_catalog(1, "release") +
                                    "\n[software.executor]\nmode = \"raw\"\n");
  passed &= expect(has_issue(
                       unknown_table.issues,
                       catalog::CatalogIssueCode::unknown_execution_semantics),
                   "unknown operational tables must fail closed");

  auto non_http = codec.decode(replace_once(
      one_item_catalog(1, "release"), "https://example.test/core",
      "ftp://example.test/core"));
  auto non_http_runtime =
      catalog::validate_for_runtime(*non_http.document, policy);
  passed &= expect(!non_http_runtime.accepted() &&
                       has_issue(non_http_runtime.issues,
                                 catalog::CatalogIssueCode::invalid_field),
                   "non-HTTP sources must reject the package");
  auto empty_authority = codec.decode(replace_once(
      one_item_catalog(1, "release"), "https://example.test/core",
      "https://?resource"));
  passed &= expect(!catalog::validate_for_runtime(*empty_authority.document,
                                                   policy)
                        .accepted(),
                   "HTTP sources must contain a non-empty authority");

  auto duplicate_text = one_item_catalog(1, "release") +
                        driver_entry("core", "Duplicate Driver");
  auto duplicate = codec.decode(duplicate_text);
  auto duplicate_runtime =
      catalog::validate_for_runtime(*duplicate.document, policy);
  passed &= expect(!duplicate_runtime.accepted() &&
                       has_issue(duplicate_runtime.issues,
                                 catalog::CatalogIssueCode::duplicate_stable_id),
                   "stable identifiers must be unique across the package");

  auto incomplete = codec.decode(replace_once(
      one_item_catalog(1, "release"), "branch = \"Windows stable\"\n", ""));
  auto incomplete_runtime =
      catalog::validate_for_runtime(*incomplete.document, policy);
  passed &= expect(!incomplete_runtime.accepted() &&
                       has_issue(incomplete_runtime.issues,
                                 catalog::CatalogIssueCode::missing_required_field),
                   "enabled entries missing product facts must reject the package");

  auto invalid_dependency = codec.decode(replace_once(
      one_item_catalog(1, "release"), "dependencies = []",
      "dependencies = [\"../unsafe\"]"));
  passed &= expect(!catalog::validate_for_runtime(*invalid_dependency.document,
                                                   policy)
                        .accepted(),
                   "invalid dependency identifiers must be package errors");

  auto unknown_edition = codec.decode(replace_once(
      one_item_catalog(1, "release"), "bundled_editions = []",
      "bundled_editions = [\"unknown_bundle\"]"));
  passed &= expect(!catalog::validate_for_runtime(*unknown_edition.document,
                                                   policy)
                        .accepted(),
                   "unknown bundled-edition semantics must fail closed");

  auto duplicate_default_locale = codec.decode(
      one_item_catalog(1, "release") +
      "\n[software.localizations.\"zh-CN\"]\nname = \"重复默认名称\"\n");
  passed &= expect(!catalog::validate_for_runtime(
                        *duplicate_default_locale.document, policy)
                        .accepted(),
                   "default-language fields must not be duplicated as localization");

  auto disabled = codec.decode(header(1, "release") +
                               software_entry("later", "", "[]", false));
  auto disabled_runtime =
      catalog::validate_for_runtime(*disabled.document, policy);
  passed &= expect(disabled.issues.empty() && disabled_runtime.accepted() &&
                       disabled_runtime.catalog->software.empty(),
                   "disabled entries require only stable id and enabled flag");

  auto disabled_partial_text = header(1, "release") +
      software_entry("later", "", "[]", false) +
      "\n[[software.sources]]\npurpose = \"project_backup\"\n";
  auto disabled_partial = codec.decode(disabled_partial_text);
  auto disabled_partial_runtime = catalog::validate_for_runtime(
      *disabled_partial.document, policy);
  passed &= expect(disabled_partial.issues.empty() &&
                       disabled_partial_runtime.accepted(),
                   "disabled entries may retain incomplete recognized product fields");

  auto empty_driver_hardware = codec.decode(
      header(1, "release") + driver_entry("driver", "Driver"));
  empty_driver_hardware.document->drivers.front().hardware_kinds.clear();
  auto empty_driver_runtime = catalog::validate_for_runtime(
      *empty_driver_hardware.document, policy);
  passed &= expect(!empty_driver_runtime.accepted() &&
                       has_issue(empty_driver_runtime.issues,
                                 catalog::CatalogIssueCode::missing_required_field),
                   "enabled driver entries require concrete hardware applicability");

  auto unsupported_driver_hardware = codec.decode(replace_once(
      header(1, "release") + driver_entry("driver", "Driver"),
      "hardware_kinds = [\"gpu\"]",
      "hardware_kinds = [\"future-accelerator\"]"));
  auto unsupported_driver_runtime = catalog::validate_for_runtime(
      *unsupported_driver_hardware.document, policy);
  passed &= expect(
      !unsupported_driver_runtime.accepted() &&
          has_issue(unsupported_driver_runtime.issues,
                    catalog::CatalogIssueCode::unknown_execution_semantics),
      "unknown driver hardware applicability must reject the package");

  auto driver_dependencies = codec.decode(replace_once(
      header(1, "release") + driver_entry("driver", "Driver"),
      "notice = \"\"", "dependencies = []\nnotice = \"\""));
  passed &= expect(
      has_issue(driver_dependencies.issues,
                catalog::CatalogIssueCode::unknown_execution_semantics),
      "schema v1 must reject speculative driver dependency semantics");

  std::string invalid_utf8 = one_item_catalog(1, "release");
  invalid_utf8.push_back(static_cast<char>(0xff));
  auto malformed = codec.decode(invalid_utf8);
  passed &= expect(has_issue(malformed.issues,
                             catalog::CatalogIssueCode::malformed_toml),
                   "invalid UTF-8 must be reported as malformed TOML");
  auto invalid_integer = codec.decode(replace_once(
      one_item_catalog(1, "release"), "revision = 1", "revision = 01"));
  passed &= expect(has_issue(invalid_integer.issues,
                             catalog::CatalogIssueCode::malformed_toml),
                   "nonconforming TOML integer syntax must be rejected");
  auto invalid_string = codec.decode(replace_once(
      one_item_catalog(1, "release"), "name = \"Core\"",
      "name = \"A\"B\""));
  passed &= expect(has_issue(invalid_string.issues,
                             catalog::CatalogIssueCode::malformed_toml),
                   "unescaped characters after a string terminator must be rejected");
  return passed;
}

[[nodiscard]] std::optional<catalog::ItemAvailability> availability(
    catalog::RuntimeSoftwareCatalog const& runtime, std::string_view id) {
  auto const found = std::ranges::find_if(
      runtime.software, [&](catalog::RuntimeSoftware const& item) {
        return item.definition.id == id;
      });
  if (found == runtime.software.end()) {
    return std::nullopt;
  }
  return found->availability;
}

[[nodiscard]] bool dependency_errors_disable_only_the_local_closure() {
  auto text = header(9, "release");
  text += software_entry("missing", "Missing", "[\"ghost\"]");
  text += software_entry("cascade", "Cascade", "[\"missing\"]");
  text += software_entry("cycle-a", "Cycle A", "[\"cycle-b\"]");
  text += software_entry("cycle-b", "Cycle B", "[\"cycle-a\"]");
  text += software_entry("cycle-up", "Cycle Up", "[\"cycle-a\"]");
  text += software_entry("independent", "Independent");
  text += software_entry("off", "", "[]", false);
  text += software_entry("uses-off", "Uses Off", "[\"off\"]");

  TomlSoftwareCatalogCodec codec;
  auto decoded = codec.decode(text);
  auto runtime = catalog::validate_for_runtime(*decoded.document,
                                                default_policy());
  bool passed = true;
  passed &= expect(decoded.issues.empty() && runtime.accepted() &&
                       runtime.catalog.has_value(),
                   "dependency errors must not reject the package");
  if (!runtime.catalog.has_value()) {
    return false;
  }
  passed &= expect(availability(*runtime.catalog, "missing") ==
                           catalog::ItemAvailability::missing_dependency &&
                       availability(*runtime.catalog, "cascade") ==
                           catalog::ItemAvailability::unavailable_dependency,
                   "missing dependencies must disable reverse dependents");
  passed &= expect(availability(*runtime.catalog, "cycle-a") ==
                           catalog::ItemAvailability::dependency_cycle &&
                       availability(*runtime.catalog, "cycle-b") ==
                           catalog::ItemAvailability::dependency_cycle &&
                       availability(*runtime.catalog, "cycle-up") ==
                           catalog::ItemAvailability::unavailable_dependency,
                   "cycles and their reverse closure must be disabled");
  passed &= expect(availability(*runtime.catalog, "uses-off") ==
                           catalog::ItemAvailability::missing_dependency &&
                       availability(*runtime.catalog, "independent") ==
                           catalog::ItemAvailability::available &&
                       !availability(*runtime.catalog, "off").has_value(),
                   "disabled dependencies are absent while independent items remain");
  auto gate = catalog::evaluate_release_gate(
      *decoded.document, runtime, default_policy(),
      catalog::content_identity(text));
  passed &= expect(!gate.passed() &&
                       has_issue(gate.issues,
                                 catalog::CatalogIssueCode::release_dependency_error),
                   "local dependency errors must still block formal release");
  return passed;
}

[[nodiscard]] bool release_gate_is_distinct_and_versioned() {
  TomlSoftwareCatalogCodec codec;
  auto const released_text = one_item_catalog(4, "release");
  auto released = codec.decode(released_text);
  auto policy = default_policy();
  auto runtime = catalog::validate_for_runtime(*released.document, policy);
  bool passed = true;
  passed &= expect(catalog::evaluate_release_gate(
                       *released.document, runtime, policy,
                       catalog::content_identity(released_text))
                       .passed(),
                   "a complete release catalog must pass the release gate");

  auto const draft_text = one_item_catalog(5, "draft");
  auto draft = codec.decode(draft_text);
  auto draft_runtime = catalog::validate_for_runtime(*draft.document, policy);
  auto draft_gate = catalog::evaluate_release_gate(
      *draft.document, draft_runtime, policy,
      catalog::content_identity(draft_text));
  passed &= expect(draft_runtime.accepted() && !draft_gate.passed() &&
                       has_issue(draft_gate.issues,
                                 catalog::CatalogIssueCode::draft_release_state),
                   "draft state is a release-only failure");

  auto required_policy = policy;
  required_policy.required_release_software = {"required"};
  auto required_gate = catalog::evaluate_release_gate(
      *released.document, runtime, required_policy,
      catalog::content_identity(released_text));
  passed &= expect(has_issue(required_gate.issues,
                             catalog::CatalogIssueCode::required_item_missing),
                   "missing required release entries must block release");

  auto profile_policy = policy;
  profile_policy.install_profiles = {{.id = "profile-v1",
                                      .software_ids = {"core"},
                                      .runtime_status = catalog::
                                          InstallProfileRuntimeStatus::available,
                                      .release_ready = false}};
  profile_policy.required_release_software = {"core"};
  auto profile_text = one_item_catalog(
      5, "release", "Core", "[]", "install_profile = \"profile-v1\"\n");
  auto profile = codec.decode(profile_text);
  auto profile_runtime =
      catalog::validate_for_runtime(*profile.document, profile_policy);
  auto profile_gate = catalog::evaluate_release_gate(
      *profile.document, profile_runtime, profile_policy,
      catalog::content_identity(profile_text));
  passed &= expect(profile_runtime.accepted() &&
                       has_issue(profile_gate.issues,
                                 catalog::CatalogIssueCode::install_profile_not_release_ready),
                   "known runtime profiles may remain release-incomplete");

  auto missing_profile_text = one_item_catalog(
      6, "release", "Core", "[]",
      "install_profile = \"missing-profile\"\n");
  auto missing_profile = codec.decode(missing_profile_text);
  auto missing_profile_runtime =
      catalog::validate_for_runtime(*missing_profile.document, policy);
  auto missing_profile_policy = policy;
  missing_profile_policy.required_release_software = {"core"};
  auto missing_profile_gate = catalog::evaluate_release_gate(
      *missing_profile.document, missing_profile_runtime,
      missing_profile_policy,
      catalog::content_identity(missing_profile_text));
  passed &= expect(missing_profile_runtime.accepted() &&
                       missing_profile_runtime.catalog.has_value() &&
                       availability(*missing_profile_runtime.catalog, "core") ==
                           catalog::ItemAvailability::install_profile_unavailable &&
                       has_issue(
                           missing_profile_runtime.issues,
                           catalog::CatalogIssueCode::install_profile_unavailable,
                           catalog::CatalogIssueScope::item) &&
                       has_issue(
                           missing_profile_gate.issues,
                           catalog::CatalogIssueCode::install_profile_not_release_ready),
                   "missing controlled profiles must disable the item without package rejection");

  auto inapplicable_text = one_item_catalog(
      6, "release", "Core", "[]",
      "install_profile = \"profile-v1\"\n");
  inapplicable_text +=
      software_entry("dependent", "Dependent", "[\"core\"]");
  auto inapplicable = codec.decode(inapplicable_text);
  auto inapplicable_policy = policy;
  inapplicable_policy.install_profiles.front().runtime_status =
      catalog::InstallProfileRuntimeStatus::inapplicable;
  auto inapplicable_runtime = catalog::validate_for_runtime(
      *inapplicable.document, inapplicable_policy);
  passed &= expect(inapplicable_runtime.accepted() &&
                       availability(*inapplicable_runtime.catalog, "core") ==
                           catalog::ItemAvailability::install_profile_unavailable &&
                       availability(*inapplicable_runtime.catalog, "dependent") ==
                           catalog::ItemAvailability::unavailable_dependency,
                   "inapplicable profiles must propagate unavailability to dependents");

  auto wrong_software_text = header(6, "release") +
      software_entry("other", "Other", "[]", true,
                     "install_profile = \"profile-v1\"\n");
  auto wrong_software = codec.decode(wrong_software_text);
  auto wrong_software_runtime = catalog::validate_for_runtime(
      *wrong_software.document, policy);
  passed &= expect(wrong_software_runtime.accepted() &&
                       availability(*wrong_software_runtime.catalog, "other") ==
                           catalog::ItemAvailability::install_profile_unavailable,
                   "a known profile attached to the wrong software must be inapplicable");

  auto unknown_semantics_policy = policy;
  unknown_semantics_policy.install_profiles.front().runtime_status =
      catalog::InstallProfileRuntimeStatus::unknown_semantics;
  auto unknown_semantics_runtime = catalog::validate_for_runtime(
      *profile.document, unknown_semantics_policy);
  passed &= expect(!unknown_semantics_runtime.accepted() &&
                       has_issue(
                           unknown_semantics_runtime.issues,
                           catalog::CatalogIssueCode::unknown_execution_semantics,
                           catalog::CatalogIssueScope::package),
                   "unrecognized controlled execution semantics must reject the package");

  auto required_profile_policy = policy;
  required_profile_policy.required_install_profiles = {
      {.software_id = "core", .profile_id = "profile-v1"}};
  auto required_profile_missing = codec.decode(one_item_catalog(7, "release"));
  auto required_profile_missing_runtime = catalog::validate_for_runtime(
      *required_profile_missing.document, required_profile_policy);
  passed &= expect(
      !required_profile_missing_runtime.accepted() &&
          has_issue(required_profile_missing_runtime.issues,
                    catalog::CatalogIssueCode::missing_required_field,
                    catalog::CatalogIssueScope::package),
      "a required controlled install profile cannot be omitted");
  auto required_profile_wrong = codec.decode(one_item_catalog(
      7, "release", "Core", "[]",
      "install_profile = \"another-profile\"\n"));
  auto required_profile_wrong_runtime = catalog::validate_for_runtime(
      *required_profile_wrong.document, required_profile_policy);
  passed &= expect(
      !required_profile_wrong_runtime.accepted() &&
          has_issue(required_profile_wrong_runtime.issues,
                    catalog::CatalogIssueCode::invalid_field,
                    catalog::CatalogIssueScope::package),
      "a required controlled install profile cannot be replaced by another id");

  auto version_policy = policy;
  version_policy.last_published = catalog::PublishedCatalogReference{
      .revision = 4,
      .content_identity = catalog::content_identity(released_text),
  };
  passed &= expect(catalog::evaluate_release_gate(
                       *released.document, runtime, version_policy,
                       catalog::content_identity(released_text))
                       .passed(),
                   "an identical published revision remains identifiable");
  auto changed_text = one_item_catalog(4, "release", "Changed");
  auto changed = codec.decode(changed_text);
  auto changed_runtime =
      catalog::validate_for_runtime(*changed.document, version_policy);
  auto conflict = catalog::evaluate_release_gate(
      *changed.document, changed_runtime, version_policy,
      catalog::content_identity(changed_text));
  passed &= expect(has_issue(conflict.issues,
                             catalog::CatalogIssueCode::release_revision_conflict),
                   "one release revision cannot identify different content");
  version_policy.last_published->revision = 5;
  auto regression = catalog::evaluate_release_gate(
      *released.document, runtime, version_policy,
      catalog::content_identity(released_text));
  passed &= expect(has_issue(regression.issues,
                             catalog::CatalogIssueCode::release_revision_regression),
                   "formal release revisions must not regress");
  return passed;
}

[[nodiscard]] bool lifecycle_updates_imports_downgrades_and_rolls_back() {
  LifecycleFixture fixture;
  fixture.files.files["built-in"] = one_item_catalog(1, "release", "Initial");
  fixture.files.files["built-in-draft"] =
      one_item_catalog(1, "draft", "Invalid Built-in");
  fixture.files.files["display-only"] =
      one_item_catalog(2, "release", "Display Name Only");
  fixture.files.files["release-2"] = replace_once(
      one_item_catalog(2, "release", "Updated", "[]", {}, "changed rule"),
      "https://example.test/core",
      "https://download.example.test/core");
  fixture.files.files["draft-3"] = one_item_catalog(3, "draft", "Draft 3");
  fixture.files.files["same-revision-conflict"] =
      one_item_catalog(1, "release", "Conflicting Revision");
  fixture.files.files["manual-1"] = one_item_catalog(1, "draft", "Old Local");
  fixture.files.files["invalid"] = replace_once(
      one_item_catalog(4, "release"), "notice = \"\"",
      "target_command = \"unsafe\"\nnotice = \"\"");

  bool passed = true;
  passed &= expect(fixture.catalog_lifecycle.restore().succeeded(),
                   "lifecycle state must initialize and restore");
  auto invalid_built_in = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::built_in, "built-in-draft", false);
  passed &= expect(!invalid_built_in.ready &&
                       invalid_built_in.runtime.accepted() &&
                       !invalid_built_in.release_gate.passed(),
                   "unreleased built-in content must not become a local trial");
  auto built = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::built_in, "built-in", false);
  passed &= expect(built.ready && built.runtime.accepted() &&
                       built.release_gate.passed(),
                   "built-in content must pass both runtime and release gates");
  auto applied = fixture.catalog_lifecycle.apply_preview(
      built.confirmation_token, false);
  passed &= expect(applied.succeeded() && applied.current_changed,
                   "offline built-in catalog must atomically become current");
  auto first = fixture.catalog_lifecycle.snapshot();
  passed &= expect(first.current.has_value() &&
                       first.current->revision == 1 &&
                       first.current_toml_bytes ==
                           fixture.files.files["built-in"] &&
                       first.current_document.has_value() &&
                       first.current_catalog.has_value() &&
                       first.current_catalog->software.front().definition.name ==
                           "Initial" &&
                       first.current->identity ==
                           lifecycle::EffectiveCatalogIdentity::released,
                   "built-in application must persist a released identity");

  auto invalid = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "invalid", false);
  passed &= expect(!invalid.ready &&
                       fixture.catalog_lifecycle.snapshot().current->revision == 1,
                   "whole-package update errors must preserve current state");
  auto same_revision_conflict = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update,
      "same-revision-conflict", false);
  passed &= expect(!same_revision_conflict.ready &&
                       has_issue(
                           same_revision_conflict.release_gate.issues,
                           catalog::CatalogIssueCode::release_revision_conflict),
                   "an update cannot reuse the active formal revision for new content");
  auto draft_update = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "draft-3", false);
  passed &= expect(!draft_update.ready && draft_update.runtime.accepted(),
                   "ordinary updates must require the formal release gate");

  auto display_only = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "display-only", false);
  passed &= expect(display_only.ready && display_only.selection_impact.changed.empty(),
                   "display-only changes must not disturb pending selections");

  auto update = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "release-2", false);
  passed &= expect(update.ready && contains(update.selection_impact.changed, "core"),
                   "update preview must report full-definition changes");
  auto const confirmed_update = fixture.files.files["release-2"];
  fixture.files.files["release-2"] =
      one_item_catalog(2, "release", "Changed After Preview");
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(update.confirmation_token, false)
                       .succeeded() &&
                       fixture.catalog_lifecycle.snapshot().current_toml_bytes ==
                           confirmed_update,
                   "confirmation must apply the exact previewed snapshot");
  auto second = fixture.catalog_lifecycle.snapshot();
  passed &= expect(second.current->revision == 2 &&
                       second.previous->revision == 1 &&
                       second.current->identity ==
                           lifecycle::EffectiveCatalogIdentity::released,
                   "machine commit must retain current and business previous together");

  auto hidden_import = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::manual_import, "manual-1", false);
  passed &= expect(!hidden_import.ready,
                   "manual import must be unavailable outside debug mode");
  auto import = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::manual_import, "manual-1", true);
  passed &= expect(import.ready && import.downgrade &&
                       import.path == "manual-1" && import.revision == 1 &&
                       import.item_count == 1,
                   "debug import must preview path, revision, count and downgrade");
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(import.confirmation_token, false)
                       .code == lifecycle::CatalogActionCode::debug_mode_required,
                   "confirmation cannot bypass the debug-mode boundary");
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(import.confirmation_token, true)
                       .succeeded(),
                   "confirmed debug downgrade must apply");
  auto downgraded = fixture.catalog_lifecycle.snapshot();
  passed &= expect(downgraded.current->revision == 1 &&
                       downgraded.previous->revision == 2 &&
                       downgraded.current->identity ==
                           lifecycle::EffectiveCatalogIdentity::local_trial,
                   "downgrade must retain the replaced release for rollback");

  auto rollback = fixture.catalog_lifecycle.preview_rollback();
  passed &= expect(rollback.ready && rollback.revision == 2,
                   "previous catalog must be revalidated before rollback");
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(rollback.confirmation_token, false)
                       .succeeded() &&
                       fixture.catalog_lifecycle.snapshot().current->revision == 2,
                   "rollback must atomically swap current and previous");
  passed &= expect(!fixture.catalog_lifecycle
                        .preview_file(lifecycle::CatalogCandidateOrigin::update,
                                      "absent", false)
                        .ready,
                   "read failures must not prepare an update");
  return passed;
}

[[nodiscard]] bool drafts_checkpoints_and_close_choices_do_not_apply() {
  LifecycleFixture fixture;
  fixture.files.files["built-in"] = one_item_catalog(1, "release", "Current");
  bool passed = true;
  passed &= expect(fixture.catalog_lifecycle.restore().succeeded(),
                   "draft fixture must restore");
  auto built = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::built_in, "built-in", false);
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(built.confirmation_token, false)
                       .succeeded(),
                   "draft fixture must establish current catalog");

  auto draft_two = replace_once(
      one_item_catalog(2, "draft", "Draft Two"),
      "https://example.test/core",
      "https://draft.example.test/core");
  passed &= expect(fixture.catalog_lifecycle
                       .edit(draft_two, lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       fixture.catalog_lifecycle.snapshot().draft.state ==
                           lifecycle::DraftWorkState::unsaved_changes,
                   "editing must retain unsaved content without applying");
  passed &= expect(fixture.catalog_lifecycle.checkpoint_unsaved().succeeded(),
                   "unsaved content must support a subject checkpoint");

  lifecycle::SoftwareCatalogLifecycle recovered(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, StateSubject{"catalog-test-user"});
  passed &= expect(recovered.restore().succeeded() &&
                       recovered.snapshot().draft.state ==
                           lifecycle::DraftWorkState::recovered_unsaved &&
                       recovered.snapshot().draft.toml_bytes == draft_two &&
                       recovered.snapshot().current->revision == 1,
                   "restart must recover unsaved content separately from current");
  passed &= expect(recovered.apply_saved_draft(true).code ==
                       lifecycle::CatalogActionCode::rejected,
                   "recovered unsaved content must block draft application");

  fixture.state_files.fail_next(
      StateFileOperation::read,
      azzs::application::StateFileSlot::checkpoint,
      "injected checkpoint recovery read failure");
  lifecycle::SoftwareCatalogLifecycle failed_recovery(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, StateSubject{"catalog-test-user"});
  passed &= expect(failed_recovery.restore().succeeded() &&
                       failed_recovery.snapshot().draft.state ==
                           lifecycle::DraftWorkState::none &&
                       !failed_recovery.snapshot().error.empty(),
                   "checkpoint recovery failure must be visible without false recovery");
  passed &= expect(recovered
                       .handle_close(lifecycle::CatalogCloseChoice::return_to_editor,
                                     lifecycle::CatalogEditorAccess::debug_mode)
                       .code == lifecycle::CatalogActionCode::returned_to_editor &&
                       recovered.snapshot().draft.state ==
                           lifecycle::DraftWorkState::recovered_unsaved,
                   "return-to-editor must preserve recovered edits");
  passed &= expect(recovered
                       .save_draft(lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded(),
                   "saving must persist the draft and consume its checkpoint");
  auto saved = recovered.snapshot();
  passed &= expect(saved.current->revision == 1 && saved.draft.saved_present &&
                       saved.draft.toml_bytes == draft_two &&
                       saved.draft.state ==
                           lifecycle::DraftWorkState::saved_not_applied,
                   "saving a draft must not replace the current catalog");

  lifecycle::SoftwareCatalogLifecycle reopened(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, StateSubject{"catalog-test-user"});
  passed &= expect(reopened.restore().succeeded() &&
                       reopened.snapshot().draft.state ==
                           lifecycle::DraftWorkState::saved_not_applied,
                   "saved drafts must survive restart without auto-application");
  passed &= expect(reopened
                       .edit(one_item_catalog(3, "draft", "More Edits"),
                             lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       reopened.apply_saved_draft(true).code ==
                           lifecycle::CatalogActionCode::rejected,
                   "new unsaved edits must block applying an older saved draft");
  passed &= expect(reopened
                       .discard_unsaved(lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       reopened.snapshot().draft.saved_present,
                   "discarding unsaved content must preserve the saved draft");
  passed &= expect(reopened.apply_saved_draft(false).code ==
                       lifecycle::CatalogActionCode::debug_mode_required,
                   "draft application must require debug mode");
  auto applied_draft = reopened.apply_saved_draft(true);
  passed &= expect(applied_draft.succeeded() &&
                       has_impact(
                           applied_draft.selection_impact, "core",
                           lifecycle::CatalogSelectionImpactReason::
                               execution_semantics_changed),
                   "saved runtime-valid draft must explicitly apply");
  auto applied = reopened.snapshot();
  passed &= expect(applied.current->revision == 2 &&
                       applied.current->identity ==
                           lifecycle::EffectiveCatalogIdentity::local_trial &&
                       !applied.draft.saved_present,
                   "draft application must persist local identity and clear draft");

  auto typed_edit = fixture.codec.decode(
      one_item_catalog(3, "draft", "Typed Edit"));
  passed &= expect(typed_edit.document.has_value() &&
                       reopened
                           .edit_document(
                               *typed_edit.document,
                               lifecycle::CatalogEditorAccess::debug_mode)
                           .succeeded() &&
                       reopened.snapshot().draft.document == typed_edit.document,
                   "graphical editing must use the same typed domain model");
  passed &= expect(reopened
                       .discard_unsaved(lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded(),
                   "typed unsaved edits must remain explicitly discardable");

  passed &= expect(reopened
                       .edit("schema_version = [broken",
                             lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       reopened
                               .save_draft(
                                   lifecycle::CatalogEditorAccess::debug_mode)
                               .code == lifecycle::CatalogActionCode::rejected &&
                       reopened.snapshot().draft.state ==
                           lifecycle::DraftWorkState::unsaved_changes,
                   "malformed TOML must remain unsaved and must not overwrite a draft");
  passed &= expect(reopened
                       .discard_unsaved(lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded(),
                   "malformed unsaved content must be explicitly discardable");

  auto invalid_semantics = replace_once(
      one_item_catalog(4, "draft"), "notice = \"\"",
      "executor_command = \"blocked\"\nnotice = \"\"");
  passed &= expect(reopened
                       .edit(invalid_semantics,
                             lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       reopened
                           .handle_close(
                               lifecycle::CatalogCloseChoice::save_draft_and_close,
                               lifecycle::CatalogEditorAccess::debug_mode)
                           .succeeded() &&
                       reopened.snapshot().draft.validation_failed &&
                       reopened.snapshot().current->revision == 2,
                   "parseable invalid maintenance content may save but never apply");
  passed &= expect(reopened.apply_saved_draft(true).code ==
                       lifecycle::CatalogActionCode::rejected,
                   "saved whole-package errors must fail runtime revalidation");
  passed &= expect(reopened
                       .delete_saved_draft(
                           lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       reopened.snapshot().current->revision == 2,
                   "deleting a draft must not change current catalog");
  return passed;
}

[[nodiscard]] bool atomic_apply_conflicts_local_errors_and_cleanup_recovery() {
  LifecycleFixture uncertain;
  uncertain.files.files["release-1"] = one_item_catalog(1, "release", "One");
  bool passed = true;
  passed &= expect(uncertain.catalog_lifecycle.restore().succeeded(),
                   "outcome-unknown fixture must restore");
  auto uncertain_preview = uncertain.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "release-1", false);
  uncertain.state_files.fail_on(InMemoryStateFileSystem::Fault{
      .operation = StateFileOperation::flush_volume,
      .slot = std::nullopt,
      .occurrence = 2,
      .error = "injected post-commit durability uncertainty",
  });
  auto resolved_unknown = uncertain.catalog_lifecycle.apply_preview(
      uncertain_preview.confirmation_token, false);
  passed &= expect(resolved_unknown.succeeded() &&
                       resolved_unknown.current_changed &&
                       uncertain.catalog_lifecycle.snapshot().current->revision == 1,
                   "an unknown commit outcome must be resolved by rereading exact payload");

  LifecycleFixture isolated;
  isolated.files.files["release-1"] = one_item_catalog(1, "release", "One");
  isolated.files.files["release-2"] = one_item_catalog(2, "release", "Two");
  passed &= expect(isolated.catalog_lifecycle.restore().succeeded(),
                   "aggregate-isolation fixture must restore");
  auto isolated_first = isolated.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "release-1", false);
  passed &= expect(isolated.catalog_lifecycle
                       .apply_preview(isolated_first.confirmation_token, false)
                       .succeeded(),
                   "aggregate-isolation fixture must establish current state");
  auto const isolated_draft_key = azzs::domain::StateKey::for_subject(
      StateSubject{"catalog-test-user"},
      azzs::domain::AggregateId{"software-catalog-draft"});
  isolated.state_files.corrupt(isolated_draft_key,
                               azzs::application::StateFileSlot::current);
  lifecycle::SoftwareCatalogLifecycle draft_read_only(
      isolated.states, isolated.log, isolated.occupancy, isolated.files,
      isolated.codec, isolated.policy, StateSubject{"catalog-test-user"});
  passed &= expect(draft_read_only.restore().code ==
                           lifecycle::CatalogActionCode::read_only &&
                       draft_read_only.snapshot().current.has_value() &&
                       draft_read_only.snapshot().current->revision == 1 &&
                       draft_read_only.snapshot().machine_access ==
                           lifecycle::CatalogAggregateAccess::writable &&
                       draft_read_only.snapshot().draft_access ==
                           lifecycle::CatalogAggregateAccess::read_only,
                   "a damaged draft aggregate must not hide the valid machine catalog");
  auto isolated_update = draft_read_only.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "release-2", false);
  passed &= expect(isolated_update.ready &&
                       draft_read_only
                           .apply_preview(isolated_update.confirmation_token,
                                          false)
                           .succeeded() &&
                       draft_read_only.snapshot().current->revision == 2,
                   "draft read-only state must not block an independent machine update");

  LifecycleFixture busy_fixture;
  busy_fixture.files.files["release-1"] =
      one_item_catalog(1, "release", "One");
  passed &= expect(busy_fixture.catalog_lifecycle.restore().succeeded(),
                   "busy aggregate fixture must restore");
  auto busy_first = busy_fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "release-1", false);
  passed &= expect(busy_fixture.catalog_lifecycle
                       .apply_preview(busy_first.confirmation_token, false)
                       .succeeded(),
                   "busy aggregate fixture must establish current state");
  auto busy_draft = one_item_catalog(2, "draft", "Saved While Machine Busy");
  passed &= expect(busy_fixture.catalog_lifecycle
                       .edit(busy_draft,
                             lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       busy_fixture.catalog_lifecycle
                           .save_draft(lifecycle::CatalogEditorAccess::debug_mode)
                           .succeeded(),
                   "busy aggregate fixture must establish a saved draft");
  auto const busy_machine_key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"software-catalog-active"});
  auto const busy_draft_key = azzs::domain::StateKey::for_subject(
      StateSubject{"catalog-test-user"},
      azzs::domain::AggregateId{"software-catalog-draft"});
  {
    auto machine_lock = busy_fixture.state_files.try_lock(busy_machine_key);
    lifecycle::SoftwareCatalogLifecycle machine_busy(
        busy_fixture.states, busy_fixture.log, busy_fixture.occupancy,
        busy_fixture.files, busy_fixture.codec, busy_fixture.policy,
        StateSubject{"catalog-test-user"});
    passed &= expect(machine_busy.restore().code ==
                             lifecycle::CatalogActionCode::occupied &&
                         machine_busy.snapshot().draft.saved_present &&
                         machine_busy.snapshot().draft.toml_bytes == busy_draft &&
                         machine_busy.snapshot().machine_access ==
                             lifecycle::CatalogAggregateAccess::occupied &&
                         machine_busy.snapshot().draft_access ==
                             lifecycle::CatalogAggregateAccess::writable,
                     "a busy machine aggregate must not hide the independent draft");
  }
  {
    auto draft_lock = busy_fixture.state_files.try_lock(busy_draft_key);
    lifecycle::SoftwareCatalogLifecycle draft_busy(
        busy_fixture.states, busy_fixture.log, busy_fixture.occupancy,
        busy_fixture.files, busy_fixture.codec, busy_fixture.policy,
        StateSubject{"catalog-test-user"});
    passed &= expect(draft_busy.restore().code ==
                             lifecycle::CatalogActionCode::occupied &&
                         draft_busy.snapshot().current.has_value() &&
                         draft_busy.snapshot().current->revision == 1 &&
                         draft_busy.snapshot().machine_access ==
                             lifecycle::CatalogAggregateAccess::writable &&
                         draft_busy.snapshot().draft_access ==
                             lifecycle::CatalogAggregateAccess::occupied,
                     "a busy draft aggregate must not hide the independent current catalog");
  }

  LifecycleFixture fixture;
  fixture.files.files["release-1"] = one_item_catalog(1, "release", "One");
  fixture.files.files["release-2"] = one_item_catalog(2, "release", "Two");
  passed &= expect(fixture.catalog_lifecycle.restore().succeeded(),
                   "atomic fixture must restore");

  lifecycle::SoftwareCatalogLifecycle stale(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, StateSubject{"catalog-test-user"});
  passed &= expect(stale.restore().succeeded(),
                   "a concurrent stale lifecycle must restore its own revision");
  auto first = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "release-1", false);
  auto second = stale.preview_file(lifecycle::CatalogCandidateOrigin::update,
                                   "release-2", false);
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(first.confirmation_token, false)
                       .succeeded(),
                   "the first CAS application must commit");
  passed &= expect(stale.apply_preview(second.confirmation_token, false).code ==
                       lifecycle::CatalogActionCode::conflict,
                   "a stale CAS must fail without partial current/previous state");

  lifecycle::SoftwareCatalogLifecycle observer(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, StateSubject{"catalog-test-user"});
  passed &= expect(observer.restore().succeeded() &&
                       observer.snapshot().current->revision == 1 &&
                       !observer.snapshot().previous.has_value(),
                   "CAS conflict must preserve the committed machine aggregate");

  auto local_text = header(2, "release");
  local_text += software_entry("broken", "Broken", "[\"missing\"]");
  local_text += software_entry("dependent", "Dependent", "[\"broken\"]");
  local_text += software_entry("independent", "Independent");
  fixture.files.files["local-errors"] = local_text;
  auto local = fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::manual_import, "local-errors", true);
  passed &= expect(local.ready && contains(local.selection_impact.disabled, "broken") &&
                       contains(local.selection_impact.disabled, "dependent") &&
                       contains(local.selection_impact.removed, "core"),
                   "local dependency errors must preview a whole-catalog switch");
  auto local_apply = fixture.catalog_lifecycle.apply_preview(
      local.confirmation_token, true);
  passed &= expect(local_apply.succeeded() && local_apply.runtime.accepted() &&
                       local_apply.runtime.catalog.has_value() &&
                       has_impact(
                           local_apply.selection_impact, "core",
                           lifecycle::CatalogSelectionImpactReason::removed) &&
                       has_impact(
                           local_apply.selection_impact, "broken",
                           lifecycle::CatalogSelectionImpactReason::
                               runtime_unavailable) &&
                       local_apply.runtime.catalog->software.size() == 3 &&
                       fixture.catalog_lifecycle.snapshot().current->revision == 2 &&
                       fixture.catalog_lifecycle.snapshot()
                           .current_catalog->software.size() == 3 &&
                       std::ranges::none_of(
                           fixture.catalog_lifecycle.snapshot()
                               .current_catalog->software,
                           [](catalog::RuntimeSoftware const& item) {
                             return item.definition.id == "core";
                           }) &&
                       !fixture.catalog_lifecycle.snapshot()
                            .current->local_issues.empty(),
                   "locally disabled items must use only the newly applied definitions");
  passed &= expect(std::ranges::any_of(
                       fixture.log.events, [](ExecutionEvent const& event) {
                         return std::ranges::any_of(
                             event.fields, [](auto const& field) {
                               return field.key == "catalog_runtime_issue" &&
                                      (field.value == "missing_dependency" ||
                                       field.value ==
                                           "unavailable_dependency");
                             });
                       }),
                   "local dependency reasons must be written to execution log");
  std::optional<std::string> local_preview_correlation;
  std::optional<std::string> local_apply_correlation;
  for (std::size_t index = 0; index < fixture.log.events.size(); ++index) {
    auto const& event = fixture.log.events[index];
    auto const has_local_path = std::ranges::any_of(
        event.fields, [](auto const& field) {
          return field.key == "candidate_path" &&
                 field.value == "local-errors";
        });
    if (!has_local_path) {
      continue;
    }
    if (event.stage == "preview") {
      local_preview_correlation = fixture.log.correlations[index];
    } else if (event.stage == "apply") {
      local_apply_correlation = fixture.log.correlations[index];
    }
  }
  passed &= expect(local_preview_correlation.has_value() &&
                       local_apply_correlation == local_preview_correlation,
                   "preview and apply logs must retain one path-bound correlation");

  auto draft_three = header(3, "draft");
  draft_three += software_entry("broken", "Broken", "[\"missing\"]");
  draft_three += software_entry("dependent", "Dependent", "[\"broken\"]");
  draft_three += software_entry("independent", "Independent");
  passed &= expect(fixture.catalog_lifecycle
                       .edit(draft_three,
                             lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       fixture.catalog_lifecycle
                           .save_draft(lifecycle::CatalogEditorAccess::debug_mode)
                           .succeeded(),
                   "cleanup fixture must persist a saved draft first");
  fixture.state_files.fail_on(InMemoryStateFileSystem::Fault{
      .operation = StateFileOperation::write,
      .slot = azzs::application::StateFileSlot::candidate,
      .occurrence = 2,
      .error = "injected draft cleanup failure",
  });
  auto cleanup_pending = fixture.catalog_lifecycle.apply_saved_draft(true);
  passed &= expect(cleanup_pending.code ==
                           lifecycle::CatalogActionCode::applied_cleanup_pending &&
                       cleanup_pending.current_changed &&
                       fixture.catalog_lifecycle.snapshot().current->revision == 3 &&
                       fixture.catalog_lifecycle.snapshot().draft.saved_present,
                   "machine apply success must not be rolled back or misreported when draft cleanup fails");

  lifecycle::SoftwareCatalogLifecycle cleanup_recovery(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, StateSubject{"catalog-test-user"});
  passed &= expect(cleanup_recovery.restore().succeeded() &&
                       cleanup_recovery.snapshot().current->revision == 3 &&
                       !cleanup_recovery.snapshot().draft.saved_present &&
                       !cleanup_recovery.snapshot().draft.cleanup_pending,
                   "restart must recognize the applied identity and retry draft cleanup");
  return passed;
}

[[nodiscard]] bool persisted_identity_and_stable_id_ledger_are_monotonic() {
  LifecycleFixture ledger_fixture;
  ledger_fixture.files.files["release-1"] =
      one_item_catalog(1, "release", "One");
  ledger_fixture.files.files["source-update"] = replace_once(
      one_item_catalog(2, "release", "One Renamed"),
      "https://example.test/core", "https://cdn.example.test/core");
  ledger_fixture.files.files["branch-reuse"] = replace_once(
      one_item_catalog(3, "release", "Repurposed"),
      "branch = \"Windows stable\"", "branch = \"Different product\"");
  ledger_fixture.files.files["kind-reuse"] =
      header(3, "release") + driver_entry("core", "Driver Reuse");
  ledger_fixture.files.files["remove-core"] =
      header(3, "release") + software_entry("replacement", "Replacement");
  ledger_fixture.files.files["removed-target-reuse"] = replace_once(
      one_item_catalog(4, "release", "Removed Target Reuse"),
      "branch = \"Windows stable\"", "branch = \"Different product\"");

  bool passed = true;
  passed &= expect(ledger_fixture.catalog_lifecycle.restore().succeeded(),
                   "stable-id fixture must restore");
  auto first = ledger_fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::built_in, "release-1", false);
  passed &= expect(first.ready &&
                       ledger_fixture.catalog_lifecycle
                           .apply_preview(first.confirmation_token, false)
                           .succeeded(),
                   "stable-id fixture must establish the first formal catalog");
  auto allowed = ledger_fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "source-update", false);
  passed &= expect(allowed.ready &&
                       ledger_fixture.catalog_lifecycle
                           .apply_preview(allowed.confirmation_token, false)
                           .succeeded(),
                   "display and source maintenance must preserve a stable id");
  auto branch_reuse = ledger_fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "branch-reuse", false);
  passed &= expect(
      !branch_reuse.ready &&
          has_issue(branch_reuse.runtime.issues,
                    catalog::CatalogIssueCode::stable_id_reused,
                    catalog::CatalogIssueScope::package),
      "changing the product branch anchor must reject stable-id reuse");
  auto kind_reuse = ledger_fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "kind-reuse", false);
  passed &= expect(
      !kind_reuse.ready &&
          has_issue(kind_reuse.runtime.issues,
                    catalog::CatalogIssueCode::stable_id_reused,
                    catalog::CatalogIssueScope::package),
      "software and driver entries cannot exchange one stable id");
  auto remove_core = ledger_fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "remove-core", false);
  passed &= expect(remove_core.ready &&
                       ledger_fixture.catalog_lifecycle
                           .apply_preview(remove_core.confirmation_token, false)
                           .succeeded(),
                   "removing an entry must remain a valid catalog update");
  auto removed_target_reuse = ledger_fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "removed-target-reuse", false);
  passed &= expect(
      !removed_target_reuse.ready &&
          has_issue(removed_target_reuse.runtime.issues,
                    catalog::CatalogIssueCode::stable_id_reused,
                    catalog::CatalogIssueScope::package),
      "a removed id cannot return with a different product identity");
  auto old_catalog_import = ledger_fixture.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::manual_import, "source-update", true);
  passed &= expect(
      old_catalog_import.ready && old_catalog_import.downgrade &&
          ledger_fixture.catalog_lifecycle
              .apply_preview(old_catalog_import.confirmation_token, true)
              .succeeded() &&
          ledger_fixture.catalog_lifecycle.snapshot().current->revision == 2,
      "debug downgrade may restore the same stable product identity");
  auto explicit_rollback = ledger_fixture.catalog_lifecycle.preview_rollback();
  passed &= expect(
      explicit_rollback.ready &&
          ledger_fixture.catalog_lifecycle
              .apply_preview(explicit_rollback.confirmation_token, false)
              .succeeded() &&
          ledger_fixture.catalog_lifecycle.snapshot().current->revision == 3,
      "the dedicated rollback path may restore the replaced catalog");

  LifecycleFixture identity_fixture;
  auto formal_policy = default_policy();
  formal_policy.required_release_software = {"core"};
  auto const formal_text = one_item_catalog(
      5, "release", "Formal", "[]",
      "install_profile = \"profile-v1\"\n");
  identity_fixture.files.files["formal"] = formal_text;
  identity_fixture.files.files["local-release"] = replace_once(
      one_item_catalog(6, "release", "Local Release", "[]",
                       "install_profile = \"profile-v1\"\n"),
      "https://example.test/core", "https://local.example.test/core");
  identity_fixture.files.files["formal-conflict"] = replace_once(
      formal_text, "https://example.test/core",
      "https://conflict.example.test/core");

  lifecycle::SoftwareCatalogLifecycle formal_lifecycle(
      identity_fixture.states, identity_fixture.log,
      identity_fixture.occupancy, identity_fixture.files,
      identity_fixture.codec, formal_policy,
      StateSubject{"catalog-test-user"});
  passed &= expect(formal_lifecycle.restore().succeeded(),
                   "formal identity fixture must restore");
  auto formal = formal_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::built_in, "formal", false);
  passed &= expect(formal.ready &&
                       formal_lifecycle
                           .apply_preview(formal.confirmation_token, false)
                           .succeeded(),
                   "formal identity fixture must persist its release baseline");

  auto local_policy = formal_policy;
  local_policy.install_profiles.front().release_ready = false;
  lifecycle::SoftwareCatalogLifecycle local_lifecycle(
      identity_fixture.states, identity_fixture.log,
      identity_fixture.occupancy, identity_fixture.files,
      identity_fixture.codec, local_policy,
      StateSubject{"catalog-test-user"});
  passed &= expect(local_lifecycle.restore().succeeded(),
                   "local-trial lifecycle must restore the formal baseline");
  auto local_release = local_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::manual_import, "local-release", true);
  passed &= expect(local_release.ready &&
                       !local_release.release_gate.passed() &&
                       local_lifecycle
                           .apply_preview(local_release.confirmation_token, true)
                           .succeeded() &&
                       local_lifecycle.snapshot().current->identity ==
                           lifecycle::EffectiveCatalogIdentity::local_trial,
                   "a release-shaped debug import must remain a local trial");

  lifecycle::SoftwareCatalogLifecycle reopened(
      identity_fixture.states, identity_fixture.log,
      identity_fixture.occupancy, identity_fixture.files,
      identity_fixture.codec, formal_policy,
      StateSubject{"catalog-test-user"});
  passed &= expect(reopened.restore().succeeded() &&
                       reopened.snapshot().current->revision == 6 &&
                       reopened.snapshot().current->identity ==
                           lifecycle::EffectiveCatalogIdentity::local_trial &&
                       reopened.snapshot().current->release_issues.empty(),
                   "policy improvement after restart must not promote a persisted local trial");
  auto formal_conflict = reopened.preview_file(
      lifecycle::CatalogCandidateOrigin::update, "formal-conflict", false);
  passed &= expect(
      !formal_conflict.ready &&
          has_issue(formal_conflict.release_gate.issues,
                    catalog::CatalogIssueCode::release_revision_conflict,
                    catalog::CatalogIssueScope::release),
      "a local trial must not erase the last formal revision identity");
  return passed;
}

[[nodiscard]] bool draft_checkpoint_cleanup_is_post_commit_and_recoverable() {
  LifecycleFixture fixture;
  bool passed = true;
  passed &= expect(fixture.catalog_lifecycle.restore().succeeded(),
                   "checkpoint cleanup fixture must restore");
  auto old_draft = one_item_catalog(1, "draft", "Old Draft");
  passed &= expect(fixture.catalog_lifecycle
                       .edit(old_draft,
                             lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       fixture.catalog_lifecycle
                           .save_draft(
                               lifecycle::CatalogEditorAccess::debug_mode)
                           .succeeded(),
                   "checkpoint cleanup fixture must establish an older saved draft");

  auto new_draft = one_item_catalog(2, "draft", "New Draft");
  passed &= expect(fixture.catalog_lifecycle
                       .edit(new_draft,
                             lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       fixture.catalog_lifecycle.checkpoint_unsaved().succeeded(),
                   "new draft edits must have a pre-save recovery checkpoint");
  fixture.state_files.fail_next(
      StateFileOperation::write,
      azzs::application::StateFileSlot::checkpoint_staging,
      "injected checkpoint replacement failure");
  auto saved_with_write_failure = fixture.catalog_lifecycle.save_draft(
      lifecycle::CatalogEditorAccess::debug_mode);
  auto after_write_failure = fixture.catalog_lifecycle.snapshot();
  passed &= expect(
      saved_with_write_failure.code ==
              lifecycle::CatalogActionCode::saved_cleanup_pending &&
          saved_with_write_failure.succeeded() &&
          after_write_failure.draft.saved_present &&
          after_write_failure.draft.toml_bytes == new_draft &&
          after_write_failure.draft.state ==
              lifecycle::DraftWorkState::saved_not_applied &&
          after_write_failure.draft.checkpoint_cleanup_pending,
      "a committed draft must stay authoritative when checkpoint replacement fails");

  fixture.state_files.fail_next(
      StateFileOperation::write,
      azzs::application::StateFileSlot::checkpoint_consumed_staging,
      "injected stale-checkpoint consumption failure");
  lifecycle::SoftwareCatalogLifecycle recovered_write_failure(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, StateSubject{"catalog-test-user"});
  passed &= expect(
      recovered_write_failure.restore().succeeded() &&
          recovered_write_failure.snapshot().draft.toml_bytes == new_draft &&
          recovered_write_failure.snapshot().draft.state ==
              lifecycle::DraftWorkState::saved_not_applied &&
          recovered_write_failure.snapshot()
              .draft.checkpoint_cleanup_pending,
      "a second cleanup interruption must not revive stale bytes as unsaved edits");

  lifecycle::SoftwareCatalogLifecycle settled_write_failure(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, StateSubject{"catalog-test-user"});
  passed &= expect(
      settled_write_failure.restore().succeeded() &&
          settled_write_failure.snapshot().draft.toml_bytes == new_draft &&
          settled_write_failure.snapshot().draft.state ==
              lifecycle::DraftWorkState::saved_not_applied &&
          !settled_write_failure.snapshot()
               .draft.checkpoint_cleanup_pending,
      "a later restart must consume cleanup-only checkpoint bytes without revival");

  auto third_draft = one_item_catalog(3, "draft", "Third Draft");
  passed &= expect(settled_write_failure
                       .edit(third_draft,
                             lifecycle::CatalogEditorAccess::debug_mode)
                       .succeeded() &&
                       settled_write_failure.checkpoint_unsaved().succeeded(),
                   "consume-failure fixture must establish another checkpoint");
  fixture.state_files.fail_next(
      StateFileOperation::write,
      azzs::application::StateFileSlot::checkpoint_consumed_staging,
      "injected checkpoint consumption failure");
  auto saved_with_consume_failure = settled_write_failure.save_draft(
      lifecycle::CatalogEditorAccess::debug_mode);
  passed &= expect(
          saved_with_consume_failure.code ==
              lifecycle::CatalogActionCode::saved_cleanup_pending &&
          saved_with_consume_failure.succeeded() &&
          settled_write_failure.snapshot().draft.toml_bytes == third_draft,
      "checkpoint consumption failure must not turn a committed save into failure");

  lifecycle::SoftwareCatalogLifecycle recovered_consume_failure(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, StateSubject{"catalog-test-user"});
  passed &= expect(
      recovered_consume_failure.restore().succeeded() &&
          recovered_consume_failure.snapshot().draft.toml_bytes == third_draft &&
          recovered_consume_failure.snapshot().draft.state ==
              lifecycle::DraftWorkState::saved_not_applied &&
          !recovered_consume_failure.snapshot()
               .draft.checkpoint_cleanup_pending,
      "restart must retry consumption without reviving the saved bytes");

  fixture.state_files.fail_next(
      StateFileOperation::write,
      azzs::application::StateFileSlot::checkpoint_consumed_staging,
      "injected deleted-draft checkpoint consumption failure");
  auto deleted_with_cleanup_failure =
      recovered_consume_failure.delete_saved_draft(
          lifecycle::CatalogEditorAccess::debug_mode);
  passed &= expect(
      deleted_with_cleanup_failure.code ==
              lifecycle::CatalogActionCode::saved_cleanup_pending &&
          deleted_with_cleanup_failure.succeeded() &&
          !recovered_consume_failure.snapshot().draft.saved_present &&
          recovered_consume_failure.snapshot()
              .draft.checkpoint_cleanup_pending,
      "deleted draft bytes must become cleanup-only when consumption fails");

  lifecycle::SoftwareCatalogLifecycle recovered_delete_failure(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, StateSubject{"catalog-test-user"});
  passed &= expect(
      recovered_delete_failure.restore().succeeded() &&
          recovered_delete_failure.snapshot().draft.state ==
              lifecycle::DraftWorkState::none &&
          !recovered_delete_failure.snapshot().draft.saved_present &&
          !recovered_delete_failure.snapshot().draft.toml_bytes.has_value() &&
          !recovered_delete_failure.snapshot()
               .draft.checkpoint_cleanup_pending,
      "cleanup-only bytes must never resurrect a deleted draft after restart");
  return passed;
}

[[nodiscard]] bool execution_log_receipts_gate_and_qualify_application() {
  LifecycleFixture preview_failure;
  preview_failure.files.files["release-1"] =
      one_item_catalog(1, "release", "One");
  bool passed = true;
  passed &= expect(preview_failure.catalog_lifecycle.restore().succeeded(),
                   "preview log failure fixture must restore");
  preview_failure.log.fail_next_append("injected preview log failure");
  auto preview = preview_failure.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::built_in, "release-1", false);
  auto blocked = preview_failure.catalog_lifecycle.apply_preview(
      preview.confirmation_token, false);
  passed &= expect(preview.ready && !preview.log_persisted &&
                       blocked.code ==
                           lifecycle::CatalogActionCode::persistence_failed &&
                       !blocked.log_persisted &&
                       !preview_failure.catalog_lifecycle.snapshot()
                            .current.has_value(),
                   "an unlogged preview must be visible but blocked before mutation");

  LifecycleFixture apply_log_failure;
  apply_log_failure.files.files["release-1"] =
      one_item_catalog(1, "release", "One");
  passed &= expect(apply_log_failure.catalog_lifecycle.restore().succeeded(),
                   "apply log failure fixture must restore");
  auto prepared = apply_log_failure.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::built_in, "release-1", false);
  passed &= expect(prepared.ready && prepared.log_persisted,
                   "a persisted preview must remain applicable");
  apply_log_failure.log.fail_next_append("injected final apply log failure");
  auto applied = apply_log_failure.catalog_lifecycle.apply_preview(
      prepared.confirmation_token, false);
  passed &= expect(
      applied.code == lifecycle::CatalogActionCode::applied_log_incomplete &&
          applied.succeeded() && applied.current_changed &&
          !applied.log_persisted &&
          apply_log_failure.catalog_lifecycle.snapshot().current->revision == 1,
      "post-commit log failure must report changed current state without rollback");

  LifecycleFixture no_change_log_failure;
  auto const same_catalog = one_item_catalog(1, "release", "One");
  no_change_log_failure.files.files["release-1"] = same_catalog;
  passed &= expect(no_change_log_failure.catalog_lifecycle.restore().succeeded(),
                   "no-change log failure fixture must restore");
  auto active = no_change_log_failure.catalog_lifecycle.preview_file(
      lifecycle::CatalogCandidateOrigin::built_in, "release-1", false);
  passed &= expect(
      active.ready &&
          no_change_log_failure.catalog_lifecycle
              .apply_preview(active.confirmation_token, false)
              .succeeded() &&
          no_change_log_failure.catalog_lifecycle
              .edit(same_catalog, lifecycle::CatalogEditorAccess::debug_mode)
              .succeeded() &&
          no_change_log_failure.catalog_lifecycle
              .save_draft(lifecycle::CatalogEditorAccess::debug_mode)
              .succeeded(),
      "no-change fixture must establish an identical saved draft");
  no_change_log_failure.log.fail_after_appends(
      1, "injected no-change final log failure");
  auto no_change =
      no_change_log_failure.catalog_lifecycle.apply_saved_draft(true);
  passed &= expect(
      no_change.code == lifecycle::CatalogActionCode::applied_log_incomplete &&
          no_change.succeeded() && !no_change.current_changed &&
          no_change.draft_changed && !no_change.log_persisted &&
          !no_change_log_failure.catalog_lifecycle.snapshot()
               .draft.saved_present &&
          no_change_log_failure.catalog_lifecycle.snapshot()
                  .current->identity ==
              lifecycle::EffectiveCatalogIdentity::released,
      "clearing an identical saved draft must report a failed final log append");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= content_identity_is_sha256();
  passed &= authoritative_catalog_loads_offline();
  passed &= toml_shape_round_trips_software_and_drivers();
  passed &= package_errors_and_disabled_minimum_are_separate();
  passed &= dependency_errors_disable_only_the_local_closure();
  passed &= release_gate_is_distinct_and_versioned();
  passed &= lifecycle_updates_imports_downgrades_and_rolls_back();
  passed &= drafts_checkpoints_and_close_choices_do_not_apply();
  passed &= atomic_apply_conflicts_local_errors_and_cleanup_recovery();
  passed &= persisted_identity_and_stable_id_ledger_are_monotonic();
  passed &= draft_checkpoint_cleanup_is_post_commit_and_recoverable();
  passed &= execution_log_receipts_gate_and_qualify_application();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "software catalog contract passed\n";
  return EXIT_SUCCESS;
}
