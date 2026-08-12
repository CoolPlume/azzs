#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "azzs/domain/software_optimization_catalog.hpp"

namespace azzs::application::sogou_optimization {

// These are the only software-owned changes that the first Sogou catalog may
// request.  Catalog data is mapped to this closed set before it reaches a
// platform adapter.
enum class SogouOptimizationAction : std::uint8_t {
  hide_status_bar,
  disable_input_indicator,
  disable_status_bar_language_bar,
  disable_paired_punctuation,
  disable_smart_punctuation,
  set_candidate_count,
  disable_system_shortcuts,
  disable_input_assistant,
  disable_startup,
  disable_i_mode_skin,
  disable_skin_recommendation,
  disable_skin_popup_recommendation,
  disable_desktop_recommendation,
  disable_search_recommendation,
  disable_equals_search,
  disable_ai_emoji,
  disable_quick_search,
  disable_selection_tool,
  disable_ai_hotkey,
  disable_ai_startup,
  disable_fast_search,
  disable_toolbox,
  disable_pdf_shell_extension,
  disable_disk_shell_extension,
  disable_compression_shell_extension,
};

enum class SogouCandidateCount : std::uint8_t {
  three = 3,
  four = 4,
  five = 5,
  six = 6,
  seven = 7,
  eight = 8,
  nine = 9,
};

enum class SogouOptimizationStatus : std::uint8_t {
  succeeded,
  already_effective,
  failed,
  pending_confirmation,
  not_installed,
  version_not_supported,
  invalid_request,
  unsupported,
  cancelled,
};

struct SogouTargetDetection final {
  SogouOptimizationStatus status{SogouOptimizationStatus::failed};
  std::optional<std::string> installed_version;
  std::string detail;
};

struct SogouOptionDetection final {
  SogouOptimizationStatus status{SogouOptimizationStatus::failed};
  std::optional<SogouCandidateCount> candidate_count;
  std::string detail;
};

struct SogouOptimizationExecution final {
  SogouOptimizationStatus status{SogouOptimizationStatus::failed};
  std::string detail;
};

// Platform adapters implement observation only through this typed seam.  They
// never receive a command, path, registry key, selector, or UI step.
class SogouOptimizationDetector {
 public:
  virtual ~SogouOptimizationDetector() = default;

  [[nodiscard]] virtual SogouTargetDetection detect_target() = 0;
  [[nodiscard]] virtual SogouOptionDetection detect_option(
      SogouOptimizationAction action,
      std::optional<SogouCandidateCount> expected_value) = 0;
};

class SogouOptimizationExecutor {
 public:
  virtual ~SogouOptimizationExecutor() = default;

  [[nodiscard]] virtual SogouOptimizationExecution execute(
      SogouOptimizationAction action,
      std::optional<SogouCandidateCount> value) = 0;
};

class SogouOptimizationPlatformAdapter : public SogouOptimizationDetector,
                                          public SogouOptimizationExecutor {
 public:
  ~SogouOptimizationPlatformAdapter() override = default;
};

// The application service owns the catalog-to-capability mapping.  This keeps
// the domain package declarative and keeps adapters independent of catalog IDs.
class SogouOptimizationService final {
 public:
  SogouOptimizationService(SogouOptimizationDetector& detector,
                           SogouOptimizationExecutor& executor) noexcept;

  [[nodiscard]] SogouTargetDetection detect_target();
  [[nodiscard]] SogouOptionDetection detect_option(
      domain::software_optimization_catalog::SoftwareOptimizationOption const&,
      std::optional<std::string_view> selected_value = std::nullopt);
  [[nodiscard]] SogouOptimizationExecution execute_option(
      domain::software_optimization_catalog::SoftwareOptimizationOption const&,
      std::optional<std::string_view> selected_value = std::nullopt);

 private:
  SogouOptimizationDetector& detector_;
  SogouOptimizationExecutor& executor_;
};

[[nodiscard]] std::span<domain::software_optimization_catalog::
                             BuiltInRuleDefinition const>
built_in_rule_definitions() noexcept;

[[nodiscard]] std::optional<SogouOptimizationAction> map_execution_rule(
    std::string_view rule_id) noexcept;

[[nodiscard]] std::optional<SogouOptimizationAction> map_detection_rule(
    std::string_view rule_id) noexcept;

[[nodiscard]] std::optional<SogouCandidateCount> parse_candidate_count(
    std::string_view value) noexcept;

[[nodiscard]] std::string_view candidate_count_name(
    SogouCandidateCount value) noexcept;

}  // namespace azzs::application::sogou_optimization
