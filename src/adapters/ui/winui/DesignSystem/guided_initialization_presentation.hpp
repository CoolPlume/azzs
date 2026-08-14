#pragma once

#include <memory>
#include <string>

#include "azzs/application/guided_initialization.hpp"
#include "presentation_contract.hpp"

namespace azzs::ui::presentation {

struct GuidedInitializationPresentationText final {
  std::string summary_accessible_name{"Recommended initialization summary"};
  std::string summary_title{"Recommended initialization"};
  std::string summary_prefix{"Completed: "};
  std::string summary_external_prefix{"; external installations recognized: "};
  std::string summary_partial_prefix{"; partial: "};
  std::string summary_failed_prefix{"; failed: "};
  std::string summary_skipped_prefix{"; skipped: "};
  std::string summary_no_applicable_prefix{"; no applicable items: "};
  std::string summary_not_executed_prefix{"; not executed: "};
  std::string summary_confirmation_prefix{"; result confirmation pending: "};
  std::string summary_explorer_restart_prefix{
      "; waiting for Explorer restart: "};
  std::string summary_restart_prefix{"; waiting for Windows restart: "};
  std::string summary_withdrawn_prefix{"; emergency withdrawn: "};
  std::string summary_error_suffix{". Review the current stage before continuing."};
  std::string start_command{"Start recommended initialization"};
  std::string refresh_command{"Refresh status"};
  std::string cancel_command{"Cancel flow"};
  std::string history_command{"View history and logs"};
  std::string skip_command{"Skip this stage for now"};
  std::string continue_command{"Continue"};
  std::string retry_command{"Retry from the dedicated page"};
  std::string open_command{"Open dedicated page"};
  std::string local_trial_accessible_name{"Software catalog identity"};
  std::string local_trial_title{"Using local trial catalog"};
  std::string local_trial_body{
      "The catalog passed local loading checks but has not passed release checks. "
      "It affects tasks created after this point."};
  std::string handoff_accessible_name{"External installation handoff"};
  std::string handoff_title{"External installation handoff"};
  std::string handoff_waiting_body{
      "The source is waiting for an external installation and a later recheck."};
  std::string handoff_recognized_body{
      "The external installation was recognized. Continue only after reviewing it."};
  std::string handoff_continue_command{"Continue this handoff"};
  std::string read_only_accessible_name{"Recommended initialization recovery"};
  std::string read_only_title{"Recommended initialization is read-only"};
  std::string read_only_body{
      "The flow record cannot be changed safely until its persisted state is repaired."};
};

[[nodiscard]] std::shared_ptr<PresentationSnapshot const>
make_guided_initialization_presentation(
    application::guided_initialization::Snapshot const& source,
    GuidedInitializationPresentationText text = {});

}  // namespace azzs::ui::presentation
