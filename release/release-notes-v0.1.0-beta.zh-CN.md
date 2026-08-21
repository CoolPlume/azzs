# Windows 初装工作台 0.1.0 Beta

<!-- 发布前替换双花括号证据占位符，并删除本注释。本文档仅适用于 ADR-0048 规定的单制品 Beta。 -->

Windows 初装工作台 v0.1.0 是早期应用测试发行，仅提供一个 x64 便携制品。

## 本次发行

- 版本：`0.1.0`
- 发行通道：`prerelease`
- 制品：`standard-x64-portable`
- Git tag：`v0.1.0`
- 发布提交：`{{RELEASE_COMMIT}}`

## 下载

| 制品 | 文件 |
| --- | --- |
| Windows 初装工作台 标准版 x64 便携版 | `Azzs-standard-x64-portable.zip` |

这是唯一的本次发行资产。ARM64、MSI/WiX、断网救援版和超大离线版不属于本次 Beta。

资产 SHA-256：`{{ASSET_STANDARD_X64_PORTABLE_SHA256}}`

## 验证边界

- 发布前必须执行：x64 Release 构建、生产目录与受控安装链无界面合同、便携包内容和运行时依赖门禁。
- 证据：build manifest `{{BUILD_MANIFEST_PATH}}`；package manifest `{{PACKAGE_MANIFEST_PATH}}`。
- 未执行：本机、虚拟机或其他候选设备上的真实第三方软件安装；mock 合同不等于实机安装通过。
- 延期：真实 Windows 11 x64 第三方软件安装验证、ARM64、MSI/WiX、安装/Repair/卸载/迁移生命周期，以及断网救援和超大离线制品。

## 使用前请注意

本项目提供的 Windows 应用发行包未签名，启动时可能出现 SmartScreen 提示。第三方来源未获项目验证；工作台记录来源、执行与结果事实，不表示来源、文件或安全已经验证。Windows 10 22H2 是最低目标版本，并同时面向 Windows 11；这不是跨版本兼容性保证。

## 安装与启动

解压 `Azzs-standard-x64-portable.zip` 后运行其中的 `Azzs.WinUI.exe`。便携版不执行安装、Repair、卸载或迁移操作。

## 反馈

请在 GitHub Issues 中说明所用版本、发行形态、架构、Windows 版本、复现步骤和可公开的错误信息；请勿提交密码、验证码、付款信息或其他敏感资料。
