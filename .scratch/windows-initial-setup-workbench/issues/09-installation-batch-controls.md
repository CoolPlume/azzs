# 实现批次停止、强制终止与重试

Type: task  
Status: ready-for-agent  
Resolution: completed
Blocked by: 08
Owner: issue-09
Claimed by: codex/issue-09-evidence
Consumers: 21, 27
Verification: 停止、暂停、继续、强制终止、关闭重开和单项重试的状态机与进程适配器测试。
Evidence freshness: 绑定当前提交和安装器适配器版本；Windows 进程证据绑定实际候选构建。

## Goal

提供含义明确的正常停止和高风险强制终止，并保留已完成结果与重试能力。

## Acceptance Criteria

- [x] 正常停止允许当前安装程序结束，并阻止后续项目启动。
- [x] 下载阶段停止会取消当前下载、删除临时文件并停止后续项目。
- [x] 下载阶段可暂停和继续当前项目，暂停时保留进度且不与停止批次混淆；无法续传时明确提示重新下载。
- [x] 强制终止按钮明确说明半安装状态风险并要求确认。
- [x] 两种停止方式都不回滚已完成安装。
- [x] 关闭工作台不再启动后续项目，也不被误当作强制终止已经由工作台作为安装批次项目启动的官方安装程序。
- [x] 关闭工作台会停止未完成或已暂停的应用内下载并清理临时文件，重新打开后等待用户明确重新开始。
- [x] 重新打开后先验证上述批次安装程序的结果，再允许用户继续或重试；来源手动交接恢复后不自动检测，仍等待用户主动选择“重新检测”。
- [x] 批次保留逐项结果，失败或未完成项目可单独重试。
- [x] 停止、强制终止和用户确认均写入执行日志。
- [x] 安装批次活动期间不能开始优化批次，并提供返回当前安装进度的入口。

## References

`SW-18` 至 `SW-20`、`SW-30` 至 `SW-32`、`CACHE-17` 至 `CACHE-20`、`RUN-01` 至 `RUN-05`、`RUN-07`、`UI-29`、`LOG-03`、`OPT-52`

## Answer

- 功能提交为 `991280cd517f36c3f3655e3e69e64f0df6cdfec3` 和 `dcadb59327faf0e53c25a6aa6f922e3faae94934`；PR [#36](https://github.com/CoolPlume/azzs/pull/36) 已以普通合并提交 `48650bbf8f98bccc51584ecca4358371a8c7072f` 进入 `codex/v1-integration`。本票据的纯文档证据提交不重复运行已绑定到功能提交的合同、构建或 CI。
- Windows 11 x64 本机 `installation-batch-runner.contract` 为 1/1 通过；`.\eng\build.ps1 -Architecture x64 -SkipCoreSmoke` 通过。GitHub Actions Windows read-only validation run `31778453030` 的 x64 Release 成功。
- 本次未重跑 host guardrails；事项 08 已记录的 host ACL/reparse 环境边界不因本事项改变。未执行 WiX、真实安装器进程终止、真实下载清理、跨重启恢复或 WinUI 实机交互验收。
- ARM64 未在本机执行，仍延期且不作为 x64 完成声明；自动 workflow 的 ARM64 Release 成功不能伪称为 ARM64 硬件、交互或安装生命周期验收。

## Comments

- 2026-08-14：事项 09 已通过 PR #36 的普通 merge 进入 integration；本票据据此结票。x64 合同、构建和 CI 证据绑定到功能分支 head，真实安装器、UI 与 ARM64 边界保持未执行或延期，不外推为通过。
