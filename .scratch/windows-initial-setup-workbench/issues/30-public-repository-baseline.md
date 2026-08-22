# 审计并规范公开 GitHub 源码仓库基线

Type: task
Status: ready-for-agent
Resolution: completed
Blocked by: 33
Owner: issue-30
Claimed by: Codex issue-30 task
Consumers: 01, 34, 35
Verification: 对现有公开历史和精确集成候选执行敏感信息、身份、历史、链接、许可及 Git 配置扫描，并在外部动作后核对仓库设置与公开分支。
Evidence freshness: 仅对已审计的公开 `main`、拟公开集成提交和当次 GitHub 配置有效；候选、历史或设置变化后重验。

## Goal

在不发布应用发行版的前提下，审计并规范已经公开的 `CoolPlume/azzs` 源码仓库，为早期只读 Windows CI 和后续事项建立可审计的公开集成基线。

## Acceptance Criteria

- [x] README 准确说明仓库标识 `azzs`、正式产品名称“Windows 初装工作台”、规格与设计阶段，以及暂无可运行程序或 Release。
- [x] CONTRIBUTING 说明中英文外部反馈、GitHub Issue 与 `.scratch/` 的职责边界及 MIT 贡献授权。
- [x] 项目自有内容使用 `Copyright (c) 2026 CoolPlume` 的标准 MIT License。
- [x] GitNexus 第三方生成文本、技能、本地索引和本机邮箱不出现在任何待公开提交中。
- [x] `.gitnexusrc` 保持纯索引模式，`AGENTS.md` 只保留项目自行维护的规则。
- [x] `.scratch/` 完整纳入公开版本，并继续作为规格和实施状态的正式事项源。
- [x] 接受已经公开的三提交历史；全部作者与提交者使用 GitHub noreply 身份，基础扫描及完整候选审计没有发现需要撤回的秘密、身份或法律禁入内容。不得仅为单一根提交外观解除保护或强制改写已经公开的对象；若完整审计发现真实泄漏，必须另行升级处理。
- [x] 按 `docs/agents/code-intelligence.md` 完成提交前门禁，并对现有公开历史和 `codex/v1-integration` 精确候选完成敏感信息、提交身份、个人路径、链接、许可和公开文件范围审计。
- [x] GitHub 仓库启用 Issues 和五个 triage 标签，允许空白 Issue，并保持 Discussions、Projects、Wiki 和 Pages 关闭；启用 Actions 及仓库默认只读工作流权限，但在事项 01 交付前不加入构建或发布工作流。
- [x] 仓库描述使用“Windows 初装工作台：帮助新装 Windows 完成驱动准备、系统优化、常用软件安装和软件优化。”，并完成 topics 设置；`main` 禁止删除和 force push，暂不要求 PR、审批或状态检查。
- [x] 首个公开 Release 后只声明支持最新正式稳定版；测试发行和更早正式稳定发行不在公开支持范围。
- [x] 公开仓库启用 GitHub 私密漏洞报告，并在 `SECURITY.md` 中明确敏感问题不得提交到公开 Issue。
- [x] 审计通过后由协调会话公开 `codex/v1-integration`，不直接合并 `main`；该动作及本事项的仓库设置修正不创建 tag、GitHub Release 或发行制品，也不把正式产品标识误写成已经发布应用。

## References

- `REL-01` 至 `REL-07`
- `REL-11`、`REL-13`
- `docs/adr/0002-github-distribution.md`
- `docs/adr/0045-separate-read-only-ci-from-release-permissions.md`
- `docs/agents/code-intelligence.md`
- `docs/agents/issue-tracker.md`
- `docs/agents/triage-labels.md`

## Comments

- 2026-08-09：维护者通过 `grill-with-docs` 确认公开仓库、许可、贡献、历史、认证和最终推送门禁；另一 Codex 会话完成后开始本地公开基线整理。
- 2026-08-10：只读核对确认 `CoolPlume/azzs` 已公开，`main` 有三个 GitHub noreply 提交并受禁止删除和非快进更新规则保护，Actions 关闭且没有工作流。维护者接受现有历史，不以强推追求单一根提交，并授权本事项审计后规范可逆仓库设置、启用默认只读 Actions 和公开集成分支；tag、Release、应用制品和 `main` 合并仍不在本事项授权范围内。
- 2026-08-10：事项 30 本地审计从精确候选 `9f3429e9f00e1a3a397f879c46f7be35e064779e` 重验 `main` 三提交历史、候选新增提交、公开文件范围、敏感信息、个人身份、路径、链接、许可和 Git 配置。`main` 的 `97a7b33`、`bb6fbe4`、`960cd7d` 与候选 `9f3429e` 的作者和提交者均为 `CoolPlume <75199132+CoolPlume@users.noreply.github.com>`；候选无凭据、私钥、个人绝对路径、个人邮箱、二进制、子模块或符号链接，`.scratch/` 完整受 Git 跟踪，GitNexus 索引只在本机生成且未进入候选。仅发现一条历史资料中的外部 HTTP 用户协议链接（`installpack-candidate-review.md:222`），当前网络无法连通 HTTP/HTTPS 版本，未据此擅改历史证据；另有本地 `refs/archive/pre-public-history-20260809` 指向旧个人邮箱提交，不在 `main` 或候选可达对象中，不得随通配 ref 推送。
- 2026-08-10：通过 `gh api` 只读核对 `CoolPlume/azzs`：仓库公开且默认分支为 `main`；Issues 已启用，五个 canonical triage 标签齐全，空白 Issue 已启用，topics 已验收为 `cpp`、`windows`、`windows-setup`、`winui3`，Discussions、Projects、Wiki 和 Pages 均关闭；活动规则集 `Protect main history` 仅包含 `deletion` 与 `non_fast_forward`，未要求 PR、审批或状态检查；Actions 当前关闭但默认工作流权限为 `read`，没有工作流或 Release；私密漏洞报告当前关闭。仓库描述仍不是验收要求的精确文案，需协调会话按事项授权修改为规格中的句子，启用 Actions 与私密漏洞报告，并在审计通过后公开 `codex/v1-integration`；本分支不执行上述外部写操作。
- 2026-08-10：协调会话完成授权动作并复核：仓库描述已使用验收文案，Actions 已启用且默认工作流权限保持 `read`、不能批准 Pull Request，私密漏洞报告已启用，`codex/v1-integration` 已按显式 refspec 公开；`main`、tag、Release 和应用制品均未改动。

## Answer

事项 30 由 Codex 任务 `019feb6f-ff81-7901-a96c-4c760938dd85` 在分支 `codex/issue-30-public-repository-baseline` 实施，原始提交为 `2b40a84492eda1088114eabff3954581b294bbcb`；协调会话独立复核后以单个 squash 合入，未创建 PR。

仓库内新增 `SECURITY.md` 与 `SUPPORT.md`，并补齐 README、CONTRIBUTING 和 ADR-0045 后的发布研究口径。公开历史、候选文件、提交身份、个人路径、敏感信息、链接和 Git 配置检查通过；GitHub 的描述、topics、Issues/关闭项、`main` 历史保护、默认只读 Actions、私密漏洞报告和公开集成分支均已核对。没有合并 `main`，也没有创建工作流、tag、Release 或应用制品。事项 01 因本事项完成而解除阻塞。
