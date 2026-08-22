#include "software_selection_presentation.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace azzs::ui::presentation {
namespace {

void restore_empty_text_fields(SoftwareSelectionPresentationText& text) {
  auto const defaults = SoftwareSelectionPresentationText{};
  if (text.accessible_name.empty()) {
    text.accessible_name = defaults.accessible_name;
  }
  if (text.available_title.empty()) {
    text.available_title = defaults.available_title;
  }
  if (text.available_body_prefix.empty()) {
    text.available_body_prefix = defaults.available_body_prefix;
  }
  if (text.available_body_suffix.empty()) {
    text.available_body_suffix = defaults.available_body_suffix;
  }
  if (text.absent_catalog_title.empty()) {
    text.absent_catalog_title = defaults.absent_catalog_title;
  }
  if (text.absent_catalog_body.empty()) {
    text.absent_catalog_body = defaults.absent_catalog_body;
  }
  if (text.not_restored_body.empty()) {
    text.not_restored_body = defaults.not_restored_body;
  }
  if (text.restore_failed_body.empty()) {
    text.restore_failed_body = defaults.restore_failed_body;
  }
  if (text.advanced_available.empty()) {
    text.advanced_available = defaults.advanced_available;
  }
  if (text.advanced_absent_catalog.empty()) {
    text.advanced_absent_catalog = defaults.advanced_absent_catalog;
  }
}

void restore_empty_cache_text_fields(OfflinePackageCachePresentationText& text) {
  auto const defaults = OfflinePackageCachePresentationText{};
  if (text.accessible_name.empty()) {
    text.accessible_name = defaults.accessible_name;
  }
  if (text.available_title.empty()) {
    text.available_title = defaults.available_title;
  }
  if (text.unavailable_title.empty()) {
    text.unavailable_title = defaults.unavailable_title;
  }
  if (text.available_body_prefix.empty()) {
    text.available_body_prefix = defaults.available_body_prefix;
  }
  if (text.unavailable_body_prefix.empty()) {
    text.unavailable_body_prefix = defaults.unavailable_body_prefix;
  }
  if (text.item_suffix.empty()) {
    text.item_suffix = defaults.item_suffix;
  }
  if (text.network_suffix.empty()) {
    text.network_suffix = defaults.network_suffix;
  }
}

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
  restore_empty_text_fields(text);
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

std::shared_ptr<PresentationSnapshot const>
make_offline_package_cache_presentation(
    application::offline_package_cache::OfflinePackageCacheSnapshot const& source,
    OfflinePackageCachePresentationText text) {
  restore_empty_cache_text_fields(text);
  auto const available =
      source.location_state == application::offline_package_cache::
                                  CacheLocationState::available;
  auto cached_count = std::size_t{};
  for (auto const& item : source.items) {
    if (item.cache_present) {
      ++cached_count;
    }
  }

  auto body = available ? text.available_body_prefix + source.selected_root.id
                        : text.unavailable_body_prefix + source.location_detail;
  body += "; " + std::to_string(cached_count) + text.item_suffix;
  if (!source.network_available) {
    body += text.network_suffix;
  }
  auto const state = available ? PresentationState::ready
                               : PresentationState::waiting_for_network;
  std::vector<ComponentProjection> components{
      {
          .id = "offline-package-cache.status",
          .automation_id = "AzzsOfflinePackageCacheStatus",
          .accessible_name = std::move(text.accessible_name),
          .kind = ComponentKind::status_band,
          .state = state,
          .announcement = AnnouncementMode::none,
          .title = available ? std::move(text.available_title)
                             : std::move(text.unavailable_title),
          .body = std::move(body),
          .stage = WorkflowStage::software_installation,
      },
  };
  return std::make_shared<PresentationSnapshot const>(std::move(components));
}

}  // namespace azzs::ui::presentation
