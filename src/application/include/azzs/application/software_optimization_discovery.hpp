#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/emergency_withdrawal_service.hpp"
#include "azzs/application/software_optimization_catalog_lifecycle.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/domain/software_optimization_discovery.hpp"

namespace azzs::application::sogou_optimization {
class SogouOptimizationService;
}

namespace azzs::application::software_optimization_discovery {

namespace discovery_domain = domain::software_optimization_discovery;
namespace catalog_domain = domain::software_optimization_catalog;

// This port is observation-only.  It receives the current validated catalog
// declaration and cannot start a process, modify a target application, or
// create an optimization batch.
class SoftwareOptimizationObservationPort {
 public:
  virtual ~SoftwareOptimizationObservationPort() = default;

  [[nodiscard]] virtual discovery_domain::TargetObservation observe_target(
      catalog_domain::TargetSoftware const& target) = 0;
  [[nodiscard]] virtual discovery_domain::OptionObservation observe_option(
      catalog_domain::TargetSoftware const& target,
      catalog_domain::SoftwareOptimizationOption const& option) = 0;
};

// The first catalog only has the project-owned Sogou capability.  The adapter
// maps validated rule identities to that closed capability set; other target
// or option identities fail closed as unknown rather than becoming a generic
// discovery or command surface.
class SogouOptimizationDiscoveryObserver final
    : public SoftwareOptimizationObservationPort {
 public:
  explicit SogouOptimizationDiscoveryObserver(
      sogou_optimization::SogouOptimizationService& service) noexcept;

  [[nodiscard]] discovery_domain::TargetObservation observe_target(
      catalog_domain::TargetSoftware const& target) override;
  [[nodiscard]] discovery_domain::OptionObservation observe_option(
      catalog_domain::TargetSoftware const& target,
      catalog_domain::SoftwareOptimizationOption const& option) override;

 private:
  sogou_optimization::SogouOptimizationService& service_;
};

enum class DiscoveryActionCode {
  refreshed,
  no_current_catalog,
  selection_changed,
  adjustment_confirmation_required,
  selection_rejected,
  no_executable_selection,
};

struct SoftwareOptimizationSubmissionRequest final {
  std::uint64_t catalog_revision{0};
  std::vector<discovery_domain::SelectedOption> selected_options;

  [[nodiscard]] bool empty() const noexcept {
    return selected_options.empty();
  }
};

struct SoftwareOptimizationDiscoverySnapshot final {
  bool has_current_catalog{false};
  bool defaults_initialized{false};
  discovery_domain::DiscoverySnapshot discovery;
  discovery_domain::SelectionState selection;
  std::string error;
};

struct DiscoveryActionResult final {
  DiscoveryActionCode code{DiscoveryActionCode::no_current_catalog};
  SoftwareOptimizationDiscoverySnapshot snapshot;
  discovery_domain::SelectionAdjustment adjustment;
  std::optional<SoftwareOptimizationSubmissionRequest> submission;
  std::string message;
};

// Owns only ephemeral selection and discovery state.  The catalog lifecycle,
// external handoff records and emergency-withdrawal safety fact remain their
// existing single writers.  Issue 27 receives a typed request only after an
// explicit later confirmation; this service never creates a batch.
class SoftwareOptimizationDiscoveryService final {
 public:
  SoftwareOptimizationDiscoveryService(
      SoftwareOptimizationCatalogLifecycle& catalogs,
      software_selection::SoftwareSelectionLifecycle const& selections,
      EmergencyWithdrawalService& withdrawals,
      SoftwareOptimizationObservationPort& observations) noexcept;

  [[nodiscard]] DiscoveryActionResult refresh();
  [[nodiscard]] SoftwareOptimizationDiscoverySnapshot snapshot() const;
  [[nodiscard]] DiscoveryActionResult change_selection(
      discovery_domain::SelectionMutation mutation);
  [[nodiscard]] DiscoveryActionResult prepare_submission() const;

 private:
  [[nodiscard]] SoftwareOptimizationDiscoverySnapshot rebuild();
  [[nodiscard]] std::vector<discovery_domain::SelectedOption>
  executable_selected_options() const;

  SoftwareOptimizationCatalogLifecycle& catalogs_;
  software_selection::SoftwareSelectionLifecycle const& selections_;
  EmergencyWithdrawalService& withdrawals_;
  SoftwareOptimizationObservationPort& observations_;
  discovery_domain::SelectionState selection_;
  SoftwareOptimizationDiscoverySnapshot snapshot_;
  bool defaults_initialized_{false};
};

[[nodiscard]] char const* to_string(DiscoveryActionCode value) noexcept;

}  // namespace azzs::application::software_optimization_discovery
