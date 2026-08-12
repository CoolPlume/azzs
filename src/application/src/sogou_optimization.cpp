#include "azzs/application/sogou_optimization.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace azzs::application::sogou_optimization {
namespace {

namespace catalog = domain::software_optimization_catalog;

struct RulePair final {
  std::string_view execute;
  std::string_view detect;
  SogouOptimizationAction action;
};

constexpr RulePair kRulePairs[]{
    {"sogou.optimize.execute.hide-status-bar-v1",
     "sogou.optimize.detect.hide-status-bar-v1",
     SogouOptimizationAction::hide_status_bar},
    {"sogou.optimize.execute.disable-input-indicator-v1",
     "sogou.optimize.detect.disable-input-indicator-v1",
     SogouOptimizationAction::disable_input_indicator},
    {"sogou.optimize.execute.disable-status-bar-language-bar-v1",
     "sogou.optimize.detect.disable-status-bar-language-bar-v1",
     SogouOptimizationAction::disable_status_bar_language_bar},
    {"sogou.optimize.execute.disable-paired-punctuation-v1",
     "sogou.optimize.detect.disable-paired-punctuation-v1",
     SogouOptimizationAction::disable_paired_punctuation},
    {"sogou.optimize.execute.disable-smart-punctuation-v1",
     "sogou.optimize.detect.disable-smart-punctuation-v1",
     SogouOptimizationAction::disable_smart_punctuation},
    {"sogou.optimize.execute.set-candidate-count-v1",
     "sogou.optimize.detect.set-candidate-count-v1",
     SogouOptimizationAction::set_candidate_count},
    {"sogou.optimize.execute.disable-system-shortcuts-v1",
     "sogou.optimize.detect.disable-system-shortcuts-v1",
     SogouOptimizationAction::disable_system_shortcuts},
    {"sogou.optimize.execute.disable-input-assistant-v1",
     "sogou.optimize.detect.disable-input-assistant-v1",
     SogouOptimizationAction::disable_input_assistant},
    {"sogou.optimize.execute.disable-startup-v1",
     "sogou.optimize.detect.disable-startup-v1",
     SogouOptimizationAction::disable_startup},
    {"sogou.optimize.execute.disable-i-mode-skin-v1",
     "sogou.optimize.detect.disable-i-mode-skin-v1",
     SogouOptimizationAction::disable_i_mode_skin},
    {"sogou.optimize.execute.disable-skin-recommendation-v1",
     "sogou.optimize.detect.disable-skin-recommendation-v1",
     SogouOptimizationAction::disable_skin_recommendation},
    {"sogou.optimize.execute.disable-skin-popup-recommendation-v1",
     "sogou.optimize.detect.disable-skin-popup-recommendation-v1",
     SogouOptimizationAction::disable_skin_popup_recommendation},
    {"sogou.optimize.execute.disable-desktop-recommendation-v1",
     "sogou.optimize.detect.disable-desktop-recommendation-v1",
     SogouOptimizationAction::disable_desktop_recommendation},
    {"sogou.optimize.execute.disable-search-recommendation-v1",
     "sogou.optimize.detect.disable-search-recommendation-v1",
     SogouOptimizationAction::disable_search_recommendation},
    {"sogou.optimize.execute.disable-equals-search-v1",
     "sogou.optimize.detect.disable-equals-search-v1",
     SogouOptimizationAction::disable_equals_search},
    {"sogou.optimize.execute.disable-ai-emoji-v1",
     "sogou.optimize.detect.disable-ai-emoji-v1",
     SogouOptimizationAction::disable_ai_emoji},
    {"sogou.optimize.execute.disable-quick-search-v1",
     "sogou.optimize.detect.disable-quick-search-v1",
     SogouOptimizationAction::disable_quick_search},
    {"sogou.optimize.execute.disable-selection-tool-v1",
     "sogou.optimize.detect.disable-selection-tool-v1",
     SogouOptimizationAction::disable_selection_tool},
    {"sogou.optimize.execute.disable-ai-hotkey-v1",
     "sogou.optimize.detect.disable-ai-hotkey-v1",
     SogouOptimizationAction::disable_ai_hotkey},
    {"sogou.optimize.execute.disable-ai-startup-v1",
     "sogou.optimize.detect.disable-ai-startup-v1",
     SogouOptimizationAction::disable_ai_startup},
    {"sogou.optimize.execute.disable-fast-search-v1",
     "sogou.optimize.detect.disable-fast-search-v1",
     SogouOptimizationAction::disable_fast_search},
    {"sogou.optimize.execute.disable-toolbox-v1",
     "sogou.optimize.detect.disable-toolbox-v1",
     SogouOptimizationAction::disable_toolbox},
    {"sogou.optimize.execute.disable-pdf-shell-extension-v1",
     "sogou.optimize.detect.disable-pdf-shell-extension-v1",
     SogouOptimizationAction::disable_pdf_shell_extension},
    {"sogou.optimize.execute.disable-disk-shell-extension-v1",
     "sogou.optimize.detect.disable-disk-shell-extension-v1",
     SogouOptimizationAction::disable_disk_shell_extension},
    {"sogou.optimize.execute.disable-compression-shell-extension-v1",
     "sogou.optimize.detect.disable-compression-shell-extension-v1",
     SogouOptimizationAction::disable_compression_shell_extension},
};

const auto kBuiltInRules = [] {
  std::array<catalog::BuiltInRuleDefinition, 52> result{};
  result[0] = {{"sogou.detect.installed.v1"},
               catalog::RulePurpose::install_detection};
  result[1] = {{"sogou.detect.version.v1"},
               catalog::RulePurpose::version_detection};
  for (std::size_t index = 0; index < std::size(kRulePairs); ++index) {
    result[index * 2 + 2] = {{std::string{kRulePairs[index].execute}},
                              catalog::RulePurpose::option_execution};
    result[index * 2 + 3] = {{std::string{kRulePairs[index].detect}},
                              catalog::RulePurpose::option_state_detection};
  }
  return result;
}();

[[nodiscard]] std::optional<SogouOptimizationAction> map_rule(
    std::string_view rule_id, bool execution) noexcept {
  for (auto const& pair : kRulePairs) {
    if ((execution ? pair.execute : pair.detect) == rule_id) {
      return pair.action;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<SogouCandidateCount> value_for_action(
    SogouOptimizationAction action, std::optional<std::string_view> value) {
  auto const requires_value =
      action == SogouOptimizationAction::set_candidate_count;
  if (requires_value && !value.has_value()) {
    return std::nullopt;
  }
  if (!requires_value && value.has_value()) {
    return std::nullopt;
  }
  return value.has_value() ? parse_candidate_count(*value) :
                             std::optional<SogouCandidateCount>{};
}

}  // namespace

SogouOptimizationService::SogouOptimizationService(
    SogouOptimizationDetector& detector,
    SogouOptimizationExecutor& executor) noexcept
    : detector_(detector), executor_(executor) {}

SogouTargetDetection SogouOptimizationService::detect_target() {
  return detector_.detect_target();
}

SogouOptionDetection SogouOptimizationService::detect_option(
    catalog::SoftwareOptimizationOption const& option,
    std::optional<std::string_view> selected_value) {
  if (option.automation != catalog::AutomationSupport::controlled ||
      option.execution.kind != catalog::RuleKind::built_in_definition ||
      option.state_detection.kind != catalog::RuleKind::built_in_definition) {
    return {.status = SogouOptimizationStatus::invalid_request,
            .detail = "option is not a controlled Sogou definition"};
  }
  auto action = map_detection_rule(option.state_detection.definition.value);
  if (!action.has_value() ||
      map_execution_rule(option.execution.definition.value) != action) {
    return {.status = SogouOptimizationStatus::invalid_request,
            .detail = "option rule identity is not a registered Sogou pair"};
  }
  auto value = value_for_action(*action, selected_value);
  if (*action == SogouOptimizationAction::set_candidate_count &&
      (!value.has_value() || !selected_value.has_value())) {
    return {.status = SogouOptimizationStatus::invalid_request,
            .detail = "candidate count must use a closed value"};
  }
  if (*action != SogouOptimizationAction::set_candidate_count &&
      selected_value.has_value()) {
    return {.status = SogouOptimizationStatus::invalid_request,
            .detail = "this option does not accept a value"};
  }
  if (value.has_value() &&
      std::find(option.allowed_values.begin(), option.allowed_values.end(),
                std::string{candidate_count_name(*value)}) ==
          option.allowed_values.end()) {
    return {.status = SogouOptimizationStatus::invalid_request,
            .detail = "candidate count is outside the catalog closed set"};
  }
  return detector_.detect_option(*action, value);
}

SogouOptimizationExecution SogouOptimizationService::execute_option(
    catalog::SoftwareOptimizationOption const& option,
    std::optional<std::string_view> selected_value) {
  if (option.automation != catalog::AutomationSupport::controlled ||
      option.execution.kind != catalog::RuleKind::built_in_definition ||
      option.state_detection.kind != catalog::RuleKind::built_in_definition) {
    return {.status = SogouOptimizationStatus::invalid_request,
            .detail = "option is not a controlled Sogou definition"};
  }
  auto action = map_execution_rule(option.execution.definition.value);
  auto detected_action =
      map_detection_rule(option.state_detection.definition.value);
  if (!action.has_value() || action != detected_action) {
    return {.status = SogouOptimizationStatus::invalid_request,
            .detail = "option rule identity is not a registered Sogou pair"};
  }

  if (!selected_value.has_value() && option.default_value.has_value()) {
    selected_value = *option.default_value;
  }
  auto value = value_for_action(*action, selected_value);
  if (*action == SogouOptimizationAction::set_candidate_count &&
      (!value.has_value() || !selected_value.has_value())) {
    return {.status = SogouOptimizationStatus::invalid_request,
            .detail = "candidate count must use a closed value"};
  }
  if (*action != SogouOptimizationAction::set_candidate_count &&
      selected_value.has_value()) {
    return {.status = SogouOptimizationStatus::invalid_request,
            .detail = "this option does not accept a value"};
  }
  if (value.has_value() &&
      std::find(option.allowed_values.begin(), option.allowed_values.end(),
                std::string{candidate_count_name(*value)}) ==
          option.allowed_values.end()) {
    return {.status = SogouOptimizationStatus::invalid_request,
            .detail = "candidate count is outside the catalog closed set"};
  }
  return executor_.execute(*action, value);
}

std::span<catalog::BuiltInRuleDefinition const> built_in_rule_definitions()
    noexcept {
  return kBuiltInRules;
}

std::optional<SogouOptimizationAction> map_execution_rule(
    std::string_view rule_id) noexcept {
  return map_rule(rule_id, true);
}

std::optional<SogouOptimizationAction> map_detection_rule(
    std::string_view rule_id) noexcept {
  return map_rule(rule_id, false);
}

std::optional<SogouCandidateCount> parse_candidate_count(
    std::string_view value) noexcept {
  if (value == "three") {
    return SogouCandidateCount::three;
  }
  if (value == "four") {
    return SogouCandidateCount::four;
  }
  if (value == "five") {
    return SogouCandidateCount::five;
  }
  if (value == "six") {
    return SogouCandidateCount::six;
  }
  if (value == "seven") {
    return SogouCandidateCount::seven;
  }
  if (value == "eight") {
    return SogouCandidateCount::eight;
  }
  if (value == "nine") {
    return SogouCandidateCount::nine;
  }
  return std::nullopt;
}

std::string_view candidate_count_name(SogouCandidateCount value) noexcept {
  switch (value) {
    case SogouCandidateCount::three:
      return "three";
    case SogouCandidateCount::four:
      return "four";
    case SogouCandidateCount::five:
      return "five";
    case SogouCandidateCount::six:
      return "six";
    case SogouCandidateCount::seven:
      return "seven";
    case SogouCandidateCount::eight:
      return "eight";
    case SogouCandidateCount::nine:
      return "nine";
  }
  return {};
}

}  // namespace azzs::application::sogou_optimization
