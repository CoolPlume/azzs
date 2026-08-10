# 传统 Windows 安装器候选研究

- 状态：研究结论；尚未选择安装器，也未形成版本锁定
- 研究日期：2026-08-10（北京时间）
- 适用范围：初装工作台安装版的传统 Windows 安装器候选、生命周期语义与 P0 样机标准
- 比较对象：WiX Toolset 7 的 MSI、WiX Toolset 7 的 Burn EXE + MSI、Inno Setup 7、NSIS 3
- 非目标：不编写产品代码或安装脚本，不修改规格、ADR、事项、`CONTEXT.md` 或发行制品矩阵
- 法律边界：许可段落只记录上游文本与项目需要确认的义务，不构成法律意见

## 1. 结论摘要

1. **现在不能直接选型。** 当前至少还有安装器本体是否必须原生 ARM64、VC Runtime 如何闭合、普通卸载保留哪些 ProgramData 状态、是否允许降级、WiX 7 OSMF 条款是否可接受、超大离线版是否需要安装形态，以及 ARM64 设备上既有 x64 安装版如何迁移等上游决定。任何一个答案变化，都可能改变首选候选。
2. **WiX MSI 与 Burn 必须分成两个候选。** MSI 是 Windows Installer 数据库与生命周期引擎；Burn 是能串联 MSI、MSP、MSU、EXE 等包的引导器。Burn 不会把任意 EXE 前置依赖自动变成一个 MSI 事务。[W05][W08]
3. **非决定性的首轮样机顺序是：WiX MSI -> 必要时 Burn + MSI -> Inno Setup；NSIS 只保留为脚本自由度和体积对照。** WiX MSI 最适合先验证标准升级、修复和回滚语义；只有选定 VC Redistributable 等 EXE 前置依赖时，Burn 才产生足够收益；Inno 是较轻的脚本式基线；NSIS 的正式发行物仍以 x86 工具和 stub 为主，生命周期责任也最依赖自写脚本。
4. **“ARM64 支持”必须拆成三件事。** 应用负载是否原生 ARM64、安装器执行路径是否原生 ARM64、构建工具是否能生成 ARM64 制品不是同一结论。MSI 平台正式支持 Arm64，但每个 MSI 只能声明一个平台；WiX 7 官方 SDK 包含原生 ARM64 Burn engine，但尚无本文可确认的真实 ARM64 runtime E2E；Inno 可部署 ARM64 负载但 Setup 本身只有 x86/x64；NSIS 3.12 正式 ZIP 中的编译器、可执行压缩 stub 和打包插件 DLL 均为 PE32 x86。[M01][M02][W11][W12][W19][W20][I04][N02]
5. **传统安装器可以不签名构建，但不等于可以顺畅安装。** 项目现行 [ADR-0005](../adr/0005-unsigned-releases.md) 允许继续做未签名 EXE/MSI；SmartScreen、UAC 的 `Publisher not verified (unsigned)` 黄色提升提示、Smart App Control 和企业策略仍可能警告或阻止，每个真实 GitHub Release 资产都必须单独实测。[M10][M22]
6. **任何候选都不能替应用证明状态安全。** MSI rollback、Inno 的安装期回滚或 Burn rollback 都不能自动证明 ProgramData 中的恢复记录、历史、草稿、缓存与状态模式在升级、降级、卸载和新版启动失败后保持正确。[M07][M08][I09]
7. **安装器只包装已经闭合的 WinUI 3 文件集。** `WindowsAppSDKSelfContained=true` 不证明 VC Runtime 已闭合。样机必须在无 Visual Studio、无 Windows App SDK、无候选 VC Runtime 的干净 x64/ARM64 系统上离线安装并启动；否则不能满足“目标系统无需预装工作台运行时”。[P01][M12]
8. **安装事务成功不等于一键更新成功。** 传统安装器只能报告其安装阶段；更新资产真实性、新版启动健康确认、事务提交后的 N-1 恢复和状态模式兼容仍属于应用更新合同，不能在候选评分中记成 MSI/Burn/Inno/NSIS 已自动解决。[P02][P04]

## 2. 项目边界与比较口径

### 2.1 已确认的项目约束

- [ADR-0002](../adr/0002-github-distribution.md)：通过 GitHub Releases 同时提供便携版与安装版。
- [ADR-0004](../adr/0004-shared-device-state.md)：便携版与安装版共享设备级恢复记录，普通卸载不得删除已生效设置或恢复记录。
- [ADR-0005](../adr/0005-unsigned-releases.md)：便携版与安装版长期不做 Windows 代码签名，也不随发行版提供 SHA-256 或源码提交关联。
- [ADR-0006](../adr/0006-opt-in-self-update.md)：更新或新版启动失败时必须恢复上一版本，且不得破坏设备级恢复记录。
- [技术基线](../engineering/technology-baseline.md)：Windows 10 22H2 为最低目标，同时提供 x64 与 ARM64 自包含发行，目标系统不依赖 .NET Runtime。
- [事项 21](../../.scratch/windows-initial-setup-workbench/issues/21-release-artifacts.md)：安装版与便携版都要在无预装工作台运行时的机器上启动，卸载保留恢复记录，发布说明披露未签名风险。
- [首版规格](../../.scratch/windows-initial-setup-workbench/spec.md) `REL-06`：ARM64 Windows 应使用 ARM64 工作台包；现有文档尚未定义已经安装 x64 版本时怎样迁移。
- [特权下载与自更新安全研究](elevated-download-execution-security.md)：现行未签名、无独立摘要的一键更新缺少真实性根；回退不能替代真实性验证。[P04]

本文只消费这些约束，不替它们补答案。特别是，现有“x64 与 ARM64 工作台架构”尚未明确要求**安装器执行路径**也必须原生 ARM64。

### 2.2 三种安装模型

| 模型 | 本文定义 | 生命周期主要所有者 | 代表候选 |
| --- | --- | --- | --- |
| MSI | Windows Installer 处理的架构特定安装数据库 | Windows Installer 标准动作 + 作者声明的组件、升级和自定义动作 | WiX 7 MSI |
| 链式引导 EXE | EXE 检测、缓存并按计划安装一个或多个包 | Burn 负责链；链内 MSI 仍由 Windows Installer 负责 | WiX 7 Burn + MSI |
| 脚本式 EXE | 安装、升级、卸载和异常处理由脚本执行 | 安装器运行时 + 项目脚本 | Inno Setup、NSIS |

这里的“安装器”只指工作台自身的发行安装器，不指工作台以后下载和启动的第三方软件安装程序。

### 2.3 待确认领域词汇

以下术语只用于本文拆开含义，尚未写入 `CONTEXT.md`：

- **应用负载架构**：最终安装的工作台 EXE/DLL 的机器架构。
- **安装器进程架构**：用户启动的 MSI 执行路径或 bootstrapper/Setup EXE 自身的机器架构。
- **生命周期引擎**：定义安装、升级、修复、卸载和失败恢复语义的执行机制。
- **程序文件**：可由相同版本发行制品重建的工作台二进制、资源和静态目录内容。
- **设备状态**：位于稳定设备根、跨发行形态与应用重启保留的恢复记录、历史、当前目录、草稿等数据；具体分类以[状态研究](windows-state-persistence-and-recovery.md)为准。
- **普通卸载**：从 Windows 应用列表移除工作台程序文件，但不等同于“删除本机数据”。
- **前置依赖闭合**：安装后首次离线启动所需的 Windows App SDK、VC Runtime 和其他非系统文件均已满足。
- **安装事务成功**：安装器完成其文件、登记、链或脚本步骤并返回成功；不表示新版工作台已通过启动健康确认。
- **跨发行架构迁移**：同一设备从 x64 安装版切换到 ARM64 安装版或反向切换，且明确处理产品身份、程序文件、登记与共享设备状态的过程。

## 3. P0 事实、不确定项与阻断关系

### P0-1：安装器本体是否必须原生 ARM64 尚未决定

这是候选分水岭。若必须原生，WiX 的 Arm64 MSI 包平台与原生 ARM64 Burn engine 是当前证据最完整的路径，但 `msiexec`、UI、自定义动作和卸载路径仍须实机逐项记录；Inno 和 NSIS 正式发行路径只能依靠 Windows 模拟层运行 Setup/stub。若只要求应用负载原生 ARM64，Inno 的 x86 Setup 和 NSIS 的 x86 stub 技术上仍可部署 ARM64 文件，但必须在 Windows 10 22H2 on Arm 与 Windows 11 on Arm 分别验收。

### P0-2：VC Runtime 策略尚未闭合，Burn 是否必要因而未知

现有[工具链研究](preimplementation-toolchain-baseline-2026-08-10.md)只确认 Windows App SDK 可以 self-contained，没有证明任意 C++/WinRT 输出不需要 VC Runtime。若 `/MT` 或合法、完整的 app-local CRT 样机通过，单 MSI 或脚本式 EXE 已足够；若选择架构匹配的 VC Redistributable，Burn 能把 EXE prerequisite 明确建模进链，但 EXE 仍不属于 MSI transaction。[W05][W08][M12]

### P0-3：安装回滚与应用更新回退不是同一合同

MSI 默认 rollback 只覆盖其执行脚本和正确编写的 rollback custom action；Inno 在卸载日志落盘前可以撤销已记录变更，落盘后普通 `[Run]` 的非零退出码只被记录、不会自动判安装失败，若在同一后置阶段改用 Pascal `Exec` 显式判断也已经越过回滚边界。Inno 也允许在 `PrepareToInstall` 中前置安装 prerequisite 并在失败时阻止主安装开始，但 prerequisite 已产生的外部副作用不受 Inno 主安装回滚保护；NSIS 的错误处理和补偿由脚本作者实现。[M07][M08][I09][I16][I17][I19][I20][N04] 这些机制都不能单独满足 ADR-0006 的“新版启动失败后恢复上一版本”，更不能替代 N/N-1 状态模式兼容。

### P0-4：ProgramData 的完整保留矩阵尚未确认

目前只正式确认已生效设置和恢复记录不删。历史、日志、当前有效目录、已保存草稿、未保存恢复点、偏好、默认缓存和自定义缓存仍有未决项。[P02] 在这些分类确定前，不能安全编写 MSI `RemoveFile`、Inno `[UninstallDelete]` 或 NSIS `Delete`/`RMDir`。[M18][I10][N04] 任何递归删除稳定设备根的方案直接淘汰。

### P0-5：未签名安装的真实阻断范围不能靠文档推断

SmartScreen 同时使用发布者信誉和文件哈希信誉；未签名新版本无法继承证书信誉，企业策略和 Smart App Control 还可能直接阻止。[M10] 本地生成后双击成功不是发布证据，必须从真实 GitHub Release 下载每个精确 MSI/EXE 后测试 Mark-of-the-Web、UAC 和策略表现。

### P0-6：WiX 7 预编译工具的 OSMF 条款需要维护者明确接受

WiX 7 源码采用 MS-RL；官方预编译 Binary Release 还受 OSMF EULA 约束。随 v7.0.0 分发的 EULA 对“创收活动且年总收入大于等于 10,000 美元”使用 `>=`，OSMF 网页却写 “more than $10,000”；本文保守地以分发 EULA 为准。已向维护者另付支持/维护费者只豁免该 Fee，不豁免 Binary Release EULA 本身；自行从源码编译不受该 EULA，但仍受 MS-RL。[W01][W02][W03][W13] 项目不能假定自己必然豁免，也不能把“自行构建”写成零成本替代。

### P0-7：安装内容体积与制品拓扑尚未冻结

[发行矩阵候选稿](../../.scratch/windows-initial-setup-workbench/release-artifact-matrix-options.md)仍在 12 个完整制品、只给标准版安装形态、应用与资源分包之间选择。Inno 在压缩内容超过 4,200,000,000 字节时要求启用多文件 disk spanning；NSIS 官方能力说明仍写安装器最大 2 GB。[I14][N04] WiX 支持多个 attached/detached container，但尚不自动按体积拆分，官方也没有给出整个 bundle EXE 的稳定支持上限。[W17][W18] MSI/Burn 对项目真实文件数、单文件大小、CAB/容器与 GitHub 资产布局必须用最终近似内容样机验证，不能只用一个空壳安装器选型。

### P0-8：标准 Repair 的离线源与缓存行为尚未验证

Windows Installer 的 source resiliency 会记录并搜索安装源，Windows Installer cache 损坏又可能直到 repair、update 或 uninstall 才暴露。[M20][M21] 因而“MSI 有 Repair”不是离线修复已经闭合的证据。样机要在删除原始下载资产、断网且不破坏系统 Installer cache 的条件下修复缺失程序文件，并记录实际 source/cache、提示、退出码与磁盘占用；Burn 还要分别验证 bundle/package cache。

### P0-9：ARM64 设备上的 x64 -> ARM64 安装迁移合同缺失

项目要求分别发布 x64 与 ARM64 工作台，并要求 ARM64 Windows 使用 ARM64 包，但 Windows 11 on Arm 仍可能已经运行或安装过 x64 版本。当前没有决定原生 ARM64 安装器应阻止、替换还是并存于既有 x64 安装，也没有固定跨架构产品身份、ARP 登记、卸载顺序和共享设备状态的结果。两份安装版静默并存并共同读写同一设备状态不是可接受的默认答案。

### P0-10：安装版一键更新跨越安装器提交边界

ADR-0006 要求下载、替换或新版启动失败时恢复上一版本；安装器 rollback 只覆盖安装事务尚未提交的阶段。事务已经成功、随后新版启动失败时，仍需要受信更新路径保留可恢复的 N-1 资产、判定启动健康并执行经过验证的恢复，同时与状态模式 N/N-1 合同配对。[P02][P04] 选择 Burn 或 MSI 都不会自动补上这个边界。

## 4. 当前稳定版本与许可快照

| 候选工具 | 截至 2026-08-10 的稳定版 | 上游日期 | 工具许可/条款 | 项目需要保留的事实 |
| --- | --- | --- | --- | --- |
| WiX Toolset | 7.0.0 | 2026-04-06 UTC（北京时间 04-07） | 源码 MS-RL；官方 Binary Release 另受 OSMF EULA | 固定 tag、MS-RL、OSMF EULA 与实际取得路径；确认是否付费或自编 [W01][W02][W03] |
| Inno Setup | 7.0.2 | 2026-07-13 | 自定义宽松许可允许商业使用、修改、再分发；需保留声明、不得冒充原版、修改版须标明 | 官网“商业用户请购买”是资助请求；免费许可文本本身仍明确允许商业使用，不能误写成强制许可费 [I01][I02][I03] |
| NSIS | 3.12 | 2026-04-19 | 主体 zlib/libpng；bzip2 模块为 bzip2 license；LZMA 模块为 CPL 1.0，带静态/动态链接例外 | 分发许可清单不能简化成“NSIS = zlib”；记录实际 stub、压缩模块和插件，再逐项决定 SBOM、许可文本与 NOTICE 义务 [N01][N02][N03] |

NSIS 主干在线文档已有未定发布日期的 3.13 占位章节，但下载页和 Git tag 截至查询时仍以 3.12 为最新稳定版；不能把主干章节当作已发布版本。[N01][N02]

### 4.1 WiX 7 OSMF 的工程影响

- `Using WiX` 页面称 `WixToolset.Sdk/7.0.0` 可由 `dotnet build` 或 Visual Studio 的 .NET Framework MSBuild 构建，并称 `wix.exe` 需要 .NET SDK 6 或更高；但 v7.0.0 NuGet 实物只带 `net8.0` CLI，SDK 的 Core MSBuild 路径也选择 `tools/net8.0`，只有完整 MSBuild 路径选择 `net472`。[W04][W14][W15][W16]
- 因此样机和 CI 先固定 .NET 8 或更高；“.NET SDK 6+”与 v7 实物的冲突保留为上游待澄清项，不能只按网页下限配置构建机。
- WiX 7 构建要求显式接受 `wix7` EULA，可用 `<AcceptEula>wix7</AcceptEula>`、`-acceptEula wix7` 或 `wix eula accept wix7`；CI 不能靠交互式首次接受。[W13]
- 这只是在**构建机**引入 .NET 工具，不等于生成的 MSI/Burn 或工作台负载依赖目标机 .NET Runtime；目标制品仍须做 PE 依赖和干净机启动检查。
- 使用 NuGet/MSBuild SDK 也属于取得官方预编译 binary 的常见路径，不能因没有手工下载 ZIP 就忽略 OSMF EULA。
- 若选择源码自编，应把编译器、依赖、补丁、二进制身份、更新节奏与可复现性作为工具链自身的维护面，不得临时从任意 fork 取二进制。

## 5. 架构、作用域与目录能力

| 维度 | WiX 7 MSI | WiX 7 Burn + MSI | Inno Setup 7 | NSIS 3.12 |
| --- | --- | --- | --- | --- |
| x64 应用负载 | 原生 x64 MSI，单独构建 | 原生 x64 Burn + x64 MSI | `SetupArchitecture=x64` 可生成原生 x64 Setup | x86 stub 可放置 x64 文件并切换 64 位注册表视图 |
| ARM64 应用负载 | 原生 Arm64 MSI，单独构建 | 官方原生 ARM64 Burn engine + 单独构建的 Arm64 MSI | 可识别 ARM64 并放置 ARM64 文件 | x86 stub 可按脚本放置 ARM64 文件 |
| 安装器执行路径 ARM64 | 可生成 Arm64 MSI 包；`msiexec`、自定义动作和 UI 的实际进程架构仍须实机记录 | 官方 SDK 包含原生 ARM64 Burn engine，源码和构建测试输入含 ARM64；尚无本文可确认的真实 ARM64 runtime E2E | 无原生 ARM64 Setup；Windows 10 Arm 用 x86 模拟，Windows 11 Arm 可用 x86 或 x64 模拟 | 官方 3.12 ZIP 的编译器、可执行压缩 stub 和打包插件 DLL 均为 PE32 x86；源码有 ARM64 构建路径，但官方 CI/正式发行物未提供验证基线 |
| 单制品多架构 | 否；一个 MSI 不能同时声明 x64 与 Arm64 | 不建议；分别生成 x64/ARM64 bundle 与主 MSI | 一个 x86 Setup 可按系统带多架构负载，但项目仍要决定是否接受模拟 | 可由脚本条件选择负载，但 installer 仍是 x86 |
| 默认作用域 | WiX `Package Scope` 默认 per-machine | 取决于链内包；固定 scope 包不会随 BA 选项改变 | `PrivilegesRequired=admin` 默认全机；`lowest` 为当前用户 | `RequestExecutionLevel` 当前默认 admin；user/highest 可选 |
| 双用途安装 | MSI 有严格的 dual-purpose authoring 规则；写 CommonAppData、服务或提升动作会限制 per-user | WiX 7 configurable-scope 只改变 dual-purpose 包；固定包不变 | 可允许 dialog/commandline 覆盖 admin/lowest | 官方 MultiUser include 可辅助，但目录、注册表和卸载仍是脚本责任 |
| Program Files | `ProgramFiles64Folder` 等标准目录 | 由主 MSI 拥有 | `{autopf}` 等常量随模式解析 | `$PROGRAMFILES64`/`$PROGRAMFILES`，需脚本选择 |
| ProgramData | `CommonAppDataFolder`；所有权和 ACL 仍需 authoring | 同主 MSI；Burn 不拥有应用数据语义 | `{commonappdata}`；官方特别提醒公开可写目录和 reparse point 风险 | 可使用 `$COMMONAPPDATA`；ACL 与状态边界完全由脚本负责 |

依据：[M01][M02][M03][M04][M05][M06][M15][M16][W06][W09][W11][W12][W19][W20][I04][I05][I06][N05][N06][N09][N12][N13]

### 5.1 ARM64 样机的判定规则

每个候选必须分别记录：

1. 最外层下载资产的 PE/包平台；
2. 实际运行的 installer/bootstrapper 进程机器类型；
3. 安装后的工作台 EXE/DLL 机器类型；
4. 自定义动作、插件、卸载器和前置依赖的机器类型；
5. Windows 10 22H2 on Arm 与 Windows 11 on Arm 是否依赖不同模拟能力。

只检查安装目录里的主 EXE 不足以声称“原生 ARM64 安装版”。反过来，installer 依靠 x86 模拟也不表示最终 ARM64 工作台负载不是原生；两项必须分别对外描述。

### 5.2 项目当前没有 per-user 的自然需求

工作台需要常驻管理员会话、共享设备级状态和 ProgramData 根。MSI dual-purpose 包的官方限制又明确不允许 per-user 路径写 `CommonAppDataFolder` 等全局位置。[M05] 因而非决定性的默认样机应先固定 **per-machine + UAC**，不要为了候选功能齐全而增加一个尚无产品场景的 per-user 模式。若以后决定支持 per-user，必须先解决状态主体、ACL、管理员凭据和跨用户可见性，不是给安装器加一个单选框即可。

## 6. 生命周期语义对照

| 生命周期 | WiX 7 MSI | WiX 7 Burn + MSI | Inno Setup 7 | NSIS 3.12 |
| --- | --- | --- | --- | --- |
| 首次安装 | Windows Installer 组件/feature 模型 | Burn 先检测与缓存，再执行包链 | 按声明节和 Pascal 脚本执行 | 按 `.nsi` section 和命令执行 |
| 升级 | WiX 7 `Package` 默认自动提供 major upgrade；样机仍须固定并验证身份、版本映射和调度 | 同主 MSI，另有 bundle 版本/缓存/related bundle 生命周期 | 相同 `AppId`、权限模式和 32/64 位模式视为同一应用，默认追加卸载日志 | 检测旧版、终止进程、替换与卸载登记均由脚本实现 |
| 阻止降级 | 默认 major upgrade 使用 `AllowDowngrades=no`，但仍须验证版本规则 | 必须同时验证 bundle 与 MSI 的 downgrade plan | 无产品级自动拒绝；旧包默认可能保留较新有版本文件、替换无版本文件，形成混合安装 | 自写版本检测与退出路径 |
| Repair | 按 `REINSTALL/REINSTALLMODE` 处理已安装 feature 的 MSI 已知资源；实际覆盖取决于模式、authoring 与源可用性 | 主 MSI 可 repair；bundle 还要验证缓存与 prerequisite repair | ARP 明确写 `NoRepair=1`，无 MSI 式标准 repair | 官方安装器示例写 `NoRepair=1`；自定义 repair 是新脚本路径 |
| 失败回滚 | 默认生成 rollback script；自定义动作的外部效果需配对 rollback action | rollback boundary 默认不是跨包 MSI transaction；只有 MSI/MSP 可加入 `Transaction=yes` | 卸载日志最终落盘前撤销已记录变更；落盘后普通 `[Run]` 非零退出不会自动判失败；`PrepareToInstall` 可阻止主安装开始，但前置 prerequisite 的副作用不在主安装回滚内 | 无通用事务模型；每个副作用、失败和补偿由脚本维护 |
| 卸载 | 移除安装器拥有的组件及显式 `RemoveFile`；不应拥有运行时状态文件 | 主 MSI 负责程序文件；Burn 还管理包缓存/登记 | 自动卸载已记录安装内容；`[UninstallDelete]` 可额外删，官方警告通配目录风险 | 只有脚本写入的 uninstaller/ARP 项和 uninstall section；删除范围由脚本决定 |
| 新版启动失败回退 | 不由 MSI 自动证明 | 不由 Burn 自动证明 | 不由 Inno 自动证明 | 不由 NSIS 自动证明 |

依据：[W06][W07][W08][M07][M08][M09][M13][M14][I07][I08][I09][I10][I15][N04][N10]

### 6.1 MSI 的标准能力也有边界

- Windows Installer 默认在安装脚本执行期间生成 rollback script，并保存被删除文件；策略或作者仍可禁用 rollback。[M07]
- 自定义动作若直接修改系统或调用外部服务，作者必须安排 rollback custom action，并处理“动作执行到一半被中断”的情况。[M08]
- `REINSTALL`/`REINSTALLMODE` 只处理原先安装的 features 和 MSI 已知资源；默认及显式模式对缺失、旧版、校验和、注册表、快捷方式的处理不同，不会校验任意 ProgramData 数据库、用户内容、动态下载或应用状态。[M09][M19]
- “有标准 Repair”也不证明离线时源一定可用。Windows Installer 会记录并搜索 source list，缓存损坏又可能直到 repair/update/uninstall 才暴露；样机必须删除原始下载资产并断网测试 repair，同时不得通过破坏系统 Installer cache 来制造用例。[M20][M21]
- WiX `MajorUpgrade` 默认把 `RemoveExistingProducts` 调度在 `afterInstallValidate`，即先完整移除旧版；新版随后失败时可能两个版本都不在。若要求失败后恢复旧版，必须评估 `afterInstallInitialize`，或在严格满足组件规则时评估 `afterInstallExecute`，并做故障注入。[W07][M17]
- MSI 产品版本比较只看前三段。若项目把第四段作为可发布更新号，WiX `AllowSameVersionUpgrades` 会引入同前三段内的降级风险，应优先修订版本映射而不是打开危险开关。[W07][M13]

### 6.2 Burn 不等于“所有包原子安装”

Burn 可以链 MSI、MSP、MSU、EXE 和其他 bundle，但 `RollbackBoundary Transaction` 默认是 `no`，且只有 MSI/MSP 能进入 `Transaction=yes` 边界。[W05][W08] 因而 VC Redistributable 这类 EXE prerequisite 成功、主 MSI 随后失败时，不能假定 redist 会被自动卸载；反向卸载也不能误删其他应用共享的 runtime。

Burn 只有在确实需要以下能力时才值得承担额外生命周期：

- 检测并部署 VC Redistributable 等独立 prerequisite；
- 统一下载/缓存多个包；
- 用一个 EXE 提供链级日志、检测与进度。

若最终发行负载已经通过 `/MT` 或 app-local CRT 闭合，单 MSI 样机应保持为更小基线。

### 6.3 Inno 的回滚分界

Inno 安装顺序明确：先写文件、快捷方式、INI、注册表和 ARP 项，随后最终写入 uninstaller EXE 与日志；一旦这一步完成，用户不能取消，后续错误不再回滚此前内容，然后才处理普通 `[Run]`。[I09] 普通 `[Run]` 启动的子进程即使返回非零，Setup 也只记录退出码而不会自动把安装判成失败；启动异常同样会被捕获并继续。若在普通 `[Run]` 所在的后置阶段改用 Pascal `Exec` 显式检查 VC Redist 退出码，判断仍发生在安装回滚边界之后。[I16][I17]

另一条官方路径是在实际安装开始前由 `PrepareToInstall` 检测并安装 prerequisite；返回非空字符串可阻止主安装继续，固定 tag 的示例也展示了失败与重启处理。[I19][I20] 这避免把主安装文件写到一半才发现 prerequisite 失败，但已经运行的 prerequisite 是独立外部安装，其副作用仍不由 Inno 主安装的卸载日志回滚。样机必须分别故障注入“前置包失败”和“前置包成功、主安装失败”，不能把前置时机误写成跨安装器事务。

同 `AppId` 的新版默认追加既有卸载日志，使多次安装的变化在卸载时按逆序撤销。[I07][I08] 这提供了升级基础，但没有自动形成“旧版禁止覆盖新版”“新版启动失败恢复 N-1”或“状态模式可降级”合同。旧安装包覆盖新版时，默认文件规则还可能保留目标机上版本号更高的文件、同时替换无版本信息文件，留下跨版本混合状态；downgrade 样机必须逐文件核对，不能只看 Setup 返回成功。[I07][I08][I18]

### 6.4 NSIS 的自由度就是维护责任

NSIS 官方把它定义为可完全控制安装器各部分的脚本系统，并明确把升级、版本检查称为可由脚本加入的逻辑。[N04] 官方 all-users 示例仍要手工：

- 选择执行级别和安装目录；
- 写卸载注册表项；
- 写 `NoRepair=1`；
- 生成 uninstaller；
- 在 uninstall section 删除内容。[N10]

这使 NSIS 很适合特殊、小型安装流，但本项目需要升级、降级、状态保留、跨架构与失败恢复时，每条自写分支都会成为长期合同和测试负担。v3.12 源码接受 ARM64 构建目标，但官方 CI 和正式 ZIP 没有给出 ARM64 验证基线；变更日志中的 “preliminary” 只明确修饰自安装脚本路径，不能泛化为整个源码 ARM64 能力等级。[N02][N09][N12][N13] 除非样机证明 NSIS 在体积或特殊流程上有明确且不可替代的收益，不应把“能写脚本”计作默认优势。

### 6.5 安装事务与一键更新是两层状态机

手动运行传统安装器时，候选负责安装事务、登记、日志和它声明的 rollback。工作台内主动更新还必须在此之外处理受信资产、运行中实例、持久化恢复点、新版启动健康和事务提交后的 N-1 恢复。[P04] 因而测试记录至少区分：

1. 安装器尚未提交即失败，由安装器自身回滚；
2. 安装器已提交但新版无法启动，由应用更新合同恢复 N-1；
3. N 已启动但状态迁移或健康检查失败，由状态兼容合同决定能否安全回退；
4. 恢复动作自身失败，仍保留可诊断且不破坏设备状态的结果。

本文不选择更新助手或替换协议，只要求任何安装器候选不得把第一种成功外推成后面三种已经解决。

## 7. ProgramData 状态与删除责任

安装器拥有程序文件，不拥有应用运行后产生的设备状态。候选 authoring 必须遵守以下初始边界，最终分类仍等待 `INSTALLER-D5/D6`：

| 数据类别 | 普通升级 | 普通 Repair | 普通卸载候选行为 | 删除本机数据 |
| --- | --- | --- | --- | --- |
| 安装目录程序文件 | 替换为 N | 恢复 N 的已声明资源 | 删除 | 不适用 |
| 设备恢复记录、待恢复状态 | 原样保留并由应用迁移合同解释 | 不碰 | 保留 | 只能走单独受保护用例 |
| 当前有效目录、紧急撤回信息 | 原样保留 | 不碰 | 保留 | 待产品决定 |
| 历史与详细日志 | 原样保留 | 不碰 | 候选为保留 | 由应用拥有的显式清理 |
| 已保存草稿、未保存恢复点 | 原样保留 | 不碰 | 未决 | 区分当前用户与全机数据 |
| 默认软件包缓存 | 原样保留并按应用期限管理 | 不碰 | 未决；卸载后没有进程执行期限清理 | 显式且不得越过待恢复保护 |
| 用户选定的外部缓存位置 | 不碰 | 不碰 | 不碰 | 不能由安装器递归清空 |

候选淘汰条件：

1. 升级、repair 或卸载通过通配符递归删除稳定 ProgramData 根；
2. 把运行时状态写入安装目录，从而让升级/repair 覆盖；
3. 由 MSI key path、Inno 卸载日志或 NSIS uninstall section 把动态状态文件当成程序文件；
4. 单独的“删除本机数据”绕过活动批次、待恢复状态、身份和 ACL 检查；
5. 安装版和便携版创建两个不兼容的状态根或 ACL。

安装器可以负责首次创建稳定 ProgramData 根和最低必要 ACL，但 ACL 版本、修复和迁移仍属于持久化适配器合同。样机必须验证 fresh install、upgrade、repair、uninstall、reinstall 和便携/安装切换后的目录所有者与 DACL。

## 8. 签名、SmartScreen 与制品真实性

| 事实 | WiX MSI/Burn | Inno | NSIS | 项目结论 |
| --- | --- | --- | --- | --- |
| 不签名能否构建 | 能；签名是可选流程 | 能；未配置 `SignTool` 时可生成未签名 Setup/Uninstall | 能；`!finalize`/`!uninstfinalize` 只是可选外部签名钩子 | 与 ADR-0005 技术上相容 |
| 若未来签名 | MSI、外部 CAB 分别处理；Burn 需先签 engine、重附加，再签完整 bundle | `SignTool` 与 `SignedUninstaller` 可集成 Authenticode | finalize 钩子可调用 SignTool | 这不修改当前“不签名”决定 |
| 未签名风险 | 具体 MSI/EXE 仍须从 GitHub 下载实测 | 同左 | 同左 | 不能承诺无警告、无人值守或企业允许 |
| 内部完整性 | MSI/Burn 自有数据库/manifest/hash 能发现部分损坏 | 编译器内部 `.issig` 不等于发布者 Authenticode | installer 可有 CRC，自定义校验可脚本化 | 都不能替项目提供发布者身份或 ADR 已拒绝的外部校验信息 |

依据：[W10][M10][M11][I11][I12][N08]

签名也不能证明：负载没有漏洞、来源可信、Windows App SDK/VC Runtime 已闭合、应用状态迁移正确，或企业策略必然放行。未签名同样不证明文件恶意；它只意味着缺少受信任发布者和 Authenticode 完整性证据。

## 9. CLI、CI、可重复构建与大体积

| 候选 | 非交互构建入口 | 固定版本 | 可重复构建现状 | 大体积风险 |
| --- | --- | --- | --- | --- |
| WiX 7 | `dotnet build`/MSBuild SDK 或 `wix build` | `WixToolset.Sdk/7.0.0` 或固定自编 tag | 本研究未找到足以承诺安装器 bit-for-bit 的官方合同 | 支持多个 container，但不会自动按体积拆分；必须验证 MSI CAB、Burn container、外部 payload 与 GitHub 资产布局 [W17][W18] |
| Inno 7 | `ISCC.exe` | 固定 7.0.2 发行物/哈希 | `[Files]` 的 `notimestamp` 只“帮助”可复现，不证明全部输出逐字节一致 | 压缩内容超过 4.2 GB 时必须启用多文件 disk spanning [I13][I14] |
| NSIS 3 | `makensis.exe` | 固定 3.12 发行物/哈希 | 构建 NSIS 工具本身支持 `SOURCE_DATE_EPOCH`；不等于其生成的任意 installer bit-for-bit | 官方功能说明仍列 installer 最大 2 GB [N04][N07][N11] |

样机不得只比较“本机能编译”。每个候选应记录：

- 精确工具版本、取得 URL、项目计算摘要与许可文件；
- 无交互命令、退出码、warning-as-error 能力、日志和中间产物；
- 相同输入连续两次与两个干净 runner 的输出摘要；
- 时间戳、文件顺序、压缩、GUID/产品码、绝对路径和环境变量造成的差异；
- x64/ARM64 产物是否共享同一声明源，还是复制两套易漂移脚本；
- 标准版、断网救援版和超大离线版的构建时间、峰值磁盘、最终文件数与单文件大小；
- 未签名构建与将来可选签名步骤是否清楚分离。

“固定输入可以稳定构建成功”和“不同机器生成 bit-for-bit 相同输出”是两级不同目标，必须由 `INSTALLER-D9` 先决定。

GitHub Release 资产身份、构建输入、runner、日志和实机验证的记录方式应复用现有发行证据研究，不在安装器样机中另造一套证明口径。[P03]

## 10. WinUI 3 与 VC Runtime 闭合

安装器样机使用的负载必须来自同一个最小 C++/WinRT + XAML 工程，不能给不同候选准备不同运行时策略。至少包含：

1. `WindowsAppSDKSelfContained=true` 的 x64 和 ARM64 Release 输出；
2. XAML/MRT 资源、WinUI 3 native dependencies 和项目自身 DLL；
3. 选定的 VC Runtime 策略；
4. 一个能证明 UI 真正初始化的离线启动烟雾测试，而不只是进程创建成功；
5. `DUMPBIN /DEPENDENTS` 等静态证据与真实干净机启动，两者缺一不可。

候选路径：

| VC Runtime 策略 | MSI | Burn + MSI | Inno/NSIS |
| --- | --- | --- | --- |
| `/MT` | 只包装已验证文件集 | 没有链 prerequisite 收益 | 只包装已验证文件集 |
| app-local CRT | 只包装许可允许且架构完整的 DLL | 没有链 prerequisite 收益 | 同左；脚本不应动态从网络补依赖 |
| VC Redistributable | 不应借危险 nested install/custom action 隐式解决；改用 bootstrapper | 显式检测并链 EXE redist，是 Burn 的主要合理场景 | 可启动 redist，但检测、退出码、重启和失败边界全部进入项目脚本 |

P0 干净机至少包括：

- Windows 10 22H2 x64；
- Windows 11 x64；
- Windows 10 22H2 ARM64；
- Windows 11 ARM64；
- 无 Visual Studio、无预装 Windows App SDK、无候选 VC Runtime、断网；
- fresh install 后立即启动，uninstall/reinstall 后再次启动。

若某个真实 OS/架构无法取得，必须记录为证据缺口，不能由 cross-build 或另一 OS 模拟替代。

## 11. 候选短名单

### A. WiX 7：每架构独立 MSI

**样机形态：** x64 MSI + ARM64 MSI；per-machine；同一声明源；默认阻止降级；只安装工作台程序文件和必要的稳定目录/ACL。

**进入样机的理由：** 原生平台语义、标准 maintenance/repair、企业部署熟悉度、默认 rollback、升级规则集中。

**主要风险：** MSI component/key-path 规则学习成本；错误 custom action 会破坏 rollback；版本前三段限制；ProgramData ownership 容易写错；WiX 7 OSMF；大体积内容待验证。

**通过前提：** VC Runtime 无需独立 EXE prerequisite，或 prerequisite 已由产品另行闭合。

### B. WiX 7：每架构独立 Burn EXE + MSI 主包

**样机形态：** x64 Burn + x64 MSI、ARM64 Burn + ARM64 MSI；只在确需 VC Redistributable 时加入架构匹配 EXE 包。

**进入样机的理由：** 对 prerequisite 的检测、缓存、链式日志和统一入口强于单 MSI。

**主要风险：** bundle 与 MSI 两层版本/缓存/卸载生命周期；EXE prerequisite 不进入 MSI transaction；未签名 EXE 入口；签名若未来恢复需两阶段；测试矩阵更大。

**停止条件：** `/MT` 或 app-local 已通过且没有其他多包需求时，不继续把 Burn 作为默认。

### C. Inno Setup 7：x64 Setup + x86-on-Arm Setup

**样机形态：** x64 Windows 使用原生 x64 Setup；ARM64 payload 使用可在 Windows 10/11 on Arm 模拟运行的 x86 Setup。两者安装原生工作台负载，使用同 `AppId` 规划但防止跨架构误识别。

**进入样机的理由：** authoring 较直接、CLI 稳定、安装与自动卸载能力完整、许可宽松、对小型 EXE 分发友好。

**主要风险：** 没有原生 ARM64 Setup；无 MSI 式 repair；降级若不显式阻止可能形成混合安装；状态保留、redist 检测与失败补偿要写脚本；普通 `[Run]` 非零退出不自动失败且已不在回滚边界；超大内容会变成多文件。

### D. NSIS 3.12：官方 x86 工具链对照

**样机形态：** 只用官方 3.12 ZIP、官方插件和 x86 unicode 可执行压缩 stub，分别放置 x64/ARM64 工作台负载；不把源码可构建 ARM64 或第三方 special build 当成当前正式基线。

**进入样机的理由：** 测量最小 stub、压缩和脚本控制能否带来实际收益。

**主要风险：** installer 进程依赖 x86 模拟；ARM64 支持仍是源码/初步路径；upgrade/repair/rollback/ARP/状态删除全是脚本责任；2 GB 限制；插件架构和复合许可清单。

**停止条件：** 若输出体积和特殊控制没有显著优于 Inno，或生命周期脚本明显增加，不继续。

### 11.1 不增加第五个生命周期引擎

- MSIX 与现行长期不签名决定冲突，已有[工具链研究](preimplementation-toolchain-baseline-2026-08-10.md)说明，不在本轮重复比较。
- CPack 的 WiX/NSIS generator 只是生成相应后端输入，不提供新的安装生命周期；若以后采用，只作为 build front-end 评估。
- 自研 bootstrapper 会把提升、下载、缓存、回滚和自更新安全面全部转给项目，不符合首版范围。
- 商业 installer 和自动更新框架不是本轮对象；只有上述候选均被明确门槛淘汰后，才重新开研究范围。

## 12. 推荐样机比较标准

### 12.1 先过淘汰门槛，再比较偏好

以下任一失败即淘汰，不做加权平均掩盖：

1. x64/ARM64 原生工作台负载在对应干净机离线安装并启动失败；
2. 不可接受地依赖目标机预装 Windows App SDK、VC Runtime 或 .NET Runtime；
3. fresh install、upgrade、repair/重装、失败注入或卸载会损坏/删除应保留设备状态；
4. 无法生成明确架构、版本和内容版身份的非交互制品；
5. 工具许可/OSMF、第三方模块许可或取得路径不能进入发布证据；
6. 未签名真实下载资产在项目声明支持的 Windows/策略基线上完全无法安装，且现有 ADR 不允许缓解；
7. 大体积目标内容无法按已选发行矩阵可靠承载。

### 12.2 通过门槛后的比较维度

不要先拍权重。所有候选用同一负载、同一故障点、同一 VM 快照和同一证据模板，记录原始数据后再由维护者决定权重。

| 维度 | 记录内容 | 更优信号 |
| --- | --- | --- |
| 平台闭合 | installer 与 payload 架构、最低 OS、模拟依赖 | x64/ARM64 行为一致，限制可准确披露 |
| 生命周期正确性 | fresh/upgrade/downgrade/repair/uninstall/reinstall | 标准能力覆盖多，自写补偿少 |
| 状态保留 | 每类 ProgramData 数据前后摘要、代次、ACL | 所有非目标状态字节和权限保持 |
| 失败恢复 | 注入点、退出码、残留、下次动作 | 回到 N 或明确可恢复状态，无孤儿登记 |
| 前置依赖 | 检测、离线安装、重启、失败 | 依赖闭合清楚，不误删共享 runtime |
| 未签名体验 | GitHub 下载、SmartScreen/UAC/SAC/策略 | 能准确引导且支持范围内未被完全阻断 |
| CI 可维护性 | 固定输入、命令、日志、warning、两架构复用 | 单一声明源、可审查 diff、失败可诊断 |
| 工具治理 | 许可、取得、更新、漏洞响应 | 条款清楚、版本可锁、维护路径可持续 |
| 规模 | 工具/脚本行数、自定义动作数、测试分支数 | 生命周期特例和私有代码更少 |
| 制品成本 | 大小、文件数、构建时间、峰值磁盘 | 满足场景后更小更简单，而非只追最小 EXE |

### 12.3 P0 场景矩阵

运行环境目标覆盖 Windows 10 22H2 x64、Windows 11 x64、Windows 10 22H2 ARM64 和 Windows 11 ARM64 四格；若某格无法取得，必须按 `RELEASE-D8`／`INSTALLER-D10` 明确它是阻断性缺口还是非阻断披露，不能用另一 OS 或交叉构建结果代替。

| 场景 | 必须观测 |
| --- | --- |
| fresh install | UAC 主体、安装目录、ProgramData 根/ACL、ARP 登记、首次离线启动 |
| N-1 -> N | 程序文件全部切到 N；设备状态按迁移合同处理；旧登记/缓存无孤儿 |
| N -> N-1 | 按决定阻止，或在 N/N-1 合同下安全完成；逐文件排除跨版本混合；未知新状态绝不清空 |
| 同版本 repair/重装 | 删除原始下载资产并断网；缺失程序文件恢复；记录实际 source/cache；动态状态、日志、草稿和恢复记录不被覆盖 |
| 安装失败 | 在复制中、登记后、prerequisite 后、主包后分别强制失败；记录 rollback 边界 |
| 新版启动失败 | 验证 ADR-0006 的应用级 N-1 恢复，不把“安装完成”当成更新成功 |
| 运行中升级/一键更新 | 活跃工作台、文件占用和进行中批次分别测试；记录停止、取消、重启、健康确认及 N-1 资产责任 |
| 普通卸载 | 只移除已决定的程序文件/登记；逐类核对 ProgramData 与 ACL |
| 重装 | 发现保留状态并按兼容合同启动，不初始化空状态覆盖旧状态 |
| 便携/安装切换 | 两种形态读取同一设备状态并竞争同一设备锁 |
| 发行架构选择 | x64 设备拒绝 ARM64 资产；ARM64 fresh install 与既有 x64 -> ARM64 迁移按 `INSTALLER-D13` 验证，不留下并行登记或混合组件 |
| 真实下载 | 从 draft/release 下载后的 MotW、SmartScreen、UAC、SAC、企业策略 |
| 无网/无 runtime | 所有外部网络被阻断，仍能安装并首次启动；或准确证明该内容版本就要求联网 |
| 大体积 | 近似最终文件数/大小，验证空间预检、中断、分片、缓存和清理 |

每个场景保存：候选版本、OS build、CPU/进程架构、命令行、退出码、安装器日志、应用启动日志、安装前后文件/注册表/ACL 清单、状态代次与摘要、截图或录屏索引。源代码审查不能冒充真实设备证据。

## 13. 维护者决策树

这些是产品和维护决定，不是官方资料能替用户回答的事实。应先回答第一轮根问题，再按答案决定是否需要后续分支；推荐仅是首版默认，不是已确认合同。

### 13.1 依赖与当前 frontier

| 状态 | 问题 | 先决条件 |
| --- | --- | --- |
| 现在可回答 | `D1`、`D2`、`D4`、`D5`、`D9`、`D13` | 不需要安装器样机替维护者作事实判断 |
| 等事实后回答 | `D3` | 最小 WinUI 3 负载对 `/MT`、app-local 与 VC Redistributable 的许可/运行验证 |
| 等其他合同后回答 | `D6`、`D7`、`D8`、`D10` | 分别依赖 `STATE-D11`、`STATE-D3/D8`、`SEC-D3/RELEASE-D8` 与前述支持边界 |
| 最后收口 | `D11`、`D14`、`D12` | 发行矩阵 Q7、受信更新路径、状态兼容与同负载样机数据 |

`STATE-D*` 见[状态研究](windows-state-persistence-and-recovery.md)，`SEC-D*` 见[特权下载与自更新安全研究](elevated-download-execution-security.md)，`RELEASE-D*` 见[发行来源与 CI 证据研究](release-provenance-and-ci-evidence.md)，Q7 见[发行矩阵候选稿](../../.scratch/windows-initial-setup-workbench/release-artifact-matrix-options.md)。

第一轮回答后必须重算 frontier；下列后续问题不是要求维护者现在越过未完成事实直接选择。

### 第一轮：决定候选边界

❓ **INSTALLER-D1 - 安装器执行路径是否也必须在 x64/ARM64 上原生？** 对 MSI 要分别看包平台、`msiexec`、UI 和自定义动作；对 EXE 要看 bootstrapper/Setup、插件和 uninstaller。还是只要求最终工作台负载原生，允许 ARM64 用户通过系统模拟运行安装入口？

➡️ 推荐：若“原生 ARM64 安装版”会出现在用户承诺中，则 installer/bootstrapper、uninstaller 和必要插件都要求原生；否则准确写成“安装 ARM64 原生工作台的 x86/x64 安装器”，并保留 Inno 对照。

❓ **INSTALLER-D2 - 是否要求 Windows 标准 Repair 和企业 MSI 部署语义？** 还是首版只要求 install/upgrade/uninstall/reinstall？

➡️ 推荐：安装版面向公开 GitHub 用户仍优先保留标准 repair，尤其是自包含文件集较多时；若明确不要 repair，不能仍在验收或文案中暗示 MSI 式自修复。

❓ **INSTALLER-D4 - 是否接受 WiX 7 官方 Binary Release 的 OSMF 条款？** 若不接受，是否愿意维护源码自编工具链？

➡️ 推荐：维护者按项目实际用途书面记录适用性；不要默认豁免。若既不接受条款也不愿自编，WiX 7 候选直接退出，不要在实现中途再发现。

❓ **INSTALLER-D5 - 普通卸载是否只删程序文件并保留全部 ProgramData 状态？**

➡️ 推荐：是。至少在完整状态分类定案前，采用最保守保留策略；安装器不通过猜测代替数据所有权决定。

❓ **INSTALLER-D9 - 可重复构建目标是哪一级？** 固定输入稳定成功，还是跨干净 builder 的 bit-for-bit 相同？

➡️ 推荐：首版先把“精确版本 + 锁定输入 + 两次构建差异可解释”设为 P0；若要求 bit-for-bit，把时间戳、压缩、GUID、绝对路径和 builder 镜像一并列为正式门禁。

❓ **INSTALLER-D13 - ARM64 Windows 上的 x64 安装版怎样处理？** fresh install 是否拒绝 x64 资产；若设备已装 x64 版本，原生 ARM64 安装版是阻止、受控替换还是并存？

➡️ 推荐：ARM64 fresh install 使用原生 ARM64 资产；既有 x64 安装版采用互斥的受控迁移，保留共享设备状态并清理旧程序登记。不要默认允许两个架构的安装版并存后共同读写同一状态根。

### 第二轮：依赖第一轮答案

❓ **INSTALLER-D3 - VC Runtime 最终采用哪条闭合路径？** `/MT`、app-local，还是链式部署 VC Redistributable？该问题必须等最小负载样机先证明哪些路径真实可行。

➡️ 推荐：先验证 `/MT` 与 app-local 的许可、更新责任和干净机运行；只有二者不能闭合时才用 Burn 验证 redist，避免先引入双层生命周期。不可行路径由事实淘汰，不交给维护者凭偏好选择。

❓ **INSTALLER-D6 - 是否提供独立“删除本机数据”入口，谁拥有该动作？** 该问题依赖 D5 与 `STATE-D11` 的逐类保留矩阵。

➡️ 推荐：若提供，由应用核心拥有受保护用例，区分当前用户与全机数据并检查待恢复状态；安装器只调用类型化入口或完全不提供，不能复制删除规则。

❓ **INSTALLER-D7 - 是否允许安装旧版本覆盖新版本？** 该问题依赖 `STATE-D3/D8` 的状态模式 N/N-1 合同。

➡️ 推荐：默认阻止降级。只有状态研究中的 N/N-1 读取/写入、迁移前快照和外部效果已经闭合，才允许受控降级；MSI/Inno/NSIS 都不能替代该前提。

❓ **INSTALLER-D8 - 未签名传统安装器的支持边界和淘汰门槛是什么？** ADR-0005 已经固定长期未签名；这里不重复询问是否签名，而要决定默认消费设备、SmartScreen/SAC 和哪些企业策略是必须支持还是明确不支持。`SEC-D3` 的一键更新真实性根是另一项先决决定。

➡️ 推荐：默认消费版 Windows 的真实 GitHub 下载资产至少要有可准确说明的人工继续路径；强制阻止未签名软件的企业策略可在未承诺企业支持时列为明确边界。任何缺口都记实，不把“未出现 SmartScreen”当安全证明，也不声称安装器选型解决了自更新真实性。

❓ **INSTALLER-D10 - 哪些 P0 样机组合通过后才允许选型？** 该问题依赖 D1、D2、D3、D5、D8、D13 与 `RELEASE-D8`。

➡️ 推荐：以第 12.3 节四个 OS/架构格为完整目标，逐项执行 fresh、upgrade、downgrade gate、repair/重装、失败注入、卸载保留、重装、真实下载与无 runtime 离线启动。无法取得的环境只能记 `not-run`；是否阻止选型或公开发行由 `RELEASE-D8` 明确决定，不能把缺格改写成通过。

### 第三轮：发行形态收口

❓ **INSTALLER-D11 - 断网救援版与超大离线版是否需要安装形态？** 该问题依赖发行矩阵 Q7 与真实体积。

➡️ 推荐：除非存在明确长期安装场景，只给标准版做安装器；断网救援版和超大离线版保留便携形态，避免把大资源、许可和升级/卸载矩阵复制到安装器中。若仍选择多种安装内容版，必须继续决定它们互斥替换还是并存，并为内容版切换建立稳定产品身份与测试，而不是让同版本不同内容静默碰撞。

❓ **INSTALLER-D14 - 安装版一键更新怎样跨越安装器提交边界？** 在 `SEC-D3` 已建立受信更新路径、`STATE-D8` 已闭合 N/N-1 后，是否复用公开 MSI/EXE，谁保留上一版本、判定新版健康并触发恢复？

➡️ 推荐：把安装器成功只定义为包事务完成；应用更新协调责任单独持有受信资产、停止运行实例、启动健康确认和事务提交后的 N-1 恢复。可以复用同一公开安装资产，但不能把 `msiexec`/Setup 返回成功直接提升为“一键更新成功”。

❓ **INSTALLER-D12 - 最终选择规则是什么？** 该问题依赖所有适用的 `D1` 至 `D14` 与同负载样机数据。

➡️ 推荐：先按淘汰门槛筛选，再优先选自写生命周期代码最少、状态边界最容易证明的候选；只有通过候选在关键能力相当时，才用体积、构建速度和界面定制做次级排序。

## 14. 能证明与不能证明

### 14.1 研究和样机可以证明

- 某个固定工具版本能否由固定命令生成指定平台的 MSI/EXE；
- installer、uninstaller、plugin 和应用负载的机器架构；
- 在给定 VM/设备、策略和前置依赖状态下的实际安装结果；
- 标准/自定义生命周期在已注入故障点的残留和日志；
- 普通升级、repair、卸载对已枚举 ProgramData 数据和 ACL 的影响；
- 固定输入在所测 builder 上的输出摘要与差异；
- 查询日上游许可、版本和已公开文档。

### 14.2 不能由安装器或本文证明

- 工作台没有漏洞、恶意行为或供应链风险；
- 未签名资产在所有家庭/企业设备都可绕过 SmartScreen 或策略；
- MSI rollback、Burn transaction 或 Inno rollback 能保护未建模的应用状态；
- `WindowsAppSDKSelfContained=true` 自动闭合 VC Runtime；
- x64/ARM64 cross-build 等于在真实 ARM64 上执行通过；
- 一次 successful install 等于 upgrade、repair、uninstall、reinstall 和 N-1 回退都正确；
- `notimestamp` 或 `SOURCE_DATE_EPOCH` 单独等于 bit-for-bit reproducible；
- 上游当前许可、OSMF 阈值、版本或下载布局永远不变；
- 只检查主 EXE 就能证明全部自定义动作、插件和卸载器架构正确。

## 15. 非决定性的验证顺序

1. 先回答当前 frontier：`INSTALLER-D1`、`D2`、`D4`、`D5`、`D9`、`D13`，并与状态研究的卸载/降级合同对齐。
2. 用一个最小 x64/ARM64 WinUI 3 C++/WinRT 负载闭合 Windows App SDK 与 VC Runtime，再回答 `INSTALLER-D3`。
3. 用 WiX MSI 验证标准 install/upgrade/downgrade gate/repair/rollback/uninstall，不加入自定义动作能避免的逻辑。
4. 只有 VC Redistributable 必须链式部署时，复制同一 MSI 进入 Burn 样机，验证 EXE prerequisite 的非事务边界。
5. 用 Inno 对同一负载实现最小等价生命周期，记录为达到同一门槛新增的脚本与测试。
6. NSIS 只在 Inno 通过后仍存在明确的体积或特殊流程问题时继续；否则在早期停止。
7. 用接近最终标准版、断网救援版和超大离线版内容测文件数与大小，结合 `INSTALLER-D11` 收口发行拓扑。
8. 从真实 GitHub Release 下载未签名资产，测试 MotW、SmartScreen、UAC、SAC 和企业策略；本地产物不能代替，再回答 `INSTALLER-D8/D10`。
9. 在 `SEC-D3` 与 `STATE-D8` 有答案后验证运行中升级、新版健康确认和事务提交后的 N-1 恢复，再回答 `INSTALLER-D14`。
10. 将样机原始证据回填到维护者选择记录并回答 `INSTALLER-D12`；只有决定确认后，才更新 ADR、规格、事项和版本锁定。

## 16. 官方来源

除单独注明外，所有来源访问日期均为 2026-08-10。

### 16.1 WiX Toolset

| 编号 | 官方资料 | 本文用途 |
| --- | --- | --- |
| W01 | [WiX Toolset v7.0.0 release](https://github.com/wixtoolset/wix/releases/tag/v7.0.0) | 版本、发布日期、Binary Release 与 OSMF 提示 |
| W02 | [WiX v7.0.0 LICENSE.TXT](https://github.com/wixtoolset/wix/blob/v7.0.0/LICENSE.TXT) | 源码 MS-RL 条款 |
| W03 | [WiX v7.0.0 OSMFEULA.txt](https://github.com/wixtoolset/wix/blob/v7.0.0/OSMFEULA.txt) | 收入阈值、豁免、自编与 Binary Release 边界 |
| W04 | [Using WiX](https://docs.firegiant.com/wix/using-wix/) | `WixToolset.Sdk/7.0.0`、MSBuild/`dotnet build`、`.NET SDK 6+` CLI |
| W05 | [Burn bundles](https://docs.firegiant.com/wix/tools/burn/) | MSI/MSP/MSU/EXE/bundle 链式引导能力 |
| W06 | [Package element](https://docs.firegiant.com/wix/schema/wxs/package/) | 默认 per-machine、`UpgradeStrategy=majorUpgrade`、自动 major upgrade 与版本属性 |
| W07 | [MajorUpgrade element](https://docs.firegiant.com/wix/schema/wxs/majorupgrade/) | 默认阻止降级、同版本与调度风险 |
| W08 | [RollbackBoundary element](https://docs.firegiant.com/wix/schema/wxs/rollbackboundary/) | 默认非 transaction、仅 MSI/MSP 可进 MSI transaction |
| W09 | [Configurable scope bundles](https://docs.firegiant.com/wix/whatsnew/configurable_scope_bundles/) | WiX 7 dual-purpose scope；固定包不随 BA 选择变化 |
| W10 | [Signing packages and bundles](https://docs.firegiant.com/wix/tools/signing/) | MSI/CAB 签名与 Burn engine + whole bundle 两阶段签名 |
| W11 | [BurnPlatforms source at v7.0.0](https://github.com/wixtoolset/wix/blob/v7.0.0/src/api/wix/WixToolset.Extensibility/Data/BurnPlatforms.cs) | x86/x64/ARM64 平台枚举 |
| W12 | [ARM64 Burn E2E project at v7.0.0](https://github.com/wixtoolset/wix/blob/v7.0.0/src/test/burn/TestData/BasicFunctionalityTests/BundleA_arm64/BundleA_arm64.wixproj) | `<InstallerPlatform>arm64</InstallerPlatform>` 实际测试输入 |
| W13 | [Open Source Maintenance Fee](https://docs.firegiant.com/wix/osmf/) | WiX 官方对 OSMF 的说明 |
| W14 | [wix 7.0.0 NuGet package](https://www.nuget.org/packages/wix/7.0.0) | CLI 包版本与目标框架实物 |
| W15 | [WixToolset.Sdk 7.0.0 NuGet package](https://www.nuget.org/packages/WixToolset.Sdk/7.0.0) | MSBuild SDK 包版本与目标框架实物 |
| W16 | [WiX v7.0.0 `wix.targets`](https://github.com/wixtoolset/wix/blob/v7.0.0/src/wix/WixToolset.Sdk/tools/wix.targets) | Core MSBuild `net8.0` 与完整 MSBuild `net472` 工具选择 |
| W17 | [Container element](https://docs.firegiant.com/wix/schema/wxs/container/) | attached/detached container 与多容器能力 |
| W18 | [WiX issue #6521](https://github.com/wixtoolset/issues/issues/6521) | 按体积自动拆分 container 尚未实现 |
| W19 | [WixToolset.Sdk 7.0.0 nupkg](https://api.nuget.org/v3-flatcontainer/wixtoolset.sdk/7.0.0/wixtoolset.sdk.7.0.0.nupkg) | 官方包内 x86/x64/ARM64 Burn engine 的 PE 实物 |
| W20 | [WiX v7.0.0 BasicFunctionalityTests](https://github.com/wixtoolset/wix/blob/v7.0.0/src/test/burn/WixToolsetTest.BurnE2E/BasicFunctionalityTests.cs) | 运行用例未调用 W12 的 ARM64 测试输入，不能声称已有 ARM64 runtime E2E |

### 16.2 Microsoft / Windows Installer

| 编号 | 官方资料 | 本文用途 |
| --- | --- | --- |
| M01 | [Template Summary property](https://learn.microsoft.com/en-us/windows/win32/msi/template-summary) | x64/Arm64 平台与单 MSI 不支持多平台 |
| M02 | [64-bit Windows Installer packages](https://learn.microsoft.com/en-us/windows/win32/msi/64-bit-windows-installer-packages) | Arm64 包、schema 500、64 位组件要求 |
| M03 | [ALLUSERS property](https://learn.microsoft.com/en-us/windows/win32/msi/allusers) | per-user/per-machine/双用途上下文 |
| M04 | [Installation context](https://learn.microsoft.com/en-us/windows/win32/msi/installation-context) | 安装上下文对目录、注册表和用户的影响 |
| M05 | [Single package authoring](https://learn.microsoft.com/en-us/windows/win32/msi/single-package-authoring) | dual-purpose 包限制、CommonAppData 等全局资源限制 |
| M06 | [Using Windows Installer with UAC](https://learn.microsoft.com/en-us/windows/win32/msi/using-windows-installer-with-uac) | per-machine 提升与 consent/credential prompt |
| M07 | [Rollback installation](https://learn.microsoft.com/en-us/windows/win32/msi/rollback-installation) | 默认 rollback script、保存删除文件与禁用边界 |
| M08 | [Rollback custom actions](https://learn.microsoft.com/en-us/windows/win32/msi/rollback-custom-actions) | 外部副作用需要配对 rollback action |
| M09 | [REINSTALL](https://learn.microsoft.com/en-us/windows/win32/msi/reinstall) | maintenance/repair 只覆盖已安装 feature |
| M10 | [SmartScreen reputation for Windows app developers](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation) | 文件/发布者信誉、未签名逐版、企业策略与 SAC |
| M11 | [Digital signatures and Windows Installer](https://learn.microsoft.com/en-us/windows/win32/msi/digital-signatures-and-windows-installer) | MSI 签名是可用能力，不是格式的无条件运行前提 |
| M12 | [Latest supported Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-180) | 架构/版本匹配与 redist 取得 |
| M13 | [Major upgrades](https://learn.microsoft.com/en-us/windows/win32/msi/major-upgrades) | ProductCode、UpgradeCode、版本前三段和上下文 |
| M14 | [Prevent an old package from installing over a newer version](https://learn.microsoft.com/en-us/windows/win32/msi/preventing-an-old-package-from-installing-over-a-newer-version) | MSI 降级阻止必须由作者声明 |
| M15 | [ProgramFiles64Folder property](https://learn.microsoft.com/en-us/windows/win32/msi/programfiles64folder) | 64 位 Program Files 标准目录 |
| M16 | [CommonAppDataFolder property](https://learn.microsoft.com/en-us/windows/win32/msi/commonappdatafolder) | ProgramData 语义 |
| M17 | [RemoveExistingProducts action](https://learn.microsoft.com/en-us/windows/win32/msi/removeexistingproducts-action) | major upgrade 调度与 rollback 边界 |
| M18 | [RemoveFile table](https://learn.microsoft.com/en-us/windows/win32/msi/removefile-table) | 显式删除、通配文件与空目录风险 |
| M19 | [REINSTALLMODE](https://learn.microsoft.com/en-us/windows/win32/msi/reinstallmode) | repair 模式对文件、注册表、快捷方式与校验和的不同覆盖 |
| M20 | [Source Resiliency](https://learn.microsoft.com/en-us/windows/win32/msi/source-resiliency) | 初始安装源、source list、搜索顺序与无 UI 浏览限制 |
| M21 | [Restore missing Windows Installer cache files](https://learn.microsoft.com/en-us/troubleshoot/windows-client/application-management/missing-windows-installer-cache) | cache 损坏可能在 repair/update/uninstall 才暴露 |
| M22 | [How User Account Control works](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works) | 未签名程序的 `Publisher not verified (unsigned)` 分类与黄色提升提示 |

### 16.3 Inno Setup

| 编号 | 官方资料 | 本文用途 |
| --- | --- | --- |
| I01 | [Inno Setup downloads](https://jrsoftware.org/isdl.php) | 7.0.2 x64/x86、日期、64 位 edition 与商业提示 |
| I02 | [Inno Setup 7.0.2 release](https://github.com/jrsoftware/issrc/releases/tag/is-7_0_2) | 固定 release/tag |
| I03 | [Inno Setup license](https://github.com/jrsoftware/issrc/blob/is-7_0_2/license.txt) | 商业使用、修改、再分发与标记条件 |
| I04 | [SetupArchitecture](https://jrsoftware.org/ishelp/index.php?topic=setup_setuparchitecture) | x86/x64 Setup 与 Windows on Arm 模拟范围 |
| I05 | [Architecture identifiers](https://jrsoftware.org/ishelp/index.php?topic=archidentifiers) | ARM64/x64/x86 负载与系统匹配 |
| I06 | [PrivilegesRequired](https://jrsoftware.org/ishelp/index.php?topic=setup_privilegesrequired) | admin/lowest 与 UAC；覆盖入口另见同站帮助 |
| I07 | [Same Application](https://jrsoftware.org/ishelp/index.php?topic=sameappnotes) | `AppId`、权限模式与 32/64 位模式共同决定同应用 |
| I08 | [Appending to Existing Uninstall Logs](https://jrsoftware.org/ishelp/index.php?topic=appendnotes) | 新版覆盖与卸载日志追加 |
| I09 | [Installation Order](https://jrsoftware.org/ishelp/index.php?topic=installorder) | uninstaller 落盘后的不可取消与非回滚 `[Run]` |
| I10 | [`[UninstallDelete]` section](https://jrsoftware.org/ishelp/index.php?topic=uninstalldeletesection) | 额外删除能力与通配目录警告 |
| I11 | [SignTool](https://jrsoftware.org/ishelp/index.php?topic=setup_signtool) | 可选 Authenticode 集成 |
| I12 | [SignedUninstaller](https://jrsoftware.org/ishelp/index.php?topic=setup_signeduninstaller) | 可选 signed uninstaller 与 Unknown publisher |
| I13 | [`[Files]` section `notimestamp`](https://jrsoftware.org/ishelp/index.php?topic=filessection) | 仅帮助可复现构建 |
| I14 | [DiskSpanning](https://jrsoftware.org/ishelp/index.php?topic=setup_diskspanning) | 超过 4.2 GB 压缩内容必须分片 |
| I15 | [Inno Setup 7.0.2 source: ARP `NoRepair=1`](https://github.com/jrsoftware/issrc/blob/is-7_0_2/Projects/Src/Setup.Install.pas#L331) | 无标准 ARP Repair |
| I16 | [Inno Setup 7.0.2 `[Run]` execution source](https://github.com/jrsoftware/issrc/blob/is-7_0_2/Projects/Src/Setup.MainFunc.pas#L4095-L4171) | 子进程非零退出只记录，启动异常被捕获后继续 |
| I17 | [Pascal `Exec`](https://jrsoftware.org/ishelp/topic_isxfunc_exec.htm) | 显式取得进程退出码的脚本路径 |
| I18 | [`[Files]` section](https://jrsoftware.org/ishelp/topic_filessection.htm) | 默认版本比较会保留较新有版本文件，降级可能形成混合安装 |
| I19 | [Setup event functions](https://jrsoftware.org/ishelp/topic_scriptevents.htm) | `PrepareToInstall` 可前置检测/安装 prerequisite，并以返回值阻止主安装开始 |
| I20 | [Inno Setup 7.0.2 `CodePrepareToInstall.iss`](https://github.com/jrsoftware/issrc/blob/is-7_0_2/Examples/CodePrepareToInstall.iss) | 固定 tag 的 prerequisite 失败与重启处理示例 |

### 16.4 NSIS

| 编号 | 官方资料 | 本文用途 |
| --- | --- | --- |
| N01 | [NSIS download](https://nsis.sourceforge.io/Download) | 3.12 稳定版与发布日期 |
| N02 | [NSIS 3.12 changelog](https://nsis.sourceforge.io/Docs/AppendixF.html#v3.12) | 3.12 与“Preliminary ARM64 support in makensis.nsi” |
| N03 | [NSIS v3.12 COPYING](https://github.com/NSIS-Dev/nsis/blob/v312/COPYING) | zlib/libpng、bzip2、CPL/LZMA 例外 |
| N04 | [NSIS introduction and features](https://nsis.sourceforge.io/Docs/Chapter1.html) | 脚本所有权、升级/版本检查、uninstaller 与 2 GB |
| N05 | [RequestExecutionLevel](https://nsis.sourceforge.io/Docs/Chapter4.html#arequestexecutionlevel) | user/highest/admin 与 admin 默认值 |
| N06 | [NSIS constants and context](https://nsis.sourceforge.io/Docs/Chapter4.html#varconstant) | Program Files/CommonAppData；同章 `SetRegView`/`SetShellVarContext` |
| N07 | [Compiler usage](https://nsis.sourceforge.io/Docs/Chapter3.html#usage) | `makensis` CLI |
| N08 | [`!finalize` and `!uninstfinalize`](https://nsis.sourceforge.io/Docs/Chapter4.html#finalize) | 可选 Authenticode 外部命令 |
| N09 | [NSIS v3.12 SConstruct](https://github.com/NSIS-Dev/nsis/blob/v312/SConstruct) | `TARGET_ARCH=x86/amd64/arm64` 源码构建路径 |
| N10 | [Official all-users example](https://github.com/NSIS-Dev/nsis/blob/v312/Examples/install-shared.nsi) | 手写 ARP、`NoRepair=1`、uninstaller 与删除 |
| N11 | [NSIS build documentation](https://github.com/NSIS-Dev/nsis/blob/v312/Docs/src/build.but) | `SOURCE_DATE_EPOCH` 只用于构建 NSIS 工具本身 |
| N12 | [NSIS v3.12 GitHub build workflow](https://github.com/NSIS-Dev/nsis/blob/v312/.github/workflows/build.yml) | 官方 CI 没有 ARM64 构建基线 |
| N13 | [NSIS v3.12 `makensis.nsi`](https://github.com/NSIS-Dev/nsis/blob/v312/Examples/makensis.nsi) | 变更日志所称 preliminary ARM64 support 的自安装脚本范围 |
| N14 | [SourceForge NSIS 3.12 `nsis-3.12.zip`](https://sourceforge.net/projects/nsis/files/NSIS%203/3.12/nsis-3.12.zip/) | 本地 PE 检查所用固定官方项目文件的取得链 |

### 16.5 项目内复用证据

| 编号 | 项目资料 | 本文用途 |
| --- | --- | --- |
| P01 | [预实现工具链基线](preimplementation-toolchain-baseline-2026-08-10.md) | WinUI 3 self-contained、VC Runtime 缺口、未签名/MSIX 边界 |
| P02 | [Windows 状态持久化与恢复](windows-state-persistence-and-recovery.md) | ProgramData 分类、N/N-1、卸载/清理与 ACL 缺口 |
| P03 | [发行来源与 CI 证据](release-provenance-and-ci-evidence.md) | GitHub asset、未签名、构建证据和实机边界 |
| P04 | [特权下载与自更新安全](elevated-download-execution-security.md) | 自更新真实性根、事务提交后健康确认与 N-1 恢复边界 |

## 17. 本地发行物检查与证据限制

为验证 WiX 7 官方包是否实际包含原生 ARM64 Burn engine，本研究下载并解压了 `WixToolset.Sdk 7.0.0` 官方 nupkg：[W19]

- nupkg SHA-256：`af8f72fb2550e9c2cf00b6eaf5e3ed811514aa2b3f344dfa342540af39676979`
- `tools/net8.0/arm64/burn.exe`：PE32+ Aarch64
- `tools/net8.0/x64/burn.exe`：PE32+ x86-64
- `tools/net8.0/x86/burn.exe`：PE32 Intel 80386

这证明官方 SDK 包中三种 Burn engine 的机器类型，不证明自定义 BA、链内包、工作台负载或真实 ARM64 执行通过。W12 只是 ARM64 构建测试输入，v7.0.0 的对应运行用例没有调用它，不能升级成 runtime E2E 证据。[W12][W20]

为验证 Inno Setup 7.0.2 的官方工具发行物，本研究下载了固定 GitHub Release 中的两个安装器：[I02]

- `innosetup-7.0.2-x64.exe`：SHA-256 `5ad54ca3def786f8f4212552e54cc6d8d61329e2d24a1cfee0571d42c2684ff1`，PE32+ x86-64
- `innosetup-7.0.2-x86.exe`：SHA-256 `2b8734490a83f1ed074022b85d46c5ce9c3e2fbe9b63a45c28a74b478ac3a94f`，PE32 Intel 80386

两项本地摘要与 GitHub Release API 的 asset digest 一致。这只证明 Inno 工具自身的官方安装器，不证明项目以后由 `ISCC.exe` 生成的 Setup/uninstaller 机器类型、脚本行为或 ARM64 模拟运行；后者仍按官方架构文档与真实样机验收。[I04][I05]

为验证 NSIS“正式下载物是否原生 ARM64”，本研究从固定的 SourceForge NSIS 3.12 项目文件页下载并解压了 `nsis-3.12.zip`：[N14]

- ZIP SHA-256：`56581f90db321581c5381193d796fffcf2d24b2f8fed2160a6c6a3baa67f2c4f`
- `makensis.exe`：PE32 Intel 80386
- 所有随 ZIP 提供的可执行压缩 stubs：PE32 Intel 80386；`Stubs/uninst` 是图标资源，不是 PE
- 所有随 ZIP 打包的官方 plugin DLL：PE32 Intel 80386

该检查只证明这个精确下载物，不证明未来版本、第三方 special build 或自行从 v3.12 源码构建的 ARM64 结果。正式版本锁定时必须从上游重新下载、记录最终 URL/大小/摘要并在 Windows 上复核 PE headers。

其他限制：

- 本文没有在 Windows 上构建任何候选，也没有运行安装、升级、repair、rollback、卸载或 SmartScreen 测试。
- FireGiant/WiX、Microsoft、Inno 和 NSIS 网页会更新；实现时锁定 tag/包版本和许可快照，不把查询日“latest”当永久事实。
- 官网未明确承诺的能力均按不确定项处理。例如 Inno/NSIS 生成产物是否 bit-for-bit、WiX 对项目超大离线内容的实际容器上限，都必须由样机验证。
- 本文没有把维护者推荐答案回写正式领域词汇或 ADR，因为 `grill-with-docs` 会话尚未得到用户回答，不能伪造共享理解。
- 本文没有选择安装器。只有 `INSTALLER-D*` 与相关状态/发行矩阵决定完成、P0 样机证据通过后，才应形成 ADR、版本锁和实现事项。
