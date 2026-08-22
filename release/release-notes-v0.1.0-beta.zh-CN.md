# Windows 初装工作台 0.1.0 Beta

Windows 初装工作台 v0.1.0 是早期应用测试发行，仅提供一个 x64 便携制品。

## 本次发行

- 版本：`0.1.0`
- 发行通道：`prerelease`
- 制品：`standard-x64-portable`
- Git tag：`v0.1.0`
- 制品构建源提交：以 `out/manifests/package-standard-x64-portable.json` 的 `sourceCommit` 为准；最终 GitHub Release 由 `v0.1.0` tag 指向普通合入 `main` 的提交。

## 下载

| 制品 | 文件 |
| --- | --- |
| Windows 初装工作台 标准版 x64 便携版 | `Azzs-standard-x64-portable.zip` |

这是唯一的本次发行资产。ARM64、MSI/WiX、断网救援版和超大离线版不属于本次 Beta。

资产 SHA-256 以最终 GitHub Release 资产 digest 为准，并在事项 40 的发布证据中记录；本说明不引用文档提交前生成的临时候选摘要。

## 验证边界

- 发布前必须执行：x64 Release 构建、生产目录与受控安装链无界面合同、便携包内容和运行时依赖门禁。
- 证据：build manifest `out/manifests/windows-x64-release.json`；package manifest `out/manifests/package-standard-x64-portable.json`。
- 正式目录保留 11 项；Cheat Engine 与几何画板在各自条目下明确不可用，不阻断其他项目。
- 当前宿主未能创建部分 ACL/reparse 测试夹具，相关合同失败按环境前置记录，未宣称为全量合同通过。
- 未执行：本机、虚拟机或其他候选设备上的真实第三方软件安装；mock 合同不等于实机安装通过。
- 延期：真实 Windows 11 x64 第三方软件安装验证、ARM64、MSI/WiX、安装/Repair/卸载/迁移生命周期，以及断网救援和超大离线制品。

## 使用前请注意

本项目提供的 Windows 应用发行包未签名，启动时可能出现 SmartScreen 提示。第三方来源未获项目验证；工作台记录来源、执行与结果事实，不表示来源、文件或安全已经验证。Windows 10 22H2 是最低目标版本，并同时面向 Windows 11；这不是跨版本兼容性保证。

## 安装与启动

解压 `Azzs-standard-x64-portable.zip` 后运行其中的 `Azzs.WinUI.exe`。便携版不执行安装、Repair、卸载或迁移操作。

## 反馈

请在 GitHub Issues 中说明所用版本、发行形态、架构、Windows 版本、复现步骤和可公开的错误信息；请勿提交密码、验证码、付款信息或其他敏感资料。
