# 研究与证据索引

Status: evidence-only  
Last updated: 2026-08-10

本目录保存带访问日期、来源边界和限制的一手资料研究。研究可以提出候选工程门禁和维护者问题，但不是主规格、ADR、法律意见、实机通过记录或发布批准。

## 如何使用

- 产品行为和状态转换以[主规格](../../.scratch/windows-initial-setup-workbench/spec.md)为准；研究中的推荐答案只有在维护者确认并显式同步后才生效。
- 难逆、非显然且存在真实取舍的已确认决定进入[ADR](../adr/)；不要直接改写旧 ADR 来伪造历史一致性。
- 领域词义以[领域语言](../../CONTEXT.md)为准；研究中的候选术语不自动进入词汇表。
- 第三方内容、构建来源、实机和发布证据的状态集中在[实现前证据登记册](../../.scratch/windows-initial-setup-workbench/preimplementation-evidence-register.md)。研究代理只能提出结论，只有登记册中记录适用范围、审核人和日期的 `accepted` 才能作为某项门禁已满足的证据；具体门禁是否要求该类证据以主规格和 ADR 为准。候选资源的随包和公开发布不以该状态、许可证据或再分发结论作为门槛，见 [ADR-0036](../adr/0036-candidate-resources-may-be-bundled-and-released.md)。
- 当前产品问题、依赖顺序与推荐短答见[晨间决策影响图](../../.scratch/windows-initial-setup-workbench/morning-decision-impact-map.md)。

## 工具链、安装与发行

| 研究 | 回答什么 | 当前不能证明 |
| --- | --- | --- |
| [预实现工具链基线](preimplementation-toolchain-baseline-2026-08-10.md) | Windows App SDK、VS/MSVC、Windows SDK、C++ 模式、自包含与 x64/ARM64 的查询日候选 | 版本已经锁定、Win10/ARM64/干净机已经通过 |
| [传统 Windows 安装器候选](traditional-installer-options.md) | WiX MSI、Burn + MSI、Inno、NSIS 的架构、许可、生命周期、体积与同负载样机顺序 | 已选择安装器、Repair/rollback 自动保护应用状态 |
| [公开构建来源、CI 与发行证据](release-provenance-and-ci-evidence.md) | GitHub runner、digest、attestation、immutable release、SBOM、证据保留与实机边界 | 绿色 CI 等于 Win10/UAC/SmartScreen 通过、软件安全或许可闭合 |

## 安全与状态

| 研究 | 回答什么 | 当前不能证明 |
| --- | --- | --- |
| [提权下载与执行安全](elevated-download-execution-security.md) | URL、重定向、缓存、发布者、管理员子进程、自更新、控制元数据、UI Automation 与终止目标的边界 | 某项真实性策略已经获接受、真实 Windows 吊销/SmartScreen 已验证 |
| [Windows 状态持久化与恢复](windows-state-persistence-and-recovery.md) | SID/ACL、ProgramData、原子提交、模式迁移、多实例、检查点、日志退化与卸载保留 | 任意硬件断电后绝对持久、Q5 身份/介质/兼容政策已确定 |

## 目录内容与第三方资源

| 研究 | 回答什么 | 当前不能证明 |
| --- | --- | --- |
| [第三方二进制公开再分发基线](third-party-binary-redistribution-baseline.md) | 32 个拟随包对象的公开镜像依据、条件、身份缺口、回退和 `REDIST-D*` | 法律批准、任一对象已达到 `accepted`、候选库存等于发行清单 |
| [`installPack` 未知软件识别](installpack-unknown-software-identification.md) | 五个本地候选安装包的产品身份、静态元数据与展示建议 | 安装行为、安全、最新版或公开再分发权 |

## 界面工程

| 研究 | 回答什么 | 当前不能证明 |
| --- | --- | --- |
| [WinUI 3 动效工程](winui3-motion-engineering.md) | 动效目的、频率、时长、Composition、可中断性、减少动画与性能取证 | 取代[项目 WinUI 3 设计准则](../design/winui3-apple-inspired.md)或真实设备性能通过 |

## 复核触发器

以下任一发生时，先重查对应一手来源，再更新研究日期和证据状态：工具链或安装器版本变化；GitHub runner、attestation、retention 或 Release 能力变化；上游许可、NOTICE、资产名、架构或下载渠道变化；Windows 支持范围、SmartScreen/SAC 行为或目标 OS 矩阵变化；规格、ADR 或制品矩阵改变了研究前提。
