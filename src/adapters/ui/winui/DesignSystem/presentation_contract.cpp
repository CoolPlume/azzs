#include "presentation_contract.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace azzs::ui::presentation {

PresentationSnapshot::PresentationSnapshot(
    std::vector<ComponentProjection> components)
    : components_{std::move(components)} {
  std::unordered_set<std::string> component_ids;
  std::unordered_set<std::string> automation_ids;
  for (auto const& component : components_) {
    if (component.id.empty() || component.automation_id.empty() ||
        component.accessible_name.empty()) {
      throw std::invalid_argument{
          "presentation components require stable and accessible identity"};
    }
    if (!component_ids.insert(component.id).second ||
        !automation_ids.insert(component.automation_id).second) {
      throw std::invalid_argument{
          "presentation component identities must be unique"};
    }
    if (component.advanced_only &&
        (component.risk == RiskLevel::high ||
         component.risk == RiskLevel::critical ||
         component.state == PresentationState::failed ||
         component.state == PresentationState::pending_confirmation ||
         component.state == PresentationState::withdrawn)) {
      throw std::invalid_argument{
          "blocking state cannot be hidden in advanced view"};
    }

    if (component.progress.has_value()) {
      auto const& progress = *component.progress;
      switch (progress.kind) {
        case ProgressKind::none:
          if (progress.completed.has_value() || progress.total.has_value() ||
              !progress.accessible_value.empty()) {
            throw std::invalid_argument{
                "hidden progress cannot expose values or text"};
          }
          break;
        case ProgressKind::determinate:
          if (!progress.completed.has_value() || !progress.total.has_value() ||
              *progress.total == 0 || *progress.completed > *progress.total ||
              progress.accessible_value.empty()) {
            throw std::invalid_argument{
                "determinate progress requires bounded values and text"};
          }
          break;
        case ProgressKind::indeterminate:
        case ProgressKind::unknown:
          if (progress.completed.has_value() || progress.total.has_value() ||
              progress.accessible_value.empty()) {
            throw std::invalid_argument{
                "non-determinate progress requires text without numeric values"};
          }
          break;
      }
    }

    std::unordered_set<std::string> command_ids;
    std::size_t default_focus_count = 0;
    for (auto const& command : component.commands) {
      if (command.id.empty() || command.label.empty() ||
          !command_ids.insert(command.id).second ||
          command.intent.target_id != component.id ||
          command.intent.command_id != command.id) {
        throw std::invalid_argument{
            "presentation commands require stable typed intent identity"};
      }
      if (!command.enabled && command.disabled_reason.empty()) {
        throw std::invalid_argument{
            "disabled presentation commands require a reason"};
      }
      if (command.default_focus) {
        ++default_focus_count;
        if (!command.enabled || command.role == CommandRole::danger) {
          throw std::invalid_argument{
              "default presentation commands must be safe and enabled"};
        }
      }
    }
    if (default_focus_count > 1) {
      throw std::invalid_argument{
          "a presentation component can have only one default command"};
    }
  }
}

std::span<ComponentProjection const> PresentationSnapshot::components() const
    noexcept {
  return components_;
}

ComponentProjection const* PresentationSnapshot::find_component(
    std::string_view component_id) const noexcept {
  auto const match = std::ranges::find(
      components_, component_id, &ComponentProjection::id);
  return match == components_.end() ? nullptr : std::addressof(*match);
}

ReadOnlyPresentation::ReadOnlyPresentation(
    std::shared_ptr<PresentationSnapshot const> source,
    ViewMode mode)
    : source_{std::move(source)}, mode_{mode} {
  if (!source_) {
    throw std::invalid_argument{"presentation source must not be null"};
  }
}

PresentationSnapshot const& ReadOnlyPresentation::source() const noexcept {
  return *source_;
}

ViewMode ReadOnlyPresentation::mode() const noexcept {
  return mode_;
}

bool ReadOnlyPresentation::visible(
    ComponentProjection const& component) const noexcept {
  return !component.advanced_only || mode_ == ViewMode::advanced;
}

bool ReadOnlyPresentation::visible(CommandProjection const& command) const
    noexcept {
  return !command.advanced_only || mode_ == ViewMode::advanced;
}

std::optional<PresentationIntent> ReadOnlyPresentation::intent_for(
    std::string_view component_id,
    std::string_view command_id) const {
  auto const* owner = source_->find_component(component_id);
  if (owner == nullptr || !visible(*owner)) {
    return std::nullopt;
  }

  auto const match = std::ranges::find(owner->commands, command_id,
                                       &CommandProjection::id);
  if (match == owner->commands.end() || !visible(*match) || !match->enabled) {
    return std::nullopt;
  }
  return match->intent;
}

}  // namespace azzs::ui::presentation
