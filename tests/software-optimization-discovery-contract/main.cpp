#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "azzs/domain/software_optimization_discovery.hpp"

namespace {

namespace catalog = azzs::domain::software_optimization_catalog;
namespace discovery = azzs::domain::software_optimization_discovery;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "software optimization discovery contract failed: " << message
              << '\n';
  }
  return condition;
}

[[nodiscard]] catalog::SoftwareOptimizationCatalog test_catalog() {
  auto required = catalog::SoftwareOptimizationOption{
      .id = {"required-option"},
      .scheme_id = {"scheme"},
      .supported_versions = {"16.0", "17.0"},
      .impact = "required change",
      .required = true,
      .automation = catalog::AutomationSupport::controlled,
      .execution = {catalog::RuleKind::built_in_definition, {"execute-required"}},
      .state_detection = {catalog::RuleKind::built_in_definition, {"detect-required"}},
  };
  auto low = catalog::SoftwareOptimizationOption{
      .id = {"low-option"},
      .scheme_id = {"scheme"},
      .supported_versions = {"16.0", "17.0"},
      .impact = "low impact change",
      .default_selected = true,
      .automation = catalog::AutomationSupport::controlled,
      .execution = {catalog::RuleKind::built_in_definition, {"execute-low"}},
      .state_detection = {catalog::RuleKind::built_in_definition, {"detect-low"}},
      .required_option_ids = {{"required-option"}},
  };
  auto conflicting = catalog::SoftwareOptimizationOption{
      .id = {"conflicting-option"},
      .scheme_id = {"scheme"},
      .supported_versions = {"16.0", "17.0"},
      .impact = "incompatible change",
      .automation = catalog::AutomationSupport::controlled,
      .execution = {catalog::RuleKind::built_in_definition, {"execute-conflict"}},
      .state_detection = {catalog::RuleKind::built_in_definition, {"detect-conflict"}},
      .conflicting_option_ids = {{"low-option"}},
  };
  return {
      .revision = 4,
      .publication_state = catalog::PublicationState::release,
      .targets = {{
          .id = {"target"},
          .identity_anchor = {"vendor.product"},
          .required_first_release = true,
          .support_mode = catalog::SupportMode::supported,
          .supported_versions = {"16.0", "17.0"},
          .install_detection = {catalog::RuleKind::built_in_definition, {"detect-install"}},
          .version_detection = {catalog::RuleKind::built_in_definition, {"detect-version"}},
          .installation_item_id = catalog::StableId{"software-item"},
      }},
      .schemes = {{
          .id = {"scheme"},
          .target_id = {"target"},
          .required_first_release = true,
          .automation = catalog::AutomationSupport::controlled,
          .supported_versions = {"16.0", "17.0"},
          .impact = "product-owned configuration",
          .options = {low, required, conflicting},
      }},
  };
}

[[nodiscard]] bool discovery_contract() {
  auto catalog_value = test_catalog();
  auto defaults = discovery::default_selection(catalog_value);
  bool passed = expect(defaults.options.size() == 2,
                       "default selection must include low-impact and required options");

  std::vector<discovery::TargetObservation> targets{{
      .target_id = {"target"},
      .presence = discovery::TargetPresence::externally_recognized,
      .installed_version = "16.7",
      .detail = "external install recognized",
  }};
  std::vector<discovery::OptionObservation> options{
      {{"low-option"}, discovery::OptionState::needs_optimization, "not effective"},
      {{"required-option"}, discovery::OptionState::optimized, "already effective"},
      {{"conflicting-option"}, discovery::OptionState::needs_optimization, "not effective"},
  };
  auto snapshot = discovery::discover({
      .catalog = catalog_value,
      .targets = targets,
      .options = options,
      .withdrawn_operations = {},
      .selection = defaults,
  });
  passed &= expect(snapshot.targets.size() == 1 &&
                       snapshot.targets.front().presence ==
                           discovery::TargetPresence::externally_recognized &&
                       snapshot.targets.front().schemes.front().state ==
                           discovery::SchemeState::can_optimize,
                   "external recognition must participate in normal discovery without a workbench install record");

  auto all_optimized = options;
  for (auto& option : all_optimized) {
    option.state = discovery::OptionState::optimized;
  }
  auto optimized = discovery::discover({
      .catalog = catalog_value,
      .targets = targets,
      .options = all_optimized,
      .withdrawn_operations = {},
      .selection = defaults,
  });
  passed &= expect(
      optimized.targets.front().schemes.front().state ==
          discovery::SchemeState::optimized &&
          !optimized.targets.front().first_release_implementation_error,
      "an implemented first-release scheme remains valid when every option is already optimized");

  auto option_version_catalog = catalog_value;
  for (auto& option : option_version_catalog.schemes.front().options) {
    option.supported_versions = {"18.0", "19.0"};
  }
  auto option_version = discovery::discover({
      .catalog = option_version_catalog,
      .targets = targets,
      .options = options,
      .withdrawn_operations = {},
      .selection = defaults,
  });
  auto const executable = discovery::executable_selected_options(
      option_version, defaults);
  passed &= expect(
      option_version.targets.front().schemes.front().state ==
          discovery::SchemeState::version_not_applicable &&
          option_version.targets.front().schemes.front().options.front().state ==
              discovery::OptionState::version_not_applicable &&
          executable.empty(),
      "option version ranges must remove incompatible selections from submission");

  targets.front().installed_version.reset();
  auto version_unknown = discovery::discover({
      .catalog = catalog_value,
      .targets = targets,
      .options = options,
      .withdrawn_operations = {},
      .selection = defaults,
  });
  passed &= expect(version_unknown.targets.size() == 1 &&
                       version_unknown.targets.front().schemes.front().state ==
                           discovery::SchemeState::version_not_applicable,
                   "external recognition must not manufacture a matching version");

  targets.front().presence = discovery::TargetPresence::absent;
  targets.front().installed_version = "16.7";
  auto absent = discovery::discover({
      .catalog = catalog_value,
      .targets = targets,
      .options = options,
      .withdrawn_operations = {},
      .selection = defaults,
  });
  passed &= expect(absent.targets.empty(),
                   "undetected software must not appear solely because the catalog has a scheme");

  targets.front().presence = discovery::TargetPresence::detected;
  std::vector<discovery::WithdrawnOperation> withdrawn{{{"scheme"}, "critical risk"}};
  auto blocked = discovery::discover({
      .catalog = catalog_value,
      .targets = targets,
      .options = options,
      .withdrawn_operations = withdrawn,
      .selection = defaults,
  });
  passed &= expect(blocked.targets.front().schemes.front().state ==
                       discovery::SchemeState::emergency_withdrawn,
                   "an emergency withdrawal must block new optimization selection");
  return passed;
}

[[nodiscard]] bool selection_contract() {
  auto catalog_value = test_catalog();
  discovery::SelectionState empty;
  auto proposed = discovery::change_selection(
      catalog_value, empty,
      {.scheme_id = {"scheme"}, .option_id = {"low-option"}, .selected = true});
  bool passed = expect(
      proposed.code == discovery::SelectionChangeCode::confirmation_required &&
          !proposed.applied && proposed.adjustment.added_required.size() == 1,
      "required option auto-selection must wait for confirmation");
  auto accepted = discovery::change_selection(
      catalog_value, empty,
      {.scheme_id = {"scheme"},
       .option_id = {"low-option"},
       .selected = true,
       .accept_adjustments = true});
  passed &= expect(accepted.applied && accepted.state.options.size() == 2,
                   "confirmed required adjustment must become the only selection state");
  auto conflicting = discovery::change_selection(
      catalog_value, accepted.state,
      {.scheme_id = {"scheme"}, .option_id = {"conflicting-option"}, .selected = true});
  passed &= expect(
      conflicting.code == discovery::SelectionChangeCode::confirmation_required &&
          conflicting.adjustment.removed_conflicting.size() == 1,
      "conflicting option adjustment must wait for confirmation");
  auto required_deselect = discovery::change_selection(
      catalog_value, accepted.state,
      {.scheme_id = {"scheme"}, .option_id = {"required-option"}, .selected = false});
  passed &= expect(required_deselect.code == discovery::SelectionChangeCode::rejected,
                   "required options cannot be deselected");
  return passed;
}

}  // namespace

int main() {
  bool passed = discovery_contract();
  passed &= selection_contract();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "software optimization discovery contract passed\n";
  return EXIT_SUCCESS;
}
