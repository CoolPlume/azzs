#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace azzs::domain::software_optimization_catalog {

struct StableId final {
  std::string value;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(StableId const&) const = default;
};

struct VersionRange final {
  std::string minimum;
  std::string maximum;

  auto operator<=>(VersionRange const&) const = default;
};

enum class PublicationState {
  draft,
  release,
};

enum class SupportMode {
  supported,
  recognition_only,
};

enum class RuleKind {
  none,
  built_in_definition,
};

struct ControlledRule final {
  RuleKind kind{RuleKind::none};
  StableId definition;

  auto operator<=>(ControlledRule const&) const = default;
};

enum class RulePurpose {
  install_detection,
  version_detection,
  option_execution,
  option_state_detection,
};

// The registry contains only project-maintained built-in definition identities.
// Catalog data can reference these identities but cannot provide executable
// code, command text, paths, selectors, or implementation parameters.
struct BuiltInRuleDefinition final {
  StableId id;
  RulePurpose purpose{RulePurpose::option_execution};

  auto operator<=>(BuiltInRuleDefinition const&) const = default;
};

enum class AutomationSupport {
  controlled,
  manual_only,
};

enum class RiskLevel {
  low,
  medium,
  high,
};

enum class ExitRequirement {
  none,
  graceful_exit,
};

enum class RestartRequirement {
  none,
  explorer,
  windows,
};

enum class SchemeAvailability {
  available,
  manual_only,
  configuration_error,
};

enum class CatalogIssueCode {
  syntax_error,
  unknown_schema,
  unknown_execution_semantics,
  missing_required_field,
  invalid_file_field,
  duplicate_stable_id,
  stable_id_reuse,
  invalid_rule,
  invalid_version_range,
  missing_reference,
  relationship_cycle,
  relationship_conflict,
  first_release_incomplete,
  compatibility_baseline_missing,
  compatibility_baseline_empty,
  compatibility_baseline_mismatch,
};

struct CatalogIssue final {
  CatalogIssueCode code{CatalogIssueCode::syntax_error};
  std::string entity_id;
  std::string detail;

  auto operator<=>(CatalogIssue const&) const = default;
};

struct TargetSoftware final {
  StableId id;
  StableId identity_anchor;
  bool required_first_release{false};
  SupportMode support_mode{SupportMode::recognition_only};
  VersionRange supported_versions;
  ControlledRule install_detection;
  ControlledRule version_detection;
  std::optional<StableId> installation_item_id;
  std::string explanation_source;

  auto operator<=>(TargetSoftware const&) const = default;
};

struct SoftwareOptimizationOption final {
  StableId id;
  StableId scheme_id;
  VersionRange supported_versions;
  std::string impact;
  bool default_selected{false};
  bool required{false};
  AutomationSupport automation{AutomationSupport::manual_only};
  ControlledRule execution;
  ControlledRule state_detection;
  std::vector<StableId> required_option_ids;
  std::vector<StableId> conflicting_option_ids;
  std::vector<std::string> allowed_values;
  std::optional<std::string> default_value;
  std::string explanation_source;

  auto operator<=>(SoftwareOptimizationOption const&) const = default;
};

struct SoftwareOptimizationScheme final {
  StableId id;
  StableId target_id;
  bool required_first_release{false};
  AutomationSupport automation{AutomationSupport::manual_only};
  VersionRange supported_versions;
  std::string impact;
  RiskLevel risk{RiskLevel::medium};
  ExitRequirement exit_requirement{ExitRequirement::none};
  RestartRequirement restart_requirement{RestartRequirement::none};
  std::vector<StableId> required_scheme_ids;
  std::vector<StableId> conflicting_scheme_ids;
  std::vector<SoftwareOptimizationOption> options;
  std::string explanation_source;
  std::string manual_emergency_explanation;
  SchemeAvailability availability{SchemeAvailability::available};
  std::vector<CatalogIssue> configuration_issues;

  auto operator<=>(SoftwareOptimizationScheme const&) const = default;
};

struct CompatibilityBaseline final {
  StableId id;
  StableId target_id;
  StableId software_item_id;
  StableId installer_baseline_id;
  VersionRange installed_versions;

  auto operator<=>(CompatibilityBaseline const&) const = default;
};

struct SoftwareOptimizationCatalog final {
  std::uint64_t revision{0};
  PublicationState publication_state{PublicationState::draft};
  std::vector<TargetSoftware> targets;
  std::vector<SoftwareOptimizationScheme> schemes;
  std::vector<CompatibilityBaseline> compatibility_baselines;
  std::vector<CatalogIssue> release_issues;

  [[nodiscard]] TargetSoftware const* find_target(
      std::string_view id) const noexcept;
  [[nodiscard]] SoftwareOptimizationScheme const* find_scheme(
      std::string_view id) const noexcept;

  auto operator<=>(SoftwareOptimizationCatalog const&) const = default;
};

struct CatalogLoadResult final {
  std::optional<SoftwareOptimizationCatalog> catalog;
  std::vector<CatalogIssue> package_issues;

  [[nodiscard]] bool accepted() const noexcept {
    return catalog.has_value() && package_issues.empty();
  }
};

enum class StableEntityKind {
  target,
  scheme,
  option,
  compatibility_baseline,
};

struct StableIdentityRecord final {
  StableId id;
  StableEntityKind kind{StableEntityKind::target};
  std::string semantic_fingerprint;

  auto operator<=>(StableIdentityRecord const&) const = default;
};

struct CatalogSummary final {
  std::uint64_t revision{0};
  std::size_t target_count{0};
  std::size_t scheme_count{0};
  std::size_t option_count{0};
  std::size_t disabled_scheme_count{0};
  // Cross-catalog installer compatibility is assessed separately.
  bool intrinsic_release_eligible{false};
};

struct FrozenScheme final {
  std::uint64_t catalog_revision{0};
  SoftwareOptimizationScheme scheme;

  auto operator<=>(FrozenScheme const&) const = default;
};

struct SoftwareCatalogInstallerBaseline final {
  StableId software_item_id;
  StableId installer_baseline_id;
  VersionRange installed_versions;

  auto operator<=>(SoftwareCatalogInstallerBaseline const&) const = default;
};

struct CompatibilityAssessment final {
  bool compatible{false};
  std::vector<CatalogIssue> issues;
};

[[nodiscard]] CatalogLoadResult load_catalog(
    std::string_view source,
    std::span<BuiltInRuleDefinition const> built_in_rules);

[[nodiscard]] CatalogSummary summarize(
    SoftwareOptimizationCatalog const& catalog) noexcept;

[[nodiscard]] std::optional<FrozenScheme> freeze_scheme(
    SoftwareOptimizationCatalog const& catalog, std::string_view scheme_id);

[[nodiscard]] std::vector<StableIdentityRecord> stable_identities(
    SoftwareOptimizationCatalog const& catalog);

[[nodiscard]] std::vector<CatalogIssue> validate_stable_identity_history(
    SoftwareOptimizationCatalog const& catalog,
    std::span<StableIdentityRecord const> history);

[[nodiscard]] std::vector<StableIdentityRecord> merge_stable_identity_history(
    std::span<StableIdentityRecord const> history,
    SoftwareOptimizationCatalog const& catalog);

[[nodiscard]] std::vector<StableId> schemes_lost_or_changed(
    SoftwareOptimizationCatalog const& current,
    SoftwareOptimizationCatalog const& candidate);

[[nodiscard]] CompatibilityAssessment assess_release_compatibility(
    SoftwareOptimizationCatalog const& catalog,
    std::span<SoftwareCatalogInstallerBaseline const> software_baselines);

}  // namespace azzs::domain::software_optimization_catalog
