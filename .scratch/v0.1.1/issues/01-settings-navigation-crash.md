# 修复应用设置导航闪退

Type: task
Status: ready-for-agent
Resolution: open
Blocked by: none
Owner: issue-01
Claimed by: none
Consumers: 07
Verification: 使用可注入快照/绑定失败夹具和 Windows x64 UI 自动化点击“应用设置”，证明进程仍存活、当前页面保留、中文错误可见且重试/返回语义可用；正常快照路径和重复导航均通过。
Evidence freshness: 绑定设置服务、页面绑定、组合根或资源版本的精确提交、构建目录和 Windows x64 环境；任一异常边界或导航装配变化后重跑。

## Goal

点击“应用设置”不得导致应用闪退；发生可恢复异常时用户仍有可理解的恢复路径。

## Ownership Boundary

本事项拥有导航事件到设置页呈现边界的异常转换、页面错误状态和重试/返回命令接缝。`ApplicationSettingsService` 仍拥有设置快照、持久化和业务错误；本事项不得在 UI 中读写设置文件、注册表或创建第二份默认设置。

## Acceptance Criteria

- [ ] 正常快照、缺失可选值、持久化读取失败和资源/绑定异常都不会让进程退出。
- [ ] 异常被转换为类型化或稳定可识别的 UI 状态，不以空对象、未初始化 optional 或静默默认值继续渲染。
- [ ] 失败时保留当前页面与当前核心状态，不自动覆盖设置，不伪报“应用设置”已打开。
- [ ] 提供中文错误标题/正文及“重试”和“返回当前页面（或继续当前页面）”入口；重试不会递归创建页面或叠加订阅。
- [ ] 进程存活、窗口可继续导航、键盘焦点和屏幕阅读器播报不被异常路径破坏。
- [ ] 既有启动装配失败边界保持不变；不可恢复的启动错误不被伪装成设置页恢复成功。

## References

`V011-G01`、`V011-SET-01` 至 `V011-SET-06`、ADR-0048、事项 18、`docs/engineering/architecture-and-code-quality.md`、`docs/design/winui3-apple-inspired.md`

## Comments

- 2026-08-23（集成证据核对）：实现提交 `defd9a1bb8ffd898a38eef6c2f507147b32b384d` 已通过普通合并 `d2a5ed3aee676efc54b23bd4c9f7f670daa7757c` 进入当前本地 `codex/v011-integration`，当前候选头为 `80713109b1cd71be19689f0c4fbbc4775d413797`。改动包含设置导航事务/快照、绑定和持久化异常接缝及 UI 合同夹具；尚未据此结票。
- 已知静态验证：`python tests/ui-design-contract/check_winui_async_contract.py .` 在设置链审查中通过；仍需在当前候选头重新执行受影响合同和 Windows 11 x64 UI 自动化，确认单一恢复责任、进程存活、当前页/焦点保留、中文错误以及重试/返回行为。未做真实点击验收。
- 未验证边界：ARM64、DPI/窄窗、多触摸、Narrator/屏幕阅读器、真实异常快照/绑定/持久化故障和安装生命周期均未验证；不得以静态合同或其他主机结果替代。
