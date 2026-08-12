#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/architecture_selection.hpp"
#include "azzs/application/clock.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/domain/software_selection.hpp"

namespace azzs::application::software_selection {

namespace selection_domain = domain::software_selection;
namespace catalog_domain = domain::software_catalog;

enum class SelectionLifecycleMode {
  not_restored,
  ready,
  read_only,
  failed,
};

enum class SelectionActionCode {
  succeeded,
  no_current_catalog,
  not_restored,
  rejected,
  resolver_failed,
  network_unavailable,
  source_not_declared,
  invalid_resolution,
  persistence_failed,
  read_only,
  detector_failed,
  launcher_failed,
};

struct SelectionActionResult final {
  SelectionActionCode code{SelectionActionCode::rejected};
  bool state_changed{false};
  std::string message;
  std::optional<selection_domain::ResolvedSourceSnapshot> resolved_source;
  std::optional<selection_domain::ExternalHandoffRecord> handoff;
  std::optional<architecture_selection::selection_domain::SelectionResult>
      architecture;

  [[nodiscard]] bool succeeded() const noexcept {
    return code == SelectionActionCode::succeeded ||
           code == SelectionActionCode::no_current_catalog;
  }
};

struct SourceResolutionResult final {
  bool resolved{false};
  std::optional<selection_domain::ResolvedSourceSnapshot> snapshot;
  std::string error;
};

// The resolver receives one exact declaration selected by the user. It cannot
// browse the catalog, substitute an address or accept an arbitrary URL.
class ControlledSourceResolver {
 public:
  virtual ~ControlledSourceResolver() = default;
  [[nodiscard]] virtual SourceResolutionResult resolve(
      std::string_view software_id,
      catalog_domain::CatalogSource const& declared_source) = 0;
};

class NetworkObserver {
 public:
  virtual ~NetworkObserver() = default;
  [[nodiscard]] virtual bool available() const noexcept = 0;
};

struct PresenceDetection final {
  bool completed{false};
  bool present{false};
  std::string detail;
};

// Detection rules are project-owned and keyed only by a stable catalog ID.
class SoftwarePresenceDetector {
 public:
  virtual ~SoftwarePresenceDetector() = default;
  [[nodiscard]] virtual PresenceDetection detect(
      std::string_view software_id) = 0;
};

class ExternalAddressLauncher {
 public:
  virtual ~ExternalAddressLauncher() = default;
  [[nodiscard]] virtual bool open_declared_address(
      std::string_view software_id,
      catalog_domain::CatalogSource const& declared_source,
      std::string& error) = 0;
};

struct SoftwareSelectionSnapshot final {
  SelectionLifecycleMode mode{SelectionLifecycleMode::not_restored};
  bool has_current_catalog{false};
  bool subject_writable{false};
  bool machine_writable{false};
  selection_domain::SelectionState selection;
  std::vector<selection_domain::SelectionItem> items;
  std::vector<selection_domain::ResolvedSourceSnapshot> sources;
  std::vector<selection_domain::ExternalHandoffRecord> handoffs;
  std::string error;
};

// Owns selection, controlled-resolution snapshots and external handoff facts.
// Restoring this lifecycle is deliberately local-only: it decodes persisted
// values and never touches network, browser, detector or installation code.
class SoftwareSelectionLifecycle final {
 public:
  SoftwareSelectionLifecycle(
      DeviceStateStore& states, Clock const& clock, ExecutionLog& log,
      architecture_selection::ArchitectureSelectionLifecycle& architectures,
      ControlledSourceResolver& resolver, NetworkObserver const& network,
      SoftwarePresenceDetector& detector, ExternalAddressLauncher& launcher,
      domain::StateSubject state_subject);

  [[nodiscard]] SelectionActionResult restore();
  [[nodiscard]] SoftwareSelectionSnapshot snapshot() const;

  [[nodiscard]] SelectionActionResult on_catalog_replaced(
      catalog_domain::RuntimeSoftwareCatalog catalog,
      software_catalog::CatalogSelectionImpact impact = {});
  [[nodiscard]] SelectionActionResult select(std::string_view software_id,
                                              bool selected);
  [[nodiscard]] SelectionActionResult resolve_declared_source(
      std::string_view software_id,
      catalog_domain::CatalogSource const& declared_source);
  [[nodiscard]] SelectionActionResult evaluate_architecture(
      std::string_view software_id,
      selection_domain::ResolvedSourceSnapshot const& source);
  [[nodiscard]] SelectionActionResult begin_external_handoff(
      std::string_view software_id,
      catalog_domain::CatalogSource const& declared_source);
  [[nodiscard]] SelectionActionResult detect_external_install(
      std::string_view software_id);
  [[nodiscard]] SelectionActionResult skip_external_handoff(
      std::string_view software_id);

 private:
  [[nodiscard]] SelectionActionResult persist_subject();
  [[nodiscard]] SelectionActionResult persist_machine();
  [[nodiscard]] bool source_matches_catalog(
      selection_domain::ResolvedSourceSnapshot const& source,
      catalog_domain::CatalogSource const& declared_source) const noexcept;
  [[nodiscard]] std::vector<std::string> impact_ids(
      software_catalog::CatalogSelectionImpactReason reason) const;
  void log_event(std::string_view stage, ExecutionResult result,
                 std::string_view software_id = {},
                 std::string_view detail = {});

  DeviceStateStore& states_;
  Clock const& clock_;
  ExecutionLog& log_;
  architecture_selection::ArchitectureSelectionLifecycle& architectures_;
  ControlledSourceResolver& resolver_;
  NetworkObserver const& network_;
  SoftwarePresenceDetector& detector_;
  ExternalAddressLauncher& launcher_;
  domain::StateSubject state_subject_;
  SelectionLifecycleMode mode_{SelectionLifecycleMode::not_restored};
  bool subject_writable_{false};
  bool machine_writable_{false};
  std::optional<domain::RevisionToken> subject_revision_;
  std::optional<domain::RevisionToken> machine_revision_;
  selection_domain::SelectionState selection_;
  std::vector<selection_domain::ResolvedSourceSnapshot> sources_;
  std::vector<selection_domain::ExternalHandoffRecord> handoffs_;
  std::optional<catalog_domain::RuntimeSoftwareCatalog> catalog_;
  software_catalog::CatalogSelectionImpact impact_;
  std::string error_;
};

[[nodiscard]] char const* to_string(SelectionLifecycleMode mode) noexcept;
[[nodiscard]] char const* to_string(SelectionActionCode code) noexcept;

}  // namespace azzs::application::software_selection
