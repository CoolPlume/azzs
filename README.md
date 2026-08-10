# Windows 初装工作台

Windows 初装工作台：帮助新装 Windows 完成驱动准备、系统优化、常用软件安装和软件优化。`azzs` 是仓库标识。

> 当前处于初始实现阶段。仓库已经包含应用与页面骨架，但尚未完成 Windows 编译、实机兼容和发行验证，也没有可供下载的安装包或 GitHub Release；具体状态以事项和验证证据为准。

## 目标范围

- 计划中的首个应用版本面向 Windows 10 22H2 与 Windows 11 普通个人设备。
- Windows 10 22H2 是最低目标版本，不是经过完整兼容性测试的保证。
- 首版计划使用 WinUI 3、C++/WinRT、XAML 与可移植的 C++ 核心。
- macOS 只保留未来迁移边界，不属于首版发行或测试范围。
- 计划提供默认关闭的调试模式：它生成极详细、写入前脱敏且仅保存在本机的日志，可导出为单个自包含诊断文件；同一模式还提供使用核心目录模型与校验的图形化软件目录编辑器。

## 文档导航

- [首版规格](.scratch/windows-initial-setup-workbench/spec.md)
- [领域语言](CONTEXT.md)
- [架构决策](docs/adr/)
- [技术基线](docs/engineering/technology-baseline.md)
- [架构与代码质量](docs/engineering/architecture-and-code-quality.md)
- [WinUI 3 Apple 风格设计准则](docs/design/winui3-apple-inspired.md)
- [研究与证据索引](docs/research/README.md)（含工具链、安装器、发布、安全、状态、目录内容与动效）
- [软件目录维护文件](catalog/software-catalog.toml)与[填写指南](docs/maintainers/software-catalog-input.md)
- [实现事项](.scratch/windows-initial-setup-workbench/issues/)
- [安全政策](SECURITY.md)
- [支持政策](SUPPORT.md)

## 开发入口

可移植核心可在非 Windows 主机上配置、编译并运行无界面 smoke：

```sh
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug
```

Windows 11 构建机使用统一 PowerShell 入口生成 x64 可运行候选或 ARM64 编译链接候选：

```powershell
pwsh ./eng/build.ps1 -Architecture x64
pwsh ./eng/build.ps1 -Architecture ARM64
pwsh ./eng/package-portable.ps1 -Architecture x64 -SkipBuild
pwsh ./eng/package-installer.ps1 -Architecture x64 -SkipBuild -AcceptWixEula
```

安装入口固定使用 WiX Toolset SDK 7.0.0，只有显式传入 `-AcceptWixEula` 才会还原并构建；该参数表示执行者已经按实际用途完成 WiX 7 条款确认。生成入口不代表安装生命周期、干净机启动或目标 Windows 版本已经验证。

## 参与贡献

项目接受中文或英文的 GitHub Issue 与 Pull Request，中文优先。GitHub Issue 是公开反馈入口；确认处理的事项会在 `.scratch/` 中建立正式票据并相互链接。具体要求见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## English summary

Windows Initial Setup Workbench is a Windows desktop workbench that helps users prepare a freshly installed PC; `azzs` is the repository identifier. The repository now contains the initial application skeleton, but Windows builds, installers, compatibility, and releases remain unverified. Chinese is the primary project language, while Issues and Pull Requests in English are welcome.

## 支持范围

当前没有公开 Release，因此暂无可支持的应用版本。首个公开 Release 后，项目只支持最新应用正式稳定发行；应用测试发行和更早正式稳定发行不在公开支持范围。

## 许可证

本项目自有内容按 [MIT License](LICENSE) 发布。
