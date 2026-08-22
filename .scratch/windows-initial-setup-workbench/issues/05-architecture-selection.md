# 实现软件包架构选择规则

Type: task  
Status: ready-for-agent  
Resolution: completed
Blocked by: 02, 03
Owner: issue-05
Claimed by: Codex issue-05 task
Consumers: 06, 07, 12, 18, 21
Verification: x64、ARM64、检测失败、偏好回退和重启变化的架构决策表无界面测试。
Evidence freshness: 绑定当前提交；每次启动或恢复使用当次架构观察，旧观察不得复用。

## Goal

依据当前 Windows 架构和用户偏好，为软件选择兼容安装包并处理检测失败或架构变化。

## Acceptance Criteria

- [ ] 每次启动和重启恢复时重新检测 Windows 架构。
- [ ] x64 系统选择 x64 兼容包，仅有 ARM64 包的软件被禁用。
- [ ] ARM64 系统优先 ARM64 包，缺少时按当前偏好提示或回退 x64。
- [ ] 默认逐项确认回退；拒绝只跳过当前软件，重试时重新评估。
- [ ] 检测失败只暂停需要架构选择的软件，不阻断设置和架构无关项目。
- [ ] 架构变化不改写旧批次或已安装状态，并留下详细日志。

## References

`ARCH-01` 至 `ARCH-18`

## Comments

- 2026-08-12：Resolution completed。feature head `9ad64284c6bd2385a662dc018d47c1b9374a31ea` 经 PR #10（https://github.com/CoolPlume/azzs/pull/10）合入；该 feature 的 Windows read-only validation run `31569127992` 在 x64 Release 与 ARM64 Release 均成功。PR 已 Squash 到 `codex/v1-integration`，integration squash 为 `f9fbe320444ff4c525373f23638c72f456632900`；后续最终 integration run `31573791901` 在 `194b3a3948e3fdb77d52625013cd7258f04b80b0` 上的 x64/ARM64 均成功，host guardrails 为 15/15 通过。
- 未验证边界：上述是 GitHub 自动 CI 和 host guardrails 证据，不是 Windows 实机通过；未执行真实 Windows UI、UAC 或设备验证。未获完整自动证据的验收项保持未勾选。
