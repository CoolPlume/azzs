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
};

struct ResolvedPackage final {
  architecture::PackageCandidate candidate;
  PackageType package_type{PackageType::full_package};
  bool complete_package{true};
  bool network_required{false};

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
};

struct ExternalHandoffRecord final {
  std::string software_id;
  std::string declared_address;
  ExternalHandoffStatus status{ExternalHandoffStatus::none};
  std::string detail;

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

}  // namespace azzs::domain::software_selection
