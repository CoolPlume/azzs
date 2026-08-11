#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azzs::domain::software_catalog {

struct DisplayExtension final {
  std::string key;
  std::string text;
  std::vector<std::string> text_list;
  bool list{false};

  auto operator<=>(DisplayExtension const&) const = default;
};

enum class ReleaseState {
  draft,
  release,
};

enum class SoftwareTier {
  basic,
  normal,
};

enum class VersionPolicy {
  latest_stable,
  latest_stable_with_history,
  fixed,
  maintainer_provided,
};

enum class SourcePurpose {
  primary,
  alternative,
  project_backup,
};

enum class DriverEntryType {
  assistant,
  vendor_page,
};

struct CatalogLocalization final {
  std::string locale;
  std::optional<std::string> name;
  std::optional<std::string> notice;
  std::optional<std::string> optimization_note;
  std::optional<std::string> education_description;
  std::vector<DisplayExtension> display_extensions;

  auto operator<=>(CatalogLocalization const&) const = default;
};

struct CatalogCategory final {
  std::string id;
  std::string name;
  std::vector<CatalogLocalization> localizations;
  std::vector<DisplayExtension> display_extensions;

  auto operator<=>(CatalogCategory const&) const = default;
};

struct SourceHistory final {
  std::string version;
  std::string address;
  std::string reason;
  bool visible{true};
  std::vector<DisplayExtension> display_extensions;

  auto operator<=>(SourceHistory const&) const = default;
};

struct CatalogSource final {
  std::optional<SourcePurpose> purpose;
  std::string address;
  std::optional<std::string> version;
  std::vector<SourceHistory> history;
  std::vector<DisplayExtension> display_extensions;

  auto operator<=>(CatalogSource const&) const = default;
};

struct EducationResource final {
  std::string address;
  std::string description;
  std::vector<DisplayExtension> display_extensions;

  auto operator<=>(EducationResource const&) const = default;
};

struct SoftwareDefinition final {
  std::string id;
  bool enabled{false};
  bool enabled_declared{false};
  std::string name;
  std::optional<SoftwareTier> tier;
  std::string category_id;
  std::string branch;
  std::optional<VersionPolicy> version_policy;
  std::optional<std::string> fixed_version;
  std::vector<std::string> dependencies;
  bool dependencies_declared{false};
  std::vector<std::string> bundled_editions;
  bool bundled_editions_declared{false};
  std::string notice;
  std::optional<std::string> optimization_note;
  std::optional<std::string> install_profile;
  std::vector<CatalogSource> sources;
  std::optional<EducationResource> education;
  std::vector<CatalogLocalization> localizations;
  std::vector<DisplayExtension> display_extensions;

  auto operator<=>(SoftwareDefinition const&) const = default;
};

// Driver content is owned by issue 19. Issue 03 owns only the versioned shape
// and lifecycle, so this type intentionally contains no vendor-specific facts.
struct DriverDefinition final {
  std::string id;
  bool enabled{false};
  bool enabled_declared{false};
  std::string name;
  std::optional<DriverEntryType> entry_type;
  std::vector<std::string> hardware_kinds;
  bool hardware_kinds_declared{false};
  std::string branch;
  std::optional<VersionPolicy> version_policy;
  std::optional<std::string> fixed_version;
  std::string notice;
  std::vector<CatalogSource> sources;
  std::vector<CatalogLocalization> localizations;
  std::vector<DisplayExtension> display_extensions;

  auto operator<=>(DriverDefinition const&) const = default;
};

struct SoftwareCatalogDocument final {
  std::uint32_t schema_version{0};
  std::string catalog_id;
  std::uint64_t revision{0};
  std::optional<ReleaseState> release_state;
  std::string default_locale;
  std::vector<CatalogCategory> categories;
  std::vector<SoftwareDefinition> software;
  std::vector<DriverDefinition> drivers;
  std::vector<DisplayExtension> display_extensions;

  auto operator<=>(SoftwareCatalogDocument const&) const = default;
};

enum class CatalogIssueScope {
  package,
  item,
  release,
};

enum class CatalogIssueCode {
  malformed_toml,
  unsupported_schema,
  missing_required_field,
  invalid_field,
  duplicate_field,
  duplicate_stable_id,
  stable_id_reused,
  unknown_execution_semantics,
  invalid_reference,
  missing_dependency,
  dependency_cycle,
  unavailable_dependency,
  draft_release_state,
  required_item_missing,
  required_item_disabled,
  release_revision_regression,
  release_revision_conflict,
  release_dependency_error,
  install_profile_unavailable,
  install_profile_not_release_ready,
  prohibited_content,
};

struct CatalogIssue final {
  CatalogIssueScope scope{CatalogIssueScope::package};
  CatalogIssueCode code{CatalogIssueCode::invalid_field};
  std::string location;
  std::string item_id;
  std::string message;

  auto operator<=>(CatalogIssue const&) const = default;
};

// The catalog contains only a stable reference. The executable capability and
// its completion/detection facts remain project-owned and are supplied here by
// the later execution owner rather than duplicated in TOML.
enum class InstallProfileRuntimeStatus {
  available,
  missing,
  inapplicable,
  unknown_semantics,
};

struct InstallProfileSupport final {
  std::string id;
  std::vector<std::string> software_ids;
  InstallProfileRuntimeStatus runtime_status{
      InstallProfileRuntimeStatus::missing};
  bool release_ready{false};

  auto operator<=>(InstallProfileSupport const&) const = default;
};

struct PublishedCatalogReference final {
  std::uint64_t revision{0};
  std::string content_identity;

  auto operator<=>(PublishedCatalogReference const&) const = default;
};

struct RequiredInstallProfile final {
  std::string software_id;
  std::string profile_id;

  auto operator<=>(RequiredInstallProfile const&) const = default;
};

struct SoftwareCatalogPolicy final {
  std::uint32_t supported_schema_version{1};
  std::vector<InstallProfileSupport> install_profiles;
  std::vector<RequiredInstallProfile> required_install_profiles;
  std::vector<std::string> supported_driver_hardware_kinds;
  std::vector<std::string> required_release_software;
  std::optional<PublishedCatalogReference> last_published;
};

enum class ItemAvailability {
  available,
  install_profile_unavailable,
  missing_dependency,
  dependency_cycle,
  unavailable_dependency,
};

struct RuntimeSoftware final {
  SoftwareDefinition definition;
  ItemAvailability availability{ItemAvailability::available};
  std::vector<std::string> reasons;

  auto operator<=>(RuntimeSoftware const&) const = default;
};

struct RuntimeDriver final {
  DriverDefinition definition;
  ItemAvailability availability{ItemAvailability::available};
  std::vector<std::string> reasons;

  auto operator<=>(RuntimeDriver const&) const = default;
};

struct RuntimeSoftwareCatalog final {
  std::uint32_t schema_version{0};
  std::uint64_t revision{0};
  ReleaseState release_state{ReleaseState::draft};
  std::string default_locale;
  std::vector<CatalogCategory> categories;
  std::vector<RuntimeSoftware> software;
  std::vector<RuntimeDriver> drivers;

  auto operator<=>(RuntimeSoftwareCatalog const&) const = default;
};

enum class RuntimeLoadOutcome {
  accepted,
  rejected,
};

struct RuntimeCatalogLoad final {
  RuntimeLoadOutcome outcome{RuntimeLoadOutcome::rejected};
  std::optional<RuntimeSoftwareCatalog> catalog;
  std::vector<CatalogIssue> issues;

  [[nodiscard]] bool accepted() const noexcept {
    return outcome == RuntimeLoadOutcome::accepted && catalog.has_value();
  }
};

enum class ReleaseGateOutcome {
  passed,
  failed,
  not_evaluated,
};

struct SoftwareCatalogReleaseGate final {
  ReleaseGateOutcome outcome{ReleaseGateOutcome::not_evaluated};
  std::vector<CatalogIssue> issues;

  [[nodiscard]] bool passed() const noexcept {
    return outcome == ReleaseGateOutcome::passed;
  }
};

[[nodiscard]] bool valid_stable_id(std::string_view value) noexcept;
[[nodiscard]] bool valid_locale(std::string_view value) noexcept;
[[nodiscard]] bool valid_http_address(std::string_view value) noexcept;

[[nodiscard]] RuntimeCatalogLoad validate_for_runtime(
    SoftwareCatalogDocument const& document,
    SoftwareCatalogPolicy const& policy);

[[nodiscard]] SoftwareCatalogReleaseGate evaluate_release_gate(
    SoftwareCatalogDocument const& document,
    RuntimeCatalogLoad const& runtime,
    SoftwareCatalogPolicy const& policy,
    std::string_view content_identity);

[[nodiscard]] std::string content_identity(std::string_view bytes);

}  // namespace azzs::domain::software_catalog
