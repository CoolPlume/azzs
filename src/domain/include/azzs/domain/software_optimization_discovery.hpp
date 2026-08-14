#pragma once

#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/domain/software_optimization_catalog.hpp"

namespace azzs::domain::software_optimization_discovery {

namespace catalog = domain::software_optimization_catalog;

// Presence is an observed fact.  It deliberately does not encode where the
// software was installed: an external handoff only changes the displayed
// provenance after the target has been reliably recognized.
enum class TargetPresence {
  detected,
  externally_recognized,
  absent,
  unknown,
  skipped,
};

enum class OptionState {
  needs_optimization,
  optimized,
  version_not_applicable,
  unknown,
};

enum class SchemeState {
  can_optimize,
  needs_attention,
  optimized,
  version_not_applicable,
  emergency_withdrawn,
  configuration_error,
  manual_only,
};

struct TargetObservation final {
  catalog::StableId target_id;
  TargetPresence presence{TargetPresence::unknown};
  std::optional<std::string> installed_version;
  std::string detail;

  auto operator<=>(TargetObservation const&) const = default;
};

struct OptionObservation final {
  catalog::StableId option_id;
  OptionState state{OptionState::unknown};
  std::string detail;

  auto operator<=>(OptionObservation const&) const = default;
};

// Emergency withdrawal remains owned by the security service.  Discovery
// consumes only its already-authorized, presentation-safe projection.
struct WithdrawnOperation final {
  catalog::StableId operation_id;
  std::string reason;

  auto operator<=>(WithdrawnOperation const&) const = default;
};

struct SelectedOption final {
  catalog::StableId scheme_id;
  catalog::StableId option_id;
  std::optional<std::string> value;

  auto operator<=>(SelectedOption const&) const = default;
};

struct SelectionState final {
  std::vector<SelectedOption> options;

  auto operator<=>(SelectionState const&) const = default;
};

struct SelectionMutation final {
  catalog::StableId scheme_id;
  catalog::StableId option_id;
  bool selected{false};
  bool accept_adjustments{false};

  auto operator<=>(SelectionMutation const&) const = default;
};

enum class SelectionChangeCode {
  applied,
  confirmation_required,
  rejected,
};

struct SelectionAdjustment final {
  std::vector<SelectedOption> added_required;
  std::vector<SelectedOption> removed_conflicting;

  [[nodiscard]] bool empty() const noexcept {
    return added_required.empty() && removed_conflicting.empty();
  }
};

struct SelectionChange final {
  SelectionChangeCode code{SelectionChangeCode::rejected};
  bool applied{false};
  SelectionState state;
  SelectionAdjustment adjustment;
  std::string reason;
};

struct OptionDiscovery final {
  catalog::SoftwareOptimizationOption option;
  OptionState state{OptionState::unknown};
  bool selected{false};
  std::string detail;
};

struct SchemeDiscovery final {
  catalog::SoftwareOptimizationScheme scheme;
  TargetPresence target_presence{TargetPresence::unknown};
  std::optional<std::string> installed_version;
  SchemeState state{SchemeState::needs_attention};
  std::vector<OptionDiscovery> options;
  std::string detail;
};

struct TargetDiscovery final {
  catalog::TargetSoftware target;
  TargetPresence presence{TargetPresence::unknown};
  std::optional<std::string> installed_version;
  std::vector<SchemeDiscovery> schemes;
  bool no_available_optimization{false};
  bool first_release_implementation_error{false};
  std::string detail;
};

struct DiscoveryInput final {
  catalog::SoftwareOptimizationCatalog const& catalog;
  std::span<TargetObservation const> targets;
  std::span<OptionObservation const> options;
  std::span<WithdrawnOperation const> withdrawn_operations;
  SelectionState selection;
};

struct DiscoverySnapshot final {
  std::uint64_t catalog_revision{0};
  std::vector<TargetDiscovery> targets;
};

// Builds an installed-software-only projection.  Unknown, absent and skipped
// targets are intentionally omitted so a directory never becomes a list of
// software the user does not currently have.
[[nodiscard]] DiscoverySnapshot discover(DiscoveryInput const& input);

// The recommendation contains only catalog-defined low-impact defaults and
// required options.  It never creates a batch or causes an effect.
[[nodiscard]] SelectionState default_selection(
    catalog::SoftwareOptimizationCatalog const& catalog);

// Filters the current intent through the freshly discovered executable
// projection. This is the only selection set that may cross into issue 27.
[[nodiscard]] std::vector<SelectedOption> executable_selected_options(
    DiscoverySnapshot const& snapshot, SelectionState const& selection);

// Applies one checkbox intent.  Required and conflicting relationships are
// proposed first and become state only after the caller confirms them.
[[nodiscard]] SelectionChange change_selection(
    catalog::SoftwareOptimizationCatalog const& catalog,
    SelectionState current, SelectionMutation const& mutation);

[[nodiscard]] char const* to_string(TargetPresence value) noexcept;
[[nodiscard]] char const* to_string(OptionState value) noexcept;
[[nodiscard]] char const* to_string(SchemeState value) noexcept;

}  // namespace azzs::domain::software_optimization_discovery
