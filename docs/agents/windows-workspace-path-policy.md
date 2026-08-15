# Windows 工作区与 Git worktree 路径策略

本策略适用于 Windows 11 原生 Codex 会话，以及本仓库在 Windows 上的并行开发。

## 1. 路径结论

- `D:\azzs` 是主工作树和调度入口。它用于读取基线、查看状态和执行协调，不作为并行执行会话的共享写入目录。
- 新建执行 worktree 默认放在 `D:\azzs-codex\worktrees\<feature-slug>`。`D:\azzs-codex` 是 worktree 容器，不是第二个独立仓库。
- Windows Git 支持把主工作树和 linked worktree 放在不同盘符。`D:\azzs`、`D:\azzs-codex\worktrees\...` 与 `C:\...` 在 Git 语义上没有优先级差异；跨盘符不会改变分支、对象库或提交关系。
- 同盘的 `D:` 路径是默认选择，因为它缩短路径、减少构建和临时文件的路径差异，并避免把日常工作树埋在较长的用户目录下。`C:` worktree 只要路径稳定、权限正常且没有未记录的环境差异，也可以继续使用。
- 不把 worktree 放在网络共享、临时盘、同步盘、映射盘或会自动改变路径的目录中。不要用 junction、subst 或符号链接伪造仓库根路径来绕过本策略。
- Windows 路径在命令和交接记录中使用可解析的绝对路径。脚本可接受 `/`，但记录时统一使用 `D:\...` 或 `C:\...`，避免同一 worktree 出现多个拼写。

Git linked worktree 共享同一个 Git 对象库和引用数据库；每个 worktree 有自己的 `HEAD`、索引和工作文件。`.git` 文件中的链接指向共同 Git 元数据，因此移动或删除目录不能按普通文件夹操作。需要迁移时使用 `git worktree move`，确认废弃后才使用 `git worktree remove`；`git worktree prune` 只用于已确认失联的元数据。

## 2. 创建与接管

总调度先在 `D:\azzs` 执行一次：

```powershell
git fetch origin --prune
git status --short --branch
git rev-parse origin/codex/v1-integration
```

主工作树必须没有需要保护的未提交或未跟踪内容。然后从精确的远端 integration 基线创建独立 worktree：

```powershell
git worktree add -b codex/<feature-slug> D:\azzs-codex\worktrees\<feature-slug> origin/codex/v1-integration
git -C D:\azzs-codex\worktrees\<feature-slug> status --short --branch
git -C D:\azzs-codex\worktrees\<feature-slug> rev-parse HEAD
```

`<feature-slug>` 应同时用于目录和分支的可识别部分，例如目录 `workspace-path-policy` 对应分支 `codex/chore-workspace-path-policy`。一个分支只能绑定一个 worktree；一个执行会话不能与另一个会话共用工作树或同时修改同一组文件。

接管已有工作时，先检查 `git worktree list --porcelain`、目标 worktree 的 `status`、分支 HEAD、远端分支和 PR head，再决定恢复、继续或新建 worktree。不要因为路径在 `C:` 或 `D:` 就重新实现或覆盖已有成果。

## 3. 编辑、验证与证据

- 文档和代码改动都使用 `apply_patch`；不要用重定向或脚本覆盖文件。
- 改动后先运行受影响的最小检查。文档改动至少运行 `git diff --check`，并用 `rg` 检查路径、分支和命令示例没有漂移。
- 提交前核对 `git diff --cached --name-status`、`git diff --cached --check`、`git diff --check` 和未跟踪文件清单；提交和 PR 记录精确的 feature head SHA。
- 仅文档改动不要求重新运行 x64/ARM64 构建。若 CI 顺带运行 ARM64，将其作为信息记录；ARM64 专属结果不阻塞当前 x64 集成，也不能写成 ARM64 验收通过。
- 构建、测试或 UI 验收产生的证据必须绑定实际路径、提交 SHA、运行环境和命令。不能把另一盘符、另一 worktree、旧 SHA 或虚拟机结果冒充当前 Windows 原生证据。

## 4. GitHub 交付边界

1. 一个内聚工作单元完成并通过最小门禁后提交，例如：

   ```powershell
   git commit -m "docs: document Windows worktree path policy"
   git push -u origin codex/<feature-slug>
   ```

2. 先创建以 `codex/v1-integration` 为 base 的 Draft PR。确认 head、变更范围、必要 CI 和风险复核准确后，再将 PR 转为 Ready。
3. 多功能集成时，在 feature worktree 中使用普通 `git merge origin/codex/v1-integration`，解决冲突并验证新的 feature head；禁止 rebase、force push 和历史改写。
4. 默认只把 PR 普通合入 `codex/v1-integration`。未经维护者明确授权，不合入 `main`，不创建 tag 或 GitHub Release，不上传发行制品，也不接受 WiX 条款。
5. 已推送提交不使用 `--amend`；修复用新的提交保持远端 SHA 和 PR 证据可追溯。完成后报告 feature head SHA、PR/CI 状态和 integration merge SHA。

worktree 的盘符不会改变这些边界：Git 操作的安全性由分支、精确 SHA、工作树隔离和证据链决定，而不是由 `C:` 或 `D:` 决定。
