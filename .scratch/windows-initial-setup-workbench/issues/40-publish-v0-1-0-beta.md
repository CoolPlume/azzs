# 发布 v0.1.0 Beta

Type: task  
Status: ready-for-agent  
Resolution: completed
Blocked by: none
Owner: issue-40
Consumers: 39
Claimed by: v0.1.0
Verification: 从干净的 `v0.1.0` 分支提交生成并验证唯一 `standard-x64-portable`，普通合入 `main` 后核对 `v0.1.0` GitHub prerelease、tag、资产和准确发行说明。
Evidence freshness: 绑定发布提交、产品版本、目录修订、x64 制品、GitHub tag/Release 和发布说明；任一项变化后重新验证。

## Goal

把满足 ADR-0048 的 `v0.1.0` 单 x64 便携 Beta 以准确、可恢复且不虚报实机验证的方式发布到 GitHub Releases。

## Boundary

本事项只发布 `standard-x64-portable`，不关闭事项 21 的八制品长期发行合同，也不关闭事项 39 的真实 Windows 安装验证。发布准备、构建和门禁全部在 `v0.1.0` 分支完成；通过后普通 merge 到 `main`，再直接创建 GitHub prerelease。不得新增加写权限 GitHub Actions，公开分支、tag、Release 标题和说明不得使用 `codex` 命名。

## Acceptance Criteria

- [x] `release/product-version.json` 保持应用版本 `0.1.0`，发布通道为 `prerelease`，并通过产品版本合同；Git tag 与 GitHub Release 为 `v0.1.0`。
- [x] 从干净 x64 构建生成唯一 `standard-x64-portable`，包完整性、运行时依赖和候选启动检查通过；不生成 ARM64、MSI/WiX、断网救援或超大离线制品。
- [x] 与本版本改动相关的目录、安装、ZIP 档案、架构和 x64 合同测试通过；测试确认 11 项目录均保留、单项不可用只影响自身且其他可用项仍可规划；任何宿主前置无法满足的检查准确记录，不写成通过。
- [x] 发布说明显著声明这是早期 Beta，自动化验证已执行，真实第三方软件安装尚未在本机、虚拟机或候选设备执行；不得以 mock 或安装器退出码宣称实机安装通过。
- [x] 版本分支以普通 merge 合入 `main` 后，GitHub `v0.1.0` prerelease、tag 和唯一资产均指向精确合入提交，且资产校验信息可追溯。

## References

ADR-0048，事项 21、22、34、38、39，`REL-01` 至 `REL-13`。

## Comments

- 2026-08-21：这是 ADR-0048 的版本限定发布事项。事项 21、22、34 的长期八制品/正式发行范围继续保持 open，不能被本 Beta 的单制品结果改变。
- 2026-08-22：`v0.1.0` 分支已通过普通 merge 合入 `main`，合入提交为 `5807280360901980fb0237403817daef1cc0d511`；带注释 tag `v0.1.0` 的 peeled commit 为同一 SHA。GitHub Release 已发布为 `Azzs v0.1.0 Beta` prerelease，URL 为 https://github.com/CoolPlume/azzs/releases/tag/v0.1.0。

## Answer

事项 40 已完成。发布范围严格为单一 `standard-x64-portable` x64 便携制品；事项 21、22、34 的长期八制品/正式发行范围，以及事项 39 的真实 Windows 安装验证均保持 open。

最终发布证据：

- `release/product-version.json` 为应用版本 `0.1.0`、发行通道 `prerelease`；`release.version.contract` 通过。
- x64 Release build manifest `out/manifests/windows-x64-release.json` 为 `result = succeeded`、`source.dirty = false`；package manifest 的 `sourceCommit` 为 `ecbb55e749af4a0db74f08228f7c51214bd7ed3a`。该提交与最终合入提交的树一致，tag 仍绑定普通合入提交而未移动。
- 内置打包 verifier、独立 `eng/verify-portable-package.ps1` 和发布相关定向 CTest（6/6）通过；全量 CTest 为 41/46，通过项未被宿主权限问题冒充全绿。
- 正式目录保留 11 项；Cheat Engine 与几何画板仅在各自条目下不可用，不阻断其他项目。5 个全量 CTest 失败均为当前宿主无法创建 ACL/reparse 夹具的环境前置（含 `ERROR_PRIVILEGE_NOT_HELD (1314)`），不是本次发布隐藏的产品通过结论。
- GitHub Release 资产唯一为 `Azzs-standard-x64-portable.zip`，大小 `46263566` bytes；GitHub digest 与本地 SHA-256 均为 `6cc3fbc27c157844c7565f1b03d0bea54d8751257e8b429e48e8bcf0024579e6`。下载地址为 https://github.com/CoolPlume/azzs/releases/download/v0.1.0/Azzs-standard-x64-portable.zip。
- 发行说明明确这是早期 Beta，并明确记录真实第三方软件安装尚未在本机、虚拟机或候选设备执行；mock 只作为回归证据，不等于实机安装通过。
- ARM64、MSI/WiX、真实 Windows 11 x64 第三方安装、UAC/UI、DPI、安装/Repair/卸载/迁移生命周期、rescue 和 large-offline 制品均未在本事项中宣称完成。
