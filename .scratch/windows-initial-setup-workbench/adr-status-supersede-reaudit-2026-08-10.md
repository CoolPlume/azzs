# ADR 状态与替代关系复核

Status: in-progress  
Review date: 2026-08-10

> **历史证据声明**：本文件仅保留截至 2026-08-10 09:03 CST 的工作树快照、当时未决判断和机械计数，不再作为现行实施合同。产品与工程结论已由[首版规格](spec.md)、[领域词汇](../../CONTEXT.md)、[ADR](../../docs/adr/)、本目录中 `Status: resolved` 的答案登记和[现行事项](issues/)取代；有冲突时以后者为准，本文计数与状态使用前必须重新验证。

本文是实现前的只读核对记录。它不改变 `docs/adr/`、主规格、领域词汇或产品代码，也不把 scratch 审计转换成正式决定。语义判断沿用[ADR 对齐审计](adr-alignment-audit-2026-08-10.md)，本文件只补齐 ADR 自身是否能表达“当前有效、已接受、被替代”的证据。

## 1. 结论

- 当前共有 28 份 ADR（`0001` 至 `0028`）。28/28 均是标题加正文的极简文件，均没有机器可读的 `Status`、日期、`Supersedes`、`Superseded by`、`Replaces` 或等价字段。
- `docs/adr/` 内没有检出 `supersed`、`replaced`、`obsolet`、`撤代`、`被替代` 或 `取代` 关系文本。因此不能仅从 ADR 文件判断旧决定是否仍有效，也不能从 ADR 文件追溯替代链。
- 现有 `adr-alignment-audit-2026-08-10.md`、`documentation-authority-audit-2026-08-10.md` 和其他 scratch 文件是审计或候选输入，不是正式 ADR 状态。它们可以指出冲突和推荐动作，但不能静默使 ADR 失效。
- 本轮没有发现新的独立 P0/P1 根问题。已发现的语义问题都可归并到已有的 Q2、Q3、Q4a-h、Q5a-l、Q6、Q7、Q8、Q9、Q10、Q16-Q19 或对应事项；新增的是 ADR 可追溯性元数据缺口。

### 1.1 机械核对快照

| 检查项 | 结果 |
| --- | --- |
| ADR 文件数 | 28 |
| 缺少 `Status` 的 ADR | 28/28 |
| 缺少日期字段的 ADR | 28/28 |
| 缺少 supersede/replaces 关系字段的 ADR | 28/28 |
| ADR 正文中的显式替代关系 | 0 条 |
| 当前规格 SHA-256 | `6ce66ec05907e315fb2d429ceab270c3cd8c9bb6887f62c8438707d12951cc4e` |

复核命令（在仓库根目录执行）：

```sh
find docs/adr -maxdepth 1 -type f -name '*.md' | sort
rg -n -i '^(status|状态|date|created|updated|日期|创建|更新):' docs/adr
rg -n -i 'supersed|replaced|obsolet|撤代|被替代|取代' docs/adr
```

第二、三条命令无输出并不表示决定不存在，只表示 ADR 文件没有记录这些事实。

## 2. 逐 ADR 状态与动作

“当前语义状态”描述本轮阅读后可以安全采用的范围，不等于正式 `Status` 值。`A` 表示方向与现行规格基本一致，`B` 表示已被后续确认改变方向但尚未显式替代，`C` 表示真实取舍或安全合同仍未决，`D` 表示决定仍成立但只需同步术语/元数据。

| ADR | 当前语义状态 | 直接引用或消费处 | 冲突/待决 Q | 建议动作（不在本轮执行） |
| --- | --- | --- | --- | --- |
| [0001 常驻管理员会话](../../docs/adr/0001-elevated-session.md) | A/C：提权会话仍是架构前提，下载与子进程真实性合同未闭合 | [`SEC-01..03`](spec.md:73)、[`SEC-07`](spec.md:85)、[提权安全研究](../../docs/research/elevated-download-execution-security.md:314) | Q4a-f；高完整性解析、子进程令牌、外部安装器 | 保留会话决定；补威胁模型和子进程令牌合同，必要时另立安全 ADR，不把风险披露当验证门禁。 |
| [0002 GitHub 分发](../../docs/adr/0002-github-distribution.md) | A：公开 GitHub 与便携/安装双形态仍成立，制品组合未冻结 | [`REL-01..07`](spec.md:59)、[发布事项 21](issues/21-release-artifacts.md:27)、[发行证据研究](../../docs/research/release-provenance-and-ci-evidence.md:176) | Q7、Q8、Q10；三档矩阵、实机门槛、provenance | 保留分发渠道；待 Q7/Q8/Q10 后用独立发行 ADR 固定制品矩阵和证据。 |
| [0003 离线能力](../../docs/adr/0003-offline-capability.md) | D：离线启动和本地资源能力仍成立，术语从两档转为三档 | [`REL-03..05`](spec.md:61)、[发行与资源术语](../../CONTEXT.md:53)、[发布事项 21](issues/21-release-artifacts.md:27) | Q2、Q3、Q7、Q9；救援工具和离线内容边界 | 保留离线能力；三档命名和内容责任由新发行 ADR/规格同步，不静默改写旧 ADR。 |
| [0004 共享设备状态](../../docs/adr/0004-shared-device-state.md) | C：跨发行包共享原则仍有价值，“设备级”没有主体、SID 和 ACL 语义 | [持久化研究](../../docs/research/windows-state-persistence-and-recovery.md:21)、[事项 02](issues/02-device-state-and-execution-log.md:8)、[事项 12](issues/12-restart-resume.md:8) | Q5a-l；交互用户/执行主体、ProgramData、跨用户可见性、卸载保留 | 等 Q5 收口后新增“状态作用域与主体隔离”ADR，并显式 `Supersedes: ADR-0004`；保留可验证的共享范围。 |
| [0005 未签名发行](../../docs/adr/0005-unsigned-releases.md) | C：不做 Authenticode 仍是选择，但“不提供摘要/源码关联”与现有研究及 GitHub 能力绑在一起 | [`SEC-04..06`](spec.md:79)、[更新事项](issues/17-application-update.md:15)、[发布事项](issues/21-release-artifacts.md:21)、[发行证据研究](../../docs/research/release-provenance-and-ci-evidence.md:176) | Q10、`RELEASE-D1/D5`；与 Q4a 的自更新真实性相互影响 | 拆分“Windows 代码签名”和“摘要/provenance/immutable release”；若允许后者，新 ADR 显式 supersede 复合决定中的相应部分。 |
| [0006 主动自更新](../../docs/adr/0006-opt-in-self-update.md) | C：主动触发和失败回退方向存在，自动替换的真实性、健康检查和状态迁移未定义 | [`UPD-01..07`](spec.md:675)、[应用更新事项](issues/17-application-update.md:13)、[提权安全研究](../../docs/research/elevated-download-execution-security.md:314) | Q4a、Q19；活动批次互斥、启动健康超时、回退失败、N/N-1 | 在信任根和健康合同闭合前不视为可实施输入；保留手动更新，按决定新增或 supersede 自更新 ADR。 |
| [0007 重启恢复](../../docs/adr/0007-resume-after-restart.md) | A：恢复后等待用户明确继续、历史不可改写仍与规格一致 | [重启恢复事项](issues/12-restart-resume.md:8)、[设备状态事项](issues/02-device-state-and-execution-log.md:8)、[持久化研究](../../docs/research/windows-state-persistence-and-recovery.md:149) | Q5b-e/f-l；主体、检查点耐久、版本迁移和卸载边界 | 保留；在持久化合同确定后补状态主体和检查点证据，不需要静默替代。 |
| [0008 独立硬件检测](../../docs/adr/0008-independent-hardware-detection.md) | A：自行实现检测、避免直接复制第三方代码仍成立 | [驱动获取事项](issues/14-driver-acquisition-page.md:9)、[`DRV` 需求](spec.md:215) | Q3 仅影响检测结果之后的驱动交接边界 | 保留；把硬件检测事实与外部驱动交接结果分开记录。 |
| [0009 驱动获取边界](../../docs/adr/0009-driver-acquisition-boundary.md) | C：硬件概览和入口推荐成立，“导入”可能越界为工作台安装 | [驱动获取页事项](issues/14-driver-acquisition-page.md:9)、[`DRV` 需求](spec.md:215) | Q3；本地交接、INF/EXE 选择/验证/安装责任 | 若只做本地驱动交接则保留并改用该术语；若工作台安装，先写信任/适用性/回滚合同并显式 supersede。 |
| [0010 来源优先级](../../docs/adr/0010-per-software-install-source-priority.md) | A/C：按软件声明用途和默认顺序仍成立，来源真实性不能由优先级解决 | [`SRC` 与解析需求](spec.md:396)、[目录生命周期事项](issues/03-software-driver-catalog-lifecycle.md:9)、[提权安全研究](../../docs/research/elevated-download-execution-security.md:319) | Q4b、Q10；第三方包源信任、签名/摘要、自动管理员执行 | 保留来源用途/优先级；把真实性和最低安全门禁迁入独立安全 ADR，禁止静默换源。 |
| [0011 目录包主动更新](../../docs/adr/0011-opt-in-catalog-package-updates.md) | A：随发行版提供、主动更新/导入/回退、批次快照隔离仍成立 | [目录生命周期事项](issues/03-software-driver-catalog-lifecycle.md:9)、[`CAT` 需求](spec.md:519) | Q7、Q9、Q10；内容版、再分发权和来源证明 | 保留目录生命周期决定；发布来源/权利门禁由发行 ADR 和证据登记册承接。 |
| [0012 离线资源版](../../docs/adr/0012-offline-resource-edition.md) | B：两档在线/离线已被标准版、断网救援版、超大离线版方向取代 | [`REL-03..05`](spec.md:61)、[三档领域定义](../../CONTEXT.md:53)、[制品矩阵候选](release-artifact-matrix-options.md:1)、[发布事项](issues/21-release-artifacts.md:27) | Q2、Q3、Q7、Q9；三档拓扑、救援工具、内容权利和可携带集合 | Q2/Q3/Q7/Q9 收口后新增 superseding ADR；在此之前不得把两档或三档任一方当最终制品合同。 |
| [0013 软件包缓存](../../docs/adr/0013-software-package-cache-retention.md) | A/C：7 天及可选保留策略成立，缓存并发/ACL/TOCTOU未完整 | [`CACHE-02..15`](spec.md:473)、[缓存事项](issues/07-offline-package-cache.md:24)、[提权安全研究](../../docs/research/elevated-download-execution-security.md:252) | Q4e、Q5b-d；外置/网络缓存、临时文件、跨实例锁和执行前回到受保护暂存 | 保留清理策略；在规格/架构附录补锁域、权限、原子提交和不可消费半成品合同。 |
| [0014 详细本机日志](../../docs/adr/0014-local-detailed-log-retention.md) | A/C：本机永久保留、默认不上传、自包含脱敏导出成立，容量/故障退化仍需冻结 | [日志事项](issues/15-history-and-logs.md:8)、[架构日志适配器边界](../../docs/engineering/architecture-and-code-quality.md:105)、[`LOG` 需求](spec.md:784) | Q5e/f-l；磁盘上限、原子导出、采集缺口和主体隔离 | 保留本机日志决定；明确“永久”与配额/用户清理的关系，继续由事项 15 和诊断合同测试验证。 |
| [0015 设置目录独立版本](../../docs/adr/0015-separate-settings-catalog-package.md) | A：系统设置目录与软件/驱动/软件优化目录分离仍成立 | [设置目录事项](issues/20-initial-settings-catalog.md:8)、[`CAT` 目录边界](spec.md:519) | Q5（恢复主体）和 Q4g-h（跨类别阻止）可能补充控制面，不改变包边界 | 保留；只把状态主体和紧急控制元数据作为增量合同，不重写独立版本决定。 |
| [0016 驱动优先流程](../../docs/adr/0016-driver-first-initialization-flow.md) | A：推荐顺序是驱动、系统优化、软件安装、软件优化，且可跳过 | [引导流程事项](issues/16-guided-initialization-flow.md:8)、[`FLOW` 需求](spec.md:265) | Q3、Q6、Q17；外部驱动交接、当前状态/操作结果分层、流程聚合 | 保留非强制顺序；补交接/待确认/跳过结果语义，不将推荐顺序变成强制向导。 |
| [0017 简体中文优先](../../docs/adr/0017-simplified-chinese-first-with-language-expansion.md) | A：首版简体中文，保留语言扩展能力仍与规格一致 | [`UI`/语言需求](spec.md:158)、[设计系统事项](issues/24-winui3-design-system.md:8)、[领域词汇](../../CONTEXT.md:22) | 未发现直接 Q；新增语言仍不应改变核心合同 | 保留；只需补 Status/日期，未来语言扩展另行评审。 |
| [0018 Windows 最低目标](../../docs/adr/0018-windows-minimum-target-version.md) | A：Windows 10 22H2 为设计最早目标，同时面向 Windows 11，不承诺完整兼容矩阵 | [`TECH` 目标需求](spec.md:36)、[领域定义](../../CONTEXT.md:31)、[工具链研究](../../docs/research/preimplementation-toolchain-baseline-2026-08-10.md:16) | Q8、`INSTALLER-D*`；实机证据和具体能力缺口 | 保留；继续把“最低设计目标”与“已验证支持”分开，不把未实机标记当通过。 |
| [0019 WinUI 3/核心边界](../../docs/adr/0019-winui3-ui-and-cpp-core-boundary.md) | A/C：同进程 UI/核心分离、核心唯一状态来源成立；救援 EXE 归属未决 | [`TECH-01..06`](spec.md:102)、[`TECH-22..31`](spec.md:123)、[架构规范](../../docs/engineering/architecture-and-code-quality.md:25) | Q2、Q4f、事项 31；受控能力和提权解析边界 | 保留为默认架构；若救援工具随提权工作台启动，必须先成为受控制品或另立明确例外 ADR。 |
| [0020 可移植 C++ 核心](../../docs/adr/0020-portable-cpp-core-for-future-macos.md) | A：首版 Windows UI、未来 macOS 仅复用可移植核心/数据契约仍成立 | [`TECH-07..11`](spec.md:108)、[架构规范](../../docs/engineering/architecture-and-code-quality.md:38) | 未发现直接语义冲突；平台适配仍非首版范围 | 保留；不为未来 macOS 提前添加界面或假想适配器。 |
| [0021 Apple 原则适配 WinUI 3](../../docs/adr/0021-adapt-apple-design-to-winui3.md) | A：Apple 设计原则仅作为 WinUI 3 的方法来源，平台惯例和无障碍优先 | [`UI-01..10`](spec.md:158)、[WinUI 3 设计规范](../../docs/design/winui3-apple-inspired.md:1)、[设计事项](issues/24-winui3-design-system.md:8) | 未发现直接语义冲突；实现前仍需按设计规范验收 | 保留；不把 ADR 当页面行为清单，具体动效合同归设计文档/规格。 |
| [0022 C++26 工具链](../../docs/adr/0022-cpp26-with-newest-stable-toolchain.md) | C：目标口径与 MSVC 实际开关和稳定特性子集未闭合 | [`TECH-12..18`](spec.md:113)、[技术基线](../../docs/engineering/technology-baseline.md:1)、[工具链研究](../../docs/research/preimplementation-toolchain-baseline-2026-08-10.md:16) | 工具链决策分支、`INSTALLER-D3/D14`；MSVC `/std:c++latest` 与“不用实验能力”边界 | 先做零产品功能样机并锁定精确版本/特性允许清单；若要求正式 C++26 开关，显式 supersede ADR-0022。 |
| [0023 独立软件优化目录](../../docs/adr/0023-separate-software-optimization-catalog.md) | A/C：优化目录独立版本、更新、导入、回退仍成立，跨类别安全控制未定义 | [优化目录事项](issues/25-software-optimization-catalog-lifecycle.md:8)、[`OPT` 需求](spec.md:914) | Q4g-h；紧急阻止是否跨到软件来源/设置/工作台版本 | 保留包边界；不要把跨类别撤回悄然塞进本 ADR，必要时新增安全控制 ADR。 |
| [0024 紧急撤回优先快照](../../docs/adr/0024-emergency-withdrawal-overrides-optimization-snapshot.md) | A/C：软件优化未执行步骤可被紧急撤回，其他类别未定义 | [优化撤回事项](issues/28-software-optimization-emergency-withdrawal.md:8)、[优化需求](spec.md:953) | Q4g-h；“严重风险”控制面和跨类别阻止/解除 | 保留软件优化范围；若扩展至其他目录、安装档案或工作台版本，新增跨类别 ADR 并显式引用本 ADR。 |
| [0025 紧急通知独立更新](../../docs/adr/0025-emergency-notices-separate-from-catalog-updates.md) | A/C：通知只更新安全阻止、不替换普通目录的生命周期仍成立，真实性/新鲜度未闭合 | [优化撤回事项](issues/28-software-optimization-emergency-withdrawal.md:8)、[通知与安全研究](../../docs/research/elevated-download-execution-security.md:315) | Q4c-d、Q4g-h；传输信任、离线旧修订、解除权限和控制元数据 | 保留通知与目录分离；在共同安全控制 ADR 中明确信任根、版本、防回滚和过期显示。 |
| [0026 按功能所有者分类优化](../../docs/adr/0026-classify-optimization-by-feature-owner.md) | A：系统优化/软件优化按被调整功能所有者划分仍成立 | [优化需求](spec.md:914)、[优化目录事项](issues/25-software-optimization-catalog-lifecycle.md:8)、[领域术语](../../CONTEXT.md:300) | Q5；分类决定不等于持久化主体或用户账号模型 | 保留；在状态作用域讨论中明确本 ADR 不回答存储主体，避免误读。 |
| [0027 模块化单体与受控接缝](../../docs/adr/0027-modular-monolith-and-controlled-extension-seams.md) | C：默认架构与目录数据扩展边界成立，外部救援 EXE 的捆绑/启动与门禁冲突 | [`TECH-22..32`](spec.md:123)、[架构规范](../../docs/engineering/architecture-and-code-quality.md:25)、[事项 31](issues/31-architecture-and-code-quality-guardrails.md:8)、[候选审核](installpack-candidate-review.md:138) | Q2、Q4a/f/h；项目是否对随包并由工作台启动的工具负实现、构建、许可、完整性和测试责任 | 保留为默认；若工具不受同一门禁，改为外部交接；若必须捆绑启动，新增明确安全例外 ADR，不放宽目录插件边界。 |
| [0028 TOML 直接读取软件目录](../../docs/adr/0028-directly-readable-software-catalog-source.md) | A/C：TOML 权威输入、图形编辑器共享同一模型和校验成立，启动恢复呈现与持久化耐久仍待决 | [`CAT-01..52`](spec.md:519)、[领域定义](../../CONTEXT.md:140)、[目录编辑事项](issues/32-debug-mode-and-software-catalog-editor.md:8)、[架构规范](../../docs/engineering/architecture-and-code-quality.md:62) | Q1、Q5b/e、Q16、Q18；恢复内容呈现、原子提交、导入权限、本机试用身份 | 保留来源/编辑器边界；把启动呈现和耐久上限写入规格/持久化合同，不创建第二事实来源。 |

## 3. 明日同步门禁

### 3.1 元数据格式（建议，不在本轮改写）

正式 ADR 需要至少能独立判断以下字段：

```text
Status: proposed | accepted | superseded | rejected
Date: YYYY-MM-DD
Supersedes: ADR-xxxx       # 没有时省略
Superseded by: ADR-yyyy    # 被替代的旧 ADR 才填写
```

新增替代决定时保留旧 ADR 正文和编号，新增 ADR 写明 `Supersedes`、理由、迁移影响和生效范围；旧 ADR 只补 `Status: superseded` 与反向编号。若只是补字段或澄清实现合同，不应伪造 supersede 关系。

### 3.2 写回顺序

1. 先回答 Q2、Q3、Q4a-b、Q5a-e、Q6、Q8、Q9、Q10、Q11a-b、Q12-Q15；Q1 继续等待另一会话的真实回答。
2. 根据答案重算本表；只有方向真正改变时才新增 superseding ADR，先写原因和影响，再同步规格与事项。
3. 三档发行、来源真实性、状态主体、跨类别紧急阻止和 C++ 工具链分别保持原子决定；不要用一个 ADR 复盖多个不相同的信任根或生命周期。
4. 对状态为 A/D 的 ADR 只补元数据和必要链接，不因为没有机器可读状态就宣称它们失效。
5. 写回后重跑需求覆盖、事项依赖、领域术语、Markdown 链接和 `git diff --check`；未完成代码实现前保持发布门禁关闭。

## 4. 边界声明

- 本文没有批准任何救援工具、第三方安装器、目录资源、发行制品、代码签名策略或实机验证缺口。
- 研究文档中的事实、scratch 审计中的推荐答案和另一会话尚未得到用户确认的 Q1 都不是正式决定。
- 由于 ADR 文件当前缺少日期，无法从文件自身判断“最新决定”；本次审计日期只表示核对时间，不是 ADR 的创建时间或生效时间。
