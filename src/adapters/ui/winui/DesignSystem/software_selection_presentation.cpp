#include "software_selection_presentation.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace azzs::ui::presentation {
namespace {

[[nodiscard]] std::string selection_summary(
    application::software_selection::SoftwareSelectionSnapshot const& source,
    SoftwareSelectionPresentationText const& text) {
  if (!source.has_current_catalog) {
    return text.absent_catalog_body;
  }
  auto const selected = source.selection.selected_software_ids.size();
  return text.available_body_prefix + std::to_string(selected) +
         text.available_body_suffix;
}

}  // namespace

std::shared_ptr<PresentationSnapshot const>
make_software_selection_presentation(
    application::software_selection::SoftwareSelectionSnapshot const& source,
    SoftwareSelectionPresentationText text) {
  auto state = PresentationState::ready;
  auto title = text.available_title;
  auto body = selection_summary(source, text);
  auto advanced_detail = std::string{};

  if (!source.has_current_catalog) {
    state = PresentationState::waiting_for_network;
    title = text.absent_catalog_title;
    if (source.mode ==
        application::software_selection::SelectionLifecycleMode::failed) {
      state = PresentationState::failed;
      body = source.error.empty() ? text.restore_failed_body : source.error;
    } else if (source.mode ==
               application::software_selection::SelectionLifecycleMode::
                   not_restored) {
      body = text.not_restored_body;
    }
    advanced_detail = text.advanced_absent_catalog;
  } else {
    advanced_detail = text.advanced_available;
  }

  std::vector<ComponentProjection> components{
      {
          .id = "software-selection.status",
          .automation_id = "AzzsSoftwareSelectionStatus",
          .accessible_name = std::move(text.accessible_name),
          .kind = ComponentKind::status_band,
          .state = state,
          .announcement = AnnouncementMode::polite,
          .title = std::move(title),
          .body = std::move(body),
          .advanced_detail = std::move(advanced_detail),
          .stage = WorkflowStage::software_installation,
      },
  };
  return std::make_shared<PresentationSnapshot const>(std::move(components));
}

}  // namespace azzs::ui::presentation
