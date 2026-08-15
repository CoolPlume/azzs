# Windows 原生 Codex 总调度接管

状态日期：2026-08-13  
主执行环境：Windows 11 25H2 原生机器  
集成分支：`codex/v1-integration`

## 1. 接管目标

从本次接管起，Windows 原生 Codex 负责持续推进项目到可用 x64 成品。macOS 与虚拟机不再承担日常开发、构建或实机验证，也不再安排两端反复交接。

当前里程碑按事项结票为 `20/35`（57%）。统计只表示事项生命周期，不表示全部实机风险已经消除。

## 2. 当前架构决策

- 当前只投入 x64。新代码、Windows 构建、合同测试、实机验证和候选制品优先完成 x64。
- 保留已有 ARM64 代码、目录语义、构建配置和自动工作流；不删除、不关闭、不主动扩展。
- 自动 CI 顺带运行 ARM64 时无需阻止，但 ARM64 结果不再是当前 x64 集成门禁。ARM64 专属失败记录为延期，不重跑、不修复；若同一问题影响 x64，则修共享根因。
- 所有证据准确写“ARM64 未执行/延期”。八制品等长期发布标准仍保留，不能把 x64 结果写成 ARM64 或八制品通过。
- x64 功能和发行候选全部完成后，再统一恢复 ARM64 工作，不在每个事项中零散投入。

## 3. GitHub 权威状态

虚拟机回传的完整脱敏记录见 `docs/agents/windows-vm-return-2026-08-13.md`；本节记录回传后已经核对和集成的最新状态。

- PR #31 已合入 `codex/v1-integration`：merge SHA `1e2ffbf43828ae73507068755beac8f2fb9cac32`。
- PR #31 最终 feature SHA `97bf8a7193fab77f00e8ef5eaab3661fe23fa64d`；GitHub run `31663573410` 的 x64 和 ARM64 job 均成功。不要重跑该 SHA。
- PR #26 仍为 Draft，feature branch `codex/issue-07-offline-package-cache`，head `9bc4432cd7e418b5379dd24cf7c0a1dc1a0a75d4`，与最新 integration 有冲突。
- 事项 07 的真实 Windows x64 focused 合同已在该 head 前的同一修复上通过：缺失 `.partial` 的 `symlink_status` 返回 system category error value 2，原代码错误地与 generic `std::errc` 比较；修复改用 error-condition 比较。首个 writer 为 `code=0`，第二个为 `code=1`，`local-package-cache-adapter.contract` 通过。
- PR #26 的最终 head 尚无可信集成 CI；必须在普通 merge 最新 integration 后重新验证 x64，不能直接把旧 focused 结果外推到冲突解决后的 SHA。

## 4. 无效或延期的 Windows 观察

- 虚拟机中的事项 20 没有建立应用前状态、真实 UAC、Explorer 重启、应用后状态和受控恢复的完整证据链；没有提交。不要复用临时 UI 自动化脚本或 VM 笔记。
- 虚拟机中的事项 29 没有得到有效签名的官方搜狗 16.7 安装器，也没有完成安装、25 项 UIA 身份冻结或前后状态验证；没有提交。适配器继续 fail-closed 为 `pending_confirmation`，不得放宽签名或身份规则。
- 上述两项暂不重做。等 x64 主功能接近发行候选时，将确实影响发布的 Windows 人工场景合并为一次验收批次；此前优先实现产品。

## 5. 第一个执行单元：收口事项 07

1. 从最新 `origin/codex/v1-integration` 和 `origin/codex/issue-07-offline-package-cache` 创建独立 worktree；确认 PR #26 head 为 `9bc4432...`。
2. 在 feature 分支执行普通 `git merge origin/codex/v1-integration`，解决冲突。保留 integration 中 PR #31 的高级视图偏好、锁定 C++/WinRT 投影和现有组合根；保留事项 07 的缓存能力及 Windows absent-path 修复。禁止 rebase、force push 或历史改写。
3. 按 `docs/agents/code-intelligence.md` 检查受影响符号和暂存范围。只运行合并后会改变结论的 x64 定向合同、x64 构建和必要一次 host guardrails；不主动运行 ARM64。
4. x64 通过后提交冲突解决（若产生工作树修改）、推送 feature 分支，更新 PR #26。若现有 GitHub 工作流自动运行 ARM64，只把它当信息，不等待 ARM64 专属修复。
5. 将 PR #26 转 Ready 并普通合入 `codex/v1-integration`。随后用独立证据分支准确更新事项 07 的验收项、Windows x64 证据、未验证边界和 `Resolution: completed`，再通过 PR 合入 integration。
6. 事项 07 结票后，总进度应为 `21/35`（60%），立即报告一次总百分比并启动事项 08。

## 6. 后续关键路径

按机械依赖推进，不提前做发布事项：

1. 事项 08：串行软件安装批次。
2. 事项 09 与事项 26：在 08 后可并行；写不同 worktree，避免共享状态冲突。
3. 事项 27：等待 09 与 26。
4. 事项 12：等待 08、27 及既有前置。
5. 事项 14、15、18：各自前置满足后并行；人工 UI 只保留发布决策所必需部分。
6. 事项 16、32：在其全部前置完成后推进。
7. 事项 21、22、34、35：最后处理。达到发行阶段时先把 x64 临时里程碑与原八制品长期标准明确分开，未经维护者明确授权不合入 `main`、不创建 tag 或 Release、不接受 WiX 条款。

每完成一个事项，重新机械扫描 `Blocked by`，只启动 `Resolution: open`、前置全为 `completed` 的事项。每完成一个大部分向维护者报告总完成百分比。

## 7. 总调度运行方式

- 总调度使用 GPT-5.6 Sol Ultra，只负责依赖、分发、集成、阻断和总进度；实现交给独立执行会话。
- 执行会话最低使用已验证连通的 GPT-5.6 Luna，且 Luna 仅用于固定、可重复、低风险的机械 Git/文档、日志摘取和状态采样；局部实现、定向测试和 PR 收口用 GPT-5.6 Terra XHigh，普通复杂实现用 Terra Max，共享状态、连续 Windows 根因或复杂冲突用 Terra Ultra。
- 初始并发 2 至 3 条执行链；稳定时一次增加一条，429、并发上限或工作树竞争时立即收缩。不要为了占满并发启动审查或重复验证。
- 同一 SHA 已成功的测试、构建和 CI 不重复。失败只提取首个真实错误；低阶尝试无法收敛时升级，不连续猜修。
- 执行会话自行完成实现、最小 x64 验证、提交、推送、Draft/Ready PR 和 integration 合入。普通操作不要求维护者审批；中断后自行检查工作树、远端和 PR 再继续。
- 达到内聚进度就提交并推送。默认只合入 `codex/v1-integration`；禁止 rebase、force push、改写历史、删除维护者内容或把未执行证据写成通过。

## 8. 新总会话启动检查

新 Windows 总调度只做一次下列检查，之后直接执行事项 07：

```powershell
git fetch origin --prune
git switch codex/v1-integration
git pull --ff-only origin codex/v1-integration
git status --short
git log -1 --oneline
gh auth status
gh pr view 26 --repo CoolPlume/azzs --json state,isDraft,headRefOid,baseRefName,mergeStateStatus,url
```

若本地刚从 GitHub 克隆且没有 integration 本地分支，使用：

```powershell
git switch --track -c codex/v1-integration origin/codex/v1-integration
```

完成标准不是再写一份计划，而是事项 07 已合入并结票，事项 08 已实际启动；之后继续按依赖推进，直到 x64 成品和发行前门禁完成。
