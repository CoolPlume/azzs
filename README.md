# Windows 初装工作台

Windows 初装工作台：帮助新装 Windows 完成驱动准备、系统优化、常用软件安装和软件优化。`azzs` 是仓库标识。

> 当前处于规格与设计阶段。仓库尚无可运行应用、安装包或 GitHub Release；文档同时记录已确认目标、未决问题、候选内容和研究证据，具体状态以各文件标注为准，均不代表功能已经实现。

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

## 参与贡献

项目接受中文或英文的 GitHub Issue 与 Pull Request，中文优先。GitHub Issue 是公开反馈入口；确认处理的事项会在 `.scratch/` 中建立正式票据并相互链接。具体要求见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## English summary

Windows Initial Setup Workbench is a planned Windows desktop workbench that helps users prepare a freshly installed PC; `azzs` is the repository identifier. The project is currently in the specification and design phase; this repository does not yet provide a runnable application, installer, or GitHub Release. Chinese is the primary project language, while Issues and Pull Requests in English are welcome.

## 支持范围

当前没有公开 Release，因此暂无可支持的应用版本。首个公开 Release 后，项目只支持最新应用正式稳定发行；应用测试发行和更早正式稳定发行不在公开支持范围。

## 许可证

本项目自有内容按 [MIT License](LICENSE) 发布。
