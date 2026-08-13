# Windows 虚拟机回传记录（2026-08-13）

本记录保存一次已结束的 Windows 11 虚拟机会话结果。所有路径均为仓库相对路径或已脱敏；没有账号标识、凭据、安装器副本、注册表导出、截图或原始 UI Automation 树。虚拟机已恢复快照，后续不得把临时脚本或未闭合观察当作证据。

## A. PR #31 C++/WinRT 链接修复

- 起始 head：`1796671ce62b06fe01b035ef33c5ddd17bd382b1`
- 推送 head：`97bf8a7193fab77f00e8ef5eaab3661fe23fa64d`
- 分支：`codex/fix-issue-10-force-attempt-ui`
- 提交：`fix: align CMake C++ WinRT projection`
- GitHub run：`31663573410`，x64 与 ARM64 job 均成功

CMake Windows adapter 改用锁定的 `Microsoft.Windows.CppWinRT 3.0.260715.1` 生成投影，构建脚本在 CMake 配置前恢复 solution，使静态库与 WinUI 主机使用同一投影版本，没有通过链接器选项绕过 `LNK2038`。

本机 x64 CMake 和 WinUI 链接完成。完整 `eng/build.ps1 -Architecture x64` 为 `20/22` 测试通过；两个剩余失败需要提升权限的 ACL root/目录符号链接环境，因此该次本机结果不是完整 x64 门禁。ARM64 只编译链接，没有在虚拟机运行 ARM64 程序或测试。

回传后 PR #31 已普通合入 `codex/v1-integration`，merge SHA 为 `1e2ffbf43828ae73507068755beac8f2fb9cac32`。

## B. 事项 07 Windows 缓存缺失路径修复

- 诊断基线：`a17092ad1c20039d874b99e6e0e2d622ea61f581`
- 推送 head：`9bc4432cd7e418b5379dd24cf7c0a1dc1a0a75d4`
- 分支：`codex/issue-07-offline-package-cache`
- 提交：`fix: accept Windows cache paths that are absent`
- PR：#26，仍为 Draft

真实 Windows x64 上，缺失的 `.partial` 路径使 `symlink_status` 返回 system category、value `2`。原代码将该错误与 generic category 的 `std::errc` 直接比较，因此误判“文件不存在”。修复将三处缺失判断改为 error-condition 比较。

focused x64 `local-package-cache-adapter.contract` 通过；首次 begin 为 acquired（`code=0`），第二次为 busy（`code=1`）。没有运行 ARM64 程序或测试。回传时最终 head 尚无 GitHub Actions 结果，且在 PR #31 合入后 PR #26 与最新 integration 有冲突；必须在普通 merge 和 x64 集成验证后才能合入。

## C. 事项 20 Explorer 设置观察无效

虚拟机没有使用证据要求的固定实现 worktree 完成闭环。一次手动选择经典右键菜单并点击应用后没有得到可靠结果；没有建立应用前状态、UAC 结果、Explorer 重启、应用后状态和受控恢复链。Windows 10 风格 Explorer 映射未执行，`ITBar7Layout` 未读取或写入。

没有生产修改、提交或证据分支更新。临时 UI 自动化脚本和其观察不属于证据，不得复用。事项 20 已有仓库证据保持原等级；本次虚拟机活动不能追加为通过。

## D. 事项 29 搜狗 16.7 观察无效

一次官方页面安装器观察报告版本 `16.7.0.4673`，但没有得到有效 Authenticode 签名。安装器启动后无法安全控制，虚拟机随后恢复快照；后续没有下载、安装或操作搜狗。

没有冻结 UIA 身份、没有执行 25 个选项、没有提交或证据分支更新。Windows adapter 必须继续 fail-closed 为 `pending_confirmation`；不得接受签名例外。未来只有在官方来源、SHA-256、受支持版本和有效发布者签名全部成立后才能重新开始 UIA 证据采集。

## 环境限制

虚拟机观测到 Windows 11 x64、MSVC `14.51.36231`、CMake `4.3.1-msvc1`、Windows SDK `10.0.28000.2526`。Visual Studio 实际为 `18.9.12105.275`，而当时锁定门禁要求 `18.8.2`；维护者仅为诊断临时允许本机构建，不能把该例外当作锁定工具链放行证据。

本记录至此结束。新的 Windows 原生总调度按 `docs/agents/windows-native-takeover.md` 继续，不再回到虚拟机 A/B/C/D 工作包。
