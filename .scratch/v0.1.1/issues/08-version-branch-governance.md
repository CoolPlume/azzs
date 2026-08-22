# 0.1.1 版本分支治理与记录

Type: task
Status: ready-for-agent
Resolution: open
Blocked by: none
Owner: issue-08
Claimed by: none
Consumers: none
Verification: 从远端读取分支清单、保护设置和精确提交关系，证明存在且仅存在长期版本分支 `0.1.1`，历史分支未删除，并在事项证据中记录日期、SHA 和 GitHub URL。
Evidence freshness: 绑定 GitHub 分支清单、保护设置、集成提交和检查日期；任何分支创建、删除、改名、保护或发布流程变化后重查。

## Goal

建立可长期追溯、一个版本一个分支的 GitHub 记录方式，并为 0.1.1 提供唯一裸版本号分支。

## Ownership Boundary

本事项拥有版本分支命名、创建、保护、历史保留和交接记录；不改变功能实现或发布授权。集成目标、PR、CI 和发布门禁仍遵守 `docs/agents/orchestration.md` 与现有 ADR。

## Acceptance Criteria

- [ ] 创建并推送长期分支 `0.1.1`，其起点和首个文档提交可追溯到 `codex/v1-integration`。
- [ ] GitHub 不存在 `v0.1.1`、`codex/v0.1.1` 或其他同版本长期别名；后续版本按同一规则使用 `0.2.0` 等裸名称。
- [ ] 既有 `v0.1.0`、`codex/v0.1.0-*` 等历史分支不删除、不重命名、不追溯整理。
- [ ] 版本分支保护普通合并和可恢复记录，禁止 rebase、force push、历史改写；未经维护者授权不创建 tag/Release 或合入 `main`。
- [ ] 执行会话的临时 worktree/事项分支在交付后有明确去留，不得产生第二条同版本长期分支；事项中记录最终 SHA、合并关系和未验证边界。

## References

`V011-G06`、`V011-BRANCH-01` 至 `V011-BRANCH-06`、ADR-0051、`docs/agents/orchestration.md`、`docs/agents/windows-native-takeover.md`

## Comments

- 2026-08-23 07:14:10 +08:00（再次实时 GitHub 核对，supersedes earlier candidate facts）：远端 `0.1.1` 指向 `180cb20ee60ee58915b917a6b91b4781aaba570b`，远端 `codex/v1-integration` 指向 `d8a4367adc1e322351126e8ab3676d30584475d5`；merge-base 为 `180cb20ee60ee58915b917a6b91b4781aaba570b`，集成分支相对版本分支 ahead 42、behind 0。GitHub 分支清单只发现裸 `0.1.1`，未发现 `v0.1.1`、`codex/v0.1.1` 或同版本长期别名。
- 使用已认证 `gh api repos/CoolPlume/azzs/branches/0.1.1/protection --include` 得到 HTTP 404 `Branch not protected`；保护规则未配置/未确认，不能写成已保护。未创建 tag/Release、未合入 `main`、未接受 WiX 条款；关联 PR/CI 未在本次核对中虚构。
- `Resolution: open` 保持不变。当前仅有静态合同和 Git 关系证据；ARM64、DPI、安装生命周期、实机 UI/无障碍以及后续保护/CI 放行仍未验证。

- 2026-08-23 06:58:09（Asia/Shanghai，实时 Git/GitHub 核对）：远端分支 [`0.1.1`](https://github.com/CoolPlume/azzs/tree/0.1.1) 指向 [`180cb20ee60ee58915b917a6b91b4781aaba570b`](https://github.com/CoolPlume/azzs/commit/180cb20ee60ee58915b917a6b91b4781aaba570b)，远端集成分支 [`codex/v1-integration`](https://github.com/CoolPlume/azzs/tree/codex/v1-integration) 指向 [`ccca70b6562b52d6ac8d9a77545decd886bd2733`](https://github.com/CoolPlume/azzs/commit/ccca70b6562b52d6ac8d9a77545decd886bd2733)。Git 复核确认集成分支相对 `0.1.1` 为 ahead 37、behind 0，merge-base 为 `180cb20ee60ee58915b917a6b91b4781aaba570b`（[compare](https://github.com/CoolPlume/azzs/compare/0.1.1...codex/v1-integration)）；此前记录的 `ec80d9e`、`6b0e8e25...` 和“少 19 个祖先提交”均已过时，不再作为当前事实。
- 同一时刻的远端分支清单仅发现一个裸 `0.1.1` 长期版本分支；未发现 `v0.1.1`、`codex/v0.1.1` 或其他同版本长期别名。`0.1.1` 的分支保护查询返回 HTTP 404 `Branch not protected`（[protection API](https://api.github.com/repos/CoolPlume/azzs/branches/0.1.1/protection)），所以保护规则未确认/未配置，不能写成已保护；`main` 的保护状态与本事项无关。
- GitHub 对 `codex/v1-integration`、`0.1.1` 及其当前头提交的关联 PR 查询未返回结果，故本次核对不虚构 PR 号或合并链接；分支、提交和 compare 链接如上。当前远端分支清单可确认保留 `codex/v0.1.0-controlled-acquisition`，但未列出裸 `v0.1.0`；本轮未删除或改名任何历史分支。未创建 tag/Release、未合入 `main`、未接受 WiX 条款。
- `Resolution: open` 保持不变。当前集成树的 x64 CMake/WinUI Release 构建与 `ui.presentation.contract`、`product.identity.contract` 已通过；仍未验证版本分支保护实际规则、后续 PR/CI 放行、ARM64、DPI、安装生命周期及实机 UI/无障碍。本条评论只记录可复核的远端事实，不把分支存在或静态文档核对写成事项完成。
