# 冻结首个应用骨架的混合构建与部署基线

Status: accepted  
Date: 2026-08-10  
Related: ADR-0018, ADR-0019, ADR-0020, ADR-0022, ADR-0031, ADR-0043, ADR-0045

首个应用骨架采用一条混合但唯一的 Windows 构建路径：CMake 4.1.2 与 CTest 拥有可移植领域、应用核心、无界面测试和非 XAML Windows 适配器，MSBuild 拥有 C++/WinRT、XAML 宿主与最终可执行文件，`eng/build.ps1` 是连接两者的唯一非交互入口。Windows 候选固定为 Visual Studio 2026 Stable 18.8.2、稳定 MSVC 14.50/v145、Windows SDK release 10.0.28000.2526（MSBuild target 10.0.28000.0）、Microsoft.WindowsAppSDK 2.3.1 与 Microsoft.Windows.CppWinRT 3.0.260715.1；MSVC 的 C++26 目标映射为 `/std:c++latest`，在正式 `/std:c++26` 可用前只采用已稳定实现且不进入持久格式或稳定 ABI 的语言能力。

GitHub 托管镜像没有预装锁定的 SDK release，因此早期只读 CI 使用 Microsoft 固定直链的独立 bootstrapper，并同时固定文件长度、SHA-256、有效 Microsoft Authenticode 签名和安装后 x64/ARM64 文件条件。该路径仍需联网获取由 bootstrapper 验证的 payload，不等于离线 hermetic 工具链；事项 01 不为此托管 SDK ISO 或构建永久缓存。

首个部署候选使用 unpackaged Windows App SDK self-contained 输出和静态 CRT `/MT`，同一目录分别进入便携 ZIP 与 WiX Toolset SDK 7.0.0 的原生 x64/ARM64 机器级 MSI 开发入口。WiX 还原必须由执行者显式确认适用条款；该入口不替事项 21 决定完整升级、跨架构迁移和发行生命周期。self-contained、`/MT`、原生 ARM64 安装链、Windows 10 22H2 启动以及干净机离线运行仍须在 Windows 候选上验证，在此之前都只是固定且可替换的工程假设。

MSBuild 的最低加载版本保持 10.0.17763.0，使早于 Windows 10 22H2 的系统有机会进入应用并看到核心投影的非阻断风险提示；Windows 10 22H2（10.0.19045）仍只是项目设计目标，不是兼容性保证。高频一级导航不增加自定义动效，并显式抑制页面切换动画，以保持快速重复操作、键盘导航和减少动画偏好下一致。
