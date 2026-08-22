# 发布 v0.1.0 Beta

Type: task  
Status: ready-for-agent  
Resolution: open
Blocked by: 38
Owner: issue-40
Consumers: 39
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
