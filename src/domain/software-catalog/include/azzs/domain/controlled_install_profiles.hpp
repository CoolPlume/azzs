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

// Execution details stay in the project-owned Windows adapter. The profile
// carries only the closed capability identity that an application snapshot may
// freeze; it never carries commands, paths, selectors, or vendor data.
enum class ControlledWindowsExecutionKind {
  project_owned_windows_executor,
};

enum class WindowsExecutionReadiness {
  declaration_only,
  project_executor_registered,
};

// A launched installer process is never a completion boundary by itself.
enum class InstallationCompletionBoundary {
  post_install_then_result_detection,
  post_install_then_restart_verification,
};

enum class PostInstallBehavior {
  none,
  controlled_preferences,
};

enum class RestartVerification {
  not_required,
  required_after_restart,
};

enum class ResultDetectionStrategy {
  project_owned_presence_probe,
  user_confirmation_only,
};

// This scope deliberately excludes identity, credential, payment, and other
// personal-data interactions. Those always remain in the official installer.
enum class InstallerInteractionScope {
  non_identity_preferences_only,
  official_identity_required,
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
  ControlledWindowsExecutionKind execution_kind{
      ControlledWindowsExecutionKind::project_owned_windows_executor};
  WindowsExecutionReadiness execution{
      WindowsExecutionReadiness::declaration_only};
  InstallationCompletionBoundary completion_boundary{
      InstallationCompletionBoundary::post_install_then_result_detection};
  PostInstallBehavior post_install_behavior{PostInstallBehavior::none};
  RestartVerification restart_verification{RestartVerification::not_required};
  ResultDetectionStrategy result_detection{
      ResultDetectionStrategy::project_owned_presence_probe};
  InstallerInteractionScope interaction_scope{
      InstallerInteractionScope::non_identity_preferences_only};

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
  invalid_execution_kind,
  invalid_architecture_facts,
  invalid_capability_facts,
  invalid_execution_readiness,
  invalid_completion_boundary,
  invalid_post_install_behavior,
  invalid_restart_verification,
  invalid_result_detection_strategy,
  invalid_interaction_scope,
  inconsistent_completion_semantics,
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
