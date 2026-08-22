# 需求追踪与事项一致性复核

Status: in-progress  
Review date: 2026-08-10 05:30 CST  
Scope: 仅复核当前磁盘快照；未修改产品代码、正式规格或既有事项。

> **历史证据声明**：本文件仅保留截至 2026-08-10 09:03 CST 的工作树快照、当时未决判断和机械计数，不再作为现行实施合同。产品与工程结论已由[首版规格](spec.md)、[领域词汇](../../CONTEXT.md)、[ADR](../../docs/adr/)、本目录中 `Status: resolved` 的答案登记和[现行事项](issues/)取代；有冲突时以后者为准，本文计数与状态使用前必须重新验证。

## 结论

旧版[规格与事项覆盖审计](issue-coverage-audit-2026-08-10.md)的机械数字仍然有效：主规格有 501 个唯一需求 ID，24 个编号族，族内无缺号；事项目录有 32 张、编号 01--32；需求引用展开后为 1334 条关系，501/501 覆盖；状态仍为 21 个 `ready-for-agent`、10 个 `needs-info`、1 个 `ready-for-human`；`Blocked by` 有 116 条关系，未知依赖、自依赖和图环均为 0。

本轮没有发现因近期规格/事项修改造成的机械漏项或断链。审计开始时候选公开集合为 96 份 Markdown、202 个本地链接，缺失 0 个；并发整理随后新增了本报告及晨间所有权表，截至 05:32 的当前候选集合为 98 份 Markdown、208 个本地链接，缺失仍为 0 个。

## 横切合同附录：不能由事项标签隐含推导

本轮反向阅读事项正文、主规格和研究后，没有新增产品根问题，但确认以下四类合同目前分散在不同需求族中。它们应由各自候选 owner 在正式同步时写成可观察字段和负向验收；在此之前，任何事项状态都不能替代它们。

### 1. 状态标签与证据类型必须分离

- `Status: ready-for-agent` 只表示事项的文字输入暂时足够交给 agent；它不表示代码已实现、合同测试已通过、真实 Windows 已通过或可以进入公开 Release。
- `draft`、`本机试用`、`开发构建`、`候选 Release`、`公开 Release`、`未实机验证` 和 `pass` 分属规格、目录、工程和发行轴；不能压成一个 `ready` 或“完成”。
- `Blocked by` 只表示解除前置；正文中的“事项 N 消费”多数是 `Consumer` 关系，不能机械追加为阻塞。建议每条需求最终至少有 `owner_issue`、`consumer_issues`、`verification_kind`、`verification_id`、`evidence_location`、`evidence_status`、`last_verified_at` 和 `review_owner`。这些字段是追踪合同建议，不是当前已确认的事项格式。

### 2. 时间与证据新鲜度

任何会改变外部状态或安全结论的事实都需要记录事实发生时间、观察时间、适用范围和失效条件，而不是只记录“通过”：来源解析/签名或摘要验证、紧急阻止快照、目录/应用更新清单、安装器结果、硬件状态、Windows 实机矩阵、许可和公开制品证据至少要能回答“在哪台环境、针对哪个精确版本/架构、于何时观察、多久有效”。离线快照、缓存命中、重启后检测和长时间未运行的测试都必须显式显示 `stale/unknown`，不能用旧 `pass` 继续授权管理员操作。时钟来源（本机墙钟、单调时钟或服务器时间）及时间回拨/时区变化处理也应归持久化或安全合同 owner，不由页面自行比较字符串日期。

### 3. 幂等、重放与并发占用

每个可能重试、恢复、重启或从外部交接回来的用例，都要定义稳定的意图/批次/操作标识、重复调用结果、重放是否重新产生外部副作用、取消后的可重试边界，以及设备占用何时真正释放。文件锁或进程锁本身不等于业务批次占用；安装器后代进程、服务、待重启屏障、`install_result_unknown` 和应用更新健康检查未收口前，第二实例不得以“锁已释放”推断可以并行执行。结果检测器必须无副作用、绑定精确目标身份，并能区分“意图已持久化、外部效果可能发生、效果已观察、结果已确认”。

### 4. 先锁定合同再解释标签

明日同步每个 Q 或前沿分支时，最小闭环应包含：唯一 owner、消费者列表、一个正常路径、一个失败关闭路径、一个重复/恢复路径、所需证据类型和证据新鲜度。若这些信息尚未具备，应继续标为 `needs-info`，即使事项已有完整需求引用；若只完成文档或样机，应明确写成相应工程状态，不得提升为产品或发行通过。

## 仍未闭合的追踪问题

### 1. 501/501 不是唯一 owner 证明

32 张事项没有 `Owner`、`Consumer` 或 `Verification` 元数据字段；当前只能从 `References` 推断“被提及”。当前关系为 185 个需求只被一张事项引用、316 个需求被多张事项引用（1334 条关系），与旧审计一致。建议维护者在决定同步后，为每个需求补 `owner_issue`、`consumer_issues`、`verification_kind`、`verification_id`、`evidence_location` 和审核责任；在此之前不要把覆盖率当成实现责任闭合。

证据：`issue-coverage-audit-2026-08-10.md:57-79`、`preimplementation-evidence-register.md:131-141`；机械检查显示 32/32 事项均没有上述字段。

### 2. 事项 31/32 存在语义闭环风险

事项 32 的 `Blocked by: 31`（[32-debug-mode-and-software-catalog-editor.md:5](issues/32-debug-mode-and-software-catalog-editor.md:5)），但事项 31 的验收又要求无界面测试覆盖完整目录编辑器、调试日志和恢复场景（[31-architecture-and-code-quality-guardrails.md:22](issues/31-architecture-and-code-quality-guardrails.md:22)）。因此形式依赖图无环并不代表可执行顺序无环：若 31 的“门禁建立”必须等待 32 的完整功能测试，32 就无法先行关闭。建议把 31 的早期架构/合同检查与发布阶段综合门禁拆开，或明确 32 提供测试场景、31 只提供检查框架，不互相要求对方先完成全部验收。

该风险已在旧审计中指出（`issue-coverage-audit-2026-08-10.md:61,85`），本轮确认近期修改没有消除它。

### 3. 明文事项依赖与 `Blocked by` 仍有表达差异

若把正文中的“事项 N”视为依赖声明，仍可发现未写入 `Blocked by` 的交叉消费关系，例如事项 01 提及事项 31、事项 18 提及事项 32、事项 24 提及事项 18/32、事项 29 提及事项 22。它们多数是消费者/验收协作关系而非真正前置，不能机械追加阻塞；但应在 owner/consumer 字段或验收合同中明确，否则维护者容易把消费关系误读为执行前置。

## 近期变更核对

- 事项 29 已包含 `Blocked by: 19, 25`，与正文依赖搜狗版本基线一致。
- `SCOPE-02/03` 已由事项 01 引用，`SCOPE-04` 已由事项 25 引用。
- 事项 20 已依赖事项 10；当前无未知依赖或图环。
- `README.md` 与 `CONTRIBUTING.md` 的本地链接均可解析，且 README 明确当前没有可运行应用、安装包或 Release；没有发现把文档阶段写成已实现的旧状态。
- ADR-0013 的“缓存默认 7 天、可选立即删除/30 天/不自动删除”与 `CACHE-03`、`CACHE-21` 一致，本轮不登记为冲突。

## 可复现命令

以下命令均为只读检查（在仓库根目录执行）：

```sh
# 需求定义、唯一性和族内连续性
ruby -e 's=File.read(".scratch/windows-initial-setup-workbench/spec.md"); ids=s.scan(/^\s*(?:-\s+)?`([A-Z][A-Z0-9]+-\d+)`/).flatten; puts [ids.length, ids.uniq.length].inspect'

# 事项编号、状态和 Blocked by 图
rg --files .scratch/windows-initial-setup-workbench/issues | sort
rg -n '^(Status|Blocked by|Type):' .scratch/windows-initial-setup-workbench/issues

# 覆盖/链接（精确展开范围的完整脚本见本次审计会话记录）
ruby -e 'root=".scratch/windows-initial-setup-workbench/issues"; puts Dir["#{root}/*.md"].length'
git diff --check
```

本报告保留“表面覆盖通过、所有权与可执行语义仍需维护者收口”的结论，不授权开始产品实现。
