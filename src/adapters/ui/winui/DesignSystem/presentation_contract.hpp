#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace azzs::ui::presentation {

enum class ViewMode {
  standard,
  advanced,
};

enum class ComponentKind {
  navigation,
  group,
  list,
  detail,
  summary,
  status_band,
  inline_error,
  disabled_reason,
  risk_confirmation,
  progress,
  waiting,
  failure,
  pending_confirmation,
  emergency_withdrawal,
  result_locator,
  stage_summary,
  settings_form,
  catalog_editor,
};

enum class PresentationState {
  neutral,
  ready,
  in_progress,
  waiting_for_network,
  waiting_for_restart,
  recovered_unsaved,
  saved_not_applied,
  source_handoff,
  failed,
  pending_confirmation,
  withdrawn,
  disabled,
  local_trial,
  completed,
};

enum class RiskLevel {
  none,
  low,
  medium,
  high,
  critical,
};

enum class ProgressKind {
  none,
  determinate,
  indeterminate,
  unknown,
};

enum class WorkflowStage {
  drivers,
  system_optimization,
  software_installation,
  software_optimization,
};

enum class AnnouncementMode {
  none,
  polite,
  assertive,
};

enum class CommandRole {
  primary,
  secondary,
  danger,
  navigation,
};

enum class IntentKind {
  open_details,
  toggle_item_selection,
  expand_summary,
  retry,
  open_source,
  confirm_risk,
  stop_safely,
  open_emergency_withdrawal,
  locate_result,
  continue_workflow,
};

struct PresentationIntent final {
  IntentKind kind;
  std::string target_id;
  std::string command_id;

  bool operator==(PresentationIntent const&) const = default;
};

struct CommandProjection final {
  std::string id;
  std::string label;
  CommandRole role{CommandRole::secondary};
  bool enabled{true};
  bool default_focus{false};
  bool advanced_only{false};
  std::string disabled_reason;
  PresentationIntent intent;
};

struct ProgressProjection final {
  ProgressKind kind{ProgressKind::none};
  std::optional<std::uint64_t> completed;
  std::optional<std::uint64_t> total;
  std::string accessible_value;
};

struct ComponentProjection final {
  std::string id;
  std::string automation_id;
  std::string accessible_name;
  ComponentKind kind{ComponentKind::group};
  PresentationState state{PresentationState::neutral};
  RiskLevel risk{RiskLevel::none};
  AnnouncementMode announcement{AnnouncementMode::none};
  std::string title;
  std::string body;
  std::string advanced_detail;
  bool advanced_only{false};
  std::optional<WorkflowStage> stage;
  std::optional<ProgressProjection> progress;
  std::vector<CommandProjection> commands;
};

class PresentationSnapshot final {
 public:
  explicit PresentationSnapshot(std::vector<ComponentProjection> components);

  [[nodiscard]] std::span<ComponentProjection const> components() const
      noexcept;
  [[nodiscard]] ComponentProjection const* find_component(
      std::string_view component_id) const noexcept;

 private:
  std::vector<ComponentProjection> components_;
};

class ReadOnlyPresentation final {
 public:
  ReadOnlyPresentation(std::shared_ptr<PresentationSnapshot const> source,
                       ViewMode mode);

  [[nodiscard]] PresentationSnapshot const& source() const noexcept;
  [[nodiscard]] ViewMode mode() const noexcept;
  [[nodiscard]] bool visible(ComponentProjection const& component) const
      noexcept;
  [[nodiscard]] bool visible(CommandProjection const& command) const noexcept;
  [[nodiscard]] std::optional<PresentationIntent> intent_for(
      std::string_view component_id,
      std::string_view command_id) const;

 private:
  std::shared_ptr<PresentationSnapshot const> source_;
  ViewMode mode_;
};

}  // namespace azzs::ui::presentation
