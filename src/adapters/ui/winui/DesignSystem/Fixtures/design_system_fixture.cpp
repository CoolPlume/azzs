#include "design_system_fixture.hpp"

#include <utility>
#include <vector>

namespace azzs::ui::presentation {

namespace {

[[nodiscard]] CommandProjection command(
    std::string id,
    std::string label,
    CommandRole role,
    IntentKind intent_kind,
    std::string target_id,
    bool enabled = true,
    bool default_focus = false,
    bool advanced_only = false,
    std::string disabled_reason = {}) {
  auto const command_id = id;
  return CommandProjection{
      .id = std::move(id),
      .label = std::move(label),
      .role = role,
      .enabled = enabled,
      .default_focus = default_focus,
      .advanced_only = advanced_only,
      .disabled_reason = std::move(disabled_reason),
      .intent = PresentationIntent{
          .kind = intent_kind,
          .target_id = std::move(target_id),
          .command_id = command_id,
      },
  };
}

[[nodiscard]] ComponentProjection component(
    std::string id,
    std::string automation_id,
    std::string accessible_name,
    ComponentKind kind,
    PresentationState state,
    std::string title,
    std::string body) {
  return ComponentProjection{
      .id = std::move(id),
      .automation_id = std::move(automation_id),
      .accessible_name = std::move(accessible_name),
      .kind = kind,
      .state = state,
      .title = std::move(title),
      .body = std::move(body),
  };
}

}  // namespace

std::shared_ptr<PresentationSnapshot const> make_design_system_fixture() {
  std::vector<ComponentProjection> components;

  components.push_back(component(
      "fixture.navigation", "AzzsFixtureNavigation", "设计系统一级导航",
      ComponentKind::navigation, PresentationState::ready, "一级导航",
      "一级页面切换使用 Windows 原生 NavigationView，并始终即时完成。"));

  components.push_back(component(
      "fixture.group", "AzzsFixtureGroup", "设计系统分组",
      ComponentKind::group, PresentationState::ready, "可扫描分组",
      "分组靠标题、间距和顺序表达关系，不把整个页面包装成漂浮卡片。"));

  auto long_text = component(
      "fixture.long-chinese", "AzzsFixtureLongChinese", "长中文列表夹具",
      ComponentKind::list, PresentationState::ready,
      "一个用于验证长中文、未知版本、来源说明和多行禁用原因仍可扫描的固定条目",
      "此处故意保留很长的简体中文：当窗口变窄、Windows 文本缩放提高或显示器采用 4K 与 225% 显示缩放时，标题、说明、状态和命令都应换行而不是互相覆盖、被裁掉或把工具栏推离可见区域。该夹具不代表任何软件已通过业务验收。");
  long_text.commands.push_back(command(
      "open-details", "查看详情", CommandRole::navigation,
      IntentKind::open_details, long_text.id));
  long_text.commands.push_back(command(
      "toggle-selection", "选择此项", CommandRole::secondary,
      IntentKind::toggle_item_selection, long_text.id));
  components.push_back(std::move(long_text));

  auto detail = component(
      "fixture.advanced-detail", "AzzsFixtureAdvancedDetail",
      "高级视图附加详情", ComponentKind::detail,
      PresentationState::ready, "高级详情",
      "标准视图与高级视图引用同一组件状态。高级视图只附加低频说明。");
  detail.advanced_only = true;
  detail.advanced_detail =
      "来源修订 fixture-r17；这里只是呈现细节，不产生第二份选择或状态。";
  detail.commands.push_back(command(
      "open-details", "查看高级详情", CommandRole::navigation,
      IntentKind::open_details, detail.id, true, false, true));
  components.push_back(std::move(detail));

  auto shared_view = component(
      "fixture.shared-view", "AzzsFixtureSharedView", "标准与高级共享状态",
      ComponentKind::detail, PresentationState::ready, "共享状态",
      "标准视图和高级视图显示同一状态、选择和主要命令。");
  shared_view.advanced_detail =
      "高级视图只增加消费者明确提供的低频来源修订与诊断说明。";
  shared_view.commands.push_back(command(
      "open-details", "查看详情", CommandRole::navigation,
      IntentKind::open_details, shared_view.id));
  shared_view.commands.push_back(command(
      "open-source", "查看低频来源", CommandRole::secondary,
      IntentKind::open_source, shared_view.id, true, false, true));
  components.push_back(std::move(shared_view));

  auto summary = component(
      "fixture.summary", "AzzsFixtureSummary", "只读摘要夹具",
      ComponentKind::summary, PresentationState::ready, "摘要",
      "成功 2 项，失败 1 项，跳过 1 项，等待重启 1 项。");
  summary.commands.push_back(command(
      "expand-summary", "展开逐项结果", CommandRole::secondary,
      IntentKind::expand_summary, summary.id));
  components.push_back(std::move(summary));

  constexpr struct {
    WorkflowStage stage;
    char const* id;
    char const* automation_id;
    char const* title;
  } stages[] = {
      {WorkflowStage::drivers, "fixture.stage.drivers",
       "AzzsFixtureStageDrivers", "驱动"},
      {WorkflowStage::system_optimization,
       "fixture.stage.system-optimization",
       "AzzsFixtureStageSystemOptimization", "系统优化"},
      {WorkflowStage::software_installation,
       "fixture.stage.software-installation",
       "AzzsFixtureStageSoftwareInstallation", "软件安装"},
      {WorkflowStage::software_optimization,
       "fixture.stage.software-optimization",
       "AzzsFixtureStageSoftwareOptimization", "软件优化"},
  };
  for (auto const& stage_fixture : stages) {
    auto stage = component(
        stage_fixture.id, stage_fixture.automation_id, stage_fixture.title,
        ComponentKind::stage_summary, PresentationState::neutral,
        stage_fixture.title, "阶段状态仅由消费者提供，本组件不推进流程。");
    stage.stage = stage_fixture.stage;
    components.push_back(std::move(stage));
  }

  auto local_trial = component(
      "fixture.local-trial", "AzzsFixtureLocalTrial", "本机试用目录状态",
      ComponentKind::status_band, PresentationState::local_trial,
      "正在使用本机试用目录",
      "它已通过本机加载检查，但尚未通过正式发布检查。软件列表和之后创建的任务将使用它；已有任务不受影响。");
  local_trial.risk = RiskLevel::medium;
  local_trial.announcement = AnnouncementMode::polite;
  components.push_back(std::move(local_trial));

  auto recovered = component(
      "fixture.recovered-unsaved", "AzzsFixtureRecoveredUnsaved",
      "恢复的未保存修改状态", ComponentKind::status_band,
      PresentationState::recovered_unsaved, "恢复的未保存修改",
      "异常退出后找回了未保存内容；它尚未保存、校验或应用，不覆盖已有草稿和当前目录。");
  recovered.announcement = AnnouncementMode::polite;
  components.push_back(std::move(recovered));

  auto saved = component(
      "fixture.saved-not-applied", "AzzsFixtureSavedNotApplied",
      "已保存未应用状态", ComponentKind::status_band,
      PresentationState::saved_not_applied, "已保存未应用",
      "草稿已经保存，但软件列表和当前目录保持不变；只有显式应用成功后才切换身份。");
  saved.announcement = AnnouncementMode::polite;
  components.push_back(std::move(saved));

  auto handoff = component(
      "fixture.source-handoff", "AzzsFixtureSourceHandoff", "来源交接状态",
      ComponentKind::status_band, PresentationState::source_handoff,
      "来源交接", "解析结果来自前一阶段；消费者仍需确认来源事实后才能创建业务批次。");
  handoff.announcement = AnnouncementMode::polite;
  components.push_back(std::move(handoff));

  auto waiting_network = component(
      "fixture.waiting-network", "AzzsFixtureWaitingNetwork", "等待联网状态",
      ComponentKind::waiting, PresentationState::waiting_for_network,
      "等待联网", "网络恢复后可以重新解析来源；当前没有下载、安装或系统修改正在执行。");
  waiting_network.announcement = AnnouncementMode::polite;
  components.push_back(std::move(waiting_network));

  auto error = component(
      "fixture.inline-error", "AzzsFixtureInlineError", "实现错误状态",
      ComponentKind::inline_error, PresentationState::failed, "实现错误",
      "目录声明支持此版本，但没有可执行呈现方案。请查看缺失字段并返回原位置修复。");
  error.risk = RiskLevel::high;
  error.announcement = AnnouncementMode::assertive;
  error.commands.push_back(command(
      "locate-error", "定位错误", CommandRole::navigation,
      IntentKind::locate_result, error.id));
  components.push_back(std::move(error));

  auto disabled = component(
      "fixture.disabled-reason", "AzzsFixtureDisabledReason", "禁用原因",
      ComponentKind::disabled_reason, PresentationState::disabled,
      "当前命令不可用",
      "等待联网：取得来源事实前不能创建下载或安装批次。");
  disabled.commands.push_back(command(
      "retry-disabled", "重新尝试", CommandRole::secondary,
      IntentKind::retry, disabled.id, false, false, false,
      "设备尚未联网，当前无法重新解析来源。"));
  components.push_back(std::move(disabled));

  auto risk = component(
      "fixture.risk-confirmation", "AzzsFixtureRiskConfirmation",
      "高风险确认", ComponentKind::risk_confirmation,
      PresentationState::pending_confirmation, "确认强制终止优化",
      "强制终止可能留下不完整配置。普通的安全停止保持默认焦点。");
  risk.risk = RiskLevel::critical;
  risk.commands.push_back(command(
      "stop-safely", "停止优化", CommandRole::primary,
      IntentKind::stop_safely, risk.id, true, true));
  risk.commands.push_back(command(
      "confirm-danger", "强制终止优化", CommandRole::danger,
      IntentKind::confirm_risk, risk.id));
  components.push_back(std::move(risk));

  auto determinate_progress = component(
      "fixture.determinate-progress", "AzzsFixtureDeterminateProgress",
      "确定下载进度", ComponentKind::progress,
      PresentationState::in_progress, "正在下载示例软件",
      "已下载 37 MB，共 100 MB；速度：12 MB/s；预计剩余 6 秒。");
  determinate_progress.announcement = AnnouncementMode::polite;
  determinate_progress.progress = ProgressProjection{
      .kind = ProgressKind::determinate,
      .completed = 37,
      .total = 100,
      .accessible_value = "已下载 37 MB，共 100 MB，完成 37%",
  };
  components.push_back(std::move(determinate_progress));

  auto unknown_progress = component(
      "fixture.unknown-progress", "AzzsFixtureUnknownProgress",
      "未知下载进度", ComponentKind::progress,
      PresentationState::in_progress, "正在下载示例软件",
      "大小：正在计算；速度：无法估算；剩余时间：无法估算。");
  unknown_progress.announcement = AnnouncementMode::polite;
  unknown_progress.progress = ProgressProjection{
      .kind = ProgressKind::unknown,
      .completed = std::nullopt,
      .total = std::nullopt,
      .accessible_value = "正在下载；总大小、速度和剩余时间正在计算",
  };
  components.push_back(std::move(unknown_progress));

  auto waiting = component(
      "fixture.waiting-restart", "AzzsFixtureWaitingRestart", "等待重启状态",
      ComponentKind::waiting, PresentationState::waiting_for_restart,
      "等待资源管理器重启",
      "修改已经写入；重启资源管理器后再验证结果。可以立即重启，也可以稍后处理。");
  waiting.announcement = AnnouncementMode::polite;
  components.push_back(std::move(waiting));

  auto failure = component(
      "fixture.failure", "AzzsFixtureFailure", "失败状态",
      ComponentKind::failure, PresentationState::failed, "来源解析失败",
      "网络可用，但多个候选安装包无法唯一确定架构。没有创建批次，也没有静默换源。");
  failure.risk = RiskLevel::medium;
  failure.announcement = AnnouncementMode::assertive;
  failure.commands.push_back(command(
      "retry-source", "重新尝试", CommandRole::primary,
      IntentKind::retry, failure.id, true, true));
  failure.commands.push_back(command(
      "open-source", "查看原始地址", CommandRole::secondary,
      IntentKind::open_source, failure.id));
  components.push_back(std::move(failure));

  auto pending = component(
      "fixture.pending-confirmation", "AzzsFixturePendingConfirmation",
      "待确认状态", ComponentKind::pending_confirmation,
      PresentationState::pending_confirmation, "优化结果待确认",
      "异常退出后无法确认最后一步结果。先重新检测，不直接重跑。");
  pending.announcement = AnnouncementMode::polite;
  pending.commands.push_back(command(
      "retry-detection", "重新检测", CommandRole::primary,
      IntentKind::retry, pending.id, true, true));
  components.push_back(std::move(pending));

  auto withdrawal = component(
      "fixture.emergency-withdrawal", "AzzsFixtureEmergencyWithdrawal",
      "紧急撤回状态", ComponentKind::emergency_withdrawal,
      PresentationState::withdrawn, "紧急撤回",
      "此方案已被维护者撤回。现有结果保持可见；新的执行入口已禁用。");
  withdrawal.risk = RiskLevel::critical;
  withdrawal.announcement = AnnouncementMode::assertive;
  withdrawal.commands.push_back(command(
      "open-withdrawal", "查看撤回说明", CommandRole::navigation,
      IntentKind::open_emergency_withdrawal, withdrawal.id));
  components.push_back(std::move(withdrawal));

  auto locator = component(
      "fixture.result-locator", "AzzsFixtureResultLocator", "结果定位",
      ComponentKind::result_locator, PresentationState::completed,
      "最近一次结果",
      "2026-08-11 10:24，搜狗输入法推荐优化：部分完成。");
  locator.commands.push_back(command(
      "locate-result", "在历史与日志中定位", CommandRole::navigation,
      IntentKind::locate_result, locator.id));
  components.push_back(std::move(locator));

  components.push_back(component(
      "fixture.settings-form", "AzzsFixtureSettingsForm", "设置页字段布局",
      ComponentKind::settings_form, PresentationState::ready, "设置字段",
      "标签、说明、当前值、就地错误和命令使用响应式 Auto 轨道。"));

  components.push_back(component(
      "fixture.catalog-editor", "AzzsFixtureCatalogEditor", "目录编辑器布局",
      ComponentKind::catalog_editor, PresentationState::ready,
      "密集目录编辑器",
      "分类、搜索、列表、详情和验证摘要共享焦点与错误定位合同；夹具不会保存或应用目录。"));

  return std::make_shared<PresentationSnapshot const>(std::move(components));
}

}  // namespace azzs::ui::presentation
