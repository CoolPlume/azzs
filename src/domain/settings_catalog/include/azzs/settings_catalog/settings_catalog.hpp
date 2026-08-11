#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azzs::domain::settings_catalog {

inline constexpr std::uint32_t supported_schema_version = 1;

struct StableId final {
  std::string value;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(StableId const&) const = default;
};

enum class RecoveryRequirement : std::uint8_t {
  unavailable = 0,
  restore_record_required = 1,
};

enum class RestartRequirement : std::uint8_t {
  none = 0,
  explorer = 1,
  windows = 2,
};

struct WindowsVersionRange final {
  std::optional<std::string> minimum;
  std::optional<std::string> maximum;

  friend bool operator==(WindowsVersionRange const&,
                         WindowsVersionRange const&) = default;
};

struct ControlledSettingSemantics final {
  // This key names the enduring Windows target. A catalog transition must use
  // a new StableId when this identity changes.
  std::string identity;
  std::string apply_capability;
  std::string detect_capability;
  std::optional<std::string> recover_capability;

  friend bool operator==(ControlledSettingSemantics const&,
                         ControlledSettingSemantics const&) = default;
};

struct SettingDefinition final {
  StableId id;
  std::string display_name;
  std::string description;
  std::optional<std::string> source_url;
  WindowsVersionRange known_windows_range;
  bool default_selected{false};
  RecoveryRequirement recovery_requirement{
      RecoveryRequirement::unavailable};
  RestartRequirement restart_requirement{RestartRequirement::none};
  ControlledSettingSemantics semantics;

  friend bool operator==(SettingDefinition const&,
                         SettingDefinition const&) = default;
};

struct PlanMember final {
  StableId setting_id;
  std::uint32_t order{0};
  bool default_selected{false};
  std::vector<StableId> depends_on;

  friend bool operator==(PlanMember const&, PlanMember const&) = default;
};

struct OptimizationPlan final {
  StableId id;
  std::string display_name;
  std::string description;
  std::vector<PlanMember> members;

  friend bool operator==(OptimizationPlan const&,
                         OptimizationPlan const&) = default;
};

// A system settings catalog is an independently versioned package. Unknown
// display-only fields are retained only as evidence that they were ignored;
// unknown operation semantics make the whole candidate invalid.
struct SettingsCatalog final {
  std::uint32_t schema_version{supported_schema_version};
  std::uint64_t revision{0};
  std::vector<SettingDefinition> settings;
  std::vector<OptimizationPlan> plans;
  std::vector<std::string> ignored_display_fields;
  std::vector<std::string> unknown_semantic_fields;

  friend bool operator==(SettingsCatalog const&,
                         SettingsCatalog const&) = default;
};

struct SupportedCapabilities final {
  std::vector<std::string> apply;
  std::vector<std::string> detect;
  std::vector<std::string> recover;
};

enum class CatalogProblemCode {
  unsupported_schema,
  invalid_revision,
  invalid_stable_id,
  duplicate_stable_id,
  duplicate_target_identity,
  invalid_required_field,
  invalid_source_url,
  unknown_execution_semantics,
  stable_identity_reused,
  empty_plan,
  missing_plan_member,
  duplicate_plan_member,
  invalid_plan_dependency,
  duplicate_plan_order,
  cyclic_plan_dependencies,
};

struct CatalogProblem final {
  CatalogProblemCode code{CatalogProblemCode::invalid_required_field};
  std::optional<StableId> item_id;
  std::string detail;
};

struct PlanAvailability final {
  StableId plan_id;
  bool enabled{true};
  std::vector<CatalogProblem> problems;
};

struct ValidatedSettingsCatalog final {
  SettingsCatalog catalog;
  std::vector<PlanAvailability> plans;
};

struct CatalogValidationResult final {
  std::optional<ValidatedSettingsCatalog> validated;
  std::vector<CatalogProblem> problems;
};

[[nodiscard]] CatalogValidationResult validate(
    SettingsCatalog catalog, SupportedCapabilities const& capabilities);

// Transition validation is deliberately separate from package validation:
// immutable target identity can only be checked against the current catalog.
[[nodiscard]] std::vector<CatalogProblem> validate_transition(
    ValidatedSettingsCatalog const& current,
    ValidatedSettingsCatalog const& candidate);

[[nodiscard]] SettingDefinition const* find_setting(
    ValidatedSettingsCatalog const& catalog, StableId const& id) noexcept;

[[nodiscard]] PlanAvailability const* find_plan_availability(
    ValidatedSettingsCatalog const& catalog, StableId const& id) noexcept;

enum class CatalogItemKind {
  setting,
  optimization_plan,
};

enum class CatalogChangeKind {
  added,
  changed,
  retired,
};

struct CatalogItemChange final {
  CatalogChangeKind change{CatalogChangeKind::changed};
  CatalogItemKind kind{CatalogItemKind::setting};
  StableId id;
  std::string display_name;
};

struct CatalogChangePreview final {
  std::uint64_t current_revision{0};
  std::uint64_t candidate_revision{0};
  bool downgrade{false};
  std::vector<CatalogItemChange> added;
  std::vector<CatalogItemChange> changed;
  std::vector<CatalogItemChange> retired;
};

[[nodiscard]] CatalogChangePreview preview_changes(
    ValidatedSettingsCatalog const& current,
    ValidatedSettingsCatalog const& candidate);

}  // namespace azzs::domain::settings_catalog
