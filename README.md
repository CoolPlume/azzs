# Windows 初装工作台（暂定名）

`azzs` 是当前仓库的工作名，不是已经确定的产品正式名称。

本项目规划一款本机运行的 Windows 初装工作台，帮助普通用户在全新安装或尚未投入使用的 OEM 环境中，依次完成驱动准备、系统优化、常用软件安装和软件优化。

> 当前处于规格与设计阶段。仓库尚无可运行应用、安装包或 GitHub Release；现有文档描述的是已经确认的目标与边界，不代表功能已经实现。

## 目标范围

- 计划中的首个应用版本面向 Windows 10 22H2 与 Windows 11 普通个人设备。
- Windows 10 22H2 是最低目标版本，不是经过完整兼容性测试的保证。
- 首版计划使用 WinUI 3、C++/WinRT、XAML 与可移植的 C++ 核心。
- macOS 只保留未来迁移边界，不属于首版发行或测试范围。

## 文档导航

- [首版规格](.scratch/windows-initial-setup-workbench/spec.md)
- [领域语言](CONTEXT.md)
- [架构决策](docs/adr/)
- [技术基线](docs/engineering/technology-baseline.md)
- [WinUI 3 Apple 风格设计准则](docs/design/winui3-apple-inspired.md)
- [实现事项](.scratch/windows-initial-setup-workbench/issues/)

## 参与贡献

项目接受中文或英文的 GitHub Issue 与 Pull Request，中文优先。GitHub Issue 是公开反馈入口；确认处理的事项会在 `.scratch/` 中建立正式票据并相互链接。具体要求见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## English summary

`azzs` is the working repository name for a planned Windows desktop workbench that helps users prepare a freshly installed PC. The project is currently in the specification and design phase; this repository does not yet provide a runnable application, installer, or GitHub Release. Chinese is the primary project language, while Issues and Pull Requests in English are welcome.

## 许可证

本项目自有内容按 [MIT License](LICENSE) 发布。
