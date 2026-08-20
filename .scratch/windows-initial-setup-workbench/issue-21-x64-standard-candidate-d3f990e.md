# 当前提交标准 x64 Portable 候选

日期：2026-08-20  
源提交：`d3f990e1d08b93c8dca7a1a654126a5c63aa68cd`  
验证工作树：`D:\azzs-codex\worktrees\current-x64-final`

## 构建与打包

在全新 D 盘 worktree 中运行 `eng\build.ps1 -Architecture x64 -SkipCoreSmoke`，Release 构建清单为 `result = succeeded`、`source.commit = d3f990e1d08b93c8dca7a1a654126a5c63aa68cd`、`source.dirty = false`，记录 305 个构建输出；`Azzs.WinUI.exe` 为 4,152,320 字节。

随后运行 `eng\package-portable.ps1 -ArtifactId standard-x64-portable -SkipBuild`，再使用完整 staging、ZIP 和 package manifest 参数运行独立 `eng\verify-portable-package.ps1`，两者均退出成功。

- ZIP：`D:\azzs-codex\artifacts\issue21\standard-x64-portable-d3f990e\Azzs-standard-x64-portable.zip`
- 大小：46,168,410 字节
- SHA-256：`E058862719D4B6340BB47FB56A30297CF76632B1F25DCB5C5BCABD08BA3E45A3`
- package manifest：`D:\azzs-codex\artifacts\issue21\standard-x64-portable-d3f990e\package-standard-x64-portable.json`
- `package.path`：`out/packages/Azzs-standard-x64-portable.zip`
- `sourceCommit`：`d3f990e1d08b93c8dca7a1a654126a5c63aa68cd`
- 包内目录资源：`catalog/software-catalog.toml`、`catalog/software-optimization-catalog.toml`
- 固定 rescue 目录保留为空；标准包不包含救援可执行文件

## 启动采样

从 staging 根启动 `Azzs.WinUI.exe`，连续 12 次、每次约 500 ms 采样均为 `hasExited = false`、`responding = true`、有效窗口句柄，标题为“Windows 初装工作台”。将同一 ZIP 解压到 `D:\azzs-codex\diagnostics\startup-standard-x64-d3f990e-2` 后重复 12/12 采样，逐样本结果保存在该目录的 `startup-samples.json`。采样结束后均由脚本主动终止，不代表应用自然退出。

## 边界

- 完整 CTest 42 项中 38 项通过，4 项被当前 medium-integrity 宿主前置阻断：ACL root、目录/文件符号链接权限和隔离设备 reparse root；不能宣称 42 项全绿。
- ARM64、MSI/WiX、安装/升级/卸载生命周期、真实 UAC/安装器和完整真实设备验收未执行；未接受 WiX 条款。
- `rescue-x64-portable` 与 `large-offline-x64-portable` 的锁定输入仍为空，打包入口按 `has no locked inputs` fail-closed；没有创建占位或未知 EXE。
- `catalog/software-catalog.toml` 仍为 `release_state = "draft"`，不能生成正式大离线候选或声称八制品发行完成。
- 本候选不创建 tag、GitHub Release 或公开上传；事项 21 的 `Resolution: open` 保持不变。
