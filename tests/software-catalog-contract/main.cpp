#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/adapters/infrastructure/software_catalog_file.hpp"
#include "azzs/application/debug_mode_catalog_editor.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/domain/controlled_install_profiles.hpp"
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
using azzs::application::DebugModeCatalogEditor;
using azzs::application::DebugModePreferenceRead;
using azzs::application::DebugModePreferenceReadStatus;
using azzs::application::DebugModePreferenceStore;
using azzs::application::DebugModePreferenceWriteStatus;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogDebugModeRead;
using azzs::application::ExecutionLogDebugModeResult;
using azzs::application::ExecutionLogDebugModeStatus;
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

[[nodiscard]] bool has_issue_for_item(
    std::vector<catalog::CatalogIssue> const& issues,
    catalog::CatalogIssueCode code, std::string_view item_id,
    std::optional<catalog::CatalogIssueScope> scope = std::nullopt) {
  return std::ranges::any_of(issues, [&](catalog::CatalogIssue const& issue) {
    return issue.code == code && issue.item_id == item_id &&
           (!scope.has_value() || issue.scope == *scope);
  });
}

[[nodiscard]] bool contains(std::vector<std::string> const& values,
                            std::string_view value) {
  return std::ranges::find(values, value) != values.end();
}

template <typename Value, typename Projection>
[[nodiscard]] Value const* find_by_id(std::vector<Value> const& values,
                                      std::string_view id,
                                      Projection projection) {
  auto const found = std::ranges::find(values, id, projection);
  return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] catalog::CatalogSource const* primary_source(
    std::vector<catalog::CatalogSource> const& sources) {
  auto const found = std::ranges::find_if(
      sources, [](catalog::CatalogSource const& source) {
        return source.purpose == catalog::SourcePurpose::primary;
      });
  return found == sources.end() ? nullptr : &*found;
}

[[nodiscard]] bool contains_prohibited_identity(std::string_view value) {
  if (value.find("破解") != std::string_view::npos ||
      value.find("授权绕过") != std::string_view::npos) {
    return true;
  }

  std::string normalized;
  normalized.reserve(value.size() + 2U);
  normalized.push_back(' ');
  for (char character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
    normalized.push_back((character >= 'a' && character <= 'z') ||
                                 (character >= '0' && character <= '9')
                             ? character
                             : ' ');
  }
  normalized.push_back(' ');
  return normalized.find(" crack ") != std::string::npos ||
         normalized.find(" keygen ") != std::string::npos;
}

template <typename Item>
[[nodiscard]] bool item_has_no_prohibited_identity(Item const& item) {
  if (contains_prohibited_identity(item.id) ||
      contains_prohibited_identity(item.name) ||
      contains_prohibited_identity(item.branch) ||
      contains_prohibited_identity(item.notice) ||
      (item.fixed_version.has_value() &&
       contains_prohibited_identity(*item.fixed_version))) {
    return false;
  }
  for (auto const& source : item.sources) {
    if (contains_prohibited_identity(source.address) ||
        (source.version.has_value() &&
         contains_prohibited_identity(*source.version))) {
      return false;
    }
    for (auto const& history : source.history) {
      if (contains_prohibited_identity(history.version) ||
          contains_prohibited_identity(history.address)) {
        return false;
      }
    }
  }
  return true;
}

template <typename Profile>
concept ProfileExposesForbiddenExecutionField =
    requires(Profile const& profile) { profile.command; } ||
    requires(Profile const& profile) { profile.arguments; } ||
    requires(Profile const& profile) { profile.script; } ||
    requires(Profile const& profile) { profile.download_url; } ||
    requires(Profile const& profile) { profile.path; } ||
    requires(Profile const& profile) { profile.selector; } ||
    requires(Profile const& profile) { profile.registry; };

static_assert(!ProfileExposesForbiddenExecutionField<
              catalog::ControlledInstallProfile>);

[[nodiscard]] bool has_profile_issue(
    catalog::ControlledInstallProfileValidation const& validation,
    catalog::ControlledInstallProfileIssueCode code) {
  return std::ranges::any_of(
      validation.issues,
      [&](catalog::ControlledInstallProfileIssue const& issue) {
        return issue.code == code;
      });
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

  [[nodiscard]] ExecutionLogDebugModeResult set_debug_mode(
      bool enabled) override {
    debug_mode_enabled = enabled;
    return {.status = ExecutionLogDebugModeStatus::applied,
            .enabled = debug_mode_enabled};
  }

  [[nodiscard]] ExecutionLogDebugModeRead debug_mode() const override {
    return {.available = true, .enabled = debug_mode_enabled};
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
  bool debug_mode_enabled{false};

 private:
  struct PendingAppendFailure final {
    std::size_t successful_appends_before_failure{};
    std::string error;
  };

  std::optional<PendingAppendFailure> append_failure_;
  std::uint64_t next_{1};
};

class InMemoryDebugModePreferenceStore final : public DebugModePreferenceStore {
 public:
  [[nodiscard]] DebugModePreferenceRead read_debug_mode() override {
    return read;
  }

  [[nodiscard]] DebugModePreferenceWriteStatus write_debug_mode(
      bool enabled) override {
    if (write_status == DebugModePreferenceWriteStatus::saved) {
      read.enabled = enabled;
    }
    return write_status;
  }

  DebugModePreferenceRead read{
      .status = DebugModePreferenceReadStatus::loaded};
  DebugModePreferenceWriteStatus write_status{
      DebugModePreferenceWriteStatus::saved};
};

class MemoryCatalogFiles final : public lifecycle::SoftwareCatalogFileReader {
 public:
  [[nodiscard]] lifecycle::CatalogFileRead read_built_in() const override {
    return read(built_in_path);
  }

  [[nodiscard]] lifecycle::CatalogFileRead read_update() const override {
    return read(update_path);
  }

  [[nodiscard]] lifecycle::CatalogFileRead read_manual_import(
      std::string const& path) const override {
    return read(path);
  }

  [[nodiscard]] lifecycle::CatalogFileRead read(
      std::string const& path) const {
    auto const found = files.find(path);
    if (found == files.end()) {
      return {.path = path, .error = "fixture file is absent"};
    }
    return {.succeeded = true, .path = path, .bytes = found->second};
  }

  std::string built_in_path;
  std::string update_path;
  std::map<std::string, std::string> files;
};

// Models a newer reader that still understands schema v1 while persisting the
// exact older bytes. It lets this contract create an N/N-1 aggregate where the
// current catalog is readable by this workbench and its previous catalog is not.
class SchemaTwoCompatibilityCodec final : public lifecycle::SoftwareCatalogCodec {
 public:
  [[nodiscard]] lifecycle::CatalogDecodeResult decode(
      std::string_view bytes) const override {
    auto decoded = codec_.decode(bytes);
    if (decoded.document.has_value() && decoded.document->schema_version == 1) {
      decoded.document->schema_version = 2;
    }
    return decoded;
  }

  [[nodiscard]] std::string encode(
      catalog::SoftwareCatalogDocument const& document) const override {
    return codec_.encode(document);
  }

 private:
  TomlSoftwareCatalogCodec codec_;
};

class MutableCatalogMaintenanceAccess final
    : public lifecycle::CatalogMaintenanceAccess {
 public:
  [[nodiscard]] lifecycle::CatalogEditorAccess editor_access() const noexcept
      override {
    return access;
  }

  lifecycle::CatalogEditorAccess access{
      lifecycle::CatalogEditorAccess::debug_mode};
};

template <typename Lifecycle>
concept CallerChoosesCatalogOrigin = requires(Lifecycle& value,
                                              std::string path) {
  value.preview_file(lifecycle::CatalogCandidateOrigin::built_in, path, false);
};

template <typename Lifecycle>
concept CallerSuppliesCatalogEditorAccess = requires(Lifecycle& value,
                                                     std::string bytes) {
  value.edit(bytes, lifecycle::CatalogEditorAccess::debug_mode);
};

static_assert(
    !CallerChoosesCatalogOrigin<lifecycle::SoftwareCatalogLifecycle>);
static_assert(
    !CallerSuppliesCatalogEditorAccess<lifecycle::SoftwareCatalogLifecycle>);

struct LifecycleFixture final {
  explicit LifecycleFixture(
      catalog::SoftwareCatalogPolicy configured_policy = default_policy(),
      catalog::SoftwareCatalogReleaseGateMode release_gate_mode =
          catalog::SoftwareCatalogReleaseGateMode::formal)
      : states(state_files, clock),
        tokens("catalog-lease-"),
        occupancy(occupancy_storage, tokens),
        policy(std::move(configured_policy)),
        catalog_lifecycle(states, log, occupancy, files, codec, policy,
                          maintenance_access,
                          StateSubject{"catalog-test-user"},
                          release_gate_mode) {}

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
  MutableCatalogMaintenanceAccess maintenance_access;
  lifecycle::SoftwareCatalogLifecycle catalog_lifecycle;
};

struct DebugModeCatalogEditorFixture final {
  DebugModeCatalogEditorFixture()
      : states(state_files, clock),
        tokens("catalog-editor-lease-"),
        occupancy(occupancy_storage, tokens),
        preferences(std::make_shared<InMemoryDebugModePreferenceStore>()),
        editor(log, preferences),
        policy(default_policy()) {}

  [[nodiscard]] std::unique_ptr<lifecycle::SoftwareCatalogLifecycle>
  make_lifecycle() {
    return std::make_unique<lifecycle::SoftwareCatalogLifecycle>(
        states, log, occupancy, files, codec, policy, editor,
        StateSubject{"catalog-test-user"});
  }

  InMemoryStateFileSystem state_files;
  FixedClock clock{WallClockTime{1234ms}};
  DeviceStateStore states;
  InMemoryOperationOccupancyStorage occupancy_storage;
  SequenceLeaseTokenSource tokens;
  SharedOperationOccupancy occupancy;
  RecordingExecutionLog log;
  std::shared_ptr<InMemoryDebugModePreferenceStore> preferences;
  DebugModeCatalogEditor editor;
  MemoryCatalogFiles files;
  TomlSoftwareCatalogCodec codec;
  catalog::SoftwareCatalogPolicy policy;
};

[[nodiscard]] lifecycle::CatalogCandidatePreview preview_built_in(
    lifecycle::SoftwareCatalogLifecycle& catalog_lifecycle,
    MemoryCatalogFiles& files, std::string path) {
  files.built_in_path = std::move(path);
  return catalog_lifecycle.preview_built_in();
}

[[nodiscard]] lifecycle::CatalogCandidatePreview preview_update(
    lifecycle::SoftwareCatalogLifecycle& catalog_lifecycle,
    MemoryCatalogFiles& files, std::string path) {
  files.update_path = std::move(path);
  return catalog_lifecycle.preview_update();
}

[[nodiscard]] lifecycle::CatalogCandidatePreview preview_manual_import(
    lifecycle::SoftwareCatalogLifecycle& catalog_lifecycle,
    std::string path) {
  return catalog_lifecycle.preview_manual_import(std::move(path));
}

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
  auto policy = catalog::initial_software_catalog_policy();
  auto runtime = catalog::validate_for_runtime(*decoded.document, policy);
  auto formal_gate = catalog::evaluate_release_gate(
      *decoded.document, runtime, policy,
      catalog::content_identity(file.bytes));
  auto beta_gate = catalog::evaluate_beta_candidate_release_gate(
      *decoded.document, runtime, policy,
      catalog::content_identity(file.bytes));
  passed &= expect(runtime.accepted() && runtime.catalog.has_value(),
                   "the authoritative release catalog must runtime-load");
  passed &= expect(runtime.catalog.has_value() &&
                       runtime.catalog->software.size() == 11 &&
                       runtime.catalog->drivers.size() == 3,
                   "enabled initial software and driver entries must enter one runtime package");
  if (runtime.catalog.has_value()) {
    auto cheat_engine = std::ranges::find_if(
        runtime.catalog->software, [](catalog::RuntimeSoftware const& item) {
          return item.definition.id == "cheat-engine";
        });
    auto geometers = std::ranges::find_if(
        runtime.catalog->software, [](catalog::RuntimeSoftware const& item) {
          return item.definition.id == "the-geometers-sketchpad";
        });
    passed &= expect(
        cheat_engine != runtime.catalog->software.end() &&
            cheat_engine->availability ==
                catalog::ItemAvailability::install_profile_unavailable &&
            geometers != runtime.catalog->software.end() &&
            geometers->availability ==
                catalog::ItemAvailability::install_profile_unavailable,
        "unresolved controlled profiles must disable only their own catalog items");
    passed &= expect(
        std::ranges::count_if(
            runtime.catalog->software, [](catalog::RuntimeSoftware const& item) {
              return item.availability == catalog::ItemAvailability::available;
            }) == 9,
        "the nine registered controlled profiles must remain independently available");
    passed &= expect(
        std::ranges::all_of(
            runtime.issues, [](catalog::CatalogIssue const& issue) {
              return issue.code != catalog::CatalogIssueCode::install_profile_unavailable ||
                     issue.scope == catalog::CatalogIssueScope::item;
            }),
        "a missing profile must be reported at item scope rather than rejecting the package");
  }
  auto const sogou_runtime = std::ranges::find_if(
      runtime.catalog->software, [](catalog::RuntimeSoftware const& item) {
        return item.definition.id == "sogou-input";
      });
  passed &= expect(sogou_runtime != runtime.catalog->software.end() &&
                       sogou_runtime->availability ==
                           catalog::ItemAvailability::available,
                   "the registered Sogou profile must be available to the Beta runtime");
  passed &= expect(
      !formal_gate.passed() &&
          has_issue_for_item(formal_gate.issues,
                             catalog::CatalogIssueCode::unknown_execution_semantics,
                             "qq", catalog::CatalogIssueScope::release),
      "unknown third-party facts must block the formal release gate");
  passed &= expect(beta_gate.passed(),
                   "the explicit v0.1.0 Beta candidate gate must pass the authoritative catalog");

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

[[nodiscard]] bool initial_catalog_content_contract() {
  LocalSoftwareCatalogFileReader reader;
  TomlSoftwareCatalogCodec codec;
  auto const file = reader.read(AZZS_SOFTWARE_CATALOG_PATH);
  auto decoded = codec.decode(file.bytes);
  bool passed = true;
  passed &= expect(decoded.document.has_value() && decoded.issues.empty(),
                   "initial catalog content must decode before content checks");
  if (!decoded.document.has_value() || !decoded.issues.empty()) {
    return false;
  }

  auto policy = catalog::initial_software_catalog_policy();
  std::vector<std::string> expected_ids{
      "qq", "sogou-input", "game-cheats-manager", "cheat-engine",
      "office-tool-plus", "internet-download-manager",
      "the-geometers-sketchpad", "java-runtime", "dotnet-runtime",
      "directx-runtime", "powershell-7"};
  auto actual_ids = expected_ids;
  actual_ids.clear();
  for (auto const& software : decoded.document->software) {
    actual_ids.push_back(software.id);
    passed &= expect(item_has_no_prohibited_identity(software),
                     "software catalog must exclude prohibited resource identities");
    if (software.enabled) {
      passed &= expect(!software.category_id.empty() && !software.branch.empty() &&
                           software.version_policy.has_value() &&
                           software.dependencies_declared &&
                           software.bundled_editions_declared &&
                           std::ranges::count_if(
                               software.sources, [](catalog::CatalogSource const& source) {
                                 return source.purpose == catalog::SourcePurpose::primary;
                               }) == 1,
                       "enabled software must have complete catalog structure and one primary source");
    }
  }
  std::ranges::sort(expected_ids);
  std::ranges::sort(actual_ids);
  passed &= expect(actual_ids == expected_ids &&
                       std::ranges::adjacent_find(actual_ids) == actual_ids.end(),
                   "the catalog must contain exactly eleven unique first-release software ids");

  for (auto const id : {"game-cheats-manager", "cheat-engine",
                        "office-tool-plus", "internet-download-manager",
                        "the-geometers-sketchpad"}) {
    auto const* software = find_by_id(
        decoded.document->software, id, &catalog::SoftwareDefinition::id);
    passed &= expect(software != nullptr && software->enabled &&
                         software->tier == catalog::SoftwareTier::normal &&
                         !software->notice.empty(),
                     "the five prompted applications must be selectable normal software with notices");
  }

  auto const profiles = catalog::initial_controlled_install_profiles();
  auto const facts = catalog::initial_software_install_facts();
  std::vector<std::string> executable_ids{
      "qq", "sogou-input", "game-cheats-manager", "office-tool-plus",
      "internet-download-manager", "java-runtime", "dotnet-runtime",
      "directx-runtime", "powershell-7"};
  passed &= expect(profiles.size() == 9 && facts.size() == 9,
                   "initial declarations must cover nine executable profiles and nine software facts");
  passed &= expect(catalog::validate_controlled_install_profiles(profiles).accepted() &&
                       catalog::validate_software_install_facts(facts).accepted(),
                   "initial declaration registries must satisfy their value contracts");
  std::vector<std::string> fact_ids;
  fact_ids.reserve(facts.size());
  for (auto const& fact : facts) {
    fact_ids.push_back(fact.software_id);
    passed &= expect(
        fact.capabilities.architectures.knowledge == catalog::FactKnowledge::unknown &&
            fact.capabilities.architectures.values.empty() &&
            fact.capabilities.offline_install == catalog::CapabilitySupport::unknown &&
            fact.capabilities.silent_install == catalog::CapabilitySupport::unknown &&
            fact.capabilities.completion_boundary == catalog::CapabilitySupport::unknown &&
            fact.capabilities.post_install_behavior == catalog::CapabilitySupport::unknown &&
            fact.capabilities.restart_verification == catalog::CapabilitySupport::unknown &&
            fact.capabilities.result_detection == catalog::CapabilitySupport::unknown,
        "initial third-party installation facts must remain explicitly unknown");
  }
  std::ranges::sort(fact_ids);
  std::ranges::sort(executable_ids);
  passed &= expect(fact_ids == executable_ids,
                   "typed install facts must cover the nine executable software ids");
  auto const dotnet_facts = std::ranges::find(
      facts, "dotnet-runtime", &catalog::SoftwareInstallFacts::software_id);
  passed &= expect(dotnet_facts != facts.end() &&
                       dotnet_facts->capabilities.architectures.knowledge ==
                           catalog::FactKnowledge::unknown &&
                       dotnet_facts->capabilities.architectures.values.empty(),
                    "the initial .NET architecture fact must remain unknown");
  if (!profiles.empty()) {
    auto const& profile = profiles.front();
    passed &= expect(profile.id == "sogou-input-defaults-v1" &&
                         profile.software_id == "sogou-input" &&
                         profile.execution_kind ==
                             catalog::ControlledWindowsExecutionKind::
                                 project_owned_windows_executor &&
                          profile.execution ==
                              catalog::WindowsExecutionReadiness::
                                  project_executor_registered &&
                         profile.completion_boundary ==
                             catalog::InstallationCompletionBoundary::
                                 post_install_then_result_detection &&
                         profile.post_install_behavior ==
                             catalog::PostInstallBehavior::controlled_preferences &&
                         profile.restart_verification ==
                             catalog::RestartVerification::not_required &&
                         profile.result_detection ==
                             catalog::ResultDetectionStrategy::
                                 project_owned_presence_probe &&
                         profile.interaction_scope ==
                             catalog::InstallerInteractionScope::
                                 non_identity_preferences_only &&
                         profile.baselines.size() == 1 &&
                         profile.baselines.front().version == "16.7" &&
                         profile.preferences.size() == 2,
                       "Sogou must freeze closed project-owned completion and interaction semantics");
    if (profile.preferences.size() == 2) {
      auto const& first = profile.preferences[0];
      auto const& second = profile.preferences[1];
      passed &= expect(first.phase == catalog::InstallPhase::custom_install &&
                           first.effect == catalog::InstallPreferenceEffect::disable_sogou_search_candidates &&
                           first.default_choice == catalog::PreferenceDefault::decline &&
                           second.phase == catalog::InstallPhase::installation_complete &&
                           second.effect == catalog::InstallPreferenceEffect::decline_sogou_tencent_yuanbao &&
                           second.default_choice == catalog::PreferenceDefault::decline &&
                           first.disposition_order == second.disposition_order,
                       "Sogou preferences must default to decline with the fixed three-level fallback");
    }
    auto invalid_profile = profile;
    invalid_profile.result_detection =
        static_cast<catalog::ResultDetectionStrategy>(99);
    auto const invalid_profiles =
        catalog::validate_controlled_install_profiles(
            std::span<catalog::ControlledInstallProfile const>{&invalid_profile,
                                                                1});
    passed &= expect(
        !invalid_profiles.accepted() && has_profile_issue(
                                           invalid_profiles,
                                           catalog::ControlledInstallProfileIssueCode::
                                               invalid_result_detection_strategy),
        "unrecognized controlled profile semantics must fail closed validation");
    invalid_profile = profile;
    invalid_profile.completion_boundary =
        catalog::InstallationCompletionBoundary::
            post_install_then_restart_verification;
    auto const inconsistent_profiles =
        catalog::validate_controlled_install_profiles(
            std::span<catalog::ControlledInstallProfile const>{&invalid_profile,
                                                                1});
    passed &= expect(
        !inconsistent_profiles.accepted() && has_profile_issue(
                                               inconsistent_profiles,
                                               catalog::ControlledInstallProfileIssueCode::
                                                   inconsistent_completion_semantics),
        "restart completion semantics must not be inferred from process exit or mismatched facts");
  }

  std::vector<std::string> required = policy.required_release_software;
  std::ranges::sort(required);
  passed &= expect(required == executable_ids && policy.supported_driver_hardware_kinds ==
                       std::vector<std::string>{"gpu"},
                   "the initial policy must require the nine executable software ids and the registered GPU kind");
  std::vector<std::string> release_fact_ids;
  release_fact_ids.reserve(policy.required_release_install_facts.size());
  for (auto const& requirement : policy.required_release_install_facts) {
    release_fact_ids.push_back(requirement.software_id);
    passed &= expect(!requirement.complete(),
                     "unvalidated third-party facts must not be release-ready");
  }
  std::ranges::sort(release_fact_ids);
  passed &= expect(release_fact_ids == executable_ids,
                   "the release policy must consume install facts for every executable software id");
  auto const* sogou_profile = find_by_id(
      policy.install_profiles, "sogou-input-defaults-v1",
      &catalog::InstallProfileSupport::id);
  passed &= expect(sogou_profile != nullptr &&
                       sogou_profile->runtime_status ==
                           catalog::InstallProfileRuntimeStatus::available &&
                       !sogou_profile->release_ready &&
                       policy.required_install_profiles.size() == 9,
                   "registered controlled profiles must be runtime-available without release evidence");
  passed &= expect(
      std::ranges::all_of(policy.install_profiles, [](auto const& support) {
        return support.runtime_status ==
                   catalog::InstallProfileRuntimeStatus::available &&
               !support.release_ready;
      }),
      "every initial controlled profile must separate runtime availability from release readiness");

  auto released = codec.decode(file.bytes);
  passed &= expect(released.document.has_value() && released.issues.empty(),
                   "a release-gate candidate must retain the authoritative TOML shape");
  if (!released.document.has_value() || !released.issues.empty()) {
    return false;
  }
  auto evaluate_release_candidate = [&](catalog::SoftwareCatalogDocument const& candidate,
                                        catalog::SoftwareCatalogPolicy const& candidate_policy) {
    auto runtime = catalog::validate_for_runtime(candidate, candidate_policy);
    auto gate = catalog::evaluate_release_gate(
        candidate, runtime, candidate_policy, catalog::content_identity(file.bytes));
    return std::pair{std::move(runtime), std::move(gate)};
  };
  auto evaluate_beta_candidate = [&](catalog::SoftwareCatalogDocument const& candidate,
                                     catalog::SoftwareCatalogPolicy const& candidate_policy) {
    auto runtime = catalog::validate_for_runtime(candidate, candidate_policy);
    auto gate = catalog::evaluate_beta_candidate_release_gate(
        candidate, runtime, candidate_policy, catalog::content_identity(file.bytes));
    return std::pair{std::move(runtime), std::move(gate)};
  };
  auto release_only = evaluate_release_candidate(*released.document, policy);
  passed &= expect(
      release_only.first.accepted() && !release_only.second.passed() &&
          has_issue_for_item(
              release_only.second.issues,
              catalog::CatalogIssueCode::unknown_execution_semantics, "qq",
              catalog::CatalogIssueScope::release),
      "the formal gate must reject the candidate while third-party facts are unknown");
  auto beta_only = evaluate_beta_candidate(*released.document, policy);
  passed &= expect(beta_only.first.accepted() && beta_only.second.passed(),
                   "the explicit Beta candidate gate must skip only unknown installation facts");

  auto unknown_facts_policy = policy;
  unknown_facts_policy.required_release_install_facts.front().
      result_detection_confirmed = false;
  auto unknown_facts = evaluate_release_candidate(*released.document,
                                                  unknown_facts_policy);
  passed &= expect(
      unknown_facts.first.accepted() && !unknown_facts.second.passed() &&
          has_issue_for_item(
              unknown_facts.second.issues,
              catalog::CatalogIssueCode::unknown_execution_semantics, "qq",
              catalog::CatalogIssueScope::release),
      "an unknown execution fact must still block the release gate");
  auto unknown_facts_beta =
      evaluate_beta_candidate(*released.document, unknown_facts_policy);
  passed &= expect(unknown_facts_beta.first.accepted() &&
                       unknown_facts_beta.second.passed() &&
                       !has_issue(unknown_facts_beta.second.issues,
                                  catalog::CatalogIssueCode::unknown_execution_semantics,
                                  catalog::CatalogIssueScope::release),
                   "the explicit Beta candidate gate may defer unknown installation facts");

  auto sogou_ready_policy = policy;
  auto sogou_profile_support = std::ranges::find(
      sogou_ready_policy.install_profiles, "sogou-input-defaults-v1",
      &catalog::InstallProfileSupport::id);
  if (sogou_profile_support == sogou_ready_policy.install_profiles.end()) {
    return false;
  }
  sogou_profile_support->runtime_status =
      catalog::InstallProfileRuntimeStatus::available;
  sogou_profile_support->release_ready = true;
  for (auto& support : sogou_ready_policy.install_profiles) {
    support.runtime_status = catalog::InstallProfileRuntimeStatus::available;
    support.release_ready = true;
  }
  for (auto& requirement : sogou_ready_policy.required_release_install_facts) {
    requirement.architectures_confirmed = true;
    requirement.offline_install_confirmed = true;
    requirement.silent_install_confirmed = true;
    requirement.completion_boundary_confirmed = true;
    requirement.post_install_behavior_confirmed = true;
    requirement.restart_verification_confirmed = true;
    requirement.result_detection_confirmed = true;
  }
  auto sogou_only =
      evaluate_release_candidate(*released.document, sogou_ready_policy);
  passed &= expect(sogou_only.first.accepted() && sogou_only.second.passed(),
                   "explicitly evidenced profiles and facts may pass the formal release gate");

  auto const* required_amd = find_by_id(
      policy.required_release_drivers, "amd-auto-detect-and-install",
      &catalog::RequiredReleaseDriverEntry::id);
  auto const* required_intel = find_by_id(
      policy.required_release_drivers, "intel-gpu-driver-page",
      &catalog::RequiredReleaseDriverEntry::id);
  auto const* required_nvidia = find_by_id(
      policy.required_release_drivers, "nvidia-gpu-driver-page",
      &catalog::RequiredReleaseDriverEntry::id);
  passed &= expect(
      policy.required_release_drivers.size() == 3 && required_amd != nullptr &&
          required_intel != nullptr && required_nvidia != nullptr &&
          required_amd->entry_type == catalog::DriverEntryType::assistant &&
          required_intel->entry_type == catalog::DriverEntryType::vendor_page &&
          required_nvidia->entry_type == catalog::DriverEntryType::vendor_page &&
          required_amd->hardware_kind == "gpu" &&
          required_intel->hardware_kind == "gpu" &&
          required_nvidia->hardware_kind == "gpu" &&
          required_amd->primary_source_address ==
              "https://www.amd.com/en/support/download/drivers.html" &&
          required_intel->primary_source_address ==
              "https://www.intel.com/content/www/us/en/download-center/home.html" &&
          required_nvidia->primary_source_address ==
              "https://www.nvidia.com/Download/index.aspx",
      "the release policy must require the three exact official GPU driver entries");
  for (auto const& requirement : policy.required_release_drivers) {
    auto const* driver = find_by_id(
        released.document->drivers, requirement.id,
        &catalog::DriverDefinition::id);
    auto const* source = driver == nullptr ? nullptr : primary_source(driver->sources);
    passed &= expect(driver != nullptr && driver->enabled &&
                         driver->entry_type == requirement.entry_type &&
                         contains(driver->hardware_kinds,
                                  requirement.hardware_kind) &&
                         source != nullptr &&
                         source->address == requirement.primary_source_address,
                     "the authoritative catalog must match each required driver release policy entry");
  }

  auto missing_intel = *released.document;
  auto missing_intel_driver = std::ranges::find(
      missing_intel.drivers, "intel-gpu-driver-page",
      &catalog::DriverDefinition::id);
  if (missing_intel_driver == missing_intel.drivers.end()) {
    return false;
  }
  missing_intel.drivers.erase(missing_intel_driver);
  auto missing_intel_result = evaluate_release_candidate(missing_intel, policy);
  auto missing_intel_beta = evaluate_beta_candidate(missing_intel, policy);
  passed &= expect(
      missing_intel_result.first.accepted() &&
          has_issue_for_item(
              missing_intel_result.second.issues,
              catalog::CatalogIssueCode::required_item_missing,
              "intel-gpu-driver-page", catalog::CatalogIssueScope::release),
      "deleting a required initial driver must block release");
  passed &= expect(
      missing_intel_beta.first.accepted() &&
          has_issue_for_item(missing_intel_beta.second.issues,
                             catalog::CatalogIssueCode::required_item_missing,
                             "intel-gpu-driver-page",
                             catalog::CatalogIssueScope::release),
      "the Beta candidate gate must still require every initial driver");

  auto disabled_nvidia = *released.document;
  auto disabled_nvidia_driver = std::ranges::find(
      disabled_nvidia.drivers, "nvidia-gpu-driver-page",
      &catalog::DriverDefinition::id);
  if (disabled_nvidia_driver == disabled_nvidia.drivers.end()) {
    return false;
  }
  disabled_nvidia_driver->enabled = false;
  auto disabled_nvidia_result =
      evaluate_release_candidate(disabled_nvidia, policy);
  auto disabled_nvidia_beta = evaluate_beta_candidate(disabled_nvidia, policy);
  passed &= expect(
      disabled_nvidia_result.first.accepted() &&
          has_issue_for_item(
              disabled_nvidia_result.second.issues,
              catalog::CatalogIssueCode::required_item_disabled,
              "nvidia-gpu-driver-page", catalog::CatalogIssueScope::release),
      "disabling a required initial driver must block release");
  passed &= expect(
      disabled_nvidia_beta.first.accepted() &&
          has_issue_for_item(disabled_nvidia_beta.second.issues,
                             catalog::CatalogIssueCode::required_item_disabled,
                             "nvidia-gpu-driver-page",
                             catalog::CatalogIssueScope::release),
      "the Beta candidate gate must still reject disabled drivers");

  auto wrong_amd_type = *released.document;
  auto wrong_amd_driver = std::ranges::find(
      wrong_amd_type.drivers, "amd-auto-detect-and-install",
      &catalog::DriverDefinition::id);
  if (wrong_amd_driver == wrong_amd_type.drivers.end()) {
    return false;
  }
  wrong_amd_driver->entry_type = catalog::DriverEntryType::vendor_page;
  auto wrong_amd_type_result =
      evaluate_release_candidate(wrong_amd_type, policy);
  auto wrong_amd_type_beta = evaluate_beta_candidate(wrong_amd_type, policy);
  passed &= expect(
      wrong_amd_type_result.first.accepted() &&
          has_issue_for_item(wrong_amd_type_result.second.issues,
                             catalog::CatalogIssueCode::invalid_field,
                             "amd-auto-detect-and-install",
                             catalog::CatalogIssueScope::release),
      "changing a required driver entry type must block release");
  passed &= expect(
      wrong_amd_type_beta.first.accepted() &&
          has_issue_for_item(wrong_amd_type_beta.second.issues,
                             catalog::CatalogIssueCode::invalid_field,
                             "amd-auto-detect-and-install",
                             catalog::CatalogIssueScope::release),
      "the Beta candidate gate must still validate driver entry types");

  auto wrong_nvidia_address = *released.document;
  auto wrong_nvidia_driver = std::ranges::find(
      wrong_nvidia_address.drivers, "nvidia-gpu-driver-page",
      &catalog::DriverDefinition::id);
  if (wrong_nvidia_driver == wrong_nvidia_address.drivers.end()) {
    return false;
  }
  auto wrong_nvidia_source = primary_source(wrong_nvidia_driver->sources);
  if (wrong_nvidia_source == nullptr) {
    return false;
  }
  auto mutable_nvidia_source = std::ranges::find_if(
      wrong_nvidia_driver->sources, [](catalog::CatalogSource const& source) {
        return source.purpose == catalog::SourcePurpose::primary;
      });
  mutable_nvidia_source->address = "https://www.nvidia.com/Download/other.aspx";
  auto wrong_nvidia_address_result =
      evaluate_release_candidate(wrong_nvidia_address, policy);
  auto wrong_nvidia_address_beta =
      evaluate_beta_candidate(wrong_nvidia_address, policy);
  passed &= expect(
      wrong_nvidia_address_result.first.accepted() &&
          has_issue_for_item(wrong_nvidia_address_result.second.issues,
                             catalog::CatalogIssueCode::invalid_field,
                             "nvidia-gpu-driver-page",
                             catalog::CatalogIssueScope::release),
      "changing a required driver primary address must block release");
  passed &= expect(
      wrong_nvidia_address_beta.first.accepted() &&
          has_issue_for_item(wrong_nvidia_address_beta.second.issues,
                             catalog::CatalogIssueCode::invalid_field,
                             "nvidia-gpu-driver-page",
                             catalog::CatalogIssueScope::release),
      "the Beta candidate gate must still validate driver source identity");

   passed &= expect(decoded.document->release_state == catalog::ReleaseState::release,
                    "the v0.1.0 catalog must be marked release");
  for (auto const& driver : decoded.document->drivers) {
    passed &= expect(driver.enabled && driver.hardware_kinds ==
                         std::vector<std::string>{"gpu"} &&
                         driver.entry_type.has_value() &&
                         item_has_no_prohibited_identity(driver),
                     "driver entries must be official GPU handoff entries without prohibited content");
  }
  passed &= expect(std::ranges::count_if(
                       decoded.document->drivers, [](catalog::DriverDefinition const& driver) {
                         return driver.entry_type == catalog::DriverEntryType::assistant;
                       }) >= 1 &&
                       std::ranges::count_if(
                           decoded.document->drivers, [](catalog::DriverDefinition const& driver) {
                             return driver.entry_type == catalog::DriverEntryType::vendor_page;
                           }) >= 1,
                   "the initial driver catalog must include an assistant and a vendor page");
  return passed;
}

[[nodiscard]] bool toml_shape_round_trips_software_and_drivers() {
  constexpr std::string_view fixture = R"toml(schema_version = 1
catalog_id = "software"
revision = 7
release_state = "release"
default_locale = "zh-CN"
x_display_banner = "预览"
x_display_sections = ["目录", "维护"]

[[categories]]
id = "tools"
name = "工具"
display_hint = ["推荐", "稳定"]

[categories.localizations."en-US"]
name = "Tools"
display_caption = "Tool category"

[[software]]
id = "core"
enabled = true
name = "工具\n套件"
tier = "basic"
category_id = "tools"
branch = "Windows 个人版"
version_policy = "fixed"
fixed_version = "2.0.0"
dependencies = []
bundled_editions = ["large_offline"]
notice = 'literal # text'
optimization_note = "关闭自启动"
install_profile = "profile-v1"
display_badge = "推荐"
display_tags = ["维护", "离线"]

[[software.sources]]
purpose = "primary"
address = "https://example.test/tool"
version = "2.0.0"
display_source = "官方"

[[software.sources.history]]
version = "1.0"
address = "https://example.test/tool/1.0"
reason = "兼容旧系统"
visible = false
display_history = ["旧版"]

[[software.sources]]
purpose = "alternative"
address = "https://mirror.example.test/tool"
display_source = "镜像"

[software.education]
address = "https://example.test/education"
description = "教育版说明"
display_education = "了解更多"

[software.localizations."en-US"]
name = "Unicode Tool"
notice = "Notice"
optimization_note = "Disable startup"
education_description = "Education description"
display_locale = ["English"]

[[drivers]]
id = "vendor-gpu"
enabled = true
name = "显卡厂商页"
entry_type = "vendor_page"
hardware_kinds = ["gpu"]
branch = "official portal"
version_policy = "fixed"
fixed_version = "600.1"
notice = "由厂商页面处理"
display_driver = ["推荐"]

[[drivers.sources]]
purpose = "primary"
address = "https://example.test/gpu"
version = "600.1"
display_source = "驱动官方"

[[drivers.sources.history]]
version = "599.0"
address = "https://example.test/gpu/599.0"
reason = "回退"
visible = true
display_history = "历史驱动"

[drivers.localizations."en-US"]
name = "GPU vendor page"
notice = "Vendor handles installation"
display_locale = "English driver"
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
  auto generic_gate = catalog::evaluate_release_gate(
      *decoded.document, runtime, default_policy(),
      catalog::content_identity(fixture));
  passed &= expect(generic_gate.passed(),
                   "ordinary driver sources must not inherit issue-19 official-address requirements");
  passed &= expect(decoded.document->software.front().name == "工具\n套件" &&
                       !decoded.document->software.front()
                            .sources.front()
                            .history.front()
                            .visible &&
                       !decoded.document->display_extensions.empty(),
                   "Unicode, escapes, history and display extensions must survive");
  auto const& category = decoded.document->categories.front();
  auto const& software = decoded.document->software.front();
  auto const& driver = decoded.document->drivers.front();
  passed &= expect(
      decoded.document->display_extensions.size() == 2 &&
          category.localizations.front().display_extensions.front().text ==
              "Tool category" &&
          software.fixed_version == "2.0.0" &&
          software.optimization_note == "关闭自启动" &&
          software.install_profile == "profile-v1" &&
          software.sources.size() == 2 &&
          software.sources.front().version == "2.0.0" &&
          software.education->display_extensions.front().text == "了解更多" &&
          software.localizations.front().education_description ==
              "Education description" &&
          driver.fixed_version == "600.1" &&
          driver.sources.front().history.front().reason == "回退" &&
          driver.localizations.front().notice ==
              "Vendor handles installation",
      "the TOML document must retain every editable catalog field family");
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
  auto beta_gate = catalog::evaluate_beta_candidate_release_gate(
      *decoded.document, runtime, default_policy(),
      catalog::content_identity(text));
  passed &= expect(!gate.passed() &&
                       has_issue(gate.issues,
                                 catalog::CatalogIssueCode::release_dependency_error),
                   "local dependency errors must still block formal release");
  passed &= expect(!beta_gate.passed() &&
                       has_issue(beta_gate.issues,
                                 catalog::CatalogIssueCode::release_dependency_error),
                   "local dependency errors must also block the Beta candidate gate");
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
  auto draft_beta_gate = catalog::evaluate_beta_candidate_release_gate(
      *draft.document, draft_runtime, policy,
      catalog::content_identity(draft_text));
  passed &= expect(draft_runtime.accepted() && !draft_gate.passed() &&
                       has_issue(draft_gate.issues,
                                 catalog::CatalogIssueCode::draft_release_state),
                   "draft state is a release-only failure");
  passed &= expect(!draft_beta_gate.passed() &&
                       has_issue(draft_beta_gate.issues,
                                 catalog::CatalogIssueCode::draft_release_state),
                   "draft state must also block the Beta candidate gate");

  auto required_policy = policy;
  required_policy.required_release_software = {"required"};
  auto required_gate = catalog::evaluate_release_gate(
      *released.document, runtime, required_policy,
      catalog::content_identity(released_text));
  auto required_beta_gate = catalog::evaluate_beta_candidate_release_gate(
      *released.document, runtime, required_policy,
      catalog::content_identity(released_text));
  passed &= expect(has_issue(required_gate.issues,
                             catalog::CatalogIssueCode::required_item_missing),
                   "missing required release entries must block release");
  passed &= expect(has_issue(required_beta_gate.issues,
                             catalog::CatalogIssueCode::required_item_missing),
                   "missing required software must block the Beta candidate gate");

  auto disabled_required_document = *released.document;
  disabled_required_document.software.front().enabled = false;
  auto disabled_required_policy = required_policy;
  disabled_required_policy.required_release_software = {"core"};
  auto disabled_required_runtime = catalog::validate_for_runtime(
      disabled_required_document, disabled_required_policy);
  auto disabled_required_beta_gate =
      catalog::evaluate_beta_candidate_release_gate(
          disabled_required_document, disabled_required_runtime,
          disabled_required_policy, catalog::content_identity(released_text));
  passed &= expect(
      disabled_required_runtime.accepted() &&
          has_issue_for_item(disabled_required_beta_gate.issues,
                             catalog::CatalogIssueCode::required_item_disabled,
                             "core", catalog::CatalogIssueScope::release),
      "disabled required software must block the Beta candidate gate");

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
  auto profile_beta_gate = catalog::evaluate_beta_candidate_release_gate(
      *profile.document, profile_runtime, profile_policy,
      catalog::content_identity(profile_text));
  passed &= expect(profile_runtime.accepted() &&
                       has_issue(profile_gate.issues,
                                 catalog::CatalogIssueCode::install_profile_not_release_ready),
                   "known runtime profiles may remain release-incomplete");
  passed &= expect(profile_beta_gate.passed(),
                   "the Beta candidate gate may defer profile release readiness");

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
  auto missing_profile_beta_gate =
      catalog::evaluate_beta_candidate_release_gate(
          *missing_profile.document, missing_profile_runtime,
          missing_profile_policy, catalog::content_identity(missing_profile_text));
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
  passed &= expect(
      has_issue(missing_profile_beta_gate.issues,
                catalog::CatalogIssueCode::install_profile_not_release_ready),
      "the Beta candidate gate must still reject missing profile references");

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

[[nodiscard]] bool lifecycle_beta_candidate_source_gates_and_preserves_strict_errors() {
  auto beta_policy = default_policy();
  beta_policy.required_release_software = {"core"};
  beta_policy.required_release_install_facts = {{.software_id = "core"}};
  auto const unknown_facts = one_item_catalog(
      1, "release", "Unknown Facts", "[]",
      "install_profile = \"profile-v1\"\n");

  LifecycleFixture beta_fixture(
      beta_policy,
      catalog::SoftwareCatalogReleaseGateMode::v0_1_0_beta_candidate);
  beta_fixture.files.files["beta-built-in"] = unknown_facts;
  beta_fixture.files.files["beta-update"] =
      one_item_catalog(2, "release", "Unknown Update", "[]",
                       "install_profile = \"profile-v1\"\n");
  beta_fixture.files.files["beta-manual"] =
      one_item_catalog(2, "release", "Unknown Manual", "[]",
                       "install_profile = \"profile-v1\"\n");

  bool passed = true;
  passed &= expect(beta_fixture.catalog_lifecycle.restore().succeeded(),
                   "Beta lifecycle fixture must restore");

  auto built = preview_built_in(beta_fixture.catalog_lifecycle,
                                beta_fixture.files, "beta-built-in");
  passed &= expect(
      built.ready && built.runtime.accepted() && built.release_gate.passed() &&
          !has_issue(built.release_gate.issues,
                     catalog::CatalogIssueCode::unknown_execution_semantics,
                     catalog::CatalogIssueScope::release),
      "v0.1.0 Beta built-in catalogs may preview with unknown install facts");
  passed &= expect(
      beta_fixture.catalog_lifecycle.apply_preview(built.confirmation_token)
          .succeeded() &&
          beta_fixture.catalog_lifecycle.snapshot().current.has_value(),
      "v0.1.0 Beta built-in catalogs with unknown facts may be applied");

  auto update =
      preview_update(beta_fixture.catalog_lifecycle, beta_fixture.files,
                     "beta-update");
  passed &= expect(
      !update.ready &&
          has_issue(update.release_gate.issues,
                    catalog::CatalogIssueCode::unknown_execution_semantics,
                    catalog::CatalogIssueScope::release),
      "updates must retain the formal gate when install facts are unknown");

  beta_fixture.maintenance_access.access =
      lifecycle::CatalogEditorAccess::debug_mode;
  auto manual =
      preview_manual_import(beta_fixture.catalog_lifecycle, "beta-manual");
  passed &= expect(
      !manual.ready &&
          has_issue(manual.release_gate.issues,
                    catalog::CatalogIssueCode::unknown_execution_semantics,
                    catalog::CatalogIssueScope::release),
      "manual imports must retain the formal gate when install facts are unknown");

  beta_fixture.files.files["beta-built-in-next"] = one_item_catalog(
      2, "release", "Unknown Facts Next", "[]",
      "install_profile = \"profile-v1\"\n");
  auto built_next = preview_built_in(beta_fixture.catalog_lifecycle,
                                     beta_fixture.files, "beta-built-in-next");
  passed &= expect(
      built_next.ready &&
          beta_fixture.catalog_lifecycle
              .apply_preview(built_next.confirmation_token)
              .succeeded(),
      "a second Beta built-in must establish a rollback generation");
  auto rollback = beta_fixture.catalog_lifecycle.preview_rollback();
  passed &= expect(
      !rollback.ready &&
          has_issue(rollback.release_gate.issues,
                    catalog::CatalogIssueCode::unknown_execution_semantics,
                    catalog::CatalogIssueScope::release),
      "rollback must retain the formal gate when the previous catalog has unknown facts");

  auto expect_built_in_error = [&](std::string label,
                                   catalog::SoftwareCatalogPolicy policy,
                                   std::string bytes,
                                   catalog::CatalogIssueCode code) {
    LifecycleFixture fixture(
        std::move(policy),
        catalog::SoftwareCatalogReleaseGateMode::v0_1_0_beta_candidate);
    fixture.files.files[label] = std::move(bytes);
    auto local_passed = expect(fixture.catalog_lifecycle.restore().succeeded(),
                               "strict Beta-error fixture must restore");
    auto preview = preview_built_in(fixture.catalog_lifecycle, fixture.files,
                                    std::move(label));
    local_passed &= expect(
        !preview.ready && has_issue(preview.release_gate.issues, code),
        "Beta candidate gate must preserve strict catalog error");
    return local_passed;
  };

  auto missing_profile_policy = beta_policy;
  auto missing_profile = one_item_catalog(
      1, "release", "Missing Profile", "[]",
      "install_profile = \"missing-profile\"\n");
  auto missing_profile_fixture = LifecycleFixture(
      missing_profile_policy,
      catalog::SoftwareCatalogReleaseGateMode::v0_1_0_beta_candidate);
  missing_profile_fixture.files.files["missing-profile"] = missing_profile;
  passed &= expect(missing_profile_fixture.catalog_lifecycle.restore().succeeded(),
                   "missing profile fixture must restore");
  auto missing_profile_preview = preview_built_in(
      missing_profile_fixture.catalog_lifecycle, missing_profile_fixture.files,
      "missing-profile");
  passed &= expect(
      !missing_profile_preview.ready &&
          has_issue(missing_profile_preview.runtime.issues,
                    catalog::CatalogIssueCode::install_profile_unavailable,
                    catalog::CatalogIssueScope::item) &&
          has_issue(missing_profile_preview.release_gate.issues,
                    catalog::CatalogIssueCode::install_profile_not_release_ready,
                    catalog::CatalogIssueScope::release),
      "a missing install profile must remain unavailable under the Beta gate");

  auto unavailable_profile_policy = beta_policy;
  unavailable_profile_policy.install_profiles.front().runtime_status =
      catalog::InstallProfileRuntimeStatus::missing;
  auto unavailable_profile = one_item_catalog(
      1, "release", "Unavailable Profile", "[]",
      "install_profile = \"profile-v1\"\n");
  auto unavailable_profile_fixture = LifecycleFixture(
      unavailable_profile_policy,
      catalog::SoftwareCatalogReleaseGateMode::v0_1_0_beta_candidate);
  unavailable_profile_fixture.files.files["unavailable-profile"] =
      unavailable_profile;
  passed &= expect(
      unavailable_profile_fixture.catalog_lifecycle.restore().succeeded(),
      "unavailable profile fixture must restore");
  auto unavailable_profile_preview = preview_built_in(
      unavailable_profile_fixture.catalog_lifecycle,
      unavailable_profile_fixture.files, "unavailable-profile");
  passed &= expect(
      !unavailable_profile_preview.ready &&
          has_issue(unavailable_profile_preview.runtime.issues,
                    catalog::CatalogIssueCode::install_profile_unavailable,
                    catalog::CatalogIssueScope::item),
      "an unavailable install profile must remain blocked under the Beta gate");

  auto dependency_policy = beta_policy;
  auto dependency = one_item_catalog(1, "release", "Missing Dependency",
                                     "[\"missing\"]");
  auto dependency_fixture = LifecycleFixture(
      dependency_policy,
      catalog::SoftwareCatalogReleaseGateMode::v0_1_0_beta_candidate);
  dependency_fixture.files.files["missing-dependency"] = dependency;
  passed &= expect(dependency_fixture.catalog_lifecycle.restore().succeeded(),
                   "dependency-error fixture must restore");
  auto dependency_preview = preview_built_in(
      dependency_fixture.catalog_lifecycle, dependency_fixture.files,
      "missing-dependency");
  passed &= expect(
      !dependency_preview.ready &&
          has_issue(dependency_preview.runtime.issues,
                    catalog::CatalogIssueCode::missing_dependency,
                    catalog::CatalogIssueScope::item) &&
          has_issue(dependency_preview.release_gate.issues,
                    catalog::CatalogIssueCode::release_dependency_error,
                    catalog::CatalogIssueScope::release),
      "dependency errors must remain strict under the Beta gate");

  auto driver_policy = beta_policy;
  driver_policy.required_release_drivers = {{
      .id = "required-driver",
      .entry_type = catalog::DriverEntryType::vendor_page,
      .hardware_kind = "gpu",
      .primary_source_address = "https://example.test/required-driver",
  }};
  passed &= expect_built_in_error(
      "missing-driver", std::move(driver_policy),
      one_item_catalog(1, "release", "Missing Driver"),
      catalog::CatalogIssueCode::required_item_missing);

  passed &= expect_built_in_error(
      "draft-catalog", beta_policy,
      one_item_catalog(1, "draft", "Draft Candidate"),
      catalog::CatalogIssueCode::draft_release_state);

  auto revision_policy = default_policy();
  LifecycleFixture revision_fixture(
      revision_policy,
      catalog::SoftwareCatalogReleaseGateMode::v0_1_0_beta_candidate);
  revision_fixture.files.files["revision-baseline"] =
      one_item_catalog(5, "release", "Revision Baseline");
  revision_fixture.files.files["revision-regression"] =
      one_item_catalog(4, "release", "Revision Regression");
  revision_fixture.files.files["revision-conflict"] =
      one_item_catalog(5, "release", "Revision Conflict");
  passed &= expect(revision_fixture.catalog_lifecycle.restore().succeeded(),
                   "revision fixture must restore");
  auto baseline = preview_built_in(revision_fixture.catalog_lifecycle,
                                   revision_fixture.files, "revision-baseline");
  passed &= expect(
      baseline.ready &&
          revision_fixture.catalog_lifecycle
              .apply_preview(baseline.confirmation_token)
              .succeeded(),
      "revision fixture must establish a formal reference");
  auto regression = preview_built_in(revision_fixture.catalog_lifecycle,
                                     revision_fixture.files,
                                     "revision-regression");
  passed &= expect(
      !regression.ready &&
          has_issue(regression.release_gate.issues,
                    catalog::CatalogIssueCode::release_revision_regression,
                    catalog::CatalogIssueScope::release),
      "Beta candidates must not bypass formal revision regression checks");
  auto conflict = preview_built_in(revision_fixture.catalog_lifecycle,
                                   revision_fixture.files, "revision-conflict");
  passed &= expect(
      !conflict.ready &&
          has_issue(conflict.release_gate.issues,
                    catalog::CatalogIssueCode::release_revision_conflict,
                    catalog::CatalogIssueScope::release),
      "Beta candidates must not bypass formal revision conflict checks");

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
  fixture.files.files["manual-1"] = one_item_catalog(1, "release", "Old Local");
  fixture.files.files["invalid"] = replace_once(
      one_item_catalog(4, "release"), "notice = \"\"",
      "target_command = \"unsafe\"\nnotice = \"\"");

  bool passed = true;
  passed &= expect(fixture.catalog_lifecycle.restore().succeeded(),
                   "lifecycle state must initialize and restore");
  auto invalid_built_in = preview_built_in(
      fixture.catalog_lifecycle, fixture.files, "built-in-draft");
  passed &= expect(!invalid_built_in.ready &&
                       invalid_built_in.runtime.accepted() &&
                       !invalid_built_in.release_gate.passed(),
                   "unreleased built-in content must not become a local trial");
  auto built =
      preview_built_in(fixture.catalog_lifecycle, fixture.files, "built-in");
  passed &= expect(built.ready && built.runtime.accepted() &&
                       built.release_gate.passed(),
                   "built-in content must pass both runtime and release gates");
  auto applied = fixture.catalog_lifecycle.apply_preview(built.confirmation_token);
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

  auto invalid =
      preview_update(fixture.catalog_lifecycle, fixture.files, "invalid");
  passed &= expect(!invalid.ready &&
                       fixture.catalog_lifecycle.snapshot().current->revision == 1,
                   "whole-package update errors must preserve current state");
  auto same_revision_conflict = preview_update(
      fixture.catalog_lifecycle, fixture.files, "same-revision-conflict");
  passed &= expect(!same_revision_conflict.ready &&
                       has_issue(
                           same_revision_conflict.release_gate.issues,
                           catalog::CatalogIssueCode::release_revision_conflict),
                   "an update cannot reuse the active formal revision for new content");
  auto draft_update =
      preview_update(fixture.catalog_lifecycle, fixture.files, "draft-3");
  passed &= expect(!draft_update.ready && draft_update.runtime.accepted(),
                   "ordinary updates must require the formal release gate");

  auto display_only = preview_update(fixture.catalog_lifecycle, fixture.files,
                                     "display-only");
  passed &= expect(display_only.ready && display_only.selection_impact.changed.empty(),
                   "display-only changes must not disturb pending selections");

  auto update =
      preview_update(fixture.catalog_lifecycle, fixture.files, "release-2");
  passed &= expect(update.ready && contains(update.selection_impact.changed, "core"),
                   "update preview must report full-definition changes");
  auto const confirmed_update = fixture.files.files["release-2"];
  fixture.files.files["release-2"] =
      one_item_catalog(2, "release", "Changed After Preview");
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(update.confirmation_token)
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

  fixture.maintenance_access.access =
      lifecycle::CatalogEditorAccess::unavailable;
  auto hidden_import =
      preview_manual_import(fixture.catalog_lifecycle, "manual-1");
  passed &= expect(!hidden_import.ready,
                   "manual import must be unavailable outside debug mode");
  fixture.maintenance_access.access = lifecycle::CatalogEditorAccess::debug_mode;
  auto import = preview_manual_import(fixture.catalog_lifecycle, "manual-1");
  passed &= expect(import.ready && import.downgrade &&
                       import.path == "manual-1" && import.revision == 1 &&
                       import.item_count == 1,
                   "debug import must preview path, revision, count and downgrade");
  fixture.maintenance_access.access =
      lifecycle::CatalogEditorAccess::unavailable;
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(import.confirmation_token)
                       .code == lifecycle::CatalogActionCode::debug_mode_required,
                   "confirmation cannot bypass the debug-mode boundary");
  fixture.maintenance_access.access = lifecycle::CatalogEditorAccess::debug_mode;
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(import.confirmation_token)
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
                       .apply_preview(rollback.confirmation_token)
                       .succeeded() &&
                       fixture.catalog_lifecycle.snapshot().current->revision == 2,
                   "rollback must atomically swap current and previous");
  passed &= expect(!preview_update(fixture.catalog_lifecycle, fixture.files,
                                   "absent")
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
  auto built =
      preview_built_in(fixture.catalog_lifecycle, fixture.files, "built-in");
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(built.confirmation_token)
                       .succeeded(),
                   "draft fixture must establish current catalog");

  auto draft_two = replace_once(
      one_item_catalog(2, "draft", "Draft Two"),
      "https://example.test/core",
      "https://draft.example.test/core");
  passed &= expect(fixture.catalog_lifecycle
                       .edit(draft_two)
                       .succeeded() &&
                       fixture.catalog_lifecycle.snapshot().draft.state ==
                           lifecycle::DraftWorkState::unsaved_changes,
                   "editing must retain unsaved content without applying");
  passed &= expect(fixture.catalog_lifecycle.checkpoint_unsaved().succeeded(),
                   "unsaved content must support a subject checkpoint");

  lifecycle::SoftwareCatalogLifecycle recovered(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(recovered.restore().succeeded() &&
                       recovered.snapshot().draft.state ==
                           lifecycle::DraftWorkState::recovered_unsaved &&
                       recovered.snapshot().draft.toml_bytes == draft_two &&
                       recovered.snapshot().current->revision == 1,
                   "restart must recover unsaved content separately from current");
  passed &= expect(recovered.apply_saved_draft().code ==
                       lifecycle::CatalogActionCode::rejected,
                   "recovered unsaved content must block draft application");

  fixture.state_files.fail_next(
      StateFileOperation::read,
      azzs::application::StateFileSlot::checkpoint,
      "injected checkpoint recovery read failure");
  lifecycle::SoftwareCatalogLifecycle failed_recovery(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(failed_recovery.restore().succeeded() &&
                       failed_recovery.snapshot().draft.state ==
                           lifecycle::DraftWorkState::none &&
                       !failed_recovery.snapshot().error.empty(),
                   "checkpoint recovery failure must be visible without false recovery");
  passed &= expect(recovered
                       .handle_close(lifecycle::CatalogCloseChoice::return_to_editor)
                       .code == lifecycle::CatalogActionCode::returned_to_editor &&
                       recovered.snapshot().draft.state ==
                           lifecycle::DraftWorkState::recovered_unsaved,
                   "return-to-editor must preserve recovered edits");
  passed &= expect(recovered
                       .save_draft()
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
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(reopened.restore().succeeded() &&
                       reopened.snapshot().draft.state ==
                           lifecycle::DraftWorkState::saved_not_applied,
                   "saved drafts must survive restart without auto-application");
  passed &= expect(reopened
                       .edit(one_item_catalog(3, "draft", "More Edits"))
                       .succeeded() &&
                       reopened.apply_saved_draft().code ==
                           lifecycle::CatalogActionCode::rejected,
                   "new unsaved edits must block applying an older saved draft");
  passed &= expect(reopened
                       .discard_unsaved()
                       .succeeded() &&
                       reopened.snapshot().draft.saved_present,
                   "discarding unsaved content must preserve the saved draft");
  fixture.maintenance_access.access =
      lifecycle::CatalogEditorAccess::unavailable;
  passed &= expect(reopened.apply_saved_draft().code ==
                       lifecycle::CatalogActionCode::debug_mode_required,
                   "draft application must require debug mode");
  fixture.maintenance_access.access = lifecycle::CatalogEditorAccess::debug_mode;
  auto applied_draft = reopened.apply_saved_draft();
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
                               *typed_edit.document)
                           .succeeded() &&
                       reopened.snapshot().draft.document == typed_edit.document,
                   "graphical editing must use the same typed domain model");
  passed &= expect(reopened
                       .discard_unsaved()
                       .succeeded(),
                   "typed unsaved edits must remain explicitly discardable");

  passed &= expect(reopened
                       .edit("schema_version = [broken")
                       .succeeded() &&
                       reopened.save_draft()
                               .code == lifecycle::CatalogActionCode::rejected &&
                       reopened.snapshot().draft.state ==
                           lifecycle::DraftWorkState::unsaved_changes,
                   "malformed TOML must remain unsaved and must not overwrite a draft");
  passed &= expect(reopened
                       .discard_unsaved()
                       .succeeded(),
                   "malformed unsaved content must be explicitly discardable");

  auto invalid_semantics = replace_once(
      one_item_catalog(4, "draft"), "notice = \"\"",
      "executor_command = \"blocked\"\nnotice = \"\"");
  passed &= expect(reopened
                       .edit(invalid_semantics)
                       .succeeded() &&
                       reopened.handle_close(
                           lifecycle::CatalogCloseChoice::save_draft_and_close)
                           .succeeded() &&
                       reopened.snapshot().draft.validation_failed &&
                       reopened.snapshot().current->revision == 2,
                   "parseable invalid maintenance content may save but never apply");
  passed &= expect(reopened.apply_saved_draft().code ==
                       lifecycle::CatalogActionCode::rejected,
                   "saved whole-package errors must fail runtime revalidation");
  passed &= expect(reopened
                       .delete_saved_draft()
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
  auto uncertain_preview = preview_update(uncertain.catalog_lifecycle,
                                          uncertain.files, "release-1");
  uncertain.state_files.fail_on(InMemoryStateFileSystem::Fault{
      .operation = StateFileOperation::flush_volume,
      .slot = std::nullopt,
      .occurrence = 2,
      .error = "injected post-commit durability uncertainty",
  });
  auto resolved_unknown =
      uncertain.catalog_lifecycle.apply_preview(uncertain_preview.confirmation_token);
  passed &= expect(resolved_unknown.succeeded() &&
                       resolved_unknown.current_changed &&
                       uncertain.catalog_lifecycle.snapshot().current->revision == 1,
                   "an unknown commit outcome must be resolved by rereading exact payload");

  LifecycleFixture isolated;
  isolated.files.files["release-1"] = one_item_catalog(1, "release", "One");
  isolated.files.files["release-2"] = one_item_catalog(2, "release", "Two");
  passed &= expect(isolated.catalog_lifecycle.restore().succeeded(),
                   "aggregate-isolation fixture must restore");
  auto isolated_first =
      preview_update(isolated.catalog_lifecycle, isolated.files, "release-1");
  passed &= expect(isolated.catalog_lifecycle
                       .apply_preview(isolated_first.confirmation_token)
                       .succeeded(),
                   "aggregate-isolation fixture must establish current state");
  auto const isolated_draft_key = azzs::domain::StateKey::for_subject(
      StateSubject{"catalog-test-user"},
      azzs::domain::AggregateId{"software-catalog-draft"});
  isolated.state_files.corrupt(isolated_draft_key,
                               azzs::application::StateFileSlot::current);
  lifecycle::SoftwareCatalogLifecycle draft_read_only(
      isolated.states, isolated.log, isolated.occupancy, isolated.files,
      isolated.codec, isolated.policy, isolated.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(draft_read_only.restore().code ==
                           lifecycle::CatalogActionCode::read_only &&
                       draft_read_only.snapshot().current.has_value() &&
                       draft_read_only.snapshot().current->revision == 1 &&
                       draft_read_only.snapshot().machine_access ==
                           lifecycle::CatalogAggregateAccess::writable &&
                       draft_read_only.snapshot().draft_access ==
                           lifecycle::CatalogAggregateAccess::read_only,
                   "a damaged draft aggregate must not hide the valid machine catalog");
  auto isolated_update =
      preview_update(draft_read_only, isolated.files, "release-2");
  passed &= expect(isolated_update.ready &&
                       draft_read_only
                           .apply_preview(isolated_update.confirmation_token)
                           .succeeded() &&
                       draft_read_only.snapshot().current->revision == 2,
                   "draft read-only state must not block an independent machine update");

  LifecycleFixture busy_fixture;
  busy_fixture.files.files["release-1"] =
      one_item_catalog(1, "release", "One");
  passed &= expect(busy_fixture.catalog_lifecycle.restore().succeeded(),
                   "busy aggregate fixture must restore");
  auto busy_first = preview_update(busy_fixture.catalog_lifecycle,
                                   busy_fixture.files, "release-1");
  passed &= expect(busy_fixture.catalog_lifecycle
                       .apply_preview(busy_first.confirmation_token)
                       .succeeded(),
                   "busy aggregate fixture must establish current state");
  auto busy_draft = one_item_catalog(2, "draft", "Saved While Machine Busy");
  passed &= expect(busy_fixture.catalog_lifecycle
                       .edit(busy_draft)
                       .succeeded() &&
                       busy_fixture.catalog_lifecycle.save_draft()
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
        busy_fixture.maintenance_access, StateSubject{"catalog-test-user"});
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
        busy_fixture.maintenance_access, StateSubject{"catalog-test-user"});
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
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(stale.restore().succeeded(),
                   "a concurrent stale lifecycle must restore its own revision");
  auto first =
      preview_update(fixture.catalog_lifecycle, fixture.files, "release-1");
  auto second = preview_update(stale, fixture.files, "release-2");
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(first.confirmation_token)
                       .succeeded(),
                   "the first CAS application must commit");
  passed &= expect(stale.apply_preview(second.confirmation_token).code ==
                       lifecycle::CatalogActionCode::conflict,
                   "a stale CAS must fail without partial current/previous state");

  lifecycle::SoftwareCatalogLifecycle observer(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(observer.restore().succeeded() &&
                       observer.snapshot().current->revision == 1 &&
                       !observer.snapshot().previous.has_value(),
                   "CAS conflict must preserve the committed machine aggregate");

  auto local_text = header(2, "release");
  local_text += software_entry("broken", "Broken", "[\"missing\"]");
  local_text += software_entry("dependent", "Dependent", "[\"broken\"]");
  local_text += software_entry("independent", "Independent");
  fixture.files.files["local-errors"] = local_text;
  auto local =
      preview_manual_import(fixture.catalog_lifecycle, "local-errors");
  passed &= expect(local.ready && contains(local.selection_impact.disabled, "broken") &&
                       contains(local.selection_impact.disabled, "dependent") &&
                       contains(local.selection_impact.removed, "core"),
                   "local dependency errors must preview a whole-catalog switch");
  auto local_apply = fixture.catalog_lifecycle.apply_preview(local.confirmation_token);
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
                       .edit(draft_three)
                       .succeeded() &&
                       fixture.catalog_lifecycle.save_draft()
                           .succeeded(),
                   "cleanup fixture must persist a saved draft first");
  fixture.state_files.fail_on(InMemoryStateFileSystem::Fault{
      .operation = StateFileOperation::write,
      .slot = azzs::application::StateFileSlot::candidate,
      .occurrence = 2,
      .error = "injected draft cleanup failure",
  });
  auto cleanup_pending = fixture.catalog_lifecycle.apply_saved_draft();
  passed &= expect(cleanup_pending.code ==
                           lifecycle::CatalogActionCode::applied_cleanup_pending &&
                       cleanup_pending.current_changed &&
                       fixture.catalog_lifecycle.snapshot().current->revision == 3 &&
                       fixture.catalog_lifecycle.snapshot().draft.saved_present,
                   "machine apply success must not be rolled back or misreported when draft cleanup fails");

  lifecycle::SoftwareCatalogLifecycle cleanup_recovery(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
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
  auto first = preview_built_in(ledger_fixture.catalog_lifecycle,
                                ledger_fixture.files, "release-1");
  passed &= expect(first.ready &&
                       ledger_fixture.catalog_lifecycle
                           .apply_preview(first.confirmation_token)
                           .succeeded(),
                   "stable-id fixture must establish the first formal catalog");
  auto allowed = preview_update(ledger_fixture.catalog_lifecycle,
                                ledger_fixture.files, "source-update");
  passed &= expect(allowed.ready &&
                       ledger_fixture.catalog_lifecycle
                           .apply_preview(allowed.confirmation_token)
                           .succeeded(),
                   "display and source maintenance must preserve a stable id");
  auto branch_reuse = preview_update(ledger_fixture.catalog_lifecycle,
                                     ledger_fixture.files, "branch-reuse");
  passed &= expect(
      !branch_reuse.ready &&
          has_issue(branch_reuse.runtime.issues,
                    catalog::CatalogIssueCode::stable_id_reused,
                    catalog::CatalogIssueScope::package),
      "changing the product branch anchor must reject stable-id reuse");
  auto kind_reuse = preview_update(ledger_fixture.catalog_lifecycle,
                                   ledger_fixture.files, "kind-reuse");
  passed &= expect(
      !kind_reuse.ready &&
          has_issue(kind_reuse.runtime.issues,
                    catalog::CatalogIssueCode::stable_id_reused,
                    catalog::CatalogIssueScope::package),
      "software and driver entries cannot exchange one stable id");
  auto remove_core = preview_update(ledger_fixture.catalog_lifecycle,
                                    ledger_fixture.files, "remove-core");
  passed &= expect(remove_core.ready &&
                       ledger_fixture.catalog_lifecycle
                           .apply_preview(remove_core.confirmation_token)
                           .succeeded(),
                   "removing an entry must remain a valid catalog update");
  auto removed_target_reuse = preview_update(
      ledger_fixture.catalog_lifecycle, ledger_fixture.files,
      "removed-target-reuse");
  passed &= expect(
      !removed_target_reuse.ready &&
          has_issue(removed_target_reuse.runtime.issues,
                    catalog::CatalogIssueCode::stable_id_reused,
                    catalog::CatalogIssueScope::package),
      "a removed id cannot return with a different product identity");
  auto old_catalog_import =
      preview_manual_import(ledger_fixture.catalog_lifecycle, "source-update");
  passed &= expect(
      old_catalog_import.ready && old_catalog_import.downgrade &&
          ledger_fixture.catalog_lifecycle
              .apply_preview(old_catalog_import.confirmation_token)
              .succeeded() &&
          ledger_fixture.catalog_lifecycle.snapshot().current->revision == 2,
      "debug downgrade may restore the same stable product identity");
  auto explicit_rollback = ledger_fixture.catalog_lifecycle.preview_rollback();
  passed &= expect(
      explicit_rollback.ready &&
          ledger_fixture.catalog_lifecycle
              .apply_preview(explicit_rollback.confirmation_token)
              .succeeded() &&
          ledger_fixture.catalog_lifecycle.snapshot().current->revision == 3,
      "the dedicated rollback path may restore the replaced catalog");

  LifecycleFixture identity_fixture;
  auto formal_policy = default_policy();
  formal_policy.required_release_software = {"core"};
  formal_policy.required_release_install_facts = {{
      .software_id = "core",
      .architectures_confirmed = true,
      .offline_install_confirmed = true,
      .silent_install_confirmed = true,
      .completion_boundary_confirmed = true,
      .post_install_behavior_confirmed = true,
      .restart_verification_confirmed = true,
      .result_detection_confirmed = true,
  }};
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
      identity_fixture.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(formal_lifecycle.restore().succeeded(),
                   "formal identity fixture must restore");
  auto formal = preview_built_in(formal_lifecycle, identity_fixture.files,
                                 "formal");
  passed &= expect(formal.ready &&
                       formal_lifecycle
                           .apply_preview(formal.confirmation_token)
                           .succeeded(),
                   "formal identity fixture must persist its release baseline");

  auto local_policy = formal_policy;
  local_policy.install_profiles.front().release_ready = false;
  lifecycle::SoftwareCatalogLifecycle local_lifecycle(
      identity_fixture.states, identity_fixture.log,
      identity_fixture.occupancy, identity_fixture.files,
      identity_fixture.codec, local_policy,
      identity_fixture.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(local_lifecycle.restore().succeeded(),
                   "local-trial lifecycle must restore the formal baseline");
  auto local_release =
      preview_manual_import(local_lifecycle, "local-release");
  passed &= expect(local_release.ready &&
                       !local_release.release_gate.passed() &&
                       local_lifecycle
                           .apply_preview(local_release.confirmation_token)
                           .succeeded() &&
                       local_lifecycle.snapshot().current->identity ==
                           lifecycle::EffectiveCatalogIdentity::local_trial,
                   "a release-shaped debug import must remain a local trial");

  lifecycle::SoftwareCatalogLifecycle reopened(
      identity_fixture.states, identity_fixture.log,
      identity_fixture.occupancy, identity_fixture.files,
      identity_fixture.codec, formal_policy,
      identity_fixture.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(reopened.restore().succeeded() &&
                       reopened.snapshot().current->revision == 6 &&
                       reopened.snapshot().current->identity ==
                           lifecycle::EffectiveCatalogIdentity::local_trial &&
                       reopened.snapshot().current->release_issues.empty(),
                   "policy improvement after restart must not promote a persisted local trial");
  auto formal_conflict =
      preview_update(reopened, identity_fixture.files, "formal-conflict");
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
                       .edit(old_draft)
                       .succeeded() &&
                       fixture.catalog_lifecycle.save_draft()
                           .succeeded(),
                   "checkpoint cleanup fixture must establish an older saved draft");

  auto new_draft = one_item_catalog(2, "draft", "New Draft");
  passed &= expect(fixture.catalog_lifecycle
                       .edit(new_draft)
                       .succeeded() &&
                       fixture.catalog_lifecycle.checkpoint_unsaved().succeeded(),
                   "new draft edits must have a pre-save recovery checkpoint");
  fixture.state_files.fail_next(
      StateFileOperation::write,
      azzs::application::StateFileSlot::checkpoint_staging,
      "injected checkpoint replacement failure");
  auto saved_with_write_failure = fixture.catalog_lifecycle.save_draft();
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
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
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
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
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
                       .edit(third_draft)
                       .succeeded() &&
                       settled_write_failure.checkpoint_unsaved().succeeded(),
                   "consume-failure fixture must establish another checkpoint");
  fixture.state_files.fail_next(
      StateFileOperation::write,
      azzs::application::StateFileSlot::checkpoint_consumed_staging,
      "injected checkpoint consumption failure");
  auto saved_with_consume_failure = settled_write_failure.save_draft();
  passed &= expect(
          saved_with_consume_failure.code ==
              lifecycle::CatalogActionCode::saved_cleanup_pending &&
          saved_with_consume_failure.succeeded() &&
          settled_write_failure.snapshot().draft.toml_bytes == third_draft,
      "checkpoint consumption failure must not turn a committed save into failure");

  lifecycle::SoftwareCatalogLifecycle recovered_consume_failure(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
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
  auto deleted_with_cleanup_failure = recovered_consume_failure.delete_saved_draft();
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
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
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

[[nodiscard]] bool quoted_display_extension_keys_round_trip() {
  TomlSoftwareCatalogCodec codec;
  auto decoded = codec.decode(
      one_item_catalog(1, "release") +
      "\n\"extra label_label\" = \"quoted key must survive\"\n");
  bool passed = true;
  passed &= expect(decoded.issues.empty() && decoded.document.has_value(),
                   "quoted display extension keys must decode");
  if (!decoded.document.has_value()) {
    return false;
  }
  auto const encoded = codec.encode(*decoded.document);
  auto again = codec.decode(encoded);
  passed &= expect(again.issues.empty() && again.document == decoded.document,
                   "quoted display extension keys must round-trip");
  return passed;
}

[[nodiscard]] bool file_adapter_enforces_bound_during_streaming_reads() {
  constexpr std::size_t k_catalog_limit = 16U * 1024U * 1024U;
  auto const path = std::filesystem::temp_directory_path() /
                    ("azzs-software-catalog-limit-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()) +
                     ".toml");
  {
    std::ofstream output(path, std::ios::binary);
    std::string block(64U * 1024U, 'x');
    for (std::size_t written{}; written < k_catalog_limit;
         written += block.size()) {
      output.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    output.put('y');
  }
  LocalSoftwareCatalogFileReader reader;
  auto const read = reader.read(path.string());
  std::error_code removal_error;
  std::filesystem::remove(path, removal_error);
  return expect(!read.succeeded &&
                    read.error == "catalog file exceeds the supported size",
                "streaming file reads must reject growth beyond 16 MiB");
}

[[nodiscard]] bool prohibited_content_is_rejected_by_every_catalog_entry() {
  TomlSoftwareCatalogCodec codec;
  auto explanatory_text = replace_once(
      one_item_catalog(1, "release", "Core", "[]", {},
                       "This notice explains why Keygen resources are prohibited."),
      "[[categories]]",
      "display_note = \"A Crack example is documentation, not a resource.\"\n\n"
      "[[categories]]");
  explanatory_text +=
      "\n[software.education]\n"
      "address = \"https://example.test/education\"\n"
      "description = \"This guide explains why authorization bypass is unsafe.\"\n"
      "\n[software.localizations.\"en-US\"]\n"
      "notice = \"The word crack appears here only as an explanation.\"\n"
      "display_help = \"Keygen terminology is educational text.\"\n";
  auto explanatory = codec.decode(explanatory_text);
  auto prohibited_identity = codec.decode(replace_once(
      one_item_catalog(1, "release"), "name = \"Core\"",
      "name = \"Keygen\""));
  auto prohibited = codec.decode(replace_once(
      one_item_catalog(1, "release", "Trusted"),
      "https://example.test/core", "https://example.test/keygen"));
  bool passed = true;
  passed &= expect(
      explanatory.document.has_value() &&
          catalog::validate_for_runtime(*explanatory.document, default_policy())
              .accepted(),
      "SEC-08 must not reject explanatory localization, notice, display, or education text");
  passed &= expect(
      prohibited_identity.document.has_value() &&
          !catalog::validate_for_runtime(*prohibited_identity.document,
                                         default_policy())
               .accepted(),
      "SEC-08 must reject an explicit prohibited resource identity");
  passed &= expect(prohibited.document.has_value(),
                   "prohibited-content fixture must remain structurally valid");
  if (!prohibited.document.has_value()) {
    return false;
  }
  auto runtime = catalog::validate_for_runtime(*prohibited.document,
                                                default_policy());
  passed &= expect(!runtime.accepted(),
                   "SEC-08 content must reject a runtime catalog");

  LifecycleFixture fixture;
  auto const prohibited_bytes = codec.encode(*prohibited.document);
  fixture.files.files["built-in-prohibited"] = prohibited_bytes;
  fixture.files.files["update-prohibited"] = prohibited_bytes;
  fixture.files.files["manual-prohibited"] = prohibited_bytes;
  passed &= expect(fixture.catalog_lifecycle.restore().succeeded(),
                   "prohibited manual-import fixture must restore");
  auto built_in = preview_built_in(fixture.catalog_lifecycle, fixture.files,
                                   "built-in-prohibited");
  auto update =
      preview_update(fixture.catalog_lifecycle, fixture.files, "update-prohibited");
  auto manual =
      preview_manual_import(fixture.catalog_lifecycle, "manual-prohibited");
  passed &= expect(!built_in.ready && !update.ready && !manual.ready,
                   "SEC-08 must reject built-in, update, and manual imports");
  return passed;
}

[[nodiscard]] bool untrusted_callers_cannot_promote_manual_bytes_to_built_in() {
  LifecycleFixture fixture;
  fixture.files.files["trusted-built-in"] = one_item_catalog(
      1, "release", "Trusted");
  fixture.files.files["local-release"] =
      one_item_catalog(2, "release", "Local bytes");
  bool passed = true;
  passed &= expect(fixture.catalog_lifecycle.restore().succeeded(),
                   "trusted-source fixture must restore");
  auto trusted = preview_built_in(fixture.catalog_lifecycle, fixture.files,
                                  "trusted-built-in");
  auto local =
      preview_manual_import(fixture.catalog_lifecycle, "local-release");
  passed &= expect(trusted.ready && trusted.path == "trusted-built-in" &&
                       local.ready &&
                       local.origin == lifecycle::CatalogCandidateOrigin::manual_import,
                   "callers can only choose a local path through manual import");
  passed &= expect(fixture.catalog_lifecycle
                       .apply_preview(local.confirmation_token)
                       .succeeded() &&
                       fixture.catalog_lifecycle.snapshot().current->identity ==
                           lifecycle::EffectiveCatalogIdentity::local_trial,
                   "local TOML cannot be promoted to a built-in release by origin arguments");
  return passed;
}

[[nodiscard]] bool restored_unknown_catalog_mode_fails_closed_for_writes() {
  LifecycleFixture fixture;
  auto future_policy = default_policy();
  future_policy.supported_schema_version = 2;
  fixture.files.files["future-built-in"] = replace_once(
      one_item_catalog(1, "release", "Future"), "schema_version = 1",
      "schema_version = 2");

  lifecycle::SoftwareCatalogLifecycle future(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, future_policy, fixture.maintenance_access,
      StateSubject{"catalog-test-user"});
  bool passed = true;
  passed &= expect(future.restore().succeeded(),
                   "future-workbench fixture must restore");
  auto future_preview =
      preview_built_in(future, fixture.files, "future-built-in");
  passed &= expect(future_preview.ready &&
                       future.apply_preview(future_preview.confirmation_token)
                           .succeeded(),
                   "future-workbench fixture must persist its catalog");
  auto const future_checkpoint = replace_once(
      one_item_catalog(2, "draft", "Future Checkpoint"),
      "schema_version = 1", "schema_version = 2");
  passed &= expect(future.edit(future_checkpoint).succeeded() &&
                       future.checkpoint_unsaved().succeeded(),
                   "future-workbench fixture must retain an unsaved checkpoint");

  lifecycle::SoftwareCatalogLifecycle old(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, default_policy(), fixture.maintenance_access,
      StateSubject{"catalog-test-user"});
  fixture.files.files["future-update"] = one_item_catalog(3, "release", "Update");
  fixture.files.files["future-manual"] = one_item_catalog(3, "draft", "Manual");
  auto const machine_key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"software-catalog-active"});
  auto const draft_key = azzs::domain::StateKey::for_subject(
      StateSubject{"catalog-test-user"},
      azzs::domain::AggregateId{"software-catalog-draft"});
  auto const current_before = fixture.state_files.raw_file(
      machine_key, azzs::application::StateFileSlot::current);
  auto const previous_before = fixture.state_files.raw_file(
      machine_key, azzs::application::StateFileSlot::previous);
  auto const draft_before = fixture.state_files.raw_file(
      draft_key, azzs::application::StateFileSlot::current);
  auto const log_count_before = fixture.log.events.size();
  auto restored = old.restore();
  auto built_in_preview = preview_built_in(old, fixture.files, "future-built-in");
  auto update_preview = preview_update(old, fixture.files, "future-update");
  auto manual_preview = preview_manual_import(old, "future-manual");
  auto rollback_preview = old.preview_rollback();
  passed &= expect(restored.code == lifecycle::CatalogActionCode::read_only &&
                       old.snapshot().mode ==
                           lifecycle::CatalogLifecycleMode::read_only &&
                       old.snapshot().retained_unreadable_current_toml_bytes ==
                           fixture.files.files["future-built-in"],
                   "an unknown stored catalog mode must enter read-only recovery");
  passed &= expect(
      !built_in_preview.ready && !update_preview.ready && !manual_preview.ready &&
          !rollback_preview.ready &&
          old.apply_preview("stale").code == lifecycle::CatalogActionCode::read_only &&
          fixture.state_files.raw_file(machine_key,
                                       azzs::application::StateFileSlot::current) ==
              current_before &&
          fixture.state_files.raw_file(machine_key,
                                       azzs::application::StateFileSlot::previous) ==
              previous_before &&
          fixture.state_files.raw_file(draft_key,
                                       azzs::application::StateFileSlot::current) ==
              draft_before &&
          fixture.log.events.size() == log_count_before,
      "unknown current mode must preserve state and short-circuit every preview and log path");
  auto const writes_are_closed =
      old.apply_preview("stale").code == lifecycle::CatalogActionCode::read_only &&
      old.edit(one_item_catalog(2, "draft")).code ==
          lifecycle::CatalogActionCode::read_only &&
      old.checkpoint_unsaved().code == lifecycle::CatalogActionCode::read_only &&
      old.save_draft().code == lifecycle::CatalogActionCode::read_only &&
      old.delete_saved_draft().code == lifecycle::CatalogActionCode::read_only &&
      old.discard_unsaved().code == lifecycle::CatalogActionCode::read_only &&
      old.apply_saved_draft().code == lifecycle::CatalogActionCode::read_only &&
      old.handle_close(lifecycle::CatalogCloseChoice::return_to_editor).code ==
          lifecycle::CatalogActionCode::read_only;
  passed &= expect(writes_are_closed,
                   "unknown stored catalog modes must fail closed at every write use case");
  lifecycle::SoftwareCatalogLifecycle upgraded_again(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, future_policy, fixture.maintenance_access,
      StateSubject{"catalog-test-user"});
  passed &= expect(upgraded_again.restore().succeeded() &&
                       upgraded_again.snapshot().current_toml_bytes ==
                           fixture.files.files["future-built-in"] &&
                       upgraded_again.snapshot().draft.state ==
                           lifecycle::DraftWorkState::recovered_unsaved &&
                       upgraded_again.snapshot().draft.toml_bytes ==
                           future_checkpoint,
                   "read-only recovery must preserve unknown catalog and checkpoint bytes for a newer workbench");

  LifecycleFixture previous_unknown_fixture;
  auto schema_two_policy = default_policy();
  schema_two_policy.supported_schema_version = 2;
  previous_unknown_fixture.files.files["schema-two-previous"] = replace_once(
      one_item_catalog(1, "release", "Future Previous"), "schema_version = 1",
      "schema_version = 2");
  previous_unknown_fixture.files.files["schema-one-current"] =
      one_item_catalog(2, "release", "Readable Current");
  previous_unknown_fixture.files.files["blocked-built-in"] =
      one_item_catalog(3, "release", "Blocked Built-in");
  previous_unknown_fixture.files.files["blocked-update"] =
      one_item_catalog(3, "release", "Blocked Update");
  previous_unknown_fixture.files.files["blocked-manual"] =
      one_item_catalog(3, "draft", "Blocked Manual");
  SchemaTwoCompatibilityCodec schema_two_codec;
  lifecycle::SoftwareCatalogLifecycle newer(
      previous_unknown_fixture.states, previous_unknown_fixture.log,
      previous_unknown_fixture.occupancy, previous_unknown_fixture.files,
      schema_two_codec, schema_two_policy,
      previous_unknown_fixture.maintenance_access,
      StateSubject{"catalog-test-user"});
  passed &= expect(newer.restore().succeeded(),
                   "schema-two compatibility fixture must restore");
  auto future_previous = preview_built_in(
      newer, previous_unknown_fixture.files, "schema-two-previous");
  passed &= expect(
      future_previous.ready &&
          newer.apply_preview(future_previous.confirmation_token).succeeded(),
      "schema-two compatibility fixture must establish its future previous generation");
  auto readable_current = preview_update(
      newer, previous_unknown_fixture.files, "schema-one-current");
  passed &= expect(
      readable_current.ready &&
          newer.apply_preview(readable_current.confirmation_token).succeeded(),
      "schema-two compatibility fixture must create its readable current generation");

  auto const previous_machine_key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"software-catalog-active"});
  auto const previous_draft_key = azzs::domain::StateKey::for_subject(
      StateSubject{"catalog-test-user"},
      azzs::domain::AggregateId{"software-catalog-draft"});
  static_cast<void>(previous_unknown_fixture.state_files.remove(
      previous_draft_key, azzs::application::StateFileSlot::current));
  auto const previous_current_before = previous_unknown_fixture.state_files.raw_file(
      previous_machine_key, azzs::application::StateFileSlot::current);
  auto const previous_generation_before =
      previous_unknown_fixture.state_files.raw_file(
          previous_machine_key, azzs::application::StateFileSlot::previous);
  auto const previous_draft_before = previous_unknown_fixture.state_files.raw_file(
      previous_draft_key, azzs::application::StateFileSlot::current);
  auto const previous_log_count_before =
      previous_unknown_fixture.log.events.size();
  lifecycle::SoftwareCatalogLifecycle old_with_unknown_previous(
      previous_unknown_fixture.states, previous_unknown_fixture.log,
      previous_unknown_fixture.occupancy, previous_unknown_fixture.files,
      previous_unknown_fixture.codec, default_policy(),
      previous_unknown_fixture.maintenance_access,
      StateSubject{"catalog-test-user"});
  auto previous_restored = old_with_unknown_previous.restore();
  auto blocked_built_in = preview_built_in(
      old_with_unknown_previous, previous_unknown_fixture.files,
      "blocked-built-in");
  auto blocked_update = preview_update(
      old_with_unknown_previous, previous_unknown_fixture.files,
      "blocked-update");
  auto blocked_manual = preview_manual_import(old_with_unknown_previous,
                                              "blocked-manual");
  auto blocked_rollback = old_with_unknown_previous.preview_rollback();
  passed &= expect(
      previous_restored.code == lifecycle::CatalogActionCode::read_only &&
          old_with_unknown_previous.snapshot().current.has_value() &&
          old_with_unknown_previous.snapshot().current->revision == 2 &&
          !blocked_built_in.ready && !blocked_update.ready &&
          !blocked_manual.ready && !blocked_rollback.ready &&
          old_with_unknown_previous.apply_preview("stale").code ==
              lifecycle::CatalogActionCode::read_only &&
          previous_unknown_fixture.state_files.raw_file(
              previous_machine_key, azzs::application::StateFileSlot::current) ==
              previous_current_before &&
          previous_unknown_fixture.state_files.raw_file(
              previous_machine_key, azzs::application::StateFileSlot::previous) ==
              previous_generation_before &&
          previous_unknown_fixture.state_files.raw_file(
              previous_draft_key, azzs::application::StateFileSlot::current) ==
              previous_draft_before &&
          previous_unknown_fixture.log.events.size() == previous_log_count_before,
      "an unknown previous mode must block draft initialization, application, previews, and logs while retaining both generations");
  return passed;
}

[[nodiscard]] bool checkpoint_conflicts_preserve_other_instance_edits() {
  LifecycleFixture fixture;
  bool passed = true;
  passed &= expect(fixture.catalog_lifecycle.restore().succeeded(),
                   "checkpoint-conflict fixture must initialize state");

  lifecycle::SoftwareCatalogLifecycle first(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
  lifecycle::SoftwareCatalogLifecycle second(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(first.restore().succeeded() && second.restore().succeeded(),
                   "both catalog instances must restore the same subject state");

  auto const first_bytes = one_item_catalog(1, "draft", "First edits");
  auto const second_bytes = one_item_catalog(2, "draft", "Second save");
  passed &= expect(first
                       .edit(first_bytes)
                       .succeeded() &&
                       first.checkpoint_unsaved().succeeded(),
                   "first instance must publish an unsaved checkpoint");
  passed &= expect(second
                       .edit(second_bytes)
                       .succeeded() &&
                       second.save_draft()
                           .succeeded(),
                   "second instance must save its own draft");

  lifecycle::SoftwareCatalogLifecycle recovered(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, fixture.maintenance_access, StateSubject{"catalog-test-user"});
  passed &= expect(
      recovered.restore().succeeded() &&
          recovered.snapshot().draft.saved_present &&
          recovered.snapshot().draft.state ==
              lifecycle::DraftWorkState::recovered_unsaved &&
          recovered.snapshot().draft.toml_bytes == first_bytes,
      "a conflicting checkpoint must remain recoverable until explicitly handled");
  passed &= expect(recovered.discard_unsaved().succeeded(),
                   "only an explicit discard may tombstone conflicting edits");
  lifecycle::SoftwareCatalogLifecycle settled(
      fixture.states, fixture.log, fixture.occupancy, fixture.files,
      fixture.codec, fixture.policy, fixture.maintenance_access,
      StateSubject{"catalog-test-user"});
  passed &= expect(settled.restore().succeeded() &&
                       settled.snapshot().draft.state ==
                           lifecycle::DraftWorkState::saved_not_applied &&
                       settled.snapshot().draft.toml_bytes == second_bytes,
                   "tombstoning conflicting edits must preserve the other instance's saved draft");
  return passed;
}

[[nodiscard]] bool temporary_recovery_session_cannot_delete_or_apply() {
  LifecycleFixture fixture;
  bool passed = true;
  passed &= expect(fixture.catalog_lifecycle.restore().succeeded(),
                   "temporary-session fixture must restore");
  passed &= expect(fixture.catalog_lifecycle
                       .edit(one_item_catalog(1, "draft"))
                       .succeeded() &&
                       fixture.catalog_lifecycle.save_draft()
                           .succeeded(),
                   "temporary-session fixture must establish a saved draft");
  fixture.maintenance_access.access =
      lifecycle::CatalogEditorAccess::temporary_close_recovery;
  passed &= expect(fixture.catalog_lifecycle
                       .edit(one_item_catalog(2, "draft", "Temporary Edit"))
                       .succeeded() &&
                       fixture.catalog_lifecycle.checkpoint_unsaved().code ==
                           lifecycle::CatalogActionCode::rejected &&
                       fixture.catalog_lifecycle.discard_unsaved().succeeded(),
                   "temporary close recovery may edit and discard but cannot checkpoint");
  passed &= expect(fixture.catalog_lifecycle
                       .edit(one_item_catalog(3, "draft", "Temporary Save"))
                       .succeeded() &&
                       fixture.catalog_lifecycle.save_draft().succeeded(),
                   "temporary close recovery may save an edited draft");
  passed &= expect(fixture.catalog_lifecycle.delete_saved_draft().code ==
                       lifecycle::CatalogActionCode::rejected,
      "temporary close recovery must not delete a saved draft");
  passed &= expect(
      fixture.catalog_lifecycle.apply_saved_draft().code ==
          lifecycle::CatalogActionCode::rejected,
      "temporary close recovery must not apply a saved draft");
  return passed;
}

[[nodiscard]] bool debug_mode_editor_checkpoints_and_recovers_unsaved_edits() {
  DebugModeCatalogEditorFixture fixture;
  fixture.files.files["built-in"] =
      one_item_catalog(1, "release", "Current Catalog");
  auto first = fixture.make_lifecycle();
  fixture.editor.bind_catalog_lifecycle(*first);

  bool passed = true;
  passed &= expect(first->restore().succeeded(),
                   "debug editor checkpoint fixture must restore");
  auto built = preview_built_in(*first, fixture.files, "built-in");
  passed &= expect(
      built.ready && first->apply_preview(built.confirmation_token).succeeded(),
      "debug editor checkpoint fixture must establish the current catalog");
  passed &= expect(
      fixture.editor.set_enabled(true).code ==
          azzs::application::ApplicationSettingsDebugActionCode::updated,
      "debug editor checkpoint fixture must enable debug mode");

  auto typed_edit = fixture.codec.decode(
      one_item_catalog(2, "draft", "Recovered Typed Draft"));
  passed &= expect(typed_edit.document.has_value(),
                   "debug editor checkpoint fixture must decode typed content");
  if (!typed_edit.document.has_value()) {
    return false;
  }
  passed &= expect(
      fixture.editor.edit_document(*typed_edit.document).succeeded() &&
          first->snapshot().draft.state ==
              lifecycle::DraftWorkState::unsaved_changes &&
          first->snapshot().current->revision == 1,
      "typed debug edits must remain unsaved without replacing current catalog");

  first.reset();
  auto recovered = fixture.make_lifecycle();
  fixture.editor.bind_catalog_lifecycle(*recovered);
  passed &= expect(
      recovered->restore().succeeded() &&
          recovered->snapshot().draft.state ==
              lifecycle::DraftWorkState::recovered_unsaved &&
          recovered->snapshot().draft.document == typed_edit.document &&
          recovered->snapshot().current->revision == 1,
      "debug editor edits must automatically checkpoint and recover without changing current catalog");
  passed &= expect(
      fixture.editor
              .handle_close(lifecycle::CatalogCloseChoice::return_to_editor)
              .code == lifecycle::CatalogActionCode::returned_to_editor &&
          recovered->snapshot().draft.state ==
              lifecycle::DraftWorkState::recovered_unsaved,
      "returning to the editor must retain recovered unsaved content");
  passed &= expect(
      fixture.editor
              .handle_close(
                  lifecycle::CatalogCloseChoice::save_draft_and_close)
              .succeeded() &&
          recovered->snapshot().draft.state ==
              lifecycle::DraftWorkState::saved_not_applied &&
          recovered->snapshot().draft.saved_present &&
          recovered->snapshot().current->revision == 1,
      "save-and-close must persist a draft without applying it");

  auto later_edit = fixture.codec.decode(
      one_item_catalog(3, "draft", "Discarded Typed Draft"));
  passed &= expect(later_edit.document.has_value() &&
                       fixture.editor.edit_document(*later_edit.document)
                           .succeeded(),
                   "discard-close fixture must establish later unsaved typed content");
  passed &= expect(
      fixture.editor
              .handle_close(
                  lifecycle::CatalogCloseChoice::discard_unsaved_and_close)
              .succeeded() &&
          recovered->snapshot().draft.state ==
              lifecycle::DraftWorkState::saved_not_applied &&
          recovered->snapshot().draft.document == typed_edit.document &&
          recovered->snapshot().current->revision == 1,
      "discard-and-close must restore the saved draft without applying either edit");
  return passed;
}

[[nodiscard]] bool temporary_debug_editor_recovery_is_reason_scoped_and_revocable() {
  DebugModeCatalogEditorFixture fixture;
  auto first = fixture.make_lifecycle();
  fixture.editor.bind_catalog_lifecycle(*first);

  bool passed = true;
  passed &= expect(
      first->restore().succeeded() &&
          fixture.editor.set_enabled(true).code ==
              azzs::application::ApplicationSettingsDebugActionCode::updated,
      "temporary debug editor fixture must restore with debug mode enabled");
  auto recovered_document = fixture.codec.decode(
      one_item_catalog(1, "draft", "Recovered Before Temporary Access"));
  passed &= expect(
      recovered_document.document.has_value() &&
          fixture.editor.edit_document(*recovered_document.document).succeeded() &&
          first->snapshot().draft.state == lifecycle::DraftWorkState::unsaved_changes,
      "the fixture must establish checkpointed unsaved editor content");

  first.reset();
  auto recovered = fixture.make_lifecycle();
  fixture.editor.bind_catalog_lifecycle(*recovered);
  passed &= expect(
      recovered->restore().succeeded() &&
          recovered->snapshot().draft.state ==
              lifecycle::DraftWorkState::recovered_unsaved &&
          fixture.editor.set_enabled(false).code ==
              azzs::application::ApplicationSettingsDebugActionCode::updated,
      "the hidden editor fixture must restore recovered unsaved content");

  passed &= expect(
      fixture.editor.begin_temporary_close_recovery(
          azzs::application::CatalogEditorTemporaryAccessReason::close_return) &&
          fixture.editor.editor_snapshot().settings.temporary_close_recovery,
      "a close-return flow may temporarily reopen recovered unsaved content");
  fixture.editor.end_temporary_close_recovery();

  passed &= expect(
      fixture.editor.begin_temporary_close_recovery(
          azzs::application::CatalogEditorTemporaryAccessReason::
              recovered_unsaved_continue),
      "the startup recovery action must accept only recovered unsaved content");
  auto edited_document = fixture.codec.decode(
      one_item_catalog(2, "draft", "Edited From Recovered Content"));
  passed &= expect(
      edited_document.document.has_value() &&
          fixture.editor.edit_document(*edited_document.document).succeeded() &&
          recovered->snapshot().draft.state ==
              lifecycle::DraftWorkState::unsaved_changes,
      "editing recovered content must transition it to ordinary unsaved changes");
  fixture.editor.end_temporary_close_recovery();

  passed &= expect(
      !fixture.editor.begin_temporary_close_recovery(
          azzs::application::CatalogEditorTemporaryAccessReason::
              recovered_unsaved_continue) &&
          fixture.editor.editor_access() ==
              lifecycle::CatalogEditorAccess::unavailable,
      "the startup recovery action must reject ordinary unsaved changes");
  passed &= expect(
      fixture.editor.begin_temporary_close_recovery(
          azzs::application::CatalogEditorTemporaryAccessReason::close_return) &&
          fixture.editor.checkpoint_unsaved().code ==
              lifecycle::CatalogActionCode::rejected &&
          fixture.editor.apply_saved_draft().code ==
              lifecycle::CatalogActionCode::rejected &&
          fixture.editor.save_draft().succeeded() &&
          recovered->snapshot().draft.state ==
              lifecycle::DraftWorkState::saved_not_applied,
      "a close-return flow may save but cannot checkpoint or apply ordinary unsaved changes");
  fixture.editor.end_temporary_close_recovery();

  auto const denied_after_end = fixture.editor.edit_document(*edited_document.document);
  passed &= expect(
      denied_after_end.code == lifecycle::CatalogActionCode::debug_mode_required &&
          fixture.editor.editor_access() ==
              lifecycle::CatalogEditorAccess::unavailable,
      "ending temporary recovery must restore the hidden editor boundary");
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
  auto preview = preview_built_in(preview_failure.catalog_lifecycle,
                                  preview_failure.files, "release-1");
  auto blocked =
      preview_failure.catalog_lifecycle.apply_preview(preview.confirmation_token);
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
  auto prepared = preview_built_in(apply_log_failure.catalog_lifecycle,
                                   apply_log_failure.files, "release-1");
  passed &= expect(prepared.ready && prepared.log_persisted,
                   "a persisted preview must remain applicable");
  apply_log_failure.log.fail_next_append("injected final apply log failure");
  auto applied =
      apply_log_failure.catalog_lifecycle.apply_preview(prepared.confirmation_token);
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
  auto active = preview_built_in(no_change_log_failure.catalog_lifecycle,
                                 no_change_log_failure.files, "release-1");
  passed &= expect(
      active.ready &&
          no_change_log_failure.catalog_lifecycle
              .apply_preview(active.confirmation_token)
              .succeeded() &&
          no_change_log_failure.catalog_lifecycle
              .edit(same_catalog)
              .succeeded() &&
          no_change_log_failure.catalog_lifecycle.save_draft()
              .succeeded(),
      "no-change fixture must establish an identical saved draft");
  no_change_log_failure.log.fail_after_appends(
      1, "injected no-change final log failure");
  auto no_change =
      no_change_log_failure.catalog_lifecycle.apply_saved_draft();
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
  passed &= initial_catalog_content_contract();
  passed &= toml_shape_round_trips_software_and_drivers();
  passed &= package_errors_and_disabled_minimum_are_separate();
  passed &= dependency_errors_disable_only_the_local_closure();
  passed &= release_gate_is_distinct_and_versioned();
  passed &= lifecycle_beta_candidate_source_gates_and_preserves_strict_errors();
  passed &= lifecycle_updates_imports_downgrades_and_rolls_back();
  passed &= drafts_checkpoints_and_close_choices_do_not_apply();
  passed &= atomic_apply_conflicts_local_errors_and_cleanup_recovery();
  passed &= persisted_identity_and_stable_id_ledger_are_monotonic();
  passed &= draft_checkpoint_cleanup_is_post_commit_and_recoverable();
  passed &= quoted_display_extension_keys_round_trip();
  passed &= file_adapter_enforces_bound_during_streaming_reads();
  passed &= prohibited_content_is_rejected_by_every_catalog_entry();
  passed &= untrusted_callers_cannot_promote_manual_bytes_to_built_in();
  passed &= restored_unknown_catalog_mode_fails_closed_for_writes();
  passed &= checkpoint_conflicts_preserve_other_instance_edits();
  passed &= temporary_recovery_session_cannot_delete_or_apply();
  passed &= debug_mode_editor_checkpoints_and_recovers_unsaved_edits();
  passed &= temporary_debug_editor_recovery_is_reason_scoped_and_revocable();
  passed &= execution_log_receipts_gate_and_qualify_application();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "software catalog contract passed\n";
  return EXIT_SUCCESS;
}
