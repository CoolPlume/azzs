#include "azzs/domain/architecture_selection.hpp"

#include <algorithm>
#include <utility>

namespace azzs::domain::architecture_selection {
namespace {

[[nodiscard]] std::vector<PackageCandidate> matching_candidates(
    std::string const& software_id,
    std::span<PackageCandidate const> candidates) {
  std::vector<PackageCandidate> matches;
  for (auto const& candidate : candidates) {
    if (candidate.software_id == software_id) {
      matches.push_back(candidate);
    }
  }
  return matches;
}

[[nodiscard]] std::optional<PackageCandidate> first_architecture(
    std::vector<PackageCandidate> const& candidates,
    PackageArchitecture architecture) {
  auto found = std::ranges::find(candidates, architecture,
                                 &PackageCandidate::architecture);
  if (found == candidates.end()) {
    return std::nullopt;
  }
  return *found;
}

[[nodiscard]] SelectionResult selected(
    std::string software_id, ArchitectureObservation observation,
    PackageCandidate package, SelectionStatus status, std::string reason) {
  return SelectionResult{
      .software_id = std::move(software_id),
      .status = status,
      .observation = observation,
      .package = std::move(package),
      .reason = std::move(reason),
  };
}

}  // namespace

SelectionResult select_package(
    ArchitectureObservation observation, ArchitecturePreference preference,
    std::string software_id, std::span<PackageCandidate const> candidates,
    std::optional<PackageArchitecture> one_shot_preference) {
  auto matches = matching_candidates(software_id, candidates);
  if (matches.empty()) {
    return SelectionResult{
        .software_id = std::move(software_id),
        .status = SelectionStatus::incompatible,
        .observation = observation,
        .reason = "no package candidate belongs to this software",
    };
  }

  auto const independent = first_architecture(
      matches, PackageArchitecture::architecture_independent);
  auto const x64 = first_architecture(matches, PackageArchitecture::x64);
  auto const x86 = first_architecture(matches, PackageArchitecture::x86);
  auto const arm64 = first_architecture(matches, PackageArchitecture::arm64);
  auto const unknown = first_architecture(matches, PackageArchitecture::unknown);

  if (observation.system == SystemArchitecture::unknown) {
    if (independent.has_value()) {
      return selected(std::move(software_id), observation, *independent,
                      SelectionStatus::selected_architecture_independent,
                      "architecture-independent package does not require a system architecture");
    }
    if (x64.has_value() || arm64.has_value()) {
      return SelectionResult{
          .software_id = std::move(software_id),
          .status = SelectionStatus::detection_failed_paused,
          .observation = observation,
          .reason = "system architecture could not be detected for an architecture-specific package",
      };
    }
    return SelectionResult{
        .software_id = std::move(software_id),
        .status = SelectionStatus::package_information_unavailable,
        .observation = observation,
        .reason = "package architecture could not be determined",
    };
  }

  if (observation.system == SystemArchitecture::x64) {
    if (x64.has_value()) {
      return selected(std::move(software_id), observation, *x64,
                      SelectionStatus::selected_native,
                      "x64 package matches the current system architecture");
    }
    if (independent.has_value()) {
      return selected(std::move(software_id), observation, *independent,
                      SelectionStatus::selected_architecture_independent,
                      "architecture-independent package is compatible with x64");
    }
    if (x86.has_value()) {
      return selected(std::move(software_id), observation, *x86,
                      SelectionStatus::selected_compatibility_fallback,
                      "x86 package uses the Windows x64 compatibility layer");
    }
    if (unknown.has_value()) {
      return SelectionResult{
          .software_id = std::move(software_id),
          .status = SelectionStatus::package_information_unavailable,
          .observation = observation,
          .reason = "package architecture could not be determined",
      };
    }
    return SelectionResult{
        .software_id = std::move(software_id),
        .status = SelectionStatus::incompatible,
        .observation = observation,
        .reason = "x64 Windows cannot install an ARM64-only package",
    };
  }

  // ARM64: an explicit one-shot choice is scoped to this evaluation only.
  if (one_shot_preference == PackageArchitecture::x64 && x64.has_value()) {
    return selected(std::move(software_id), observation, *x64,
                    SelectionStatus::selected_compatibility_fallback,
                    "one-shot retry preference selected x64 on ARM64");
  }
  if (one_shot_preference == PackageArchitecture::arm64 && arm64.has_value()) {
    return selected(std::move(software_id), observation, *arm64,
                    SelectionStatus::selected_native,
                    "one-shot retry preference selected ARM64");
  }
  if (preference == ArchitecturePreference::prefer_x64 && x64.has_value()) {
    return selected(std::move(software_id), observation, *x64,
                    SelectionStatus::selected_compatibility_fallback,
                    "the current preference prioritizes x64");
  }
  if (arm64.has_value()) {
    return selected(std::move(software_id), observation, *arm64,
                    SelectionStatus::selected_native,
                    "ARM64 package matches the current system architecture");
  }
  if (independent.has_value()) {
    return selected(std::move(software_id), observation, *independent,
                    SelectionStatus::selected_architecture_independent,
                    "architecture-independent package is compatible with ARM64");
  }
  if (x64.has_value()) {
    if (preference == ArchitecturePreference::prefer_arm64_prompt_fallback) {
      return SelectionResult{
          .software_id = std::move(software_id),
          .status = SelectionStatus::fallback_confirmation_required,
          .observation = observation,
          .package = *x64,
          .reason = "ARM64 package is unavailable; confirm the x64 compatibility fallback",
      };
    }
    return selected(std::move(software_id), observation, *x64,
                    SelectionStatus::selected_compatibility_fallback,
                    "ARM64 package is unavailable and the preference permits x64 fallback");
  }

  if (unknown.has_value()) {
    return SelectionResult{
        .software_id = std::move(software_id),
        .status = SelectionStatus::package_information_unavailable,
        .observation = observation,
        .reason = "package architecture could not be determined",
    };
  }

  return SelectionResult{
      .software_id = std::move(software_id),
      .status = SelectionStatus::incompatible,
      .observation = observation,
      .reason = "no package is compatible with the current architecture",
  };
}

bool architecture_changed(ArchitectureObservation previous,
                           ArchitectureObservation current) noexcept {
  return previous.detected() && current.detected() &&
         previous.system != current.system;
}

char const* to_string(SystemArchitecture architecture) noexcept {
  switch (architecture) {
    case SystemArchitecture::x64:
      return "x64";
    case SystemArchitecture::arm64:
      return "arm64";
    case SystemArchitecture::unknown:
      return "unknown";
  }
  return "unknown";
}

char const* to_string(PackageArchitecture architecture) noexcept {
  switch (architecture) {
    case PackageArchitecture::x86:
      return "x86";
    case PackageArchitecture::x64:
      return "x64";
    case PackageArchitecture::arm64:
      return "arm64";
    case PackageArchitecture::architecture_independent:
      return "architecture-independent";
    case PackageArchitecture::unknown:
      return "unknown";
  }
  return "unknown";
}

char const* to_string(ArchitecturePreference preference) noexcept {
  switch (preference) {
    case ArchitecturePreference::prefer_arm64_prompt_fallback:
      return "prefer-arm64-prompt-x64-fallback";
    case ArchitecturePreference::prefer_arm64_auto_fallback:
      return "prefer-arm64-auto-x64-fallback";
    case ArchitecturePreference::prefer_x64:
      return "prefer-x64";
  }
  return "unknown";
}

char const* to_string(SelectionStatus status) noexcept {
  switch (status) {
    case SelectionStatus::selected_native:
      return "selected-native";
    case SelectionStatus::selected_architecture_independent:
      return "selected-architecture-independent";
    case SelectionStatus::selected_compatibility_fallback:
      return "selected-compatibility-fallback";
    case SelectionStatus::fallback_confirmation_required:
      return "fallback-confirmation-required";
    case SelectionStatus::fallback_refused:
      return "fallback-refused";
    case SelectionStatus::incompatible:
      return "incompatible";
    case SelectionStatus::detection_failed_paused:
      return "detection-failed-paused";
    case SelectionStatus::package_information_unavailable:
      return "package-information-unavailable";
    case SelectionStatus::architecture_changed:
      return "architecture-changed";
  }
  return "unknown";
}

}  // namespace azzs::domain::architecture_selection
