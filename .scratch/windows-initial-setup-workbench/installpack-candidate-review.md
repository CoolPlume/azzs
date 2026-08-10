# `installPack` 首版候选清单审核稿

Status: historical-evidence

Original review state: 1-11 已确认；许可、安全责任与制品矩阵仍开放
Last updated: 2026-08-09

> **历史证据声明**：本文件保留 2026-08-09 的候选池盘点、分类建议和当时确认记录，不再作为发行、安全、许可、救援伴随工具或制品矩阵的现行合同。相关边界已由后续答案登记、[首版规格](spec.md)、[领域词汇](../../CONTEXT.md)、[ADR](../../docs/adr/)和[现行事项](issues/)取代；有冲突时以后者为准，本文中的“已确认”“携带”和“不审查”不得直接转成实现或发布结论。

本文是对维护者提供的 `installPack` 候选池所做的自动分类提案，不是最终精选软件目录。盘点只读取文件名、目录、大小和随附文本，没有运行、复制、修改或上传任何安装包。

## 1. 审核规则

- **基础默认更新**：默认选中并进入执行队列；旧版本主动更新到官网当前稳定版，已经是最新版时记录“已是最新版”并完成，不重复覆盖安装。用户仍可取消，但要提示影响。
- **普通可选**：默认不选，由用户主动选择。
- **首版展示规则**：整体采用本审核稿的展示建议，不再逐个软件确认；同一设备和目录状态下，标准视图与高级视图显示相同的软件安装项和选择状态，高级视图只增加细节与低频控制；硬件适用性规则在两种视图中保持一致。除明确属于破解或授权绕过的内容外，需要额外提示的候选软件仍进入两种视图共用的软件清单，默认不勾选。
- **超大离线版携带**：只表示发行包内存在可用资源，不会自动勾选或安装软件。
- **标准版**：工作台自身完全自包含，可在断网时启动和使用本地能力；附带工作台自身的基础断网诊断、U 盘驱动导入能力，以及驱动精灵网卡版与 360 断网急救能力的官网入口，但不携带救援伴随工具。原“在线版”和“普通离线版”不再作为两个发行包。
- **断网救援版（暂定名）**：在标准版基础上额外携带维护者另行开发并提供的两个救援伴随工具，分别用于通用网卡驱动救援和断网诊断／修复；具体名称以后仍可调整。
- **断网救援**：无可用网络时先提示用户，再允许从 U 盘等本地位置导入厂商网卡驱动，只有前两步不能解决时才提供通用网卡应急工具；不得静默跳到万能驱动。
- **救援伴随工具**：两个独立程序由维护者自行开发并以完整 EXE 形式提供，同时授权随项目公开发布；本项目不定义、审查或测试其内部实现，只约定文件接入、用户启动和流程交接。
- **维护者直供内容**：维护者提供的教育版链接、说明、资源，以及自研软件和说明，不做真实性、质量、来源或内容审查；项目只提供预留位置、填写说明和使其可被读取所需的基本字段检查，内容责任由维护者承担。这一不审查边界不取消破解／授权绕过硬禁止。
- **GitHub 备用包候选**：只有记录了明确再分发资格后才能公开托管；本地已有文件、能够安装或被自动识别都不代表可以公开再分发。
- 工作台运行时不承诺验证第三方软件包的签名、哈希或真实性；这与发布前确认再分发资格是两件不同的事。
- `._*` AppleDouble 文件、日志、个人浏览器备份和其他非安装资源自动忽略。

## 2. 已确认的分类锚点

| 项目 | 分类 | 默认选择 | 超大离线版 |
|---|---|---:|---:|
| Java 环境 | 基础默认更新 | 是 | 是 |
| .NET 环境 | 基础默认更新 | 是 | 是 |
| DirectX 兼容组件与检测修复 | 基础默认更新 | 是 | 是 |
| PowerShell 7 | 基础默认更新 | 是 | 是 |
| QQ | 常用普通软件 | 否 | 是 |
| 微信 | 常用普通软件 | 否 | 是 |
| Steam | 普通可选 | 否 | 否 |
| QQ 音乐 | 普通可选 | 否 | 否 |

“基础默认更新”每次都会执行版本检查或状态检测：旧版本更新，缺少时安装，已经是最新版时直接完成，不强制重装同一版本。DirectX 强力修复等高风险操作仍需单独说明并确认。

### 2.1 发行资源层级

| 发行资源层级 | 断网能力 | 内置安装资源 |
|---|---|---|
| 标准版 | 工作台可启动并执行基础断网诊断；允许从 U 盘导入厂商网卡驱动；联网时可打开两个第三方工具的官网 | 不携带救援伴随工具，也不内置 Java、.NET、PowerShell、QQ、微信等常规软件包 |
| 断网救援版（暂定名） | 包含标准版全部能力，并可在完全断网时启动维护者提供的两个救援伴随工具 | 自研通用网卡驱动救援工具和自研断网诊断／修复工具 EXE |
| 超大离线版 | 包含断网救援版全部能力，并可离线完成更多基础更新和软件安装 | 两个救援伴随工具，以及 Java、.NET、VC++、DirectX 兼容/修复、PowerShell 7、QQ、微信及维护者确认的其他资源 |

标准版联网后正常解析和下载官网当前稳定版；断网且本地没有资源的项目显示“等待联网”。标准版中的两个第三方入口只打开项目维护的官网地址，不把网页内容误标为本地可用。断网救援版在无网络时使用随包的救援伴随工具。超大离线版断网时使用其构建时内置的稳定版本并明确显示版本可能较旧，联网时仍优先解析官网当前稳定版。

## 3. 基础环境建议

当前候选池只有 Java 8 和第三方 DirectX Repair，没有独立的 .NET 官方安装包，也不能把 JDK 26 当成普通用户 Java 运行环境。

| 基础项 | 当前资源 | 首版建议 | 超大离线版 | 备注 |
|---|---|---|---:|---|
| 工作台自身运行环境 | 不依赖候选池 | 随 x64/ARM64 工作台自包含发行 | 是 | 不能等工作台启动后再补自身依赖 |
| PowerShell 7 | 当前候选池缺少 | 默认安装或更新官网当前稳定版 | 是 | 与 Windows PowerShell 5.1 并存，不移除系统自带版本；不使用 preview |
| Microsoft VC++ v14 | 缺少独立官方包 | 默认检查；按目标软件架构补齐 | 待许可 | x64 新机通常同时需要 x64 与 x86；旧版本只按软件需要安装 |
| .NET Framework | 缺少独立官方包 | 默认检查；Windows 已有版本满足时跳过 | 仅适用包 | 3.5 只能使用与当前 Windows 匹配的来源，不能准备一个通用离线包 |
| 现代 .NET Desktop Runtime | 缺少 | 默认检查；只安装所选软件声明需要的主版本 | 是 | SDK 只放开发分类，不用“最新版”替代所有旧主版本 |
| Java 运行环境 | `jre-8u491-windows-x64.exe` | 默认检查；建议另补可明确再分发的现代 LTS 运行环境 | 是 | Java 8 按兼容需求保留；具体软件可声明所需版本 |
| JDK 26 | `jdk-26_windows-x64_bin.exe` | 仅开发分类 | 否 | 开发套件不是普通用户运行 Java 应用的默认方案 |
| DirectX 旧版兼容组件 | 当前第三方工具内含相关数据 | 默认检查旧游戏组件，缺少时使用官方离线组件补齐 | 是 | Windows 10/11 已有系统 DirectX，不能宣传为“升级 DirectX” |
| DirectX Repair 增强版 | 7Z 与解压目录重复存在 | 默认检测；只有发现异常才普通修复 | 待作者许可 | 强力修复、批量注册 DLL 等高风险能力不得静默执行 |
| Windows 系统文件修复 | 无独立资源 | 高级系统修复入口 | 不需要 | 优先使用 Windows 自带 DISM 后接 SFC |

## 4. 常用普通软件

这些软件默认不勾选；“超大离线版携带”只表示随包可用，不改变默认选择状态。

| 软件 | 现有资源 | 首版 | 超大离线版 | GitHub 备用包 | 说明 |
|---|---|---:|---:|---|---|
| QQ | `QQ_9.9.32_260716_x64_01.exe` | 展示 | 携带 | 待腾讯许可 | 已确认锚点 |
| 微信 | `WeChatWin_4.1.11.exe` | 展示 | 携带 | 待腾讯许可 | 已确认锚点 |
| Google Chrome | `ChromeStandaloneSetup64.exe` | 展示 | 携带 | 待 Google 许可 | 已确认进入第二层资源；在线来源仍解析官网最新版 |
| 360 压缩 | `360zip_setup.exe` | 展示 | 携带 | 待厂商许可 | 已确认进入第二层资源；安装推广选项应显式处理 |
| WPS Office | `WPS_Setup_X64_22525.exe` | 展示 | 携带 | 待金山许可 | 已确认进入第二层资源；常用但体积较大 |
| PowerToys | `PowerToysSetup-0.100.2-x64.exe` | 展示 | 携带 | 待许可证复核 | 已确认进入第二层资源；高级视图可提供更多说明 |
| 1Password | `1PasswordSetup-latest.msixbundle` | 展示 | 可选携带 | 待厂商许可 | 与 Bitwarden 并列供用户选择 |
| Bitwarden | `2026.6.0`、`2026.6.1` 两版 | 展示新版本 | 可选携带 | 待上游许可复核 | 旧版本只作为历史候选 |
| 腾讯会议 | `TencentMeeting_...3.43.21.403...exe` | 展示 | 可选携带 | 待腾讯许可 | 常见会议工具 |
| ChatGPT | `ChatGPT Installer.exe` | 展示 | 否 | 不镜像引导器 | 文件无版本信息，需先确认是否为在线引导器 |

## 5. 普通可选软件

| 软件 | 现有资源 | 首版建议 | 超大离线版 | GitHub 备用包/疑点 |
|---|---|---:|---:|---|
| Steam | `SteamSetup.exe` | 游戏分类展示 | 否 | 在线引导器，不建议镜像 |
| QQ 音乐 | `QQMusic_YQQWinPCDL.exe` | 影音分类展示 | 否 | 可能是下载器，不建议镜像 |
| 网易云音乐 | `NeteaseCloudMusic_...32.exe` | 影音分类展示 | 否 | 当前是 32 位包，需补架构信息 |
| PotPlayer | `PotPlayerSetup64.exe` | 影音分类展示 | 可选 | 待厂商许可 |
| 腾讯视频 | `TencentVideo11.175.2212.0.exe` | 影音分类展示 | 否 | 待腾讯许可 |
| 百度网盘 | `BaiduNetdisk_8.6.0.102_x64.exe` | 网盘分类展示 | 否 | 体积大、更新频繁 |
| 夸克网盘 | `QuarkCloudDrivePC_...releasemini...exe` | 网盘分类展示 | 否 | 可能仍需联网 |
| Google Drive | `GoogleDriveSetup.exe` | 网盘分类展示 | 否 | 不镜像引导器 |
| 钉钉 | `dingtalk_downloader.exe` | 沟通分类展示 | 否 | 下载器，不建议镜像 |
| 幕布 | `Mubu-5.7.1-x64.exe` | 效率分类展示 | 否 | 待厂商许可 |
| AweSun | `AweSun_16.0.0.22931_x64.exe` | 远程工具展示 | 否 | 需提示远程控制权限 |
| ToDesk | `ToDesk_4.8.1.1.exe` | 远程工具展示 | 否 | 需提示远程控制权限 |
| 网易 UU 远程 | `uuyc_4.33.0.exe` | 远程工具展示 | 否 | 本地为 4.33.0；在线安装解析网易官网当前稳定版，权限配置交给官方流程 |
| Battle.net | `Battle.net-Setup.exe` | 游戏分类展示 | 否 | 联网引导器 |
| EA app | `EAappInstaller.exe` | 游戏分类展示 | 否 | 联网引导器 |
| Epic Games Launcher | `EpicInstaller-18.8.1.msi` | 游戏分类展示 | 否 | 待厂商许可 |
| Rockstar Games Launcher | `Rockstar-Games-Launcher.exe` | 游戏分类展示 | 否 | 联网引导器 |
| Ubisoft Connect | `UbisoftConnectInstaller.exe` | 游戏分类展示 | 否 | 联网引导器 |
| WeGame | `WeGameMiniLoader...exe` | 游戏分类展示 | 否 | MiniLoader 离线价值低 |
| Paradox Launcher | `paradox-launcher-installer-2026_8_1.exe` | 游戏分类展示 | 否 | 联网引导器 |
| 网易 UU | `UU-6.13.2.exe` | 游戏分类展示 | 否 | 依赖联网与账号 |
| Wand（原 WeMod） | `Wand-12.40.0.exe` | 游戏工具分类展示 | 否 | 单机游戏辅助、模组与游戏内指引工具；本地版本 12.40.0 |
| Bili23 Downloader | `Bili23-Downloader_...exe` | 媒体下载分类展示 | 否 | 下载类工具，不默认推荐 |
| Kedou 视频下载器 | `kedou_latest.exe` | 媒体下载分类展示 | 否 | 本地版本 2.1.4；外部应用嗅探所需的 HTTPS 证书能力必须另行选择 |
| Clash Verge | `Clash.Verge_2.5.2_x64-setup.exe` | 网络工具分类展示 | 否 | 网络代理工具 |
| BitComet | `bitcomet_setup.exe` | 下载工具分类展示 | 否 | P2P 工具 |
| HiBit Uninstaller | `HiBitUninstaller-setup-4.0.10.exe` | 系统工具分类展示 | 否 | 系统修改能力较强 |
| TrafficMonitor | `TrafficMonitor_V1.86_x64.zip` | 系统工具分类展示 | 否 | 待上游许可复核 |
| Twinkle Tray | `Twinkle.Tray.v1.17.2.exe` | 工具分类展示 | 否 | 可按显示器场景推荐 |
| iTunes | `iTunes64Setup.exe` | Apple 设备分类展示 | 可选 | 待 Apple 许可 |
| 360 安全卫士极速版 | `setupbeta_jisu.exe` | 安全防护分类展示 | 否 | 本地版本 15.0.3.1052；文件名中的 `beta` 不代表产品是预览版，在线安装解析官网当前稳定版 |
| 火绒安全软件 6.0 | `sysdiag-all-x64-6.0.7.2-2025.07.29.1.exe` | 安全防护分类展示 | 否 | 本地为 6.0.7.2 x64 历史版本；在线安装解析官网当前稳定版 |

360 安全卫士极速版和火绒安全软件均作为常驻安全防护软件的备选项；若用户同时选择多个同类产品，入队前提示冲突并要求保留一个，不自动叠装。

## 6. 驱动与硬件工具

驱动相关项目优先按硬件检测结果展示，不能因为离线版携带就对不匹配硬件执行。检测到无可用网络时，断网救援固定按“提示说明 → 从 U 盘等本地位置导入厂商驱动 → 用户主动尝试通用网卡应急工具”的顺序进行；工作台不自动启动最后一步。

| 项目 | 现有资源 | 首版建议 | 发行资源安排 | 说明 |
|---|---|---:|---:|---|
| 断网救援 | 工作台自身诊断与 U 盘导入能力；维护者后续提供两个自研救援伴随工具 EXE | 无可用网络时按固定顺序展示 | 标准版提供诊断与导入；断网救援版携带两个自研 EXE | 厂商网卡驱动导入优先于通用网卡驱动救援；伴随工具只能由用户主动启动 |
| NVIDIA App | `NVIDIA_app_v11.0.8.299.exe` | 检测到 NVIDIA 后展示 | 按硬件候选 | 安装时可能闪屏、断网或要求重启 |
| AMD Software | `...minimalsetup...web.exe` | 检测到 AMD 后展示 | 否 | 当前只是 Web 安装器，需另找完整离线包 |
| Logitech G HUB | `lghub_installer.exe` | 检测到罗技设备后展示 | 否 | 通常是联网安装器 |
| Samsung Magician | `Samsung_Magician_...exe` | 检测到三星存储后展示 | 按硬件候选 | 固件操作需断电风险提示 |
| HS108T Pro 驱动/固件 | `HS108T Pro驱动_固件...zip` | 只对匹配硬件展示 | 按硬件候选 | 固件升级需要独立风险提示 |
| OMEN Superhub | `omensuperhub-...zip` | 匹配 OMEN/惠普设备后展示 | 按硬件候选 | 内容与适用型号待确认 |
| 自研通用网卡驱动救援工具 | 维护者后续提供 EXE | 最后应急；不自动启动 | 断网救援版与超大离线版携带 | 工具本身及其内部行为不属于本项目文档、实现或测试范围 |
| 自研断网诊断／修复工具 | 维护者后续提供 EXE | 用户主动选择的辅助工具，不替代工作台基础诊断 | 断网救援版与超大离线版携带 | 工具本身及其内部行为不属于本项目文档、实现或测试范围 |
| 驱动精灵万能网卡版官网 | 当前候选池已有一份 EXE，但不再用于任何发行版 | 作为标准版外部网站入口 | 不携带 | 已确认保留官网入口，不自动下载第三方 EXE |
| 360 断网急救能力官网 | 当前候选池没有独立安装包 | 作为标准版外部网站入口 | 不携带 | 已确认保留官网入口，不自动下载第三方 EXE |
| 图吧工具箱 | `图吧工具箱202601.1安装包.exe` | 硬件工具入口展示 | 超大离线版携带 | 已确认进入第二层资源；本项目只借鉴能力与界面思路，不与核心耦合 |
| 爱思助手 | `i4Tools...exe` | Apple 设备工具展示 | 否 | 可能安装 Apple 驱动与服务 |
| Equalizer APO | `EqualizerAPO...exe` | 音频工具展示 | 否 | 会改变系统音频链路 |
| DLSS Swapper | `DLSS.Swapper...exe` | 匹配显卡/游戏时展示 | 否 | 不是通用驱动 |
| MSI Afterburner Beta | `MSIAfterburnerSetup467Beta2.zip` | 首版不展示 | 否 | 与“首版不展示预览版”规则冲突 |
| opentrack | `opentrack-...win32...exe` | 游戏与外设工具展示 | 否 | 头部追踪工具，架构支持待确认 |

## 7. 开发、教育与专业软件

| 软件 | 现有资源 | 首版建议 | 超大离线版 | 说明 |
|---|---|---:|---:|---|
| Visual Studio | `VisualStudioSetup.exe` | 开发分类展示 | 否 | 使用官方工作负载安装流程 |
| JetBrains Toolbox | `jetbrains-toolbox...exe` | 开发分类展示 | 否 | 联网管理开发工具 |
| JDK 26 | `jdk-26_windows-x64_bin.exe` | 开发分类展示 | 否 | 不替代基础 Java 运行环境 |
| phpStudy | `phpStudy_64.zip` | 开发分类展示 | 否 | 会影响服务、端口和防火墙 |
| CAJViewer | `CAJViewer...exe` | 教育/学术分类展示 | 否 | 可配置官方教育资源说明 |
| 希沃白板 5 | `EasiNoteSetup...exe` | 教育分类展示 | 否 | 教学场景专用 |
| MathType 正版安装器 | `MathType-win-zh-7.11.1.462.exe` | 教育/办公分类展示 | 否 | 激活由用户依法完成 |
| Microsoft 365 镜像 | `O365HomePremRetail.img` | 办公分类候选 | 暂不携带 | 体积大，来源和再分发资格待确认 |
| Origin/OriginPro | `Origin2025b_64位.zip`、`origin2026b/` | 暂不展示 | 否 | 来源、授权和重复版本待确认；预留官方教育资源入口 |
| Adobe 应用集合 | `Adobe/` 下 5 个 ISO | 暂不展示 | 否 | 文件名不能证明官方原版或可再分发；预留官方教育资源入口 |

## 8. 系统设置与受控工具

| 项目 | 现有资源 | 建议归属 | 处理建议 |
|---|---|---|---|
| 右键菜单管理 | `ContextMenuManager.NET.4.0.exe` | 系统优化及相关软件优化 | 作为受控辅助能力，不让用户输入任意命令 |
| Windows 11 资源管理器回退 | `win11_explorer_to_win10.reg` | 系统优化高级视图 | 转换成项目内置、可说明和可恢复的受控设置，不直接执行未知 REG |

## 9. 需要额外提示的普通软件与排除

| 项目 | 现有资源 | 结论 | 超大离线版 | GitHub 备用包 | 原因 |
|---|---|---|---:|---:|---|
| MathType 破解包 | `MathtypeCrack.rar` | 排除 | 否 | 否 | 明确破解内容 |
| MATLAB R2025b 集合 | `Mathworks.Matlab.R2025b.x64/` | 整体排除 | 否 | 否 | 目录明确包含 `Crack/`、许可证文件和替换 DLL；以后只保留官方教育资源入口 |
| Game Cheats Manager | `Game.Cheats.Manager.Setup.2.4.6.exe` | 游戏工具分类展示；默认不勾选 | 否 | 否 | 提示游戏修改工具可能被安全软件或联网游戏反作弊系统拦截，并可能影响游戏或账号；不得接入破解或授权绕过资源 |
| Cheat Engine | `CheatEngine77.exe` | 游戏工具分类展示；默认不勾选 | 否 | 否 | 提示可能触发安全软件或反作弊系统，并可能影响联网游戏或账号；不得接入破解或授权绕过资源 |
| Office Tool Plus | `Office_Tool_with_runtime...zip` | 办公工具分类展示；默认不勾选 | 否 | 否 | 提示它用于 Office 部署与管理，用户仍需依法取得所需许可；不得接入激活绕过配置或资源 |
| Internet Download Manager | `internet_download_manager_6.42.42.zip` | 下载工具分类展示；默认不勾选 | 否 | 否 | 提示这是需要合法许可的商业软件，且当前 ZIP 只是候选资源，不代表已经验证为官方安装包 |
| 几何画板 | `几何画板5.06最强中文版.exe` | 教育工具分类展示；默认不勾选 | 否 | 否 | 提示这是需要合法许可的软件，且当前“最强中文版”文件只是第三方重打包候选，不代表已经验证为官方安装包 |

原先名称不明的五个安装包已经完成官网与本地 PE 静态信息交叉识别，并移入“普通可选软件”。完整证据见[识别记录](../../docs/research/installpack-unknown-software-identification.md)。

上表五个需要额外提示的普通软件在标准视图和高级视图中使用同一软件安装项、分类、可选择状态和勾选结果。软件卡片就地显示简短提示；用户选中后，安装批次确认摘要再次显示与本次选择相关的提示，不在每次勾选时弹出阻断式对话框。五个软件均不随首版超大离线版携带安装资源，也不进入项目 GitHub 备用源；网络可用时按目录中的其他允许来源取得，断网且没有可用缓存时显示“等待联网”。软件是否进入精选软件目录、现有候选文件是否随发行版携带以及是否可托管到项目第三方包源是三个独立决定。

## 10. 自动忽略与去重结果

- 忽略 56 个 `._*` AppleDouble 元数据文件。
- 忽略 `debug.log`。
- 忽略个人浏览器备份 `tampermonkey-backup-chrome-...zip`。
- Bitwarden 默认保留 `2026.6.1`，`2026.6.0` 只作为历史候选。
- DirectX Repair 的 7Z 与解压目录视为同一资源，正式构建只选择一种形态。
- Origin 2025b 与 2026b 视为同一产品的不同候选版本，在来源与授权确认前都不进入首版。

## 11. 审核记录与仍开放事项

1. 已确认：基础项目默认进入执行队列；旧版更新到官网当前稳定版，最新版直接完成，不重复覆盖安装。
2. 已确认：原在线版与普通离线版合并为自包含的标准版；只有超大离线版额外携带常规基础环境和软件包。
3. 已确认：断网救援先提示用户，再允许从 U 盘等本地位置导入厂商网卡驱动，最后才允许用户主动尝试通用网卡应急工具。
4. 已确认：超大离线版断网时使用内置稳定版本并显示其版本可能较旧，联网时优先官网当前稳定版。
5. 已确认：新增断网救援版（暂定名），携带维护者自行开发、后续以 EXE 提供并授权项目公开发布的两个救援伴随工具；工作台项目不负责其内部实现或测试。
6. 已确认：标准版仍保留驱动精灵网卡版和 360 断网急救能力的官网入口，只作为在线备选，不携带或自动下载其 EXE。
7. 已确认：超大离线版自动包含断网救援版的两个救援伴随工具，是三个版本中资源最完整的版本。
8. 已确认：Chrome、360 压缩、WPS、PowerToys 和图吧工具箱进入超大离线版的第二层资源，随包携带但默认不安装。
9. 已确认：维护者直供内容不做实质审查，内容责任由维护者承担；但仍不得借教育资源或自研软件入口提供 Crack、Keygen、破解补丁或授权绕过内容。
10. 已确认：整体采用当前审核稿的展示建议，不再逐个软件询问；标准视图与高级视图不区分可选软件，高级视图只增加细节与低频控制。
11. 已确认：Game Cheats Manager、Cheat Engine、Office Tool Plus、Internet Download Manager 和几何画板进入两种视图共用的软件清单，作为默认不勾选的普通软件并显示各自必要提示；五个软件均不随首版超大离线版携带安装资源，也不进入项目 GitHub 备用源；明确的破解／授权绕过内容仍然排除。
12. 已核实：`Wand`、`kedou_latest`、`setupbeta_jisu`、`sysdiag-all-x64` 和 `uuyc` 分别对应 Wand、Kedou 视频下载器、360 安全卫士极速版、火绒安全软件和网易 UU 远程，不再按“名称不明”处理。

## 12. 官方资料依据

- [Windows App SDK 部署概览](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/deploy-overview)：工作台自身依赖必须由发行包解决。
- [PowerShell 7 在 Windows 上的官方安装说明](https://learn.microsoft.com/en-us/powershell/scripting/install/installing-powershell-on-windows?view=powershell-7.6)：PowerShell 7 与 Windows PowerShell 5.1 并存，并支持安装或更新当前稳定版。
- [最新受支持的 Microsoft Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)：版本覆盖、架构与再分发条件。
- [.NET 在 Windows 上的安装与支持](https://learn.microsoft.com/en-us/dotnet/core/install/windows)及[已安装版本检测](https://learn.microsoft.com/en-us/dotnet/core/install/how-to-detect-installed-versions?pivots=os-windows)：现代 .NET 运行时按软件所需主版本检测和安装。
- [.NET Framework 在 Windows 上的版本对应](https://learn.microsoft.com/en-us/dotnet/framework/install/on-windows-and-server)及[.NET Framework 3.5 安装限制](https://learn.microsoft.com/en-us/dotnet/framework/install/dotnet-35-windows)：Windows 自带版本与离线来源匹配要求。
- [DirectX End-User Runtimes (June 2010)](https://www.microsoft.com/en-us/download/details.aspx?id=8109)及[微软 DirectX 部署说明](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/directx-setup-for-game-developers)：旧版兼容组件不会改变 Windows 自带的 DirectX 主版本。
- [Windows DISM 与 SFC 修复说明](https://support.microsoft.com/en-us/windows/use-the-system-file-checker-tool-to-repair-missing-or-corrupted-system-files-79aa86cb-ca52-166a-92a3-966e85d4094e)：系统文件修复的官方路径。
- [Eclipse Temurin 下载](https://adoptium.net/temurin/releases/)及[许可说明](https://adoptium.net/docs/faq/)：现代 Java 运行环境和离线携带候选。
- [Oracle Java SE 支持路线](https://www.oracle.com/java/technologies/java-se-support-roadmap.html)：不同 Java 版本的支持与许可条件会变化。
- [驱动精灵官网](https://www.drivergenius.com/)及[用户协议](http://www.ijinshan.com/privacy/drivergeniusLicense.html)：官网提供网卡版，但公开协议没有授予本项目将安装包托管到 GitHub 或随公开发行版再分发的权利。
- [360 安全卫士官网](https://weishi.360.cn/)：断网急救服务目前作为安全卫士能力介绍；本次未确认可独立分发的官方断网急救箱安装包。

## 13. 后续交付

- 需求确认完成后，向维护者提供标准版、断网救援版和超大离线版的完整资源名单。
- 维护者直接在[软件目录维护文件](../../catalog/software-catalog.toml)中补充软件分类、版本、来源用途和任意受支持形式的 HTTP(S) 原始地址，并按[维护指南](../../docs/maintainers/software-catalog-input.md)填写；工作台自动识别直链、跳转、普通网页或 GitHub，维护者不提供地址类型或会变化的最终安装文件直链。
- 提供维护者直供内容的填写指南，覆盖教育版链接、说明、资源以及两个救援伴随工具和说明；指南只解释填写位置与必填字段，不审查具体内容。
