# 发布 v0.1.0 Beta

Type: task  
Status: ready-for-agent  
Resolution: open
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

- [ ] `release/product-version.json` 保持应用版本 `0.1.0`，发布通道为 `prerelease`，并通过产品版本合同；Git tag 与 GitHub Release 为 `v0.1.0`。
- [ ] 从干净 x64 构建生成唯一 `standard-x64-portable`，包完整性、运行时依赖和候选启动检查通过；不生成 ARM64、MSI/WiX、断网救援或超大离线制品。
- [ ] 与本版本改动相关的目录、安装、ZIP 档案、架构和 x64 合同测试通过；测试确认 11 项目录均保留、单项不可用只影响自身且其他可用项仍可规划；任何宿主前置无法满足的检查准确记录，不写成通过。
- [ ] 发布说明显著声明这是早期 Beta，自动化验证已执行，真实第三方软件安装尚未在本机、虚拟机或候选设备执行；不得以 mock 或安装器退出码宣称实机安装通过。
- [ ] 版本分支以普通 merge 合入 `main` 后，GitHub `v0.1.0` prerelease、tag 和唯一资产均指向精确合入提交，且资产校验信息可追溯。

## References

ADR-0048，事项 21、22、34、38、39，`REL-01` 至 `REL-13`。

## Comments

- 2026-08-21：这是 ADR-0048 的版本限定发布事项。事项 21、22、34 的长期八制品/正式发行范围继续保持 open，不能被本 Beta 的单制品结果改变。

## Answer

发布准备已完成，正在收口候选的 GitHub 交付。事项 38 已在 `v0.1.0` 候选分支完成并记录生产装配、11 项目录和项级不可用边界；本事项在普通合入 `main`、创建 `v0.1.0` tag/prerelease 并核对唯一资产前保持 `Resolution: open`。

当前自动化证据（最终候选提交变化后按同一命令重跑）：

- `release/product-version.json` 为应用版本 `0.1.0`、发行通道 `prerelease`，`release.version.contract` 通过。
- x64 Release build manifest `out/manifests/windows-x64-release.json` 为 `result = succeeded`、`source.dirty = false`；package manifest 与构建 manifest 绑定同一提交。
- `standard-x64-portable` 的内置打包 verifier 与独立 `eng/verify-portable-package.ps1` 均通过；最终 ZIP 大小与 SHA-256 在合入提交生成并上传后，以 GitHub Release asset digest 和本事项后续证据为准。
- 生产目录/来源/安装相关合同确认 11 项保留、Cheat Engine 与几何画板只在自身条目下不可用；完整 package/bundled-resource 合同的 reparse 负例受当前宿主权限阻断，不能写成全绿。

尚未完成的发布动作：推送 `v0.1.0`、创建并合入 `main` 的 PR、创建 `v0.1.0` tag/prerelease、上传唯一 ZIP，以及回填合入 SHA、Release URL、资产大小和 SHA-256。发行说明不得把 mock、安装器退出码或便携进程存活采样写成真实安装成功；事项 39 的真实 Windows 11 x64 安装验证继续保持 open。
