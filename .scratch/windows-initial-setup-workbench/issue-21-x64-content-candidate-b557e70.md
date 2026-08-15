# 事项 21 x64 便携内容包候选制品记录

日期：2026-08-15  
事项：21 - 构建 GitHub 发行制品  
候选输入：`b557e70dd507ee0bae0c1e54287dfafdf0c423c6`  
候选范围：仅标准版 x64 便携包；此记录是开发者证据，不是用户发行资产或 GitHub Release 文案。

## 结论

- `out/manifests/windows-x64-release.json` 记录 x64/Release 构建 `result=succeeded`、`source.commit=b557e70dd507ee0bae0c1e54287dfafdf0c423c6`、`source.dirty=false`。
- `out/manifests/package-standard-x64-portable.json` 记录 `artifactId=standard-x64-portable`、`kind=portable`、`architecture=x64` 和相同 `sourceCommit`。该 manifest 绑定 `release/artifact-content-manifest.v1.json`，其 SHA-256 为 `fe2bafcd36cfbd7049da7e874b4666d59fed8cfdc4e9fff12fe25aa2631aab68`；标准版候选的 `inputs` 为空，未把救援或离线内容作为已验证输入随包带入。
- `out/packages/Azzs-standard-x64-portable.zip` 包含 300 个文件、`45,094,553` bytes，SHA-256 为 `6203E402CFF6F7698A892E13F29238BAE0C53DFB792542DB9C87FE94C400A965`。对应 staging payload 同为 300 个文件、`116,642,050` bytes。
- 这只是 x64 临时候选，不能视为事项 21 的八制品候选或外部发布成功。事项 21 保持 `Resolution: open`。

## 设计与实现

- 新增版本化的便携制品内容清单与 schema，将标准版、断网救援版和超大离线版的 x64 便携制品以 artifact 驱动方式定义；受控内容保留在 `content/` 命名空间，冲突、未知路径或未锁定输入均 fail-closed。
- 打包前锁定并验证每个内容输入的版本、架构、相对路径、包内路径、长度、SHA-256、许可、来源和精确安全分类。救援伴随工具还要求已跟踪的源码、可复现构建、最小 smoke 与进程令牌合同证据；超大离线制品必须包含救援制品的受控输入超集。
- package manifest 记录内容清单哈希、已验证输入及 ZIP/staging payload 的文件哈希。独立 verifier 对运行时文件、PE x64 架构、调试构建文件排除、内容绑定和 ZIP 与 staging/manifest 的逐文件长度及 SHA-256 一致性执行复核。
- 未修改工作台产品逻辑、目录内容、机器级安装器实现或面向用户的发行说明。

## 自动化检查

| 检查 | 结果 | 说明 |
| --- | --- | --- |
| `tests/portable-package-contract/check-portable-package.ps1` | 通过 | 标准版和救援版成功 fixture，以及 15 个以上 fail-closed 输入、内容、分类、路径与 manifest 反例均通过。 |
| 从零 `eng/build.ps1 -Architecture x64 -SkipCoreSmoke` | 通过 | 生成上述干净 x64 Release manifest；该命令跳过 CTest，不能替代完整合同通过。 |
| `eng/package-portable.ps1 -ArtifactId standard-x64-portable -SkipBuild` | 通过 | 生成上述 portable manifest 和 ZIP，并执行打包后的独立 verifier。 |
| `eng/verify-portable-package.ps1` | 通过 | 独立复核 staging、ZIP、package manifest 与内容清单绑定。 |
| 历史完整 CTest 宿主边界（`8d0f21b`） | 31/33 | `execution-log.contract` 缺少 prepared ACL root，`windows-device-data.contract` 缺少目录符号链接；本候选未将这些失败写成通过，也未重跑完整 CTest。 |
| `eng/package-installer.ps1 -Architecture x64`，未传 `-AcceptWixEula` | 未执行 | 本候选没有调用 WiX 入口或接受条款，不能沿用先前候选的条款保护观察。 |

保留的失败证据：

- `out/preserved-failures/8d0f21b-x64-ctest-host-prerequisites/`
- `out/preserved-failures/8d0f21b-x64-incremental-build-state/`

## 真实设备验证

本精确候选为**未实机验证**。未执行此候选的干净机启动、UAC、SmartScreen、便携包离线启动、Windows 版本、DPI、输入、动效、目标软件或搜狗环境验证。先前候选的设备观察不外推到本候选。

## 未完成边界

- 本轮未执行 ARM64；未生成 ARM64 便携包、x64/ARM64 机器级安装包、断网救援包或超大离线包；八制品矩阵未完成。
- 未执行本候选的 WiX、MSI、机器级安装、Repair、卸载、升级或 x64 到 ARM64 迁移；WiX 条款仍未接受。
- 未完成正式目录、发行、签名/SmartScreen 风险说明或事项 22 的真实环境验收。
- 未创建 tag、GitHub Release 或上传应用二进制。
