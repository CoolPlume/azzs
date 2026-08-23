# 事项 06：x64 集成验收与 0.2.0 放行门槛

Type: task
Status: needs-triage
Resolution: open
Owner: issue-06
Consumers: none
Blocked by: 01, 02, 03, 04, 05
Verification: 在独立 `D:\azzs-codex\worktrees` 完成 x64 构建、受影响合同、Windows UI、更新查询、硬件复制和 QQ/QQ 音乐真实安装验收，并按提交/环境/来源写入证据。
Evidence freshness: 绑定最终集成 SHA、构建命令和产物路径、Windows/SDK、GitHub 查询时间、硬件设备、QQ/QQ 音乐安装器版本及用户实机验收；任何实现、目录、来源或环境变化后重跑。

## 问题

0.2.0 同时跨越 WinUI、应用核心、Windows 观测、网络查询、软件目录和安装器执行。静态合同或 CI 通过不能证明侧栏可拖、更新可查或指定软件真实可安装。

## 目标

- 汇总事项 01-05 的精确提交、定向测试、x64 构建和真实 Windows 证据。
- 单独记录 ARM64、安装生命周期、DPI/无障碍和来源授权等未执行边界。
- 由维护者亲自验收 QQ 与 QQ 音乐；任一项不稳定则从 0.2.0“可用安装”承诺中延期，不伪报完成。
- 未经维护者另行授权，不创建 tag/Release、不接受 WiX 条款、不合入 `main`。

## 验收

- 具备 `D:\azzs-codex` 下的独立 x64 `.exe`，报告构建命令和实际验证结果。
- 事项状态与 `Resolution` 分开维护；每个结论区分静态合同、自动化、Windows UI、实机安装和 GitHub 状态。

## Comments

- 本事项是版本放行门槛，不应在前置事项未完成时提前宣称 0.2.0 可用。
