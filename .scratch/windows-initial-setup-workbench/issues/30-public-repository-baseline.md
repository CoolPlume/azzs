# 建立公开 GitHub 源码仓库基线

Type: task
Status: ready-for-agent

## Goal

在不发布应用发行版的前提下，为项目建立可审计的公开 GitHub 源码仓库基线，并在维护者最终授权后把单一根提交推送到 `CoolPlume/azzs`。

## Acceptance Criteria

- [ ] README 准确说明仓库工作名、规格与设计阶段、正式产品名待定，以及暂无可运行程序或 Release。
- [ ] CONTRIBUTING 说明中英文外部反馈、GitHub Issue 与 `.scratch/` 的职责边界及 MIT 贡献授权。
- [ ] 项目自有内容使用 `Copyright (c) 2026 CoolPlume` 的标准 MIT License。
- [ ] GitNexus 第三方生成文本、技能、本地索引和本机邮箱不出现在任何待公开提交中。
- [ ] `.gitnexusrc` 保持纯索引模式，`AGENTS.md` 只保留项目自行维护的规则。
- [ ] `.scratch/` 完整纳入公开版本，并继续作为规格和实施状态的正式事项源。
- [ ] 所有尚未公开的内容合并为一个新根提交，作者与提交者均使用 GitHub noreply 身份；旧历史只保存在本机备份引用中。
- [ ] 按 `docs/agents/code-intelligence.md` 完成提交前门禁，并完成敏感信息、提交身份和公开文件范围审计。
- [ ] 只有维护者明确回复“可以推送”后，才创建空的公共仓库并只推送 `main`。
- [ ] GitHub 仓库启用 Issues 和五个 triage 标签，允许空白 Issue，并保持 Discussions、Projects、Wiki、Pages 和 Actions 关闭。
- [ ] 仓库描述与 topics 设置完成；`main` 禁止删除和 force push，暂不要求 PR、审批或状态检查。
- [ ] 首次源码推送不创建 tag、GitHub Release 或发行制品，也不改变事项 23 的正式产品命名工作。

## References

- `REL-01` 至 `REL-07`
- `docs/adr/0002-github-distribution.md`
- `docs/agents/code-intelligence.md`
- `docs/agents/issue-tracker.md`
- `docs/agents/triage-labels.md`

## Comments

- 2026-08-09：维护者通过 `grill-with-docs` 确认公开仓库、许可、贡献、历史、认证和最终推送门禁；另一 Codex 会话完成后开始本地公开基线整理。
