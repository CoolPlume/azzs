#pragma once

#include <memory>
#include <string>

#include "azzs/application/guided_initialization.hpp"
#include "presentation_contract.hpp"

namespace azzs::ui::presentation {

struct GuidedInitializationPresentationText final {
  std::string summary_accessible_name{"推荐初始化摘要"};
  std::string summary_title{"推荐初始化"};
  std::string summary_prefix{"已完成："};
  std::string summary_external_prefix{"；已识别外部安装："};
  std::string summary_partial_prefix{"；部分完成："};
  std::string summary_failed_prefix{"；失败："};
  std::string summary_skipped_prefix{"；已跳过："};
  std::string summary_no_applicable_prefix{"；无适用项目："};
  std::string summary_not_executed_prefix{"；未执行："};
  std::string summary_confirmation_prefix{"；等待确认结果："};
  std::string summary_explorer_restart_prefix{
      "；等待资源管理器重启："};
  std::string summary_restart_prefix{"；等待 Windows 重启："};
  std::string summary_withdrawn_prefix{"；已紧急撤回："};
  std::string summary_error_suffix{"。请先查看当前阶段，再继续操作。"};
  std::string start_command{"开始推荐初始化"};
  std::string refresh_command{"刷新状态"};
  std::string cancel_command{"取消流程"};
  std::string history_command{"查看历史与日志"};
  std::string skip_command{"暂时跳过此阶段"};
  std::string continue_command{"继续"};
  std::string retry_command{"从专属页面重试"};
  std::string open_command{"打开专属页面"};
  std::string local_trial_accessible_name{"软件目录身份"};
  std::string local_trial_title{"正在使用本机试用目录"};
  std::string local_trial_body{
      "此目录已通过本机加载检查，但尚未通过正式发布检查。之后创建的任务会使用它。"};
  std::string handoff_accessible_name{"外部安装交接"};
  std::string handoff_title{"外部安装交接"};
  std::string handoff_waiting_body{
      "正在等待外部安装完成，之后还需要重新检查。"};
  std::string handoff_recognized_body{
      "已识别外部安装。请核对结果后再继续。"};
  std::string handoff_continue_command{"继续此交接"};
  std::string read_only_accessible_name{"推荐初始化恢复"};
  std::string read_only_title{"推荐初始化处于只读状态"};
  std::string read_only_body{
      "流程记录的持久化状态需要先修复，当前不能安全修改。"};
  std::string read_only_disabled_reason{"流程状态为只读"};
  std::string stage_empty_body{"当前流程尚未记录此阶段。"};
  std::string raw_detail_prefix{"原始系统信息："};
  std::string raw_error_prefix{"原始错误信息："};
  std::string drivers_stage_title{"驱动"};
  std::string system_optimization_stage_title{"系统优化"};
  std::string software_installation_stage_title{"软件安装"};
  std::string software_optimization_stage_title{"软件优化"};
  std::string unknown_stage_title{"未知阶段"};
  std::string stage_pending_body{"等待执行"};
  std::string stage_active_body{"正在执行"};
  std::string stage_completed_body{"已完成"};
  std::string stage_skipped_body{"已跳过"};
  std::string stage_no_applicable_body{"没有适用项目"};
  std::string stage_partial_body{"部分完成"};
  std::string stage_failed_body{"执行失败"};
  std::string stage_confirmation_body{"等待确认结果"};
  std::string stage_waiting_explorer_body{"等待资源管理器重启"};
  std::string stage_waiting_restart_body{"等待 Windows 重启"};
  std::string stage_withdrawn_body{"已紧急撤回"};
  std::string stage_external_handoff_body{"等待外部安装交接"};
  std::string stage_not_executed_body{"未执行"};
};

[[nodiscard]] std::shared_ptr<PresentationSnapshot const>
make_guided_initialization_presentation(
    application::guided_initialization::Snapshot const& source,
    GuidedInitializationPresentationText text = {});

}  // namespace azzs::ui::presentation
