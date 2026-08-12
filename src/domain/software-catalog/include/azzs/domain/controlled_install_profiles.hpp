#pragma once

#include <array>
#include <compare>
#include <span>
#include <string>
#include <vector>

#include "azzs/domain/software_catalog.hpp"

namespace azzs::domain::software_catalog {

// These declarations describe only facts that a future project-owned Windows
// executor may consume. They intentionally cannot encode commands, scripts,
// paths, download locations, registry writes, or UI selectors.
enum class InstallerArchitecture {
  x86,
  x64,
  arm64,
};

enum class FactKnowledge {
  unknown,
  confirmed,
};

enum class CapabilitySupport {
  unknown,
  supported,
  unsupported,
};

enum class InstallPhase {
  custom_install,
  installation_complete,
};

enum class InstallPreferenceEffect {
  disable_sogou_search_candidates,
  decline_sogou_tencent_yuanbao,
};

enum class PreferenceDefault {
  accept,
  decline,
};

enum class InteractionDisposition {
  controlled_automatic,
  workbench_confirmation,
  official_installer,
};

// Issue 19 records declarations only. Adding an executable state requires the
// later Windows execution owner to extend this closed type and its contracts.
enum class WindowsExecutionReadiness {
  declaration_only,
};

struct ArchitectureFacts final {
  FactKnowledge knowledge{FactKnowledge::unknown};
  std::vector<InstallerArchitecture> values;

  auto operator<=>(ArchitectureFacts const&) const = default;
};

struct InstallCapabilityFacts final {
  ArchitectureFacts architectures;
  CapabilitySupport offline_install{CapabilitySupport::unknown};
  CapabilitySupport silent_install{CapabilitySupport::unknown};
  CapabilitySupport completion_boundary{CapabilitySupport::unknown};
  CapabilitySupport post_install_behavior{CapabilitySupport::unknown};
  CapabilitySupport restart_verification{CapabilitySupport::unknown};
  CapabilitySupport result_detection{CapabilitySupport::unknown};

  auto operator<=>(InstallCapabilityFacts const&) const = default;
};

struct SoftwareInstallFacts final {
  std::string software_id;
  InstallCapabilityFacts capabilities;

  auto operator<=>(SoftwareInstallFacts const&) const = default;
};

struct InstallerBaseline final {
  std::string id;
  std::string version;

  auto operator<=>(InstallerBaseline const&) const = default;
};

struct ControlledInstallPreference final {
  std::string id;
  InstallPhase phase{InstallPhase::custom_install};
  InstallPreferenceEffect effect{
      InstallPreferenceEffect::disable_sogou_search_candidates};
  PreferenceDefault default_choice{PreferenceDefault::decline};
  std::array<InteractionDisposition, 3> disposition_order{
      InteractionDisposition::controlled_automatic,
      InteractionDisposition::workbench_confirmation,
      InteractionDisposition::official_installer,
  };
  bool required_for_first_release{false};

  auto operator<=>(ControlledInstallPreference const&) const = default;
};

struct ControlledInstallProfile final {
  std::string id;
  std::string software_id;
  std::vector<InstallerBaseline> baselines;
  std::vector<ControlledInstallPreference> preferences;
  WindowsExecutionReadiness execution{
      WindowsExecutionReadiness::declaration_only};

  auto operator<=>(ControlledInstallProfile const&) const = default;
};

enum class ControlledInstallProfileIssueCode {
  invalid_stable_id,
  duplicate_profile_id,
  duplicate_software_id,
  invalid_baseline,
  duplicate_baseline_id,
  invalid_preference,
  duplicate_preference_id,
  invalid_architecture_facts,
  invalid_capability_facts,
  invalid_execution_readiness,
};

struct ControlledInstallProfileIssue final {
  ControlledInstallProfileIssueCode code{
      ControlledInstallProfileIssueCode::invalid_stable_id};
  std::string profile_id;
  std::string message;

  auto operator<=>(ControlledInstallProfileIssue const&) const = default;
};

struct ControlledInstallProfileValidation final {
  std::vector<ControlledInstallProfileIssue> issues;

  [[nodiscard]] bool accepted() const noexcept { return issues.empty(); }
};

[[nodiscard]] std::span<ControlledInstallProfile const>
initial_controlled_install_profiles() noexcept;

[[nodiscard]] std::span<SoftwareInstallFacts const>
initial_software_install_facts() noexcept;

[[nodiscard]] ControlledInstallProfileValidation
validate_software_install_facts(std::span<SoftwareInstallFacts const> facts);

[[nodiscard]] ControlledInstallProfileValidation
validate_controlled_install_profiles(
    std::span<ControlledInstallProfile const> profiles);

// Produces the policy consumed by the existing catalog validator. Until a
// Windows execution owner is registered, every profile remains unavailable and
// keeps formal release blocked without changing catalog lifecycle semantics.
[[nodiscard]] SoftwareCatalogPolicy initial_software_catalog_policy();

}  // namespace azzs::domain::software_catalog
