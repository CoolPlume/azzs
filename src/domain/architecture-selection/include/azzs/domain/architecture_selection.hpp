#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "azzs/domain/system_architecture.hpp"

namespace azzs::domain::architecture_selection {

enum class PackageArchitecture {
  x64,
  arm64,
  architecture_independent,
  unknown,
  x86,
};

enum class ArchitecturePreference {
  prefer_arm64_prompt_fallback,
  prefer_arm64_auto_fallback,
  prefer_x64,
};

struct PackageCandidate final {
  std::string software_id;
  PackageArchitecture architecture{PackageArchitecture::unknown};
  std::string version;
  std::string identity;

  auto operator<=>(PackageCandidate const&) const = default;
};

struct ArchitectureObservation final {
  SystemArchitecture system{SystemArchitecture::unknown};
  std::uint64_t id{0};

  [[nodiscard]] bool detected() const noexcept {
    return system != SystemArchitecture::unknown;
  }

  auto operator<=>(ArchitectureObservation const&) const = default;
};

enum class SelectionStatus {
  selected_native,
  selected_architecture_independent,
  selected_compatibility_fallback,
  fallback_confirmation_required,
  fallback_refused,
  incompatible,
  detection_failed_paused,
  package_information_unavailable,
  architecture_changed,
};

struct SelectionResult final {
  std::string software_id;
  SelectionStatus status{SelectionStatus::incompatible};
  ArchitectureObservation observation;
  std::optional<PackageCandidate> package;
  std::string reason;

  [[nodiscard]] bool selected() const noexcept {
    return status == SelectionStatus::selected_native ||
           status == SelectionStatus::selected_architecture_independent ||
           status == SelectionStatus::selected_compatibility_fallback;
  }

  [[nodiscard]] bool requires_confirmation() const noexcept {
    return status == SelectionStatus::fallback_confirmation_required;
  }

  auto operator<=>(SelectionResult const&) const = default;
};

[[nodiscard]] SelectionResult select_package(
    ArchitectureObservation observation, ArchitecturePreference preference,
    std::string software_id, std::span<PackageCandidate const> candidates,
    std::optional<PackageArchitecture> one_shot_preference = std::nullopt);

[[nodiscard]] bool architecture_changed(
    ArchitectureObservation previous,
    ArchitectureObservation current) noexcept;

[[nodiscard]] char const* to_string(SystemArchitecture architecture) noexcept;
[[nodiscard]] char const* to_string(PackageArchitecture architecture) noexcept;
[[nodiscard]] char const* to_string(ArchitecturePreference preference) noexcept;
[[nodiscard]] char const* to_string(SelectionStatus status) noexcept;

}  // namespace azzs::domain::architecture_selection
