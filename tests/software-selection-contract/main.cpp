#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/application/architecture_selection.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/domain/software_selection.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace architecture = azzs::domain::architecture_selection;
namespace catalog = azzs::domain::software_catalog;
namespace selection = azzs::domain::software_selection;
namespace app_selection = azzs::application::software_selection;
namespace catalog_lifecycle = azzs::application::software_catalog;
using azzs::application::CorrelationId;
using azzs::application::DeviceStateStore;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogReceipt;
using azzs::application::PlatformInfo;
using azzs::application::WallClockTime;
using azzs::domain::StateSubject;
using azzs::domain::SystemArchitecture;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryStateFileSystem;

[[nodiscard]] bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "software selection contract failed: " << message << '\n';
  }
  return condition;
}

class RecordingLog final : public ExecutionLog {
 public:
  [[nodiscard]] CorrelationId begin_correlation() override {
    return {.value = "software-selection-" + std::to_string(next_++)};
  }

  [[nodiscard]] ExecutionLogReceipt append(
      CorrelationId const&, ExecutionEvent const& event) override {
    events.push_back(event);
    return {.persisted = true, .sequence = next_++};
  }

  [[nodiscard]] ExecutionLogClearReceipt clear() override { return {.cleared = true}; }

  [[nodiscard]] DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const&) override {
    return {.produced = true};
  }

  std::vector<ExecutionEvent> events;

 private:
  std::uint64_t next_{1};
};

class FixedPlatform final : public PlatformInfo {
 public:
  [[nodiscard]] std::optional<azzs::domain::SystemVersion> windows_version()
      const override {
    return azzs::domain::SystemVersion{10, 0, 19045};
  }

  [[nodiscard]] SystemArchitecture windows_architecture() const override {
    return SystemArchitecture::x64;
  }
};

class FixtureResolver final : public app_selection::ControlledSourceResolver {
 public:
  [[nodiscard]] app_selection::SourceResolutionResult resolve(
      std::string_view software_id,
      catalog::CatalogSource const& declared_source) override {
    ++calls;
    if (fail) {
      return {.error = "controlled parser did not produce a release"};
    }
    return {
        .resolved = true,
        .snapshot = selection::ResolvedSourceSnapshot{
            .software_id = std::string{software_id},
            .declared_purpose = *declared_source.purpose,
            .declared_address = declared_source.address,
            .version = "2.3.4",
            .actual_address = "https://downloads.example.test/editor-2.3.4.msi",
            .hosting_mechanism = "controlled-release-asset",
            .branch = "stable",
            .packages = {{
                .candidate = {.software_id = std::string{software_id},
                              .architecture = architecture::PackageArchitecture::x64,
                              .version = "2.3.4",
                              .identity = "editor-2.3.4-x64"},
                .package_type = selection::PackageType::full_package,
                .complete_package = true,
            }},
            .resolved_at_milliseconds = 1'000,
            .capability_version = "resolver-v1",
        }};
  }

  std::size_t calls{};
  bool fail{false};
};

class FixtureNetwork final : public app_selection::NetworkObserver {
 public:
  [[nodiscard]] bool available() const noexcept override { return online; }
  bool online{true};
};

class FixtureDetector final : public app_selection::SoftwarePresenceDetector {
 public:
  [[nodiscard]] app_selection::PresenceDetection detect(
      std::string_view software_id) override {
    ++calls;
    last_id = software_id;
    return result;
  }

  std::size_t calls{};
  std::string last_id;
  app_selection::PresenceDetection result{
      .completed = true,
      .present = true,
      .detail = "registered product found",
  };
};

class FixtureLauncher final : public app_selection::ExternalAddressLauncher {
 public:
  [[nodiscard]] bool open_declared_address(
      std::string_view software_id, catalog::CatalogSource const& source,
      std::string& error) override {
    ++calls;
    last_id = software_id;
    last_address = source.address;
    error = failure;
    return failure.empty();
  }

  std::size_t calls{};
  std::string last_id;
  std::string last_address;
  std::string failure;
};

[[nodiscard]] catalog::RuntimeSoftware software(
    std::string id, catalog::SoftwareTier tier,
    std::vector<std::string> dependencies = {},
    catalog::ItemAvailability availability = catalog::ItemAvailability::available) {
  return {
      .definition = {.id = std::move(id),
                     .enabled = true,
                     .enabled_declared = true,
                     .name = "fixture",
                     .tier = tier,
                     .category_id = "tools",
                     .branch = "stable",
                     .version_policy = catalog::VersionPolicy::latest_stable,
                     .dependencies = std::move(dependencies),
                     .dependencies_declared = true,
                     .bundled_editions_declared = true,
                     .sources = {{
                         .purpose = catalog::SourcePurpose::primary,
                         .address = "https://example.test/source",
                     }}},
      .availability = availability,
  };
}

[[nodiscard]] catalog::RuntimeSoftwareCatalog fixture_catalog() {
  return {
      .schema_version = 1,
      .revision = 3,
      .release_state = catalog::ReleaseState::release,
      .default_locale = "zh-CN",
      .software = {
          software("core", catalog::SoftwareTier::basic),
          software("helper", catalog::SoftwareTier::normal, {"core"}),
          software("editor", catalog::SoftwareTier::normal),
      },
  };
}

[[nodiscard]] app_selection::CatalogSelectionProjection catalog_projection(
    catalog::RuntimeSoftwareCatalog runtime, std::string content_identity) {
  auto const item_count = runtime.software.size() + runtime.drivers.size();
  auto const revision = runtime.revision;
  return {
      .runtime = std::move(runtime),
      .active = {
          .revision = revision,
          .item_count = item_count,
          .origin = catalog_lifecycle::CatalogCandidateOrigin::built_in,
          .identity = catalog_lifecycle::EffectiveCatalogIdentity::released,
          .content_identity = std::move(content_identity),
          .application_id = "catalog-application-" + std::to_string(revision),
      },
  };
}

struct Fixture final {
  InMemoryStateFileSystem files;
  FixedClock clock{WallClockTime{std::chrono::milliseconds{2'000}}};
  DeviceStateStore states{files, clock};
  RecordingLog log;
  FixedPlatform platform;
  azzs::application::architecture_selection::ArchitectureSelectionLifecycle
      architectures{
          platform, log,
          architecture::ArchitecturePreference::prefer_arm64_prompt_fallback};
  FixtureResolver resolver;
  FixtureNetwork network;
  FixtureDetector detector;
  FixtureLauncher launcher;
  app_selection::SoftwareSelectionLifecycle lifecycle{
      states, clock, log, architectures, resolver, network, detector, launcher,
      StateSubject{"contract-user"}};
};

[[nodiscard]] catalog::CatalogSource const& primary(
    catalog::RuntimeSoftwareCatalog const& source, std::string_view id) {
  auto const item = std::ranges::find(
      source.software, id, [](catalog::RuntimeSoftware const& software) {
        return software.definition.id;
      });
  if (item == source.software.end()) {
    std::abort();
  }
  return item->definition.sources.front();
}

[[nodiscard]] bool contains(std::vector<std::string> const& values,
                            std::string_view value) {
  return std::ranges::find(values, value) != values.end();
}

[[nodiscard]] bool default_closure_and_blocked_deselection() {
  auto runtime = fixture_catalog();
  auto state = selection::default_selection(runtime);
  bool passed = true;
  passed &= expect(state.initialized && contains(state.selected_software_ids, "core"),
                   "basic software must be selected by default");
  auto selected = selection::change_selection(runtime, state, "helper", true);
  passed &= expect(selected.applied && contains(selected.state.selected_software_ids, "helper") &&
                       contains(selected.dependency_closure, "core"),
                   "selecting a normal item must include its recursive dependency closure");
  auto cleared = selection::change_selection(runtime, selected.state, "core", false);
  passed &= expect(!cleared.applied &&
                       cleared.reason == "selected software requires this dependency",
                   "a selected dependency must not be silently removed");
  return passed;
}

[[nodiscard]] bool declared_source_and_snapshot_are_fail_closed() {
  auto runtime = fixture_catalog();
  auto const& source = primary(runtime, "editor");
  auto valid = selection::ResolvedSourceSnapshot{
      .software_id = "editor",
      .declared_purpose = catalog::SourcePurpose::primary,
      .declared_address = source.address,
      .version = "1.2.3",
      .actual_address = "https://downloads.example.test/editor.msi",
      .hosting_mechanism = "controlled-release-asset",
      .branch = "stable",
      .packages = {{
          .candidate = {.software_id = "editor",
                        .architecture = architecture::PackageArchitecture::x64,
                        .version = "1.2.3",
                        .identity = "editor-x64"},
      }},
      .resolved_at_milliseconds = 1,
      .capability_version = "resolver-v1",
  };
  auto invalid = valid;
  invalid.version = "1.2.3-preview";
  bool passed = true;
  passed &= expect(selection::is_declared_source(runtime, "editor", source),
                   "only the catalog declaration can be resolved");
  passed &= expect(valid.valid(), "a complete controlled source snapshot is accepted");
  passed &= expect(!invalid.valid(), "preview releases must not be accepted as stable");
  return passed;
}

[[nodiscard]] bool lifecycle_persists_and_restore_is_local_only() {
  auto runtime = fixture_catalog();
  Fixture fixture;
  bool passed = true;
  passed &= expect(fixture.lifecycle.restore().succeeded(),
                   "selection lifecycle must restore empty state");
  passed &= expect(fixture.lifecycle
                       .on_catalog_replaced(catalog_projection(runtime, "fixture-v3"))
                       .succeeded(),
                   "catalog projection must initialize selection");
  auto resolution =
      fixture.lifecycle.resolve_declared_source("editor", primary(runtime, "editor"));
  passed &= expect(resolution.succeeded() && resolution.resolved_source.has_value(),
                   "explicit resolution must persist its immutable snapshot");
  auto handoff =
      fixture.lifecycle.begin_external_handoff("editor", primary(runtime, "editor"));
  passed &= expect(handoff.succeeded() && handoff.handoff.has_value() &&
                       handoff.handoff->status ==
                           selection::ExternalHandoffStatus::
                               waiting_for_external_install,
                   "opening a declared external handoff must not claim installation success");
  auto const resolver_calls = fixture.resolver.calls;
  auto const detector_calls = fixture.detector.calls;
  auto const launcher_calls = fixture.launcher.calls;

  app_selection::SoftwareSelectionLifecycle restored{
      fixture.states, fixture.clock, fixture.log, fixture.architectures,
      fixture.resolver, fixture.network, fixture.detector, fixture.launcher,
      StateSubject{"contract-user"}};
  auto const restored_result = restored.restore();
  auto const snapshot = restored.snapshot();
  passed &= expect(restored_result.succeeded() && snapshot.sources.size() == 1 &&
                       snapshot.handoffs.size() == 1,
                   "restore must recover selection and external handoff facts");
  passed &= expect(fixture.resolver.calls == resolver_calls &&
                       fixture.detector.calls == detector_calls &&
                       fixture.launcher.calls == launcher_calls,
                   "restore must not resolve, detect, launch, or access network");
  passed &= expect(!snapshot.active_catalog.has_value(),
                   "restored selection must not persist or recreate an active catalog");
  return passed;
}

[[nodiscard]] bool resolver_failure_never_switches_source_and_detection_is_explicit() {
  auto runtime = fixture_catalog();
  Fixture fixture;
  bool passed = true;
  static_cast<void>(fixture.lifecycle.restore());
  static_cast<void>(
      fixture.lifecycle.on_catalog_replaced(catalog_projection(runtime, "fixture-v3")));
  fixture.resolver.fail = true;
  auto const failed =
      fixture.lifecycle.resolve_declared_source("editor", primary(runtime, "editor"));
  passed &= expect(failed.code == app_selection::SelectionActionCode::resolver_failed &&
                       fixture.lifecycle.snapshot().sources.empty(),
                   "resolver failure must not silently switch to another source");

  fixture.resolver.fail = false;
  auto handoff =
      fixture.lifecycle.begin_external_handoff("editor", primary(runtime, "editor"));
  fixture.detector.result = {.completed = true, .present = false, .detail = "absent"};
  auto pending = fixture.lifecycle.detect_external_install("editor");
  passed &= expect(handoff.succeeded() && pending.succeeded() &&
                       pending.handoff.has_value() &&
                       pending.handoff->status ==
                           selection::ExternalHandoffStatus::
                               waiting_for_external_install,
                   "a negative detection must keep external handoff pending");
  fixture.detector.result = {.completed = true, .present = true, .detail = "present"};
  auto recognized = fixture.lifecycle.detect_external_install("editor");
  passed &= expect(recognized.succeeded() && recognized.handoff.has_value() &&
                       recognized.handoff->status ==
                           selection::ExternalHandoffStatus::externally_recognized,
                   "explicit detector result must be recorded as external recognition");
  return passed;
}

[[nodiscard]] bool catalog_projection_identity_is_memory_only_and_stale_is_rejected() {
  Fixture fixture;
  auto current = fixture_catalog();
  bool passed = true;
  static_cast<void>(fixture.lifecycle.restore());
  auto const applied = fixture.lifecycle.on_catalog_replaced(
      catalog_projection(current, "fixture-content-v3"));
  auto const after_applied = fixture.lifecycle.snapshot();
  passed &= expect(
      applied.succeeded() && after_applied.active_catalog.has_value() &&
          after_applied.active_catalog->revision == current.revision &&
          after_applied.active_catalog->content_identity == "fixture-content-v3",
      "selection snapshot must expose the complete active catalog identity");

  auto stale = fixture_catalog();
  stale.revision = current.revision - 1;
  auto const rejected = fixture.lifecycle.on_catalog_replaced(
      catalog_projection(stale, "fixture-content-v2"));
  auto const after_rejected = fixture.lifecycle.snapshot();
  passed &= expect(
      rejected.code == app_selection::SelectionActionCode::stale_catalog_projection &&
          after_rejected.active_catalog == after_applied.active_catalog,
      "a stale catalog projection must be rejected without replacing the active identity");
  return passed;
}

[[nodiscard]] bool catalog_changes_retain_but_block_selection() {
  auto runtime = fixture_catalog();
  auto state = selection::SelectionState{
      .initialized = true,
      .selected_software_ids = {"editor", "removed"},
  };
  auto projected = selection::project_selection(
      runtime, state, {"removed"}, {"editor"}, {});
  auto editor = std::ranges::find(
      projected, "editor", &selection::SelectionItem::software_id);
  auto removed = std::ranges::find(
      projected, "removed", &selection::SelectionItem::software_id);
  return expect(editor != projected.end() && editor->selected &&
                    editor->requires_reselection &&
                    editor->blocker ==
                        selection::SelectionBlocker::catalog_execution_changed &&
                    removed != projected.end() && removed->selected &&
                    removed->blocker ==
                        selection::SelectionBlocker::catalog_item_removed,
                "catalog changes must retain selections as explicit blocked items");
}

}  // namespace

int main() {
  auto const passed = default_closure_and_blocked_deselection() &&
                      declared_source_and_snapshot_are_fail_closed() &&
                      lifecycle_persists_and_restore_is_local_only() &&
                      resolver_failure_never_switches_source_and_detection_is_explicit() &&
                      catalog_projection_identity_is_memory_only_and_stale_is_rejected() &&
                      catalog_changes_retain_but_block_selection();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
