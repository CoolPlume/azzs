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
