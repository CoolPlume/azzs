# 最终文档与结构验证快照

Status: evidence-only  
Snapshot time: 2026-08-10 09:01 CST  
Scope: 当前工作树的文档/结构检查；不表示产品、构建、Windows 实机或公开 Release 已通过

> **历史证据声明**：本文件仅保留截至 2026-08-10 09:03 CST 的工作树快照、当时未决判断和机械计数，不再作为现行实施合同。产品与工程结论已由[首版规格](spec.md)、[领域词汇](../../CONTEXT.md)、[ADR](../../docs/adr/)、本目录中 `Status: resolved` 的答案登记和[现行事项](issues/)取代；有冲突时以后者为准，本文计数与状态使用前必须重新验证。

## 1. 检查结果

| 检查 | 结果 | 能证明什么 | 不能证明什么 |
| --- | --- | --- | --- |
| Markdown 本地链接 | Git 公开候选集合 `104` 份 Markdown，严格解析得到 `352` 条相对链接，缺失 `0` | 纳入 Git 公开候选的相对路径可解析；计入表格链接和 `file.md:line` 形式 | 外部 URL、锚点、内容正确性；被忽略的本地技能文件不计入公开候选 |
| 规格需求 ID/编号族 | `501/501` 唯一，`24` 个编号族，族内断号/重复 `0/0` | 当前 `spec.md` 的编号唯一/连续性 | 需求之间没有语义冲突、owner 唯一 |
| 事项状态与 `Blocked by` | `32` 张；`21 ready-for-agent`、`10 needs-info`、`1 ready-for-human`；依赖 `116`，未知/自依赖/图环 `0/0/0` | 文件存在、状态可读、形式依赖无环 | 事项完成、产品决策已确认 |
| TOML | Python 3.11 `tomllib` 解析通过；`6` 分类、`7` 软件、`2` 启用、`5` 禁用；`release_state=draft` | 当前目录草稿可由 `tomllib` 解析 | 来源真实、许可闭合、运行时/发布门禁通过 |
| GitNexus | 当前索引提交 `bb6fbe4`，状态 `up-to-date`，循环 `0` | 已索引提交的结构状态和循环检查 | 未跟踪文档、产品实现、Windows 兼容性 |
| Git whitespace | `git diff --check` 退出码 `0` | 当前 diff 没有 Git 检出的空白错误 | 文档语义正确 |
| 公开候选卫生 | 扫描 Git 公开候选 `109` 个文件：大于 1 MiB `0`，已知私钥/令牌模式 `0`；绝对路径/邮箱命中 `5` 个文件，其中个人 Google Drive/Gmail 路径 `1` 个文件、需人工处理 | 候选文件中的已知敏感模式、大文件和路径命中 | 未识别的秘密、许可或个人资料风险 |

## 2. 运行命令

以下命令由维护者明天继续使用；路径和运行时以本机实际环境为准：

```sh
date '+%Y-%m-%d %H:%M:%S %Z %z'
git diff --check
gitnexus status
gitnexus check --cycles
python3 -c 'import tomllib, pathlib; tomllib.loads(pathlib.Path("catalog/software-catalog.toml").read_text())'
```

本次最终数字按 `git ls-files --cached --others --exclude-standard` 计算。链接统计使用严格的 Markdown 相对链接出现次数口径，包含表格中的链接和 `file.md:line` 形式；缺失目标为 0。规格覆盖关系 `1334`、唯一 owner 缺口和外部链接/锚点内容仍需维护者后续继续审计；新增文件或决策同步后必须重新计算，不能沿用本快照。旧的 06:58/07:20 增量段保留作历史，不覆盖本节最终结果。

## 3. 当前未决

- Q1 仍由另一任务负责；本任务不唤醒、不代答。
- Q2-Q19、首版软件集合、owner/consumer/verification/freshness 字段和三轴状态仍未获维护者确认。
- 领域词汇前沿差集复核没有新增根问题；软件版本结果、批次生命周期和目录/能力扩展差集分别归并到 Q6/Q17/Q19、Q2/Q4g-h/Q16/Q18。
- ADR 对齐复核仍有待确认的历史冲突：ADR-0005/0006 的无摘要/无源码关联整体口径、ADR-0012 的两档发行口径，需在 Q10/三档决定后显式确认或 supersede，不能静默改写。
- 本快照不改变任何正式文档、ADR、事项状态或外部仓库设置。

## 4. 解释与已知例外

- 公开候选卫生扫描按 Git 纳入范围执行；另一次全工作树诊断发现被 `.gitignore` 排除的本地 GitNexus 元数据（包括 `.gitnexus/lbug`），这些不计入候选文件数；`graphify-out/` 同样不计入候选集合。
- 个人路径命中为 `.scratch/windows-initial-setup-workbench/session-handoff-2026-08-09.md` 中的 Google Drive/Gmail 路径；本轮不替维护者泛化、删除或授权公开。
- GitNexus 只反映已索引提交，不包含当前未跟踪审计文件；它不证明 Windows、构建、许可或发布通过。
- 当前工作树还包含被 `.gitignore` 排除的 `.claude/skills`、`CLAUDE.md`、`.gitnexus` 和 `graphify-out`；这些不进入公开候选统计。候选链接只发现 3 个指向已存在目录的目录级链接，无缺失目标。
- `tomllib`、链接、编号和 whitespace 检查均为结构证据，不是产品行为、实机或公开 Release 证据。

## 5. 07:20 增量快照（会话中断保护）

在新增 ADR 状态审计、词汇前沿和会话连续性记录后，用同一工作树重新做了只读复核：

| 检查 | 当前结果 | 解释边界 |
| --- | --- | --- |
| Git 公开候选集合 | `109` 个文件，其中 `104` 份 Markdown | 依据 `git ls-files --cached --others --exclude-standard`；不含被忽略的 Graphify/GitNexus/本地技能生成物 |
| Markdown 本地链接 | `305` 条相对链接，缺失 `0` | 计入 `file.md:line` 形式并按当前候选集合解析；不证明外部 URL 或锚点内容 |
| 规格需求定义 | `501/501` 唯一，`24` 个编号族，定义重复 `0`、族内缺号 `0` | 需求覆盖和语义一致性仍未证明 |
| 事项与依赖 | `32` 张；`21 ready-for-agent`、`10 needs-info`、`1 ready-for-human`；`116` 条 `Blocked by`，未知/自依赖/循环 `0/0/0` | 形式图完整不代表事项可实现 |
| 目录 TOML | `6` 分类、`7` 软件、`2` 启用、`5` 禁用，`release_state = "draft"` | 仅证明 Python `tomllib` 可解析，不证明来源、许可、安装档案或发布门禁 |
| GitNexus | `bb6fbe4`、`up-to-date`、循环 `0` | 只覆盖已索引提交，不含当前未跟踪审计文件 |
| 实现文件 | 未发现 `.cpp`、`.h/.hpp`、`.xaml`、`.cs`、`.vcxproj`、`.sln`、`CMakeLists.txt` 或 `.exe` | 只证明本轮没有开始产品实现 |
| 候选卫生 | 大于 1 MiB 为 `0`，已知私钥/令牌模式为 `0` | 绝对用户路径命中 `5` 个文件：4 个是审计/恢复命令中的本机工具路径，1 个是旧交接记录中的 Google Drive/Gmail 候选池路径；公开前仍需人工处理 |

当时部分外部状态查询不可用；未根据查询失败推断其他任务完成或失败，也未创建重复恢复安排。以上述本地快照作为中断后的恢复依据；09:00 仍需再做一次最终快照，不能沿用本节数字。

## 6. 09:01 最终一致性快照

初次机械快照时间：2026-08-10 09:01:19 CST；修正后最终复核：2026-08-10 09:03:15 CST。用户要求的 09:00 收束时间已到；本节之后不再扩展问题树。

| 检查 | 当前结果 | 解释边界 |
| --- | --- | --- |
| Git 公开候选集合 | `109` 个文件，其中 `104` 份 Markdown | 按 `git ls-files --cached --others --exclude-standard` 计算；不含被忽略的本地生成物 |
| Markdown 相对链接 | `355` 条，缺失 `0` | 计入当前候选 Markdown 中的本地相对目标（含 `file.md:line` 形式）；不证明外部 URL、锚点或内容语义 |
| 规格需求定义 | `501/501` 唯一，`24` 个编号族，族内断号 `0` | 只证明编号结构；不证明需求无冲突、owner 唯一或已确认 |
| 事项与依赖 | `32` 张；`21 ready-for-agent`、`10 needs-info`、`1 ready-for-human`；`116` 条 `Blocked by`；未知/自依赖/循环 `0/0/0` | 只证明形式图完整，不证明事项完成或可立即实现 |
| 目录 TOML | Python `tomllib` 通过；`6` 分类、`7` 软件、`2` 启用、`5` 禁用；`release_state = "draft"` | 不证明来源真实性、许可、安装档案、运行时或公开发布门禁 |
| GitNexus | 索引提交 `bb6fbe4`，当前提交相同，`up-to-date`；循环 `0` | 只覆盖已索引提交，不含未跟踪审计文件，不证明产品/Windows/发布通过 |
| Git whitespace | `git diff --check` 退出码 `0` | 只证明当前 diff 无 Git 空白错误 |
| 公开候选卫生 | 大于 1 MiB `0`；已知私钥/令牌模式 `0`；绝对用户路径命中 `5` 个文件 | 5 个命中文件为审计/恢复记录；旧交接记录中的 Google Drive/Gmail 路径仍需公开前人工处理 |
| 产品实现扫描 | 未发现 `.cpp`、`.h/.hpp`、`.xaml`、`.cs`、`.vcxproj`、`.sln`、`CMakeLists.txt` 或 `.exe` | 只证明本轮没有开始产品实现 |

本次快照没有 stage、commit、push、构建、CI、实机验证或 Release 操作。历史章节中的旧计数保留作增量轨迹；明天应以本节当前口径为起点重新计算，不把结构检查通过解释为产品、实机或公开发布通过。
