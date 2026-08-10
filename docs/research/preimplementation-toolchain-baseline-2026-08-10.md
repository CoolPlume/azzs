# 预实现工具链基线研究（2026-08-10）

- 状态：研究结论，尚未形成版本锁定决策
- 研究日期：2026-08-10（北京时间）
- 适用范围：初装工作台首次实现前的 Windows 工具链、构建、测试与发行准备
- 来源边界：只采用 Microsoft Learn、Visual Studio/MSVC 官方发布与支持页、Windows App SDK 官方发布页；通过 Jina Reader 读取。Exa 在本次环境中不可用，未安装替代工具，也未用二手资料补空白。

## 1. 先给结论

1. 截至本研究时间，Windows App SDK/WinUI 3 的最新 Stable 是 **2.3.1**，发布于 2026-07-16；它属于 Windows App SDK 2.0 的 Current 生命周期，2.0 的服务终止日为 2027-04-29。处于支持期还不够，微软同时要求使用该版本线的最新补丁才满足支持条件。[M1][M2][M3]
2. Visual Studio 2026 Stable 的最新公开版本是 **18.8.2**，发布于 2026-07-28；稳定 MSVC 基线是 **Build Tools 14.50 / 编译器 19.50**。14.51 在官方发布说明中仍标为 Preview，并明确不应进入生产环境。[M12][M14][M16]
3. 最新 Stable Windows SDK 是 **10.0.28000.2526**（2026-07）。最新 SDK 可以提供此前 Windows 版本的累计 API 表面，但应用仍必须只调用最低运行系统实际具备的 API，或做运行时能力检查。[M4][M17][M18]
4. MSVC 目前**没有** `/std:c++26`。稳定版编译器可用的前沿模式是 `/std:c++latest`；微软明确说明该模式同时打开已实现的下一份 ISO 草案能力以及仍在进行中或实验性的能力，可能发生破坏性变化或被移除。因此“使用稳定 MSVC”不能推出“全部 C++26 能力稳定”。[M15][M16]
5. Windows App SDK 支持 C++ 项目自包含部署。设置 `WindowsAppSDKSelfContained=true` 后，框架内容进入构建输出；对于 unpackaged 应用，依赖位于可执行文件旁，可按目录 xcopy 部署。不同体系结构仍要生成各自制品，且少数依赖 Singleton MSIX 包的 API 会破坏纯 xcopy 的简单性。[M9][M10]
6. 当前 Windows App SDK 在运行兼容层面仍向后兼容到 Windows 10 1809，因此 Windows 10 22H2 位于其版本范围内；x64 与 ARM64 都有官方运行时下载和 MSVC 构建路径。[M1][M3][M19][M20] 但 Windows 10 22H2 Home/Pro/Enterprise/Education 已于 2025-10-14 结束常规支持，ESU 只提供有限安全更新而不恢复一般技术支持。[M6][M7][M8] 所以只能把它写成项目的最低设计目标，不能笼统声称这是微软当前完整支持的组合。
7. “长期不签名”与“正式使用 MSIX 分发”不能同时成立。Windows 通常要求可部署 MSIX 使用受信任签名；Windows 11 虽允许通过管理员 PowerShell 和 `-AllowUnsigned` 安装特殊测试包，但微软明确要求不要用于广泛分发，而且该例外不能覆盖 Windows 10 22H2。[M26][M27] 若保持 ADR-0005，首版候选应是 unpackaged 自包含便携目录和传统安装器，而不是 MSIX；这仍会面对 SmartScreen 警告或企业策略阻断。[M28]
8. 构建系统尚不必立即二选一。微软对 WinUI 3 C++ 的直接路径是 Visual Studio/MSBuild 项目；CMake、`CMakePresets.json` 和 CTest 则有成熟的官方 Visual Studio 集成。[M11][M21][M22] 一个值得验证、但尚未替项目决定的候选布局是：**MSBuild 管 WinUI 3/XAML 宿主，CMake 管可移植核心和无界面测试**。

## 2. 可作为版本冻结候选的快照

下表是“若今天开始做工具链样机”的候选，不是仓库已经作出的选择。任何一项只有在第 8 节验证完成后，才能写入版本锁定文件。

| 层 | 2026-08-10 官方事实 | 候选基线 | 当前判断 |
| --- | --- | --- | --- |
| IDE/Build Tools | Visual Studio 2026 Stable 18.8.2 于 2026-07-28 发布。[M12] | VS 2026 18.8.2 Stable 或其可重现的固定安装布局 | 可进入样机验证 |
| MSVC | 14.50/19.50 是 VS 2026 的稳定工具集；14.51 仍为 Preview。[M14][M16] | 固定 14.50 的实际小版本，并记录 `cl /Bv` | 可进入样机验证；不能写浮动 `latest` |
| C++ 模式 | 没有 `/std:c++26`；`/std:c++latest` 含不稳定草案能力。[M15] | 若项目仍坚持 C++26 目标，候选是 `/std:c++latest` 加“允许使用的特性清单” | **必须先解决口径冲突** |
| Windows App SDK/WinUI 3 | 最新 Stable 为 2.3.1；2.0 Current 服务到 2027-04-29。[M1][M2][M3] | `Microsoft.WindowsAppSDK` 2.3.1，锁精确版本 | 可进入样机验证 |
| Windows SDK | 最新 Stable 为 10.0.28000.2526；28000 与 26100 版本线均仍受支持。[M17][M18] | 10.0.28000.2526，锁精确版本 | 可进入样机验证；调用新 OS API 仍需保护 |
| CMake | VS 2026 官方发布说明称内置 CMake 4.1.2，并支持 Visual Studio 2026 generator；Visual Studio 推荐 `CMakePresets.json`。[M14][M21] | 若核心采用 CMake，固定 4.1.2 并提交 presets | 可进入样机验证 |
| 测试入口 | CTest 已集成 Test Explorer；Google Test 与 Microsoft C++ 测试框架都有官方 VS 集成。[M22][M23][M24] | 核心用 CTest 统一命令入口；具体断言框架另行选择并锁版本 | 尚需决定依赖与可移植性取舍 |

### 2.1 为什么不建议选 Windows App SDK 1.8

1.8 在 2026-08-10 仍属 Maintenance，但服务将于 2026-09-09 结束；2.3.1 是当前 Stable，2.0 生命周期到 2027-04-29。[M1] 除非样机证明 2.3.1 破坏 Windows 10 22H2、自包含、x64/ARM64 或 C++/WinRT，否则为了只剩约一个月服务期的 1.8 建立新项目基线没有明显维护收益。这只是选择规则，不是对 2.3.1 的预先验收。

### 2.2 WinUI 3 不另设一套版本号

Windows App SDK 的 Stable 通道包含 WinUI；官方下载页把 2.3.1 源码指向 `microsoft-ui-xaml` 的 `winui3/release/2.3.1` 标签。[M1][M3] 本项目应以精确的 Windows App SDK 包版本作为 WinUI 3 版本证据，避免在文档中维护一个没有独立锁定来源的“WinUI 版本”。

## 3. Windows 10 22H2、x64 与 ARM64

### 3.1 已确认事实

- Windows App SDK 官方概览与发布通道说明当前向后兼容到 Windows 10 1809；最新 SDK 与目标 OS 版本彼此独立。[M1][M4]
- 官方 Windows App SDK 下载页为 2.3.1 分别提供 x64、x86、ARM64 运行时安装器，Redistributable ZIP 同时包含这些体系结构。[M3]
- MSVC 命令行工具可以从 x64 主机生成 x64 或 ARM64 目标，`vcvarsall.bat` 能固定目标体系结构、Windows SDK 和工具集版本。[M19]
- Windows on Arm 官方文档支持用 Visual C++ 构建原生 ARM64 C/C++ Windows 应用；Windows 10 on Arm 可模拟 x86，但 Windows 11 才增加 x64 模拟。因此为 Windows 10 22H2 on Arm 提供原生 ARM64 制品是更明确的路线，不能把 x64 制品在该系统上的运行当作兜底。[M20]
- Windows SDK 的编译时 API 表面与运行系统的真实 API 能力不同。使用较新 SDK 编译时，新 API 必须通过运行时存在性检查或被隔离在明确的平台适配分支中。[M4]

### 3.2 支持声明必须缩窄

Windows 10 22H2 的普通 Home/Pro 和 Enterprise/Education 生命周期都在 2025-10-14 结束。[M6][M7] Windows 10 ESU 要求设备为 22H2，但它只继续提供 Critical/Important 安全更新；官方明确说明 ESU 不包含一般技术支持。[M8]

同时，Windows App SDK 的 `support` 页面截至访问时仍只列到 1.8，且页面中的 Windows 版本表与当前生命周期信息没有完全同步；Windows App SDK 发布通道页则写明只有仍受支持的 Windows 版本才享受微软支持。[M1][M5] 因而本文能确认的是：

- **能确认**：Windows 10 22H2 高于 Windows App SDK 的最低运行版本，属于技术兼容范围。
- **不能确认**：微软会对“Windows 10 22H2 + Windows App SDK 2.3.1”的普通非 ESU 设备提供完整产品支持。
- **项目可继续保留的表述**：Windows 10 22H2 是项目按设计面向的最低版本；不承诺跨版本兼容性测试，也不把微软已结束支持的 OS 描述为仍获完整厂商支持。

### 3.3 不要让 MSBuild 的最低版本属性代替产品规则

官方文档区分编译所用的 `WindowsTargetPlatformVersion` 与最低运行版本；较新 Windows SDK 可以编译面向更早系统的应用。[M4] 当前项目又明确要求低于 Windows 10 22H2 时先警告而不是单纯阻止启动，因此实施时应分别验证：

1. 构建使用的 Windows SDK 版本；
2. manifest/MSBuild 的最低安装或加载门槛；
3. 产品自身对低于 22H2 的启动警告；
4. 平台 API 的逐项能力检测。

不能只设置一个 `TargetPlatformMinVersion` 就声称上述四件事都已经完成。

## 4. C++26 的真实可用性

### 4.1 官方事实

截至 2026-08-05 更新的 MSVC `/std` 文档只列出 `/std:c++14`、`/std:c++17`、`/std:c++20`、`/std:c++23preview` 和 `/std:c++latest`，没有 `/std:c++23` 或 `/std:c++26`。[M15]

`/std:c++latest` 的官方语义是启用当前已实现的下一份 ISO C++ 工作草案能力，以及部分进行中和实验性能力。微软明确警告：尚未批准的特性可能发生破坏性变化或被移除，并按 as-is 提供。[M15] MSVC 14.50 的发布说明与符合性表证明它已经实现一批 C++23/26 特性，但没有声明 C++26 已完整实现。[M14][M16]

### 4.2 与现有技术基线的张力

仓库同时要求“C++26 目标”和“发布不依赖 preview/实验能力”。稳定版 MSVC 本身满足前一半的“稳定工具链”，但 `/std:c++latest` 的能力集合不满足“每项语言能力天然稳定”的推断。开始写产品代码前至少要明确以下一种解释：

| 解释 | 好处 | 代价/风险 |
| --- | --- | --- |
| A. 用 `/std:c++latest`，只允许经过清单确认的已实现特性 | 最接近既有 C++26 目标 | 模式仍会打开更宽的草案表面；升级编译器时必须做源码、ABI 与标准库回归 |
| B. 暂用稳定的 `/std:c++20`，等正式 `/std:c++26` | 语言模式边界最稳定 | 明确偏离 ADR-0022，必须先修订决定，不能悄悄执行 |
| C. UI/WinRT 用较保守模式，核心用 `/std:c++latest` | 降低 XAML/C++/WinRT 工具链风险 | 同一产品出现两种语言模式，跨边界类型和 ABI 需要更严格约束，也偏离当前“界面与核心一致模式” |

本文不替项目选择。若保留 A，最低防线应包括：固定 MSVC 小版本、维护特性允许清单、禁止未列入特性进入持久格式和稳定接口、x64/ARM64 同编译、升级前后完整测试，以及记录 `_MSVC_LANG`、`__cplusplus`（配合 `/Zc:__cplusplus`）和相关 feature-test macros。[M15][M16]

## 5. 自包含 C++/WinRT 部署

### 5.1 官方已保证的部分

- Windows App SDK 项目默认是 framework-dependent；设置 `WindowsAppSDKSelfContained=true` 可切换为 self-contained。[M9]
- 对 packaged 应用，Windows App SDK 依赖作为内容进入 MSIX；对 packaged with external location 或 unpackaged 应用，依赖复制到 `.exe` 旁，可以 xcopy 或交给自定义安装器部署。[M9][M10]
- unpackaged self-contained 可执行项目默认启用 UndockedRegFreeWinRT 初始化；库或测试 DLL 若由未初始化的宿主加载，可能需要显式处理。[M9]
- push/app notifications 等少数 API 依赖 Singleton 包。自包含应用必须按 `IsSupported` 降级、额外部署 MSIX 依赖，或不使用这些 API；不能假设“self-contained”让全部 Windows App SDK API 都摆脱包依赖。[M9]
- 每个目标体系结构都必须包含对应二进制；对本项目而言应生成相互独立的 x64 与 ARM64 便携/安装制品。[M10]
- self-contained 的代价是体积更大、unpackaged 启动/内存成本可能更高、Windows App SDK 不会被共享框架自动服务；项目必须随每次安全或可靠性更新重建并重发。[M10]

### 5.2 Visual C++ Runtime 仍有未闭合问题

Windows App SDK 的 self-contained 文档证明其自身框架内容能随应用部署，但没有在本文查到的官方段落中承诺“任意 C++/WinRT 输出都无需 Visual C++ Runtime”。Windows App SDK 的 framework-dependent 部署架构反而明确写明 unpackaged C++ 应用需要 Visual C++ Redistributable。[M11]

MSVC 提供两条相关机制：`/MT` 把静态 CRT 链入模块，`/MD` 使用 DLL CRT；同一次链接的所有模块必须采用一致的运行库选项。[M30] 微软也允许 app-local 放置可再分发 DLL，但因无法独立服务而不推荐，首选是中央安装 Visual C++ Redistributable。[M29][M31]

因此“所有工作台发行包自带自身运行时依赖”尚不能仅凭 `WindowsAppSDKSelfContained=true` 判定完成。实施样机必须在以下候选中验证并记录一个：

1. 全部项目可用 `/MT`，且 WinUI 3、C++/WinRT、第三方库和测试配置没有运行库冲突；
2. 便携版合法携带精确、架构匹配的 app-local VC Runtime 文件，并接受自行服务责任；
3. 安装版捆绑并安装架构匹配、版本不低于构建工具的 VC Redistributable，但便携版仍需另一条闭合方案。

在干净系统上实际启动并做依赖扫描之前，不能写“无需预装任何运行时”已通过。也不能把“无 .NET 源码”当成“发行目录没有 .NET 运行时依赖”的证据。

### 5.3 单文件不是当前已确认目标

官方部署概览现已写明某些 unpackaged self-contained WinUI 3 应用支持 `PublishSingleFile`，但 self-contained 指南仍特别讨论托管发布与原生依赖，而且本次资料没有给出 C++/WinRT 单文件方案的明确合同。[M9][M10] 项目需求是自包含便携版，不是单个 EXE；首次实现不应额外承诺单文件，除非单独样机证明 C++/WinRT、XAML、MRT 资源和全部架构依赖都能被官方支持地打包和启动。

## 6. MSBuild、CMake 与测试方案

### 6.1 官方能力边界

- WinUI 3 快速入门要求 Visual Studio 2026 的 WinUI application development workload；C++ 还要安装 C++ WinUI app development tools。self-contained 配置也直接以 `.vcxproj`/MSBuild 属性说明。[M9][M25]
- Visual Studio 原生支持 CMake；微软推荐 `CMakePresets.json`，因为同一份配置可用于 Visual Studio、命令行、CI、Windows、Linux 和 macOS。Ninja 与 Visual Studio generator 都受支持。[M21]
- VS 2026 发布说明称内置 CMake 4.1.2，支持 Visual Studio 2026 generator。[M14]
- CTest 集成 Test Explorer，也能承接 Google Test 或 Boost；没有测试适配器时仍可按 CTest 测试可执行文件运行和调试，但单测试方法栈信息较弱。[M22]
- Google Test 与 Microsoft Unit Testing Framework for C++ 都随 Desktop Development with C++ 工作负载提供集成；前者仍是第三方框架，后者是 Windows/Visual Studio 专用。[M23][M24]
- `VSTest.Console.exe` 能执行自动化测试并输出 TRX；ARM 机器上的自动化测试官方要求使用该命令行入口。跨编译出 ARM64 二进制不等于已经在 ARM64 上执行测试。[M25]

### 6.2 候选布局，而非最终决定

| 方案 | 优点 | 主要风险 | 当前建议 |
| --- | --- | --- | --- |
| 全部 MSBuild | WinUI/XAML/C++/WinRT 路径最直接，x64/ARM64 配置统一 | 可移植核心没有独立跨平台构建入口 | 适合作为最小 WinUI 工具链样机，不足以单独证明未来 macOS 可移植性 |
| WinUI 用 MSBuild，核心用 CMake/CTest | 符合 Windows UI 与可移植核心边界，核心可在 CI/未来 macOS 独立构建 | 两套构建的产物连接、配置映射和依赖恢复必须只有一个权威入口 | **优先验证的候选** |
| 全部 CMake | 核心构建最统一 | 本次没有找到微软一手资料证明完整 WinUI 3 XAML/C++ 工程以纯 CMake 作为受支持首选路径 | 暂不作为默认，除非先有小样机证据 |

若验证混合方案，应要求：

1. CMake 只拥有可移植核心与其测试，不复制 WinUI/XAML 项目规则；
2. MSBuild 只拥有界面宿主、Windows 适配器和最终发行装配；
3. 一个顶层命令能固定版本并完成还原、x64/ARM64 构建和测试；
4. Debug/Release、运行库、异常模型、语言模式和警告级别在两套构建间显式映射；
5. 核心源码不能在两套构建中以不同宏或不同 ABI 被静默编译成两种语义。

### 6.3 建议的自动化检查层次

这部分是项目建议，不代表测试已经存在：

| 层 | 建议入口 | 必须覆盖 |
| --- | --- | --- |
| 可移植核心单元/性质测试 | CMake + CTest；断言框架另行选择并锁版本 | 目录解析、依赖规划、批次状态、恢复、结果判断；不启动 WinUI，不访问真实系统资源 |
| 核心与适配器合同测试 | 同一测试合同跑 fake 与 Windows adapter | 状态唯一来源、错误映射、取消/重试/恢复语义 |
| WinUI 与核心边界测试 | MSBuild 测试项目/Test Explorer 或可执行合同测试 | UI 不复制业务规则，命令与状态映射一致 |
| 构建矩阵 | x64 与 ARM64 Release 均编译 | 固定 MSVC、Windows SDK、Windows App SDK、CMake 版本；禁止 preview/floating latest |
| 执行矩阵 | x64 原生执行；ARM64 原生设备/runner 执行 | 核心与合同测试必须真实运行，不能只 cross-build |
| 发行烟雾测试 | 干净 Windows 镜像上的便携版与安装版 | 无开发工具、无预装 Windows App SDK、选定 VC Runtime 策略、断网启动、卸载边界 |

Google Test 的 VS 集成很成熟，[M23] 但是否把它作为项目依赖仍应根据许可证记录、版本锁定、离线还原和未来 macOS 测试需求另行决定。Microsoft C++ 测试框架无需新增第三方断言框架，[M24] 但会让可移植核心测试入口绑定 Visual Studio。CTest 是测试编排入口，不等于断言框架。[M22]

## 7. 未签名便携版、安装版与 MSIX

### 7.1 事实对照

| 发行形态 | 官方事实 | 对本项目的含义 |
| --- | --- | --- |
| unpackaged self-contained 便携目录/ZIP | Windows App SDK 依赖可随 `.exe` 放在同目录并 xcopy；不同架构要分别带齐文件。[M9][M10] | 与无签名 ADR 相容，但必须解决 VC Runtime、资源完整性、更新原子性与干净机启动 |
| 未签名传统 EXE/MSI 安装器 | SmartScreen 对无签名文件显示“Windows protected your PC”；用户通常需选择继续，企业策略可完全禁止继续。每个新版本的无签名文件信誉从零开始。[M28] | 可以作为候选安装版，但无法承诺无警告、无人值守或所有受管设备可安装 |
| 正常 MSIX | Windows 要求 MSIX 由有效代码签名证书签名，设备还必须信任该证书。[M26] | 与“长期不签名”直接冲突 |
| Windows 11 unsigned MSIX 测试例外 | 需特殊 OID、PowerShell `Add-AppxPackage -AllowUnsigned`；含可执行内容时通常需管理员并按 all-users 安装；微软明确不用于广泛分发。[M27] | 不能作为面向 Windows 10 22H2 用户的正式安装版，也不应包装成普通双击安装体验 |

签名不仅是显示发布者：签名 MSIX 可让 Windows 校验包内容完整性；时间戳还影响证书过期后能否继续接受安装。[M26] 当前 ADR 同时拒绝签名和项目提供的 SHA-256，因此用户无法从项目提供的信息验证发布者或下载完整性；这是已有产品取舍，不是工具链能够消除的问题。

### 7.2 SmartScreen 的确定风险

微软说明 SmartScreen 同时看发布者信誉和文件哈希信誉。无签名文件不能把信誉传到新版本，每次更新都要重新积累；用户可能看到“Windows protected your PC”，受管设备策略还可以不允许绕过。Windows 11 的 Smart App Control 甚至可能直接阻止无签名文件，除非文件已有正面信誉。[M28]

因此发行说明中的“可能出现 SmartScreen 警告”不能被当作唯一缓解。发行验收还应真实记录：

- 从 GitHub Releases 下载后的便携 EXE与安装器各自表现；
- ZIP 解压后各 executable 的 Mark-of-the-Web/SmartScreen 表现；
- 需要提权的安装/执行步骤显示什么发布者信息；
- 企业策略禁止绕过时的失败方式；
- x64 与 ARM64、在线与离线资源版是否一致。

这些结果会随文件哈希、策略和系统版本变化，只能作为当次发行证据，不能写成永久保证。[M28]

### 7.3 当前最小可行发行组合

若不改变 ADR-0005，最小候选组合是：

1. x64 unpackaged self-contained 便携 ZIP；
2. ARM64 unpackaged self-contained 便携 ZIP；
3. 用传统、未签名安装器安装同一套 x64 应用文件；
4. 用传统、未签名安装器安装同一套 ARM64 应用文件；
5. 在线/离线资源版只改变受控资源内容，不改变运行时、核心或安全边界。

这只是候选集合。安装器技术、升级/回滚、设备级状态保留、卸载清理与离线超大制品上限都尚未由本研究决定。MSIX 在保持“不签名”的前提下应从正式候选中排除，而不是等发布时才发现无法部署。[M26][M27]

## 8. 实施前必须现场验证

以下项目不能靠继续阅读文档替代。它们仍属于工具链样机/验收准备，不等于开始实现产品功能。

### P0：会阻塞项目骨架

- [ ] 在实际 Windows 构建机记录 Visual Studio edition/channel/18.8.2、安装组件清单、MSVC 的完整 `cl /Bv`、MSBuild 版本、CMake 版本和 Windows SDK 目录；证明安装布局可以离线重建。[M12][M19]
- [ ] 用 Windows App SDK 2.3.1 创建最小 C++/WinRT + XAML 应用，分别以 x64/ARM64、Debug/Release、packaged/unpackaged self-contained 构建；记录生成的 NuGet lock、MSBuild 属性和实际文件清单。[M3][M9]
- [ ] 验证 `/std:c++latest` 能贯穿 XAML 生成、C++/WinRT 生成代码、核心静态库和最终链接；列出实际采用的 C++26 特性，而不是只看编译开关。[M15][M16]
- [ ] 决定并验证 MSBuild/CMake 所有权：尤其是核心静态库如何只构建一次、如何被 WinUI 宿主引用、如何避免 Debug/Release 或 `/MT`/`/MD` 不一致。[M21][M30]
- [ ] 在没有 Visual Studio、没有预装 Windows App SDK、没有项目预置 VC Runtime 的干净系统上启动便携输出；依据结果选择 CRT 部署方案。[M9][M29][M30][M31]

### P1：会阻塞“支持 x64/ARM64、自包含、Windows 10 22H2”的声明

- [ ] x64 原生机器执行全部核心与合同测试；ARM64 原生机器执行同一套测试，不能用 cross-build 代替。[M19][M20][M25]
- [ ] 在 Windows 10 22H2 x64 与可获得的 Windows 10 22H2 ARM64 环境做最小启动、XAML 资源加载、窗口创建、文件/注册表/进程适配器烟雾；若 ARM64 环境不可得，明确留下未验证缺口，不改写成通过。
- [ ] 在 Windows 11 x64 与 ARM64 重复自包含、安装、卸载和升级烟雾，分离“平台仍受厂商支持”与“项目最低目标兼容”的证据。[M5][M20]
- [ ] 扫描最终目录的 imports/runtime 文件，证明没有 `hostfxr`、`coreclr` 或其他 .NET 运行时依赖；同时确认每个文件的目标架构，没有把 x64 DLL 混入 ARM64 制品。
- [ ] 完全断网启动，确认 WinUI/XAML/MRT 资源、目录内容和初始化核心不触发运行时下载；涉及 Singleton 的 API逐项确认未用、可降级或有显式部署方案。[M9]

### P1：会阻塞发行形态

- [ ] 从真实 GitHub Release 下载未签名便携 ZIP 与安装器，记录 SmartScreen、Smart App Control、UAC 和企业策略下的实际行为；不要用本地构建目录冒充下载路径。[M28]
- [ ] 验证安装器在没有网络和没有预装运行时的系统上可安装、启动、升级、失败回滚和卸载；卸载不得删除已有设备级恢复记录。
- [ ] 明确证明正式制品不包含 unsigned MSIX；若未来重新考虑 MSIX，先修改“不签名”决定并设计证书、信任、时间戳和身份迁移，不使用 Windows 11 测试例外。[M26][M27]
- [ ] 对在线/离线、x64/ARM64、便携/安装的文件命名和选择逻辑做机器校验，避免用户拿到错误体系结构。

### P2：构建与测试可维护性

- [ ] 让同一条非交互命令完成依赖还原、Release x64/ARM64 构建、CTest/VSTest、结果文件输出；任一步骤失败返回非零。[M22][M25]
- [ ] 固定 NuGet、CMake、测试框架及安装器依赖版本；证明断网缓存可重建，不解析浮动 `latest`。
- [ ] 保存编译器/链接器命令、二进制依赖摘要、测试结果和制品文件清单，作为后续发布证据，而不是只保存 IDE 截图。
- [ ] 在正式锁定 Windows SDK 28000 前，比较 28000.2526 与仍受支持的 26100.8876 对 WinUI/C++/WinRT、Windows 10 22H2 运行和安装器工具的影响；没有差异证据时按项目规则选更新的稳定版。[M17][M18]

## 9. 仍未能由官方资料确认

1. **C++/WinRT 单文件发布**：查到的官方单文件说明不足以证明本项目的原生 C++/WinRT + XAML + MRT 输出可以稳定做成单 EXE；当前只确认目录级 self-contained。[M9][M10]
2. **Windows App SDK 2.3.1 在 Windows 10 22H2 的完整厂商支持口径**：技术最低版本覆盖，但 OS 已结束常规支持，Windows App SDK `support` 页面又滞后于 2.x 发布。[M1][M5][M6][M7]
3. **纯 CMake 的完整 WinUI 3 XAML 工程是否是微软当前推荐/完整支持路径**：找到 CMake 与 C++ 的官方集成，但没有找到取代 WinUI `.vcxproj`/MSBuild 的同等官方指引。[M9][M21]
4. **`WindowsAppSDKSelfContained=true` 是否自动闭合本项目全部 VC Runtime 依赖**：官方资料没有给出足以替代干净机依赖验证的保证。[M9][M11][M29]
5. **Windows 10 22H2 ARM64 的可获得、可重现测试环境**：官方证明 Windows 10 on Arm 和原生 ARM64 构建存在，[M20] 但本研究没有确认项目当前能获得对应硬件/镜像及授权。
6. **传统安装器技术**：MSI、WiX、Inno Setup、NSIS 或自研 bootstrapper 均未在既有 ADR 中决定；本研究只证明 unsigned MSIX 不适合当前正式分发约束。
7. **CMake 的 C++26 映射细节**：在采用的 CMake 4.1.2 与 VS generator/Ninja 组合中，`CXX_STANDARD 26` 最终生成什么 MSVC 参数，应以 configure 输出和编译命令验证，不能从 `/std` 文档反推。

## 10. 留给项目的五个明确决定

这些问题已经可以在不继续泛化调研的情况下进入决策；本文不替维护者作答。

1. **C++26 的含义**：接受 `/std:c++latest` + 特性允许清单，还是修订 ADR 改用稳定正式语言模式？
2. **构建所有权**：全 MSBuild，还是 MSBuild（WinUI）+ CMake/CTest（核心）的双入口、单装配方案？
3. **CRT 闭合方式**：`/MT`、app-local VC Runtime，还是安装版捆绑 VC Redistributable，并如何让便携版同样自包含？
4. **安装版格式**：在不签名约束下选择哪一种传统 installer，并正式排除 MSIX；还是反过来修订“不签名”决定？
5. **Windows 10 22H2 声明**：是否接受“项目最低设计目标，但已非微软常规支持 OS”的准确措辞，并为无法获得的 ARM64 旧系统测试留下显式证据缺口？

## 11. 官方来源

除单独注明外，所有来源访问日期均为 2026-08-10。

| 编号 | 官方资料 | 页面日期/本研究用途 |
| --- | --- | --- |
| M1 | [Windows App SDK release channels](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-channels) | 页面快照列出 2.3.1 Stable（2026-07-16）、2.0 生命周期与 OS 支持条件 |
| M2 | [Windows App SDK 2.0 release notes](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-notes/windows-app-sdk-2-0?pivots=stable) | 2.3.1 发布说明；2.0 起采用 SemVer |
| M3 | [Downloads for the Windows App SDK](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/downloads) | 2.3.1 x64/x86/ARM64 installer、Redistributable 与 WinUI 源码标签 |
| M4 | [Windows versions and SDK overview](https://learn.microsoft.com/en-us/windows/apps/get-started/versioning-overview) | OS/Windows SDK/Windows App SDK 的独立版本关系、C++ MSBuild 属性、运行时能力检查 |
| M5 | [Windows App SDK and supported Windows releases](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/support) | Windows App SDK 的 OS 支持口径；页面截至访问时版本表只到 1.8 |
| M6 | [Windows 10 Home and Pro lifecycle](https://learn.microsoft.com/en-us/lifecycle/products/windows-10-home-and-pro) | 22H2 于 2025-10-14 结束支持 |
| M7 | [Windows 10 Enterprise and Education lifecycle](https://learn.microsoft.com/en-us/lifecycle/products/windows-10-enterprise-and-education) | 22H2 于 2025-10-14 结束支持 |
| M8 | [Extended Security Updates program for Windows 10](https://learn.microsoft.com/en-us/windows/whats-new/extended-security-updates) | ESU 只面向 22H2、范围与一般技术支持限制 |
| M9 | [Windows App SDK deployment guide for self-contained apps](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/self-contained-deploy/deploy-self-contained-apps) | `WindowsAppSDKSelfContained`、xcopy、UndockedRegFreeWinRT、Singleton 例外 |
| M10 | [Windows App SDK deployment overview](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/deploy-overview) | framework-dependent/self-contained 取舍、体系结构与服务责任 |
| M11 | [Windows App SDK deployment architecture](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/deployment-architecture) | unpackaged bootstrap/运行时结构与 VC Redistributable 要求 |
| M12 | [Visual Studio 2026 release history](https://learn.microsoft.com/en-us/visualstudio/releases/2026/release-history) | Stable 18.8.2，发布于 2026-07-28 |
| M13 | [Visual Studio 2026 product lifecycle and servicing](https://learn.microsoft.com/en-us/visualstudio/releases/2026/servicing-vs) | 年度版本两年 Modern Lifecycle 与通道规则 |
| M14 | [Visual Studio 2026 release notes](https://learn.microsoft.com/en-us/visualstudio/releases/2026/release-notes) | MSVC 14.50、14.51 Preview、C++23/26 进展、CMake 4.1.2、ARM64 工具状态 |
| M15 | [`/std` - Specify language standard version](https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version?view=msvc-180) | 发布/更新时间 2026-08-05；可用模式和 `/std:c++latest` 风险 |
| M16 | [Microsoft C/C++ language conformance](https://learn.microsoft.com/en-us/cpp/overview/visual-cpp-language-conformance?view=msvc-180) | MSVC 14.50/19.50 与逐项标准能力状态 |
| M17 | [Windows SDK overview](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/) | 10.0.28000 系列与仍受支持 SDK 版本线 |
| M18 | [Windows SDK downloads](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/downloads) | 10.0.28000.2526 与 10.0.26100.8876，均为 2026-07 发布项 |
| M19 | [Use the Microsoft C++ Build Tools from the command line](https://learn.microsoft.com/en-us/cpp/build/building-on-the-command-line?view=msvc-180) | x64/ARM64 cross tools、SDK/toolset 固定参数 |
| M20 | [Windows on Arm documentation](https://learn.microsoft.com/en-us/windows/arm/overview) | Windows 10/11 模拟差异、原生 ARM64 C/C++ 与 Visual Studio 支持 |
| M21 | [CMake projects in Visual Studio](https://learn.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio?view=msvc-180) | `CMakePresets.json`、Ninja/VS generator、CI/跨平台入口 |
| M22 | [Use CTest for C++ in Visual Studio](https://learn.microsoft.com/en-us/visualstudio/test/how-to-use-ctest-for-cpp) | 最后更新 2024-03-11；CTest/Test Explorer/测试适配器集成 |
| M23 | [Use Google Test for C++ in Visual Studio](https://learn.microsoft.com/en-us/visualstudio/test/how-to-use-google-test-for-cpp) | Google Test 工作负载与 Test Explorer 集成 |
| M24 | [Use the Microsoft Unit Testing Framework for C++](https://learn.microsoft.com/en-us/visualstudio/test/how-to-use-microsoft-test-framework-for-cpp) | 微软原生 C++ 测试框架和独立测试项目 |
| M25 | [`VSTest.Console.exe` command-line options](https://learn.microsoft.com/en-us/visualstudio/test/vstest-console-options) | 非交互测试、TRX、平台与 ARM 执行要求 |
| M26 | [Sign an MSIX package](https://learn.microsoft.com/en-us/windows/msix/package/signing-package-overview) | MSIX 签名/信任/时间戳与完整性要求 |
| M27 | [Create an unsigned MSIX package](https://learn.microsoft.com/en-us/windows/msix/package/unsigned-package) | 最后更新 2026-04-15；Windows 11 测试例外、OID、管理员与 `-AllowUnsigned` |
| M28 | [SmartScreen reputation for Windows app developers](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation) | 无签名文件每版信誉、警告、企业策略与 Smart App Control |
| M29 | [Latest supported Visual C++ Redistributable downloads](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-180) | VS 2017-2026 v14 运行库、版本/架构匹配要求 |
| M30 | [`/MD`, `/MT`, `/LD` - Use runtime library](https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library?view=msvc-180) | 发布/更新时间 2026-08-05；动态/静态 CRT 与链接一致性 |
| M31 | [Redistributing Visual C++ files](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-180) | 中央安装、app-local 方式、许可与服务取舍 |

## 12. 证据限制

- 本文是官方资料快照，不是 Windows 实机、构建、安装或发布通过记录。
- 官方网页会继续更新；真正锁版时应再次记录页面日期、安装器哈希、NuGet lock 和本机工具版本，不能把本文的“截至日期”当成永久 latest。
- 本文没有改动既有 ADR、规格或事项，也没有开始产品代码实现。第 10 节的决定只有在维护者确认后，才应回写技术基线或新增 ADR/事项。
