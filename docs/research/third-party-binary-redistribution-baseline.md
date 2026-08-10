# 第三方二进制公开再分发基线

- 状态：研究基线，**不是许可意见、法律意见、发布批准或 `accepted` 证据状态**
- 研究日期：2026-08-10
- 访问日期：2026-08-10
- 适用范围：拟由公开 GitHub 项目的自包含发行包、断网救援版或超大离线版携带，或由项目 GitHub Release／项目第三方包源镜像的二进制；为保证候选闭合，也并列记录两个自研救援伴随工具
- 覆盖口径：按[候选审核稿](../../.scratch/windows-initial-setup-workbench/installpack-candidate-review.md)中的携带、可选携带、按硬件候选、待许可候选，以及工作台自包含依赖计 32 项
- 操作边界：只读官方页面、条款、许可证、NOTICE 与发布 API；没有运行、修改、复制到仓库、上传或重新打包任何安装程序

本文只回答“项目能否把某个上游二进制作为公开制品的一部分再次提供”。它不回答软件是否免费、终端用户能否自行安装、工作台能否从官网取得软件，也不替代上游书面许可或专业法律审查。本文中的“不得进入”与“应外部交接”均为基于查询日事实的研究建议，不是项目发行准入决定；候选资源的公开发布政策以 ADR-0036 为准。

现有[候选审核稿](../../.scratch/windows-initial-setup-workbench/installpack-candidate-review.md)中的“超大离线版携带”是产品意图，不是再分发授权；现有[证据登记册](../../.scratch/windows-initial-setup-workbench/preimplementation-evidence-register.md)的 `accepted` 状态仍是记录证据审核结果的事实。依据 ADR-0036，这些事实不再构成候选资源随包、进入项目备用源或公开 Release 的项目门槛；本文不改变任何候选、证据或上游权利状态。

## 1. 结论

### 1.1 结论等级

| 等级 | 本文含义 | 按本研究建议能否直接进入公开制品 |
| --- | --- | --- |
| **明确允许** | 已找到直接授予分发权的上游许可证；剩余事项主要是精确制品识别和许可证履行 | **不能直接进入**。仍须完成第 3 节的版本、架构、摘要、NOTICE 和条款快照门禁，并在证据登记册中审核为 `accepted` |
| **带条件允许** | 上游授予只在特定许可主体、随本程序、对应源码、指定组件或 EULA 流程等条件下成立 | 条件和证据全部闭合前不能进入 |
| **无法证明／应外部交接** | 未找到足以支持本项目公开再分发的授权，或当前适用条款明确不提供该权利 | 不得进入当前公开制品；改用官网发布页的来源手动交接，或取得上游书面许可 |
| **身份未闭合** | 候选名称、厂商、型号、版本或实际文件集合尚不能稳定映射到唯一上游制品 | 先停止许可推断；身份闭合后重新进入本基线审核 |
| **非第三方／待制品化** | 项目维护者计划自行开发并授权公开发布，但尚无可审核的稳定制品 | 不做第三方许可推断；仍须完成所有权、授权、身份和构建清单门禁 |

“明确允许”不等于无条件。MIT 仍要求保留版权和许可文本，第三方 NOTICE 仍须完整随附；本项目也仍需证明镜像的字节就是与许可证对应的官方资产。

### 1.2 候选总览

| 候选 | 结论 | 查询日的精确事实 | 主要门禁或阻断 | 回退 |
| --- | --- | --- | --- | --- |
| PowerShell 7 | **明确允许** | 官方 MIT；安装文档列出稳定版 7.6.4 的 x64／ARM64 MSI 与 ZIP | 锁定 tag、资产、架构、摘要；随附对应 `LICENSE.txt` 与 `ThirdPartyNotices.txt` | Microsoft 官方发布页动态来源 |
| 现代 .NET Desktop Runtime | **明确允许** | .NET Runtime 为 MIT；.NET 10 下载页在查询日列出 Desktop Runtime 10.0.10 的 x86／x64／ARM64 安装器与 ZIP | 先由软件依赖声明所需主版本，再固定补丁与资产；保存对应版本许可和第三方通知 | Microsoft 官方下载页动态来源 |
| PowerToys | **明确允许** | `v0.100.2` 顶层 MIT；官方 Release 于 2026-06-26 发布 x64／ARM64、用户级／机器级安装器 | 使用原始 Release 资产；随附 tag 对应 `LICENSE`、`NOTICE.md`；不得暗示 Microsoft 背书 | GitHub 官方 Release |
| Windows App SDK | **明确允许** | 上游仓库为 MIT；自包含部署会把 Framework 包内容提取到构建输出 | 实施时冻结稳定 tag、x64／ARM64 内容、可能需要的 Singleton 包、LICENSE、第三方通知和完整 BOM | 不回退为运行时联网安装；未闭合时不发布工作台制品 |
| Microsoft VC++ Redistributable | **带条件允许** | Microsoft 文档把再分发资格限定给有效 Visual Studio 许可用户和 REDIST 清单内代码 | 证明实际许可主体、版本和“随本程序／显著主要功能”条件；只分发未修改的指定对象码 | Microsoft 官方下载页动态来源 |
| DirectX End-User Runtimes (June 2010) | **带条件允许** | 官方部署文档允许开发者随应用分发应用所需的指定 CAB 与 DirectSetup 文件；下载页为 9.29.1974.1 | 确认初装工作台是否属于条款中的依赖应用；只带实际所需组件并保留 EULA 流程 | Microsoft 官方下载页的来源手动交接 |
| Eclipse Temurin Java | **带条件允许** | Temurin 二进制为 GPLv2 + Classpath Exception；查询日 Windows x64 JRE 8 为 `jdk8u502-b07` | 同时提供完整对应源码或满足 GPLv2 第 3 节的其他路径；保留许可、NOTICE、签名和精确构建关联 | Adoptium 官网／API 动态来源 |
| Bitwarden Desktop | **带条件允许** | `desktop-v2026.6.1` 桌面包声明 GPL-3.0；仓库另含禁止对外分发的 `/bitwarden_license` 商业模块 | 证明所选官方二进制实际组成不含受限商业模块或另有授权，并闭合 GPLv3 对应源码、通知和商标义务 | Bitwarden 官方 Release 动态来源 |
| TrafficMonitor | **带条件允许** | `V1.86` 采用 Anti-996 License 1.0 Draft，明示分发权但附严格劳动法条件 | 许可主体确认持续满足条件；保留完整许可和 notice，冻结 x86／x64／ARM64EC 资产 | GitHub 官方 Release 动态来源 |
| Oracle JRE 8 | **无法证明／应外部交接** | 本地候选 8u491；Oracle 查询日当前版为 8u501；8u211 以后适用 OTN | OTN 和 Oracle FAQ 明确不提供当前 Java 8 的再分发权；NFTC 不能外推到该候选 | 改用合规 Temurin，或交接到 Oracle 官网／另取书面许可 |
| .NET Framework 3.5 来源 | **无法证明／应外部交接** | Windows 10／11 离线启用要求与目标 Windows 版本匹配的原始介质来源 | 未找到允许抽取并公开镜像 `sources\sxs` 的授权；不存在通用跨构建离线包 | Windows Update，或让用户提供合法取得且匹配的 Windows 安装介质 |
| QQ | **无法证明／应外部交接** | 本地候选 9.9.32；官方页查询日显示 Windows x64 9.9.33，另有 x86／ARM64 | 官方下载页没有授予第三方公开镜像权；未取得适用于 QQ Windows 二进制的书面许可 | QQ 官网发布页的来源手动交接 |
| 微信 | **无法证明／应外部交接** | 本地候选 4.1.11 | 微信协议只授予个人、不可转让、非排他的使用许可及一个备份副本；未明示权利需腾讯书面许可 | 微信官网发布页的来源手动交接 |
| Google Chrome | **无法证明／应外部交接** | 本地候选是未固定版本的 64 位独立安装器 | Chrome 官方二进制不因 Chromium 开源而开放；Google 条款给出个人、不可转让许可并限制复制、分发 | Chrome 官网发布页的来源手动交接，或 Google 书面许可 |
| 360 压缩 | **无法证明／应外部交接** | 官网提供 `360zip_setup.exe` 官方直链并称免费 | “可免费下载／免费使用”没有授予项目公开镜像权；未找到二进制再分发条款 | 360 压缩官网发布页的来源手动交接 |
| WPS Office | **无法证明／应外部交接** | 本地候选为 x64 构建 22525；官网提供客户端下载入口 | 官网在线服务协议不是客户端再分发许可；未找到足以支持公开镜像的授权 | WPS 官网发布页的来源手动交接，或金山办公书面许可 |
| 图吧工具箱 | **无法证明／应外部交接** | 本地候选 202601.1；官网称工具箱“开源、免费”，但同时说明它集成大量第三方工具 | 未找到顶层 LICENSE、NOTICE、完整组件清单及每一组件的再分发证据；不能从项目自述外推合集权利 | 官网发布页的来源手动交接；取得完整 BOM 和逐组件授权后重新审核 |
| DirectX Repair 增强版 | **无法证明／应外部交接**，且身份未闭合 | 候选仅能确认 7Z 与解压目录为同一资源；有限检索未确认唯一作者、官方发布页或许可 | 先固定上游身份、版本、原文件名和摘要；Microsoft DirectX 条款不能覆盖第三方修复工具 | Microsoft 官方 DirectX／DISM／SFC 路径，或经确认的作者官方页交接 |
| 1Password | **无法证明／应外部交接** | 官方条款更新于 2024-09-12；个人路径限自身非商业使用，商业条款限制复制、发布、镜像和分发服务内容 | 未取得覆盖 Windows 安装资产和公开 GitHub 聚合分发的书面许可 | 1Password 官网下载页的来源手动交接 |
| 腾讯会议 | **无法证明／应外部交接** | 官方下载页查询日显示 Windows 3.44.10.457；本地候选为 3.43.21.403 系列 | 官方页证明下载入口和版本，不授予第三方公开镜像权；有限核对未取得客户端再分发许可 | 腾讯会议官网下载页的来源手动交接 |
| PotPlayer | **无法证明／应外部交接** | 官方全球页提供 32／64 位 `Latest` 安装器，但不提供稳定版本身份 | 未找到覆盖固定安装器、编解码器组合和公开镜像的上游许可；`Latest` 不能进入发行清单 | PotPlayer 官方页的来源手动交接 |
| iTunes for Windows | **无法证明／应外部交接** | Apple 官方 Windows SLA 只允许每台自有／受控设备一份副本和一个备份 | SLA 第 3 节明确禁止重新分发或再许可；一次性永久转让不适用于项目向公众提供副本 | Apple 官方下载／Microsoft Store 交接 |
| NVIDIA App | **无法证明／应外部交接** | 本地候选为 11.0.8.299；NVIDIA 驱动许可版本日期 2025-02-25 | 唯一分发例外限随 OSI 开源内核使用的软件；Windows NVIDIA App 不落入该例外，其他分发受 2.7 限制 | NVIDIA 官方驱动／NVIDIA App 页面交接 |
| Samsung Magician | **无法证明／应外部交接** | 官网查询日为 Windows 9.0.1，文件 `Samsung_Magician_Installer_Official_9.0.1.950.exe` | 安装指南自身标记 Samsung Proprietary 且不授予知识产权许可；未找到覆盖安装器的项目再分发授权 | Samsung 官方工具页交接 |
| 腾讯视频 | **无法证明／应外部交接** | 本地候选为 11.175.2212.0；官网提供 PC 客户端下载页 | 官方下载页不构成再分发授权；有限核对未取得覆盖客户端公开镜像的许可 | 腾讯视频官网下载页的来源手动交接 |
| 幕布 | **无法证明／应外部交接** | 本地候选 5.7.1 x64；官网提供 32／64 位客户端 | 服务条款只给个人、非商业、不可转让许可，并禁止复制、转载、汇编、出版或建立镜像站点 | 幕布官网客户端页的来源手动交接 |
| Epic Games Launcher | **无法证明／应外部交接** | 本地候选 `EpicInstaller-18.8.1.msi`；当前 Epic 条款明确覆盖 launchers | 许可为个人、不可转让、不可再许可，并禁止未明确允许的复制；未取得公开镜像授权 | Epic Games 官方下载页交接 |
| HS108T Pro 驱动／固件 | **身份未闭合** | 候选只有压缩包显示名，有限检索无法确认唯一厂商、型号、官方包或许可 | 固定厂商、产品、硬件 ID、固件目标、版本、架构、原文件名、摘要和官方来源后再审核 | 只向匹配设备展示经确认的厂商驱动页入口 |
| OMEN Superhub | **身份未闭合** | 候选只有 `omensuperhub-...zip`；搜索命中的是不同名称的 OMEN Gaming Hub，不能合并身份 | 先确认是否为 HP／OMEN 制品、适用型号、版本、文件内容和官方来源，再判断许可 | HP 官方支持页／Microsoft Store 的来源手动交接 |
| 自研通用网卡驱动救援工具 | **非第三方／待制品化** | 维护者计划后续提供完整 EXE 并授权随项目公开发布 | 分配稳定内容标识，记录正式产品名、版本、架构、原文件名、SHA-256、所有者、授权声明和复核日期 | 未提供合格制品时，断网救援停留在 U 盘厂商驱动导入 |
| 自研断网诊断／修复工具 | **非第三方／待制品化** | 维护者计划后续提供完整 EXE 并授权随项目公开发布 | 与另一工具分开登记稳定身份、摘要、所有者和授权，不得以一个笼统“自研工具”证据覆盖两个制品 | 未提供合格制品时只使用工作台自身基础诊断 |
| 工作台其他自包含依赖 BOM | **身份未闭合** | 技术基线只冻结 C++/WinRT、WinUI 3、无 .NET 和自包含方向，尚未冻结精确依赖集合 | 构建后生成完整 SBOM／BOM；任何未识别 DLL、MSIX、运行时、原生库、许可或 NOTICE 默认阻断 | 收缩依赖或在下一次构建重新生成并审核 BOM |

以上 32 项全部有独立行。精确身份仍未闭合的对象至少有 4 项：DirectX Repair 增强版、HS108T Pro、OMEN Superhub 和工作台其他自包含依赖 BOM；这 4 项不能因名称相似、文件可运行或搜索命中近似产品而进入许可判断捷径。

## 2. 研究口径与证据标准

### 2.1 本文把什么视为再分发

以下行为都属于本文所称的公开再分发，不因字节保持不变而自动免责：

- 把安装器、ZIP、CAB、MSI、MSIX 或其他二进制放进超大离线版；
- 把上游文件复制到项目 GitHub Release 或项目第三方包源；
- 用项目自己的归档包包住上游二进制后公开下载；
- 从上游下载后改名，再由项目服务器或 Release 提供；
- 把上游安装介质中的一部分抽出后作为项目资源提供。

只在界面中打开官网发布页并由用户在工作台外取得文件，属于项目词汇中的**来源手动交接**，不等同于项目公开托管二进制。工作台自动抓取官网文件是否符合上游网站自动访问条款是另一项审核，本文不把它当作已通过。

### 2.2 证据优先级

本次按以下顺序判断：

1. 精确产品或二进制附带的许可证、EULA、REDIST 清单；
2. 上游官方部署／再分发文档和适用产品许可条款；
3. 与精确 tag 或发布资产对应的源码许可证和第三方 NOTICE；
4. 上游官方下载页、官方发布 API 和版本／架构清单；
5. 官方 FAQ 对许可适用版本和再分发问题的解释。

搜索结果和第三方文章只用于发现官方入口，不作为授权证据。许可证授予与制品身份必须同时成立：源码仓库是开源的，不自动证明同名官方二进制、商标、编解码器或随附组件都适用同一许可。

### 2.3 本次没有采用的推断

- “免费”“开源”“绿色”“官方下载”不等于允许第三方再分发。
- 能找到固定直链、能下载、能静态识别或能安装，不等于允许公开镜像。
- 上游允许终端用户制作一个备份，不等于允许项目向无限第三方提供副本。
- 一个合集自身声称开源，不等于其中每个第三方组件都可由另一个项目再分发。
- 某个新版本采用宽松许可，不代表旧版本或另一产品分支也采用该许可。
- 未找到授权通常只支持“证据不足”；只有适用条款明确限制时，本文才记录该限制。

## 3. 所有随包候选的发布门禁

即使第 1 节标为“明确允许”，也必须逐个资产满足下表，才能申请把证据登记册状态改为 `accepted`。

| 编号 | 必须落盘的证据 | 失败结果 |
| --- | --- | --- |
| `RD-G01` | 稳定内容标识、产品、精确版本／tag、原始文件名、架构、安装范围（用户级／机器级）和包类型 | 不进入构建清单 |
| `RD-G02` | 上游官方下载页、精确资产 URL、取得日期、文件大小、项目计算的 SHA-256、可用时的上游摘要与签名／发布者事实 | 不得把“同名文件”视为已审核制品 |
| `RD-G03` | 适用于该精确版本和公开 GitHub 渠道的许可／再分发条款；记录许可主体、地域、渠道、是否必须“随本程序” | 授权范围不明即回退官网 |
| `RD-G04` | 是否允许改名、修改、抽取或重打包；本项目默认只聚合未修改、未改名的官方资产 | 未有明确依据不得转换形态 |
| `RD-G05` | 对应版本的 LICENSE、EULA、NOTICE、第三方通知及源代码义务；记录它们在发行包内的固定位置 | 归属或源码义务未闭合即阻断 |
| `RD-G06` | 上游条款原始 URL、页面／条款日期、访问日期和不可变快照或提交哈希；指定复核人和复核日期 | 只有“当前网页链接”不能长期支撑历史制品 |
| `RD-G07` | 版本与发行架构矩阵。x64 工作台需要的 x86 依赖、ARM64 原生包和仿真边界必须逐项记录 | 不以“有一个 Windows 包”替代架构证据 |
| `RD-G08` | 授权失效、版本撤回或审核未完成时的官网动态来源／来源手动交接回退 | 无回退的候选不得成为基础流程硬依赖 |
| `RD-G09` | 构建只接受证据登记册中精确资产状态为 `accepted` 的条目；未知、过期和仅候选状态默认拒绝 | 构建失败，不允许人工临时塞包 |

条款快照应保存原始内容和获取时间，但不得误称为上游永久承诺。每次升级版本、切换安装器形态、增加架构或改变托管渠道都要重新审核；“latest”链接不能作为发行清单中的稳定身份。

## 4. 明确允许

### 4.1 PowerShell 7

| 字段 | 结论 |
| --- | --- |
| 授权依据 | PowerShell 仓库 `LICENSE.txt` 为 MIT，明确允许复制、发布、分发、再许可和销售副本，条件是软件的所有副本或实质部分保留版权及许可声明。[PS-01] |
| 二进制与版本 | Microsoft 安装文档在查询日以 7.6.4 为当前稳定版示例；官方 `v7.6.4` Release 提供 x64／ARM64 的 MSI 与 ZIP，并为四个资产给出 SHA-256；Windows PowerShell 5.1 与 PowerShell 7 并存。[PS-03][PS-04] |
| 改名／重打包 | MIT 不阻止修改，但本项目没有必要制造派生发行。建议只镜像未修改、未改名的官方 Release 资产，保留上游产品名并避免暗示 Microsoft 对本项目背书。 |
| 随附义务 | 随二进制保存与该精确 tag 对应的 `LICENSE.txt` 和 `ThirdPartyNotices.txt`；第三方通知不能用仓库顶层 MIT 文本替代。[PS-01][PS-02] |
| 尚缺证据 | 7.6.4 只是查询日基线，不是永久固定版本。发布前仍要记录精确 Release 资产 URL、架构、SHA-256、签名发布者和 tag 提交。 |
| 回退 | 解析 Microsoft 官方安装文档或 GitHub 官方 Release，不由项目托管。 |

### 4.2 现代 .NET Desktop Runtime

| 字段 | 结论 |
| --- | --- |
| 授权依据 | Microsoft 说明 .NET 免费且包括商业使用；`dotnet/runtime` 和 .NET 聚合仓库采用 MIT，许可文本明确授予分发权。[DN-01][DN-02][DN-04] |
| 二进制与版本 | .NET 10 下载页和官方发布元数据在查询日列出 Desktop Runtime 10.0.10 的 x86、x64、Arm64 安装器与二进制归档；元数据为每个文件提供 URL 和 SHA-512。[DN-05][DN-07] |
| 版本选择 | “现代 .NET”不是一个可由最新版替代的单一依赖。每个软件安装项必须先声明所需主版本，再固定受支持补丁、架构和安装器／ZIP 形态。[DN-06] |
| 改名／重打包 | 只携带 Microsoft 原始资产，不改名、不拆包；若未来构建自定义 Runtime，则属于新的派生制品审核，不沿用本结论。 |
| 随附义务 | 保存与所选版本对应的 `LICENSE.TXT` 与 `THIRD-PARTY-NOTICES.TXT`，并保留第三方组件归属。[DN-02][DN-03] |
| 尚缺证据 | 候选稿尚未确定所需主版本集合。必须按 x86／x64／ARM64 和每个主版本形成资产矩阵，不能把查询日的 .NET 10 写成所有软件的默认依赖。 |
| 回退 | Microsoft 官方 .NET 下载页动态来源。 |

### 4.3 PowerToys

| 字段 | 结论 |
| --- | --- |
| 授权依据 | `v0.100.2` tag 的顶层 `LICENSE` 为 MIT，直接授予分发权。[PT-01] |
| 二进制与版本 | GitHub 官方 Release API 显示 `v0.100.2` 于 2026-06-26 发布，包含 x64／ARM64 与用户级／机器级安装器。[PT-03] |
| 改名／重打包 | 只使用官方 Release 原始资产，不改名、不替换图标、不把项目发行误写成 Microsoft 官方软件包。 |
| 随附义务 | 随附 `v0.100.2` 对应的 `LICENSE` 和完整 `NOTICE.md`；后者列出大量第三方组件，不能只保留顶层 MIT。[PT-01][PT-02] |
| 尚缺证据 | 为实际选择的安装范围、架构和文件记录精确 URL、大小、SHA-256、签名发布者与上游条款快照。升级 tag 时重新取对应 NOTICE。 |
| 回退 | GitHub 官方 Release 动态来源。 |

### 4.4 Windows App SDK

| 字段 | 结论 |
| --- | --- |
| 授权依据 | Windows App SDK 上游仓库使用 MIT，明确授予复制、发布、分发和再许可权，条件是副本或实质部分保留版权与许可声明。[WA-01] |
| 自包含事实 | Microsoft 文档说明，启用 `WindowsAppSDKSelfContained` 后，Windows App SDK Framework 包内容会被提取到构建输出并随应用部署；打包应用把依赖作为 MSIX 内容，未打包应用则把依赖复制到 EXE 旁。[WA-02] |
| 额外包边界 | 少量 API 依赖 Singleton 等额外 MSIX 包。只有实际使用这些 API 时，才可选择外部安装、条件性降级或随安装部署相应包，不能把“自包含”误写成天然只有一组 DLL。[WA-02] |
| 与 .NET 的边界 | 本项目技术基线固定为 C++/WinRT，不依赖 .NET；Microsoft 文档中的 .NET 自包含说明不构成本项目引入 .NET Runtime 的理由。 |
| 尚缺证据 | 项目尚未冻结 Windows App SDK 精确稳定版本。实施时必须锁定 tag／包版本、x64／ARM64 输出、Framework 内容、实际需要的 Singleton 包、对应 LICENSE／第三方通知和构建后完整 BOM。引用 `main` 的许可只能建立研究基线，不能作为历史制品的不可变证据。 |
| 结论 | 许可基线为明确允许，但在精确版本和构建输出闭合前，工作台自包含发行包仍未达到发布门禁。 |

## 5. 带条件允许

### 5.1 Microsoft VC++ Redistributable

| 字段 | 结论 |
| --- | --- |
| 授权主体 | Microsoft 文档明确说，只有持有有效 Visual Studio 许可的用户才能按相应 Visual Studio 许可条款再分发 Visual C++ 文件。[VC-01][VC-02] |
| 授权对象 | Visual Studio REDIST 页面把 `[VisualStudioFolder]\VC\redist` 下列出的文件作为可未修改分发的对象码，并说明从 Microsoft 下载的对应运行库仍受相同限制。[VC-03] |
| “随本程序”条件 | Visual Studio 2026 Community 条款允许把 Distributable Code 以对象码形式随开发者程序分发，但要求开发者程序为其增加显著主要功能，并附加保护 Microsoft 的下游条款；条款还包含开发者责任与赔偿要求。[VC-04] |
| 不可采用的捷径 | 单独 V14 Runtime EULA 不是本项目的一般再分发授权来源，且包含分享／发布限制；不能只因下载页公开就认定可随包。[VC-05] |
| 版本／架构 | x86、x64、ARM64 必须分别映射到所选工具集和上游永久链接；x64 设备可能仍因 32 位软件需要 x86 包，不能按工作台架构简单合并。 |
| 改名／重打包 | 只分发 REDIST 清单内、未修改且原名的对象码／官方安装器，不抽取后组合成自有运行库。 |
| P0 缺口 | 维护者需记录实际许可主体、Visual Studio 版本／版本条款、许可取得方式、适用 REDIST 清单，并确认初装工作台是否满足“本程序／显著主要功能”。在此之前保持外部来源。 |
| 回退 | Microsoft “Latest supported VC++ Redistributable”官方下载页动态来源。 |

### 5.2 DirectX End-User Runtimes (June 2010)

| 字段 | 结论 |
| --- | --- |
| 授权依据 | Microsoft 部署文档说明开发者可以把应用所需的可选 CAB、`DXSETUP.exe`、`DSETUP.dll`、`dsetup32.dll` 和 `dxdllreg_x86.cab` 组成再分发包，并随应用分发。[DX-02] |
| 精确候选 | 官方下载页列出版本 9.29.1974.1、文件 `directx_Jun2010_redist.exe`，页面日期 2024-07-15。[DX-01] |
| 条件 | 只带应用实际需要的旧版 side-by-side 组件，保留 DirectX EULA 的接受流程，按 x86／x64 需求选择 CAB；不得宣传为升级 Windows 自带 DirectX 主版本。 |
| 关键疑点 | 官方文档的授权语境是“开发者随其应用部署该应用所需组件”。初装工作台把组件安装给将来可能运行的第三方游戏，是否满足该语境尚未确认。 |
| 改名／重打包 | 只能按官方部署方式选择规定文件，不能修改 Microsoft 二进制或绕过 EULA；也不能把第三方 DirectX Repair 的文件当成官方 June 2010 授权对象。 |
| 结论 | 在维护者确认应用依赖关系并完成专业复核前，不把它视为通用维修工具箱的公开镜像授权。 |
| 回退 | 打开 Microsoft 官方下载页进行来源手动交接。 |

### 5.3 Eclipse Temurin Java

| 字段 | 结论 |
| --- | --- |
| 授权依据 | Adoptium FAQ 明确说明 Eclipse Temurin 二进制按 GNU GPLv2 with Classpath Exception 提供，可以使用、修改和分享。[TM-01][TM-02] |
| 二进制分发义务 | GPLv2 第 3 节要求二进制分发同时提供完整对应机器可读源码，或提供至少三年有效的书面要约；以指定下载位置提供对象码时，从同一位置提供等价源码访问也可满足该条路径。[TM-02] |
| 聚合边界 | 原样把 Temurin 与本项目并列放入聚合制品，不会仅因聚合把本项目整体变成 GPL；若修改、链接或制作派生版本则需重新分析。 |
| 查询日候选 | Adoptium API 在 2026-08-10 返回 Windows x64 JRE 8 `jdk8u502-b07`，文件 `OpenJDK8U-jre_x64_windows_hotspot_8u502b07.zip`，SHA-256 为 `820210e36393da3da9b2d05376dbb2c94303010a673b14209949a36f3d721aed`。[TM-03] |
| 改名／重打包 | 建议镜像未修改、未改名的官方 ZIP／MSI；保留包内许可、NOTICE、版本信息和签名材料。修改构建会显著扩大对应源码和商标审核范围。 |
| P0 缺口 | 为每个二进制锁定完整对应源码归档、构建 tag、下载位置和保留期限。推荐在同一公开下载位置同时镜像对应源码，而不是让项目承担未来三年的书面要约履行。 |
| 回退 | 使用 Adoptium 官网／API 动态来源；在源码义务未闭合时不由项目镜像。 |

### 5.4 Bitwarden Desktop

| 字段 | 结论 |
| --- | --- |
| 上游许可结构 | `desktop-v2026.6.1` 的仓库顶层说明：默认代码为 GPLv3，但 `/bitwarden_license` 目录使用 Bitwarden License；桌面包自己的 `package.json` 声明 `GPL-3.0`。[BW-01][BW-02] |
| 受限模块 | Bitwarden License 只允许在非生产环境中作内部开发与测试，并禁止把 Commercial Modules 分发、再许可或转让给第三方。[BW-03] |
| 官方资产 | 官方 Release API 显示 `desktop-v2026.6.1` 发布于 2026-06-30，提供 x64／ARM64／ia32 APPX、在线安装器、Portable 以及其他资产，并为资产提供 SHA-256。[BW-04] |
| 不能直接外推 | 仓库默认 GPL 与桌面包元数据仍不能单独证明每个官方发布二进制的实际组成没有受限商业模块；也不能省略 GPLv3 的对应源码、安装信息、许可与第三方通知分析。 |
| 条件 | 发布前由可复核的构建清单或上游书面说明证明所选精确资产不含 `/bitwarden_license` 商业模块，或项目另有覆盖公开分发的授权；同时闭合 GPLv3 对应源码、版权通知、商标指南、架构和摘要义务。 |
| 回退 | 条件未闭合时使用 Bitwarden 官方 Release／官网动态来源，不由项目镜像。 |

### 5.5 TrafficMonitor

| 字段 | 结论 |
| --- | --- |
| 授权依据 | `V1.86` 的 LICENSE 是 Anti-996 License 1.0 Draft，明示授予使用、复制、修改、制作派生作品、分发、发布和再许可权。[TR-01] |
| 非普通宽松许可 | 每份再分发或派生副本必须显著、原样展示许可证和 notice；许可主体还必须持续满足适用劳动就业法律或 Core International Labour Standards，并不得诱导员工或承包者放弃相应权利。[TR-01] |
| 官方资产 | 官方 `V1.86` Release 于 2026-03-29 发布 x86、x64、ARM64EC 的普通版与 Lite ZIP，并为每项资产提供 SHA-256。[TR-02] |
| 条件 | 维护者必须确认本项目实际许可主体能够满足并持续证明劳动条件，且接受 Draft 许可的不确定性；同时冻结普通／Lite 选择、架构、原始文件名、摘要并完整随附许可和 notice。 |
| 回退 | 主体或条件无法确认时只使用 GitHub 官方 Release 动态来源，不由项目镜像。 |

## 6. 无法证明、身份未闭合或待制品化

### 6.1 Oracle JRE 8

- 本地 `jre-8u491-windows-x64.exe` 已落后于查询日 Oracle 页面显示的 8u501（2026-07-21），因此即使许可成立也不能用“当前版”描述。[OJ-03]
- Oracle FAQ 说明 Java 8 从 8u211 起适用 Oracle Technology Network License Agreement (OTN)，并明确 OTN 不允许再分发。[OJ-02]
- OTN 授予个人、开发、测试、原型和演示等有限用途，并限制向第三方提供、分发或转让。[OJ-01]
- Oracle No-Fee Terms and Conditions 适用于其他列明版本，Oracle FAQ 明确当前 Java 8 不采用该路径；不能把新版本的许可外推到 8u491／8u501。

**结论：** 当前 Oracle JRE 8 候选不得进入公开超大离线版或项目第三方包源。优先替换为已闭合 GPL 源码义务的 Temurin；若必须使用 Oracle JRE，由用户从 Oracle 官网取得，或由维护者获得明确覆盖公开 GitHub 再分发的书面许可。

### 6.2 .NET Framework 3.5 离线来源

- Microsoft 文档说明，在受支持的 Windows 10／11 上启用 .NET Framework 3.5 时，离线源必须来自与目标 Windows 版本相同的原始安装介质；使用不匹配来源会造成不受支持、不可维护状态。[NF-01][NF-02]
- 本次未找到允许项目从 Windows 安装介质抽出 `sources\sxs` CAB 并作为公开通用资源镜像的授权。
- 技术上也不存在一个可覆盖所有受支持 Windows 构建和语言的单一通用包。

**结论：** 不建立项目通用 .NET Framework 3.5 镜像。优先使用 Windows Update；断网时让用户提供其合法取得、版本和语言匹配的 Windows 安装介质，再按 Microsoft 指引启用。

### 6.3 QQ

- QQ 官方 Windows 下载页在查询日显示 x64 9.9.33（2026-07-30），并提供 x86／ARM64 入口；本地 9.9.32 已不是页面当前 x64 版本。[QQ-01]
- 官方页证明了产品、当前版本和腾讯下载域名，但没有授予第三方把安装器放入公开项目制品的权利。
- 本次对腾讯通用服务条款和 QQ 官方入口的有限核对，没有取得明确覆盖 QQ Windows 二进制、公开 GitHub 镜像、地域和改名／重打包方式的授权。

**结论：** 未找到足以支持公开再分发的证据，需要腾讯书面许可或改为 QQ 官网发布页的来源手动交接。

### 6.4 微信

- 《腾讯微信软件许可及服务协议》2.2.1、2.3.1 至 2.3.3 只授予个人、不可转让、非排他的许可，允许非商业地在单一终端安装使用，并为使用目的制作一个备份；未明示权利须另取腾讯书面许可。[WX-01]
- 第 3.1 至 3.2 条说明软件应直接从腾讯或腾讯授权第三方取得，并提示未经授权第三方来源不受保证。

**结论：** 该协议没有支持本公开项目向第三方提供无限副本。未找到足以支持公开再分发的证据，需要腾讯书面许可或改为微信官网发布页的来源手动交接。

### 6.5 Google Chrome

- Chrome Additional Terms 说明 Chrome 可执行组件适用 Chrome 附加条款；Chrome 中按开放源代码许可证提供的部分仍按相应开源许可证处理，但这不把整个官方 Chrome 二进制变成 Chromium 开源制品。[GC-01]
- Google Terms 对软件提供个人、不可转让的许可，并明确限制复制、修改、分发、出售或出租服务或软件；除非适用的开放源代码条款或 Google 明确许可另有规定。[GC-02]
- 本地 `ChromeStandaloneSetup64.exe` 没有固定版本证据，固定文件名也不能证明取得了公开镜像授权。

**结论：** 在本次找到的适用公开条款下，未取得 Chrome 官方二进制再分发权。需要 Google 书面许可，或改为 Chrome 官网发布页的来源手动交接。不得用 Chromium 许可证替代 Chrome 审核。

### 6.6 360 压缩

- 360 压缩官网称产品免费，并提供 `https://dl.360scdn.com/360zip_setup.exe` 官方直链。[ZP-01]
- 官网没有展示允许第三方公开镜像、随另一个公开软件包提供、改名或重打包的条款。

**结论：** “免费”和“官方直链”只支持终端用户从厂商取得软件。未找到足以支持公开再分发的证据，需要 360 书面许可或改为官网发布页的来源手动交接。

### 6.7 WPS Office

- WPS 官网提供 Windows 客户端入口，并把“服务协议”链接到金山办公在线服务协议。[WP-01][WP-02]
- 在线服务协议约束账号、在线服务、数字作品等使用，并不是把 WPS Office Windows 安装器公开再分发给第三方的许可。
- 本次未找到适用于本地 `WPS_Setup_X64_22525.exe`、公开 GitHub 镜像、版本、地域和改名／重打包方式的明确授权。

**结论：** 未找到足以支持公开再分发的证据，需要金山办公书面许可或改为 WPS 官网发布页的来源手动交接。

### 6.8 图吧工具箱

- 官网把图吧工具箱描述为“开源、免费、绿色、纯净”的硬件检测工具合集，并明确说它集成大量常见硬件检测、评分和测试工具。[TB-01]
- 本次未找到可对应 202601.1 安装包的顶层 LICENSE、NOTICE、完整物料清单、各组件版本、源码链接和每项二进制再分发条款。
- 即使工具箱自身代码开放，也不能代替其中商业软件、免费软件和开源组件各自的许可；合集作者有权收集，不自动证明下游项目也有权再次镜像整包。

**结论：** 当前不能进入公开超大离线版。回退为图吧工具箱官网的来源手动交接；只有上游提供可审核 BOM、顶层许可和逐组件再分发证据后才重新评估。

### 6.9 DirectX Repair 增强版

- 候选审核稿只能确认 7Z 与解压目录是同一资源，尚未记录作者／厂商、精确版本、原始发布文件名、官方发布页、摘要或适用许可。
- 对产品名和“增强版”的有限检索只稳定命中 Microsoft 官方 DirectX 下载与支持资料，没有得到可把当前第三方候选唯一映射到作者官方制品的证据。
- Microsoft 对 DirectX End-User Runtimes 的条件性分发许可只覆盖其列明的 Microsoft 文件，不能覆盖第三方修复工具、额外 DLL、批量注册逻辑或工具自己的代码。

**结论：** 当前同时存在身份和权利缺口，不得进入任何公开制品。先以原始候选计算 SHA-256、读取不执行的静态版本／发布者信息并由维护者提供官方出处；仍不能唯一识别时，删除该携带候选，改用 Microsoft 官方 DirectX、DISM 和 SFC 路径。

### 6.10 1Password

- 1Password 统一条款最后更新于 2024-09-12。个人条款只允许为自身非商业使用显示、复制、下载或打印服务材料；其他使用被禁止。[OP-01]
- 商业条款第 4.2 节禁止客户复制、发布、镜像或以其他方式分发服务或文档的任何部分或内容。[OP-01]
- 本次未取得明确覆盖 `1PasswordSetup-latest.msixbundle`、公开 GitHub 聚合包、全球或目标地域和未修改安装资产的独立再分发许可；`latest` 文件名也不是稳定制品身份。

**结论：** 无法证明本项目有权公开再分发 1Password Windows 安装资产。使用官网来源手动交接，除非 1Password 提供覆盖精确安装资产和渠道的书面许可。

### 6.11 腾讯会议

- 官方下载页在查询日显示 Windows 64 位版本 3.44.10.457，并另有 32 位入口；本地候选仍是 3.43.21.403 系列，已经不是页面当前版本。[TCM-01]
- 下载页证明产品身份、当前版本和官方取得渠道，不授予第三方把安装器放入公开 GitHub 制品的权利。
- 对官网和官方协议入口的有限核对未取得覆盖腾讯会议 Windows 客户端、公开镜像、地域、资产形态和下游 EULA 的再分发授权。

**结论：** 证据不足，改用腾讯会议官网下载页的来源手动交接；若坚持随包，应向腾讯取得写明精确渠道、地域和未修改安装器的书面许可。

### 6.12 PotPlayer

- PotPlayer 官方全球页提供 Kakao CDN 上的 32 位 `PotPlayerSetup.exe` 和 64 位 `PotPlayerSetup64.exe`，但地址使用 `Version/Latest`，页面没有给出可冻结的版本、摘要、许可或 NOTICE。[PP-01]
- “免费播放器”和官方直链不能推出公开镜像权；播放器还可能包含或取得不同许可的编解码器，不能只按产品名称假设整个二进制许可一致。
- 本次有限核对没有找到覆盖候选 `PotPlayerSetup64.exe` 和项目公开聚合分发的上游许可。

**结论：** 使用 PotPlayer 官方页的来源手动交接。只有取得固定版本、完整组件与许可清单、精确资产摘要和公开再分发授权后才重新评估。

### 6.13 iTunes for Windows

- Apple 官方《iTunes for Windows 软件许可协议》只授予在用户拥有或控制的兼容电脑上下载、安装和使用一份副本的有限许可，并只允许制作一个机器可读备份。[IT-01]
- 第 3 节明确禁止出租、租赁、出借、重新分发或再许可 Apple Software。直接从 Apple 取得的软件仅可在不保留任何副本、完整转移全部组件和许可的前提下作一次性永久转让；这不是面向无限公众的镜像授权。[IT-01]
- 开源组件条款只作用于列明的 Open-Sourced Components，不会把包含 Apple DLL 和 Gracenote 等组件的整个 iTunes 安装包变成可再分发制品。

**结论：** iTunes 安装器不得进入项目公开制品或项目第三方包源，回退为 Apple 官方下载入口或 Microsoft Store 交接。

### 6.14 NVIDIA App

- NVIDIA 软件许可版本日期为 2025-02-25，授予不可转让、不可再许可的许可；第 2.7 节规定除明确授予外不得出售、出租、再许可、分发或转让 SOFTWARE。[NV-01]
- 唯一直接列出的分发路径限于“供按 OSI 批准开源许可分发的操作系统内核使用”的软件，并要求二进制不修改且附协议。Windows NVIDIA App 不是随开源内核使用的软件，不能套用该窄例外。[NV-01]
- 本地候选 11.0.8.299 仍需与精确官方下载资产和适用许可快照对应；公开可下载不能替代该对应关系。

**结论：** 当前不得由项目公开镜像 NVIDIA App。按检测到 NVIDIA 硬件后交接到 NVIDIA 官方页，或取得 NVIDIA 的单独书面分发授权。

### 6.15 Samsung Magician

- Samsung 官方工具页在查询日提供 Windows 9.0.1，文件名为 `Samsung_Magician_Installer_Official_9.0.1.950.exe`，并提供安装指南与开源公告。[SM-01]
- 安装指南标记 `SAMSUNG PROPRIETARY`，说明文档和信息仍为 Samsung 专有、没有据此授予知识产权许可，并禁止未经授权复制、使用或披露这些材料；指南还说明软件仅为 Samsung SSD 用户开发和分发。[SM-02]
- 上述资料可以证明产品身份和目标用户，但不构成下游项目公开镜像安装器的授权；开源公告也只覆盖列明组件，不能替代整个官方二进制的许可。

**结论：** 未找到足以支持项目公开再分发的授权。检测到 Samsung 存储后交接到 Samsung 官方工具页，或取得覆盖精确安装器的书面许可。

### 6.16 腾讯视频

- 本地候选为 `TencentVideo11.175.2212.0.exe`；腾讯视频官方页提供 PC 客户端下载入口，但本次没有取得该候选与当前官网资产、摘要和许可版本的稳定对应。[TCV-01]
- 官方下载入口和终端用户可安装不等于第三方可镜像。对官网协议入口的有限核对没有找到覆盖 Windows 客户端、项目 GitHub Release、地域和未修改资产的再分发授权。

**结论：** 保持不携带，使用腾讯视频官网下载页的来源手动交接；若未来拟进入项目第三方包源，必须先取得腾讯书面许可和精确资产证据。

### 6.17 幕布

- 幕布官方客户端下载页提供 Windows 64 位和 32 位入口；本地候选为 5.7.1 x64，但页面没有给当前下载资产提供稳定版本和摘要。[MB-01]
- 《幕布服务条款》修订于 2023-06-27、生效于 2023-07-04，只向个人用户授予非商业、不可转让、非排他且不可再许可的使用许可；未明示权利需另取书面许可。[MB-02]
- 条款第 7.3 节还禁止未经书面许可对软件及相关服务进行复制、转载、汇编、发表、出版或建立镜像站点。[MB-02]

**结论：** 现有公开条款不支持本项目镜像幕布安装器。使用幕布官网下载页的来源手动交接，或取得十里湖科技书面许可。

### 6.18 Epic Games Launcher

- Epic 当前服务条款最后更新于 2026-05-27，并明确把 launchers 纳入受条款约束的 Licensed Products。[EG-01]
- 第 3 节授予的许可是个人、不可转让、不可再许可且可撤销；条款禁止以未明确列为允许的方式复制、展示或使用 Licensed Products，并保留未明示授予的全部权利。[EG-01]
- 本地 `EpicInstaller-18.8.1.msi` 的版本化文件名和官方 Launcher 下载页只能证明候选与取得入口，不能建立项目公开镜像权。[EG-02]

**结论：** 不携带或托管 Epic Games Launcher，改用 Epic 官方下载页交接；只有 Epic 书面授权后才重新评估。

### 6.19 HS108T Pro 驱动／固件

- 候选审核稿只有 `HS108T Pro驱动_固件...zip` 显示名，没有厂商、官网、硬件 ID、精确型号、固件目标、版本、原文件名、摘要或签名发布者。
- 对完整名称的有限检索没有得到可复核的官方产品／支持页，搜索结果与候选身份无关，不能把近似型号或第三方下载页当成证据。
- 固件一旦错配可能使硬件不可用，因此“只对匹配硬件展示”也必须建立在硬件 ID 和官方适用型号证据上，不能只匹配人类可见名称。

**结论：** 身份未闭合，许可判断暂不成立。维护者必须先提供厂商官方页与原始包；否则从发行资源清单移除，只保留经确认的厂商支持入口。

### 6.20 OMEN Superhub

- 候选审核稿只有 `omensuperhub-...zip`，内容与适用型号已被标记为待确认。
- 有限检索稳定命中 HP 的 **OMEN Gaming Hub**，但没有确认名为 **OMEN Superhub** 的同一官方制品；两个名称不能在没有文件元数据、产品代码和厂商证据时合并。

**结论：** 身份未闭合，不得携带。先以不运行文件的方式固定内部文件清单、版本资源、发布者、SHA-256 和适用型号，再由 HP 官方支持页交叉确认；无法确认时删除候选并交接到 HP 官方支持页／Microsoft Store。

### 6.21 两个自研救援伴随工具

这两个程序不是第三方许可研究对象，但“维护者后续提供并授权”仍不足以让未知 EXE 进入公开制品。它们必须作为两个独立制品分别登记，不能共用一个模糊名称或一条摘要：

| 必填字段 | 自研通用网卡驱动救援工具 | 自研断网诊断／修复工具 |
| --- | --- | --- |
| 稳定内容标识 | 独立、不可复用 | 独立、不可复用 |
| 正式产品名与版本 | 必填 | 必填 |
| 支持架构 | x64／ARM64／其他逐项声明 | x64／ARM64／其他逐项声明 |
| 原始文件名与 SHA-256 | 必填 | 必填 |
| 所有者／权利主体 | 必填 | 必填 |
| 公开再分发授权 | 权利主体明确授权项目、渠道、地域、期限与未修改资产 | 权利主体明确授权项目、渠道、地域、期限与未修改资产 |
| 内含第三方内容声明 | 由工具所有者声明其有权分发全部内含驱动、库和资源；有例外时提供清单 | 由工具所有者声明其有权分发全部内含库和资源；有例外时提供清单 |
| 复核责任人与日期 | 必填 | 必填 |

候选审核稿既定的“不定义、审查或测试其内部实现”不等于可以省略制品身份和权利声明；它只把内部质量责任留给工具维护者。任一工具未完成上述字段时，只阻断该工具进入断网救援版和超大离线版，不得用另一工具的证据替代。

### 6.22 工作台其他自包含依赖 BOM

- 技术基线只冻结 C++/WinRT、WinUI 3、C++26、无 .NET、自包含和 x64／ARM64 方向，没有冻结编译器、Windows SDK、Windows App SDK 或其他库的精确版本。
- 因此“工作台自身依赖”当前仍是未识别集合。Windows App SDK 的 MIT 结论只覆盖与精确版本对应的 Windows App SDK 内容，不能覆盖编译器运行库、第三方静态／动态库、安装器引擎、生成资源或额外 MSIX。
- 每个精确发行架构和打包形态必须从实际构建输出生成机器可读 SBOM／BOM，至少记录组件名、版本、文件、来源包、架构、SHA-256、许可证、NOTICE／源码义务和被哪个工作台模块引入；再与上一制品做依赖差分。
- 未归属文件、未知许可证、缺失 NOTICE、构建时浮动下载或无法对应源码／包版本的组件默认阻断，不允许用“它是工作台的一部分”跳过第三方审核。

**结论：** 精确 BOM 生成并逐组件审核前，只能证明架构方向，不能证明任何完整工作台发行制品已经满足再分发门禁。

## 7. 优先级

### 7.1 P0：任何公开制品生成前

| 编号 | 阻断项 | 完成证据 |
| --- | --- | --- |
| `P0-RD-01` | 第 1.2 节所有标为“无法证明／应外部交接”的候选不得出现在当前公开制品或项目第三方包源 | 构建清单逐项排除，或每项取得明确覆盖公开 GitHub 再分发的上游书面许可并重新审核 |
| `P0-RD-02` | Microsoft VC++ Redistributable 不得仅凭公开下载链接随包 | 许可主体、Visual Studio 许可版本、REDIST 清单、“随本程序”资格和精确资产记录获审核 |
| `P0-RD-03` | Temurin 不得只镜像二进制 | 每个构建的完整对应源码、GPL 履行路径、保留位置和期限闭合 |
| `P0-RD-04` | .NET Framework 3.5 不得作为通用独立资源镜像 | 构建清单排除；产品回退固定为 Windows Update 或匹配安装介质交接 |
| `P0-RD-05` | 非 `accepted` 候选不能因“超大离线版携带”或人工复制而混入制品 | 构建默认拒绝，并能输出精确缺失证据项 |
| `P0-RD-06` | DirectX Repair 增强版、HS108T Pro、OMEN Superhub 和其他自包含依赖不得按模糊名称进入清单 | 每项闭合唯一身份；工作台构建输出生成逐文件 BOM，未知组件为零 |
| `P0-RD-07` | Bitwarden 不得只凭仓库默认 GPL 随包；TrafficMonitor 不得按普通宽松许可处理 | Bitwarden 精确二进制组成和 GPLv3 义务闭合；TrafficMonitor 许可主体与 Anti-996 条件获复核 |
| `P0-RD-08` | 两个自研救援伴随工具不得共用一条笼统授权或以未知 EXE 进入制品 | 两个稳定内容标识、产品名、版本、架构、原文件名、SHA-256、所有者／授权声明和复核日期分别完整 |
| `P0-RD-09` | Windows App SDK 的 MIT 许可不得替代完整工作台制品审核 | 精确稳定版本、x64／ARM64 内容、可选 Singleton 包、LICENSE／NOTICE 和实际构建 BOM 逐项闭合 |

### 7.2 P1：首个超大离线版候选冻结前

| 编号 | 待办 | 完成证据 |
| --- | --- | --- |
| `P1-RD-01` | 固定 PowerShell、.NET Desktop Runtime、PowerToys 和 Windows App SDK 资产矩阵 | 精确版本／tag、架构、安装范围、URL、SHA-256、签名发布者、LICENSE、NOTICE 和条款快照 |
| `P1-RD-02` | 判断 DirectX 的“随应用所需组件”是否适用于初装工作台 | 维护者明确产品依赖解释并经专业复核；否则保持来源手动交接 |
| `P1-RD-03` | 为所有已允许资产定义改名／重打包政策 | 默认仅原始文件聚合；任何例外都有逐项授权和重新验签／摘要流程 |
| `P1-RD-04` | 定义条款复核和下架节奏 | 责任人、复核日期、上游条款变化检测、紧急移除和历史制品处理规则 |
| `P1-RD-05` | 固定 Bitwarden 官方资产与对应源码／组件证明 | Release 资产、SHA-256、对应源码、构建清单、GPLv3 履行和不含受限商业模块的证据形成同一记录 |
| `P1-RD-06` | 固定 TrafficMonitor 资产和许可主体复核 | 普通／Lite、架构、tag、摘要、完整 Anti-996 License／notice 和主体条件复核记录 |

## 8. 待维护者决定

这些是 `grill-with-docs` 设计树中仍开放的决策。本文只给推荐答案，不替维护者修改正式规格、ADR 或证据登记册。

**REDIST-D1：超大离线版是否允许因权利门禁而少带已经列入候选稿的软件？**

推荐：允许。把超大离线版定义为“当前已取得权利且证据通过的最大离线集合”，而不是对每个候选的携带承诺；发布权利门禁应高于候选清单意图。

**REDIST-D2：VC++ 是采用项目再分发，还是保持 Microsoft 官方动态来源？**

推荐：在维护者能够证明有效 Visual Studio 许可主体及“随本程序”资格前保持官方动态来源。不要把个人安装 Visual Studio 或能访问下载链接当作项目许可。

**REDIST-D3：Temurin 采用哪条 GPLv2 源码履行路径？**

推荐：在与二进制相同的公开下载位置提供精确对应源码归档，并保存构建 tag 和摘要。相较三年书面要约，这条路径更容易由自动门禁持续验证。

**REDIST-D4：DirectX June 2010 是工作台自身依赖，还是给未来第三方应用准备的通用组件？**

推荐：按后者理解时先不随包；只有专业复核确认该部署场景落入 Microsoft 授权语境后才转为条件性资产。

**REDIST-D5：当前无法证明再分发权的商业／专有软件是否值得逐家申请书面许可？**

推荐：Oracle JRE、QQ、微信、Chrome、360 压缩、WPS、图吧工具箱、1Password、腾讯会议、PotPlayer、iTunes、NVIDIA App、Samsung Magician、腾讯视频、幕布和 Epic Games Launcher 在首版统一采用来源手动交接。只有厂商回函明确写明公开 GitHub 项目、全球或目标地域、未修改安装器、允许随聚合包和允许项目镜像时，才逐项恢复随包评估。

**REDIST-D6：现代 .NET 与 PowerShell 的“最新版”如何进入离线制品？**

推荐：离线制品从不保存“latest”语义。软件目录先声明依赖主版本，发行清单再冻结补丁、架构和精确资产；在线路径可以另行解析当前稳定版。

**REDIST-D7：身份未闭合候选在首版冻结时如何处理？**

推荐：DirectX Repair 增强版、HS108T Pro、OMEN Superhub 和工作台其他自包含依赖只要仍缺唯一身份，就默认从公开制品排除；不以相似名称、可运行、厂商可能性或人工确认替代证据。身份后来闭合时按新候选重新审核，不追认旧文件。

**REDIST-D8：Bitwarden 是否在首版承担精确二进制组成和 GPLv3 履行成本？**

推荐：首版先采用官方来源手动交接。只有能生成或取得所选官方资产的可复核组件清单、证明不含受限商业模块，并把对应源码与许可义务纳入自动门禁时，才转为带条件携带。

**REDIST-D9：项目是否接受 TrafficMonitor 的 Anti-996 License 1.0 Draft 条件？**

推荐：在维护者明确实际许可主体、确认能够持续满足全部劳动条件并接受 Draft 许可不确定性前，不由项目镜像；如果不愿长期维护这项主体合规证据，就永久使用官方 Release 交接。

**REDIST-D10：两个自研救援伴随工具采用什么最小权利证据？**

推荐：每个工具各自保存由权利主体作出的公开再分发声明，并包含稳定内容标识、产品名、版本、架构、原文件名、SHA-256、渠道、地域、期限、内含第三方内容权利声明、复核人和日期；任何字段不得由另一个工具继承。

**REDIST-D11：工作台自包含依赖是否采用“未知即阻断”的 BOM 门禁？**

推荐：采用。对每个架构和打包形态从实际构建输出生成 SBOM／BOM，未归属文件、未知许可、缺失 NOTICE 或浮动依赖直接阻断发布；Windows App SDK 的 MIT 结论不能豁免其他依赖。

## 9. 官方来源

除非另有说明，所有页面最后访问日期均为 2026-08-10。页面未显示稳定日期时如实记为“未标注”，发布门禁仍应保存实际页面快照。

### 9.1 明确允许候选

| 编号 | 官方来源 | 页面／条款日期 | 用途 |
| --- | --- | --- | --- |
| PS-01 | [PowerShell `v7.6.4` `LICENSE.txt`](https://raw.githubusercontent.com/PowerShell/PowerShell/v7.6.4/LICENSE.txt) | tag `v7.6.4` | MIT 分发授予 |
| PS-02 | [PowerShell `v7.6.4` `ThirdPartyNotices.txt`](https://raw.githubusercontent.com/PowerShell/PowerShell/v7.6.4/ThirdPartyNotices.txt) | tag `v7.6.4` | 第三方归属 |
| PS-03 | [Installing PowerShell on Windows](https://learn.microsoft.com/en-us/powershell/scripting/install/installing-powershell-on-windows?view=powershell-7.6) | 文档视图 7.6；本次采集未记录页面更新日 | 稳定版本、架构和资产形态 |
| PS-04 | [PowerShell `v7.6.4` Release API](https://api.github.com/repos/PowerShell/PowerShell/releases/tags/v7.6.4) | 发布于 2026-07-20 | x64／ARM64 MSI、ZIP、大小与 SHA-256 |
| DN-01 | [.NET is free](https://dotnet.microsoft.com/en-us/platform/free) | 未标注 | 免费／商业使用说明和开源许可入口 |
| DN-02 | [.NET Runtime `v10.0.10` `LICENSE.TXT`](https://raw.githubusercontent.com/dotnet/runtime/v10.0.10/LICENSE.TXT) | tag `v10.0.10` | MIT 分发授予 |
| DN-03 | [.NET Runtime `v10.0.10` `THIRD-PARTY-NOTICES.TXT`](https://raw.githubusercontent.com/dotnet/runtime/v10.0.10/THIRD-PARTY-NOTICES.TXT) | tag `v10.0.10` | 第三方归属 |
| DN-04 | [.NET 聚合仓库 `LICENSE.TXT`](https://raw.githubusercontent.com/dotnet/core/main/LICENSE.TXT) | `main` 查询日内容 | .NET 顶层 MIT 许可交叉确认 |
| DN-05 | [Download .NET 10.0](https://dotnet.microsoft.com/en-us/download/dotnet/10.0) | 查询日显示 10.0.10 | Desktop Runtime 版本和 x86／x64／Arm64 资产 |
| DN-06 | [Install .NET on Windows](https://learn.microsoft.com/en-us/dotnet/core/install/windows) | 本次采集未记录页面更新日 | 安装形态与主版本边界 |
| DN-07 | [.NET 10 官方发布元数据](https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/10.0/releases.json) | 10.0.10 发布于 2026-07-14 | Desktop Runtime 精确 URL、架构和 SHA-512 |
| PT-01 | [PowerToys `v0.100.2` LICENSE](https://raw.githubusercontent.com/microsoft/PowerToys/v0.100.2/LICENSE) | tag `v0.100.2` | MIT 分发授予 |
| PT-02 | [PowerToys `v0.100.2` NOTICE](https://raw.githubusercontent.com/microsoft/PowerToys/v0.100.2/NOTICE.md) | tag `v0.100.2` | 第三方归属 |
| PT-03 | [PowerToys `v0.100.2` Release API](https://api.github.com/repos/microsoft/PowerToys/releases/tags/v0.100.2) | 发布于 2026-06-26 | 官方资产、架构和安装范围 |
| WA-01 | [Windows App SDK LICENSE](https://raw.githubusercontent.com/microsoft/WindowsAppSDK/main/LICENSE) | `main` 查询日内容；发布时必须替换为精确 tag | MIT 分发授予 |
| WA-02 | [Windows App SDK self-contained deployment guide](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/self-contained-deploy/deploy-self-contained-apps) | 本次采集未记录页面更新日 | Framework 内容提取、打包／未打包输出和 Singleton 依赖边界 |

### 9.2 带条件允许候选

| 编号 | 官方来源 | 页面／条款日期 | 用途 |
| --- | --- | --- | --- |
| VC-01 | [Redistributing Visual C++ Files](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-180) | Visual Studio 2026 文档视图；本次采集未记录更新日 | 有效 Visual Studio 许可前提 |
| VC-02 | [Latest supported Visual C++ Redistributable downloads](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-180) | Visual Studio 2026 文档视图 | x86／x64／ARM64 官方入口和许可提醒 |
| VC-03 | [Visual Studio 2026 Redistribution](https://learn.microsoft.com/en-us/visualstudio/releases/vs18/redistribution) | Visual Studio 2026 | REDIST 清单和未修改对象码范围 |
| VC-04 | [Microsoft Visual Studio Community 2026 License Terms](https://visualstudio.microsoft.com/license-terms/vs2026-ga-community/) | VS 2026 GA 条款 | “随本程序”、显著主要功能、下游条款和责任 |
| VC-05 | [Microsoft Visual C++ v14 Redistributable Runtime License](https://aka.ms/VCRedistLicense) | 本次下载的官方 DOCX 未标注稳定网页日期 | 证明独立 Runtime EULA 不能替代 VS 再分发授予 |
| DX-01 | [DirectX End-User Runtimes (June 2010)](https://www.microsoft.com/en-us/download/details.aspx?id=8109) | 页面日期 2024-07-15 | 版本、原始文件名和官方下载 |
| DX-02 | [DirectX Installation for Game Developers](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/directx-setup-for-game-developers) | 本次采集未记录页面更新日 | 可再分发文件集合和部署条件 |
| TM-01 | [Eclipse Temurin FAQ](https://adoptium.net/docs/faq/) | 未标注 | Temurin 二进制许可说明 |
| TM-02 | [GNU GPLv2 with Classpath Exception](https://openjdk.org/legal/gplv2+ce.html) | GPLv2：1991；Classpath Exception：未单列页面日期 | 分发、对应源码和聚合边界 |
| TM-03 | [Adoptium API：最新 Windows x64 JRE 8](https://api.adoptium.net/v3/assets/latest/8/hotspot?architecture=x64&image_type=jre&os=windows&vendor=eclipse) | API 查询于 2026-08-10 | 精确构建、资产与 SHA-256 |
| BW-01 | [Bitwarden clients `desktop-v2026.6.1` LICENSE](https://raw.githubusercontent.com/bitwarden/clients/desktop-v2026.6.1/LICENSE.txt) | tag `desktop-v2026.6.1` | 默认 GPLv3 与 `/bitwarden_license` 边界 |
| BW-02 | [Bitwarden Desktop `package.json`](https://raw.githubusercontent.com/bitwarden/clients/desktop-v2026.6.1/apps/desktop/package.json) | tag `desktop-v2026.6.1` | 桌面包版本与 `GPL-3.0` 声明 |
| BW-03 | [Bitwarden License v1.0](https://raw.githubusercontent.com/bitwarden/clients/desktop-v2026.6.1/LICENSE_BITWARDEN.txt) | 版本 1，2020-09-04；取自精确 tag | 商业模块只限内部开发／测试且禁止分发 |
| BW-04 | [Bitwarden `desktop-v2026.6.1` Release API](https://api.github.com/repos/bitwarden/clients/releases/tags/desktop-v2026.6.1) | 发布于 2026-06-30 | Windows 资产、架构、大小与 SHA-256 |
| TR-01 | [TrafficMonitor `V1.86` LICENSE](https://raw.githubusercontent.com/zhongyang219/TrafficMonitor/V1.86/LICENSE) | tag `V1.86`；Anti-996 License 1.0 Draft | 分发授予、原样展示和劳动条件 |
| TR-02 | [TrafficMonitor `V1.86` Release API](https://api.github.com/repos/zhongyang219/TrafficMonitor/releases/tags/V1.86) | 发布于 2026-03-29 | x86／x64／ARM64EC、普通／Lite 资产与 SHA-256 |

### 9.3 无法证明或应外部交接候选

| 编号 | 官方来源 | 页面／条款日期 | 用途 |
| --- | --- | --- | --- |
| OJ-01 | [Oracle Technology Network License Agreement for Oracle Java SE](https://www.oracle.com/downloads/licenses/javase-license1.html) | 更新于 2019-04-10 | Java 8 当前许可范围和再分发限制 |
| OJ-02 | [Oracle JDK License General FAQs](https://www.oracle.com/java/technologies/javase/jdk-faqs.html) | 更新于 2025-09-16 | 8u211 后许可适用与 OTN 不允许再分发 |
| OJ-03 | [Java Downloads for All Operating Systems](https://www.java.com/en/download/manual.jsp) | 查询日显示 8u501，发布日期 2026-07-21 | 当前 Java 8 版本 |
| NF-01 | [Install .NET Framework 3.5 on Windows](https://learn.microsoft.com/en-us/dotnet/framework/install/dotnet-35-windows) | 本次采集未记录页面更新日 | Windows 功能和匹配介质要求 |
| NF-02 | [Deploy .NET Framework 3.5 by using DISM](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/deploy-net-framework-35-by-using-deployment-image-servicing-and-management--dism?view=windows-11) | Windows 11 文档视图 | `sources\sxs` 版本匹配和部署限制 |
| QQ-01 | [QQ Windows 版下载](https://im.qq.com/pcqq) | 页面显示 x64 9.9.33，2026-07-30 | 官方版本、架构和下载域名；不构成再分发授权 |
| WX-01 | [腾讯微信软件许可及服务协议](https://weixin.qq.com/agreement/service_agreement?lang=zh_CN) | 页面未标注更新时间 | 个人许可、备份、未明示权利和获取渠道 |
| GC-01 | [Google Chrome and ChromeOS Additional Terms of Service](https://www.google.com/chrome/terms/) | Last modified 2025-09-30 | Chrome 二进制与开源组件许可边界 |
| GC-02 | [Google Terms of Service](https://policies.google.com/terms?hl=en-US) | Effective 2026-07-30 | 软件个人许可及复制／分发限制 |
| ZP-01 | [360 压缩官网](https://yasuo.360.cn/) | 页面未标注稳定条款日期 | 官方下载入口与“免费”产品描述 |
| WP-01 | [WPS 官网](https://www.wps.cn/) | 页面未标注稳定条款日期 | 官方客户端入口 |
| WP-02 | [金山办公在线服务协议](https://www.wps.cn/privacy/full_account/) | 更新于 2023-04-15 | 证明官网所链协议范围是在线服务，不是客户端再分发授予 |
| TB-01 | [图吧工具箱官网](https://www.tbtool.cn/) | 页面未标注稳定条款日期 | “开源、免费”自述及第三方工具合集性质 |
| OP-01 | [1Password Terms of Service](https://1password.com/legal/terms-of-service/) | 最后更新于 2024-09-12 | 个人非商业使用范围与商业条款 4.2 的复制／发布／镜像／分发限制 |
| TCM-01 | [腾讯会议下载中心](https://meeting.tencent.com/download/) | 查询日显示 Windows 3.44.10.457；页面未标注稳定日期 | 官方版本、32／64 位入口；不构成再分发授权 |
| PP-01 | [PotPlayer 官方全球页](https://potplayer.tv/) | 页面未标注版本或稳定日期 | Kakao CDN 32／64 位 `Latest` 安装器；不构成再分发授权 |
| IT-01 | [Apple iTunes for Windows Software License Agreement](https://www.apple.com/legal/sla/docs/iTunesWindows.pdf) | 官方 PDF 元数据显示 2024-12-10；正文未标稳定修订日 | 单机副本、备份、禁止重新分发和一次性转让边界 |
| NV-01 | [License For Customer Use of NVIDIA Software](https://www.nvidia.com/en-us/drivers/nvidia-license/) | 版本日期 2025-02-25 | OSI 开源内核窄例外与其他分发限制 |
| SM-01 | [Samsung Consumer Storage Tools](https://semiconductor.samsung.com/consumer-storage/support/tools/) | 查询日显示 Magician 9.0.1 | 官方版本、文件名、指南与开源公告入口 |
| SM-02 | [Samsung Magician 9 Installation Guide](https://download.semiconductor.samsung.com/resources/software-resources/Samsung_Magician_9_0_0_Installation_Guide.pdf) | Revision 1.9，2025-12 | 专有声明、无默示知识产权许可和 Samsung SSD 用户边界 |
| TCV-01 | [腾讯视频官方客户端下载](https://v.qq.com/download.html) | 页面未提供可稳定引用的当前资产版本 | PC 客户端官方入口；不构成再分发授权 |
| MB-01 | [幕布客户端下载](https://mubu.com/apps) | 页面未标注稳定版本或日期 | Windows 32／64 位官方入口 |
| MB-02 | [幕布服务条款](https://mubu.com/agreement) | 修订于 2023-06-27，生效于 2023-07-04 | 个人非商业不可转让许可及复制／镜像限制 |
| EG-01 | [Epic Games Terms of Service](https://www.epicgames.com/site/en-US/tos) | 最后更新于 2026-05-27 | Launcher 适用范围、个人不可转让／不可再许可和复制限制 |
| EG-02 | [Epic Games Launcher 官方下载页](https://store.epicgames.com/en-US/download) | 页面未标注稳定版本或日期 | 官方取得入口；不构成再分发授权 |

## 10. 方法限制与访问失败

- 先运行了 `agent-reach doctor --json`。Jina Reader 可用；Exa 后端因本机缺少 `mcporter` 无法调用。
- Jina Reader 读取 Google 搜索页和 Chrome 条款页时返回过 403；Chrome 条款随后通过官方原始 URL 直接读取。搜索失败不被当作条款不存在的证据。
- 部分 GitHub HTML 页面遇到限流；许可证使用 `raw.githubusercontent.com`，Release 元数据使用 GitHub 官方 API。API 只证明发布资产，不代替许可证。
- Bing 仅用于发现官方入口和核对 DirectX Repair、HS108T Pro、OMEN Superhub、腾讯会议、腾讯视频、PotPlayer 与幕布等候选身份；没有把搜索摘要作为许可或身份依据。限定检索后仍无官方映射或授权的项目按证据不足停止，没有无限泛搜。
- VC++ 官方 Runtime 许可 DOCX 下载到 `/tmp` 后只用系统文本工具读取；没有执行其中任何二进制。临时文件没有加入仓库或发行物。
- Samsung Magician 安装指南和 Apple iTunes Windows SLA 只通过网页文本提取读取，没有运行软件；PDF 没有加入仓库或发行物。
- 没有运行、上传、修改或重新签名任何候选安装器，也没有验证本地候选的实际 Authenticode 链；本文列出的上游摘要仅限官方 API 已提供的 Temurin、Bitwarden 和 TrafficMonitor 精确资产。
- 本文没有评估私下签署的 OEM／企业／渠道协议、商标许可、出口管制、专利、税务或各司法辖区强制性例外。若维护者已经拥有单独书面协议，应按该协议的主体、期限、地域、渠道和制品范围另行登记。
- 公开条款会变化。本文的 URL 和访问日期只能建立查询日基线；正式发布必须保存精确上游条款、LICENSE、NOTICE 和资产的不可变快照，并由维护者或专业人士复核。
