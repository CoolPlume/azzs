#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/execution_log.hpp"
#include "azzs/application/platform_info.hpp"
#include "azzs/domain/architecture_selection.hpp"

namespace azzs::application::architecture_selection {

namespace selection_domain = domain::architecture_selection;

struct SoftwarePackageRequest final {
  std::string software_id;
  std::vector<selection_domain::PackageCandidate> candidates;
};

struct ArchitectureRecheck final {
  selection_domain::ArchitectureObservation previous;
  selection_domain::ArchitectureObservation current;
  bool changed{false};
};

// A snapshot is frozen by the batch owner. This module only compares it to a
// fresh observation and returns pause facts; it never mutates batch history.
struct BatchPackageSnapshot final {
  std::string software_id;
  selection_domain::PackageCandidate package;
  selection_domain::ArchitectureObservation observation;
};

struct BatchArchitectureRecheck final {
  ArchitectureRecheck observation;
  std::vector<selection_domain::SelectionResult> affected;
  bool changed{false};
};

// Owns volatile architecture observations, pending fallback confirmations and
// the logging of architecture decisions. Preferences are supplied by their
// dedicated owner and changing a preference does not mutate frozen batches.
class ArchitectureSelectionLifecycle final {
 public:
  ArchitectureSelectionLifecycle(
      PlatformInfo const& platform, ExecutionLog& log,
      selection_domain::ArchitecturePreference preference) noexcept;

  void set_preference(selection_domain::ArchitecturePreference preference) noexcept;

  [[nodiscard]] ArchitectureRecheck start();
  [[nodiscard]] ArchitectureRecheck restore();
  [[nodiscard]] selection_domain::SelectionResult evaluate(
      SoftwarePackageRequest const& request);
  [[nodiscard]] selection_domain::SelectionResult confirm_fallback(
      std::string_view software_id);
  [[nodiscard]] selection_domain::SelectionResult refuse_fallback(
      std::string_view software_id);
  [[nodiscard]] selection_domain::SelectionResult retry(
      SoftwarePackageRequest const& request,
      std::optional<selection_domain::PackageArchitecture> one_shot_preference =
          std::nullopt);
  [[nodiscard]] BatchArchitectureRecheck recheck_batch(
      selection_domain::ArchitectureObservation frozen,
      std::span<BatchPackageSnapshot const> pending_items);

 private:
  struct PendingFallback final {
    SoftwarePackageRequest request;
    selection_domain::ArchitectureObservation observation;
  };

  [[nodiscard]] ArchitectureRecheck observe(std::string_view trigger);
  [[nodiscard]] selection_domain::SelectionResult evaluate_with_observation(
      SoftwarePackageRequest const& request,
      selection_domain::ArchitectureObservation observation,
      std::optional<selection_domain::PackageArchitecture> one_shot_preference,
      std::string_view trigger);
  void log_event(std::string_view stage, ExecutionResult result,
                 selection_domain::ArchitectureObservation observation,
                 std::string_view software_id = {},
                 std::optional<selection_domain::PackageArchitecture> package =
                     std::nullopt,
                 std::string_view detail = {});

  PlatformInfo const& platform_;
  ExecutionLog& log_;
  selection_domain::ArchitecturePreference preference_;
  std::optional<selection_domain::ArchitectureObservation> current_;
  std::map<std::string, PendingFallback, std::less<>> pending_fallbacks_;
  std::set<selection_domain::ArchitectureObservation>
      batches_requiring_reassessment_;
  std::uint64_t next_observation_id_{1};
};

}  // namespace azzs::application::architecture_selection
