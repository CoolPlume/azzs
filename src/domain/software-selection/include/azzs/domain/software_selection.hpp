#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/domain/architecture_selection.hpp"
#include "azzs/domain/software_catalog.hpp"

namespace azzs::domain::software_selection {

namespace architecture = domain::architecture_selection;
namespace catalog = domain::software_catalog;

enum class SelectionBlocker {
  none,
  unknown_software,
  unavailable_in_current_catalog,
  required_by_selected_software,
  catalog_item_removed,
  catalog_execution_changed,
};

struct SelectionItem final {
  std::string software_id;
  bool selected{false};
  bool basic{false};
  bool available{false};
  bool requires_reselection{false};
  SelectionBlocker blocker{SelectionBlocker::none};
  std::string reason;

  auto operator<=>(SelectionItem const&) const = default;
};

struct SelectionState final {
  bool initialized{false};
  std::vector<std::string> selected_software_ids;

  auto operator<=>(SelectionState const&) const = default;
};

struct SelectionChange final {
  bool applied{false};
  SelectionState state;
  std::vector<std::string> dependency_closure;
  std::vector<SelectionItem> items;
  std::string reason;
};

enum class PackageType {
  full_package,
  online_installer,
  external_handoff,
  // A fixed ZIP container whose explicitly registered members are the only
  // executable payloads. The archive itself is never passed to a launcher.
  archive_package,
};

struct ResolvedPackage final {
  architecture::PackageCandidate candidate;
  PackageType package_type{PackageType::full_package};
  bool complete_package{true};
  bool network_required{false};
  std::optional<std::uint64_t> expected_bytes;
  std::optional<std::string> expected_sha256;
  // Non-empty only for archive_package. Members are exact ZIP paths, in the
  // order declared by the reviewed source registration.
  std::vector<std::string> archive_members;

  auto operator<=>(ResolvedPackage const&) const = default;
};

// This is a record produced by a project-owned controlled resolver. The
// catalog retains only the maintainer's declared address and purpose.
struct ResolvedSourceSnapshot final {
  std::string software_id;
  catalog::SourcePurpose declared_purpose{catalog::SourcePurpose::primary};
  std::string declared_address;
  std::string version;
  std::string actual_address;
  std::string hosting_mechanism;
  std::string branch;
  std::vector<ResolvedPackage> packages;
  bool network_required{false};
  std::int64_t resolved_at_milliseconds{};
  std::string capability_version;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool stable_version() const noexcept;

  auto operator<=>(ResolvedSourceSnapshot const&) const = default;
};

enum class ExternalHandoffStatus {
  none,
  waiting_for_external_install,
  externally_recognized,
  skipped,
  awaiting_user_confirmation,
  completed,
};

// Facts are append-only.  A current handoff projection may change, but no
// later source resolution or presence check is allowed to rewrite these facts.
enum class ExternalHandoffFactKind {
  source_resolution_failed,
  declared_address_opened,
  returned_for_recheck,
  skipped,
  continued,
  awaiting_user_confirmation,
  user_confirmed,
  completed,
  legacy_record_imported,
};

enum class ExternalHandoffFactAvailability {
  obtained,
  not_obtained,
};

// These values are stable persistence reasons.  Dynamic adapter text belongs
// in ExternalHandoffFact::detail and is classified as sensitive at log export.
enum class ExternalHandoffNotObtainedReason {
  none,
  resolution_failed,
  no_persisted_resolved_source,
  not_captured_for_this_fact,
  legacy_record_has_no_historical_detail,
};

struct ExternalHandoffResolvedSourceFact final {
  ExternalHandoffFactAvailability availability{
      ExternalHandoffFactAvailability::not_obtained};
  ExternalHandoffNotObtainedReason not_obtained_reason{
      ExternalHandoffNotObtainedReason::no_persisted_resolved_source};
  std::string resolved_address;
  std::string resolved_version;
  std::string resolver_capability_version;
  std::int64_t resolved_at_milliseconds{};

  auto operator<=>(ExternalHandoffResolvedSourceFact const&) const = default;
};

struct ExternalHandoffFact final {
  ExternalHandoffFactKind kind{ExternalHandoffFactKind::source_resolution_failed};
  ExternalHandoffStatus status{ExternalHandoffStatus::none};
  ExternalHandoffFactAvailability timestamp_availability{
      ExternalHandoffFactAvailability::obtained};
  ExternalHandoffNotObtainedReason timestamp_not_obtained_reason{
      ExternalHandoffNotObtainedReason::none};
  std::int64_t occurred_at_milliseconds{};
  std::string correlation_id;
  std::string declared_address;
  ExternalHandoffResolvedSourceFact resolved_source;
  // Raw resolver or detector text.  Consumers must hand this to the central
  // diagnostic redactor rather than treating it as presentation-safe text.
  std::string detail;

  [[nodiscard]] bool valid() const noexcept;

  auto operator<=>(ExternalHandoffFact const&) const = default;
};

struct ExternalHandoffTimeline final {
  std::vector<ExternalHandoffFact> facts;

  [[nodiscard]] bool valid() const noexcept;

  auto operator<=>(ExternalHandoffTimeline const&) const = default;
};

struct ExternalHandoffRecord final {
  std::string software_id;
  std::string declared_address;
  // This is a current projection derived from timeline.facts.back(), retained
  // for current-state consumers.  The timeline is the historical source.
  ExternalHandoffStatus status{ExternalHandoffStatus::none};
  std::string detail;
  ExternalHandoffTimeline timeline;

  [[nodiscard]] bool valid() const noexcept;

  auto operator<=>(ExternalHandoffRecord const&) const = default;
};

[[nodiscard]] SelectionState default_selection(
    catalog::RuntimeSoftwareCatalog const& catalog);

[[nodiscard]] SelectionChange change_selection(
    catalog::RuntimeSoftwareCatalog const& catalog, SelectionState state,
    std::string_view software_id, bool selected);

[[nodiscard]] std::vector<SelectionItem> project_selection(
    catalog::RuntimeSoftwareCatalog const& catalog,
    SelectionState const& state,
    std::vector<std::string> const& removed_ids = {},
    std::vector<std::string> const& changed_ids = {},
    std::vector<std::string> const& disabled_ids = {});

[[nodiscard]] bool is_declared_source(
    catalog::RuntimeSoftwareCatalog const& catalog, std::string_view software_id,
    catalog::CatalogSource const& source) noexcept;

[[nodiscard]] char const* to_string(SelectionBlocker blocker) noexcept;
[[nodiscard]] char const* to_string(PackageType type) noexcept;
[[nodiscard]] char const* to_string(ExternalHandoffStatus status) noexcept;
[[nodiscard]] char const* to_string(ExternalHandoffFactKind kind) noexcept;
[[nodiscard]] char const* to_string(ExternalHandoffFactAvailability value) noexcept;
[[nodiscard]] char const* to_string(
    ExternalHandoffNotObtainedReason reason) noexcept;

}  // namespace azzs::domain::software_selection
