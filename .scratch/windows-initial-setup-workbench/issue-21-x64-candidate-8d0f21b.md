# 事项 21 x64 临时候选制品记录

日期：2026-08-15  
事项：21 - 构建 GitHub 发行制品  
候选输入：`8d0f21bdbca7ebaf61f1c9f00a9c5925f12416e5`  
候选范围：仅标准版 x64 便携包；此记录是开发者证据，不是用户发行资产或 GitHub Release 文案。

## 结论

- `out/manifests/windows-x64-release.json` 记录 x64/Release 构建 `result=succeeded`、`source.dirty=false`，并绑定上述输入提交。
- 修复便携打包时误删 staging 文件的问题后，`out/packages/Azzs-standard-x64-portable.zip`、`out/manifests/package-standard-x64-portable.json` 与 x64 staging payload 已通过定向合同核对。合同要求非空 payload、`Azzs.WinUI.exe`、自包含 WinUI 运行时、正确 PE x64 架构、无调试构建文件，且 ZIP 与 manifest 的文件名和长度一致。
- 这只是 x64 临时候选，不能视为事项 21 的八制品候选或外部发布成功。事项 21 保持 `Resolution: open`。

## 设计与实现

- 现有三档八制品、机器级安装、原生 ARM64 链、Repair、卸载和受控迁移合同未改变。
- 本次仅修复标准版 x64 便携打包：`Get-ChildItem -LiteralPath` 不再结合 `-Include` 直接删除文件，而是按明确扩展名删除调试构建文件；新增的便携包合同在空 staging、缺失运行时、PE 架构不符、ZIP 或 manifest 不一致时失败。
- 未修改工作台产品逻辑、目录内容、安装器实现或面向用户的发行说明。

## 自动化检查

| 检查 | 结果 | 说明 |
| --- | --- | --- |
| 初始 `eng/package-portable.ps1 -Architecture x64` | 未通过完整 CTest | 33 项中 `execution-log.contract` 缺少 prepared ACL root、`windows-device-data.contract` 缺少目录符号链接；保留原始失败证据。 |
| 从零 `eng/build.ps1 -Architecture x64 -SkipCoreSmoke` | 通过 | 生成上述 x64 Release manifest；该命令跳过 CTest，不能替代完整合同通过。 |
| `eng/package-portable.ps1 -Architecture x64 -SkipBuild` | 通过 | 定向便携包合同在打包后通过。 |
| `eng/verify-portable-package.ps1` | 通过 | 独立复核 staging、ZIP 与 package manifest。 |
| `eng/package-installer.ps1 -Architecture x64`，未传 `-AcceptWixEula` | 预期条款保护 | 以要求显式 WiX 7 条款决定的错误退出；没有生成 MSI。 |

保留的失败证据：

- `out/preserved-failures/8d0f21b-x64-ctest-host-prerequisites/`
- `out/preserved-failures/8d0f21b-x64-incremental-build-state/`

## 真实设备验证

本精确候选为**未实机验证**。未执行此候选的干净机启动、UAC、SmartScreen、便携包离线启动、Windows 版本、DPI、输入、动效、目标软件或搜狗环境验证。先前候选的设备观察不外推到本候选。

## 未完成边界

- 未生成 ARM64 便携包、x64/ARM64 机器级安装包、断网救援包或超大离线包；八制品矩阵未完成。
- WiX 条款未接受；未执行 MSI、机器级安装、Repair、卸载、升级或 x64 到 ARM64 迁移。
- 未完成正式目录、发行、签名/SmartScreen 风险说明或事项 22 的真实环境验收。
- 未创建 tag、GitHub Release 或上传应用二进制。
