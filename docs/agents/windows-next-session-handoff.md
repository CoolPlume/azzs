# 下一次 Windows 11 单次 Codex 工作包

把本文件全文作为一次 Windows 11 Codex 会话的任务说明执行。目标是在一台已安装 Visual Studio 2026 的 Windows 11 25H2 机器上，按 A、B、C、D 的顺序尽量完成相邻的 Windows 专属工作，减少主机与 Windows 之间的往返。不要修改本文件以外的文档来替代下述证据，也不要把未实际运行的步骤写成通过。

## 共同边界

- 仓库：`CoolPlume/azzs`；先 `git fetch origin --prune`。为 A、B、C、D 分别使用干净的独立 worktree，禁止让一个事项的临时诊断、构建输出或注册表改动污染另一个事项。
- 必读：根目录 `AGENTS.md`、`docs/agents/orchestration.md`、`docs/agents/issue-tracker.md`、`docs/engineering/architecture-and-code-quality.md`、`docs/agents/code-intelligence.md`；修改 WinUI/XAML 或任何 WinUI 交互前，再读 `docs/design/winui3-apple-inspired.md`。代码修改前按 `code-intelligence.md` 运行 GitNexus；对现有符号先做 upstream impact，若为 HIGH/CRITICAL，停止并回报而不是继续修改。
- Windows 固定工具链：Visual Studio 2026 Stable `18.8.2`、MSVC `14.51.36231`、VS 附带 CMake `4.3.1-msvc1`、Windows SDK `10.0.28000.2526`（目标目录 `10.0.28000.0`）。每个需构建的 worktree 都先运行 `& .\eng\ensure-windows-sdk.ps1`；用 `eng\build.ps1` 自身的版本检查作为准入证据。
- 全程禁止 `rebase`、`git push --force`、历史改写、合入任何分支、合入 `main`、创建/移动 tag、创建 GitHub Release、接受 WiX 条款、构建或发布 Release/WiX 制品。不要删除或重置维护者已有的未提交/未跟踪文件；遇到脏 worktree 就新建独立 worktree。
- 只把真实的 Windows 本机结果标为实机证据；GitHub Windows runner、host 测试、模拟器和静态阅读都不能冒充实机。每一条证据写明 SHA、架构、实际命令、结果、日志/制品路径、Windows/VS/SDK 版本与未验证边界。路径中不得包含用户名。
- 只有 A 或 B 满足各自的“允许修改”条件时才写代码。每个可修复工作单元完成后运行相应定向验证、`git diff --check`，并按 `code-intelligence.md` 执行暂存范围门禁；提交一个内聚提交、推送原有远端 feature 分支。不要把 C/D 采集到的设备信息、注册表导出、安装包或诊断数据提交到 Git。

## A. PR #31：先定位并单点修复锁定工具链失配

优先级最高。PR [#31](https://github.com/CoolPlume/azzs/pull/31) 仍是 Draft：base=`codex/v1-integration`，head branch=`codex/fix-issue-10-force-attempt-ui`，head SHA=`1796671ce62b06fe01b035ef33c5ddd17bd382b1`（`fix: include Windows property-set projection`）。不得合入或转 Ready。

已知 GitHub run [31620883198](https://github.com/CoolPlume/azzs/actions/runs/31620883198) 的首个共同根因：x64 job `94195139404` 与 ARM64 job `94195139377` 都在 WinUI 链接阶段失败：

```text
azzs_windows_adapter.lib(windows_view_preferences.obj) : error LNK2038:
mismatch detected for 'C++/WinRT version': value '2.0.250303.5' doesn't match
value '3.0.260715.1' in module.g.obj
```

`module.g.obj` 来自 `src/adapters/ui/winui/Azzs.WinUI.vcxproj` 的 `Microsoft.Windows.CppWinRT` `3.0.260715.1`；`windows_view_preferences.obj` 来自 CMake 静态库 `azzs_windows_adapter`。这说明两个编译图消耗了不同投影版本，不能通过 `/ignore`、修改链接器选项、手工编辑生成文件或仅抑制 LNK2038 绕过。

在新的 A worktree 执行：

```powershell
git fetch origin codex/fix-issue-10-force-attempt-ui
git worktree add --track ..\azzs-pr31 origin/codex/fix-issue-10-force-attempt-ui
Set-Location ..\azzs-pr31
git rev-parse HEAD
git status --short
& .\eng\ensure-windows-sdk.ps1
& .\eng\build.ps1 -Architecture x64 -SkipCoreSmoke
& .\eng\build.ps1 -Architecture ARM64 -SkipCoreSmoke
```

先保留两套 `out\logs\windows-*-release.{log,msbuild.log,binlog}` 与 `out\manifests\windows-*-release.json` 的本机副本。确认 `git rev-parse HEAD` 正是上述 SHA；若远端已推进，先记录新 SHA、PR 状态和新的首错，再只在该 PR 头上继续。

定位时只调查项目锁定的依赖和实际编译命令：`src/adapters/windows/CMakeLists.txt`、顶层 CMake 依赖配置、`src/adapters/ui/winui/Azzs.WinUI.vcxproj`、`eng/build.ps1`、`eng/ensure-windows-sdk.ps1`，以及 x64/ARM64 的 MSBuild/CMake 详细日志。分别记录 CMake 编译 `windows_view_preferences.cpp` 与 MSBuild 编译 `module.g.cpp` 的 include 路径和其 C++/WinRT 版本来源。需要时用 MSBuild 预处理/诊断或 CMake verbose 输出证明，而不是猜测环境缓存。

只有以下条件全部成立才可修改：两架构的观测指向同一个项目声明、恢复或生成路径错误；能给出一个把 CMake 静态库与 WinUI/XAML 主机统一到同一锁定 C++/WinRT 版本的最小修复；修复不改变产品行为、不引入第三方可执行插件、不放宽版本/SDK 门禁。优先修项目锁定/组装入口，不改链接器兼容性语义。

修复后在同一 SHA 派生提交上运行：

```powershell
& .\eng\build.ps1 -Architecture x64
& .\eng\build.ps1 -Architecture ARM64
git diff --check
git status --short
```

x64 必须完成构建、链接及脚本现有的 headless core smoke；ARM64 只可声明构建链接成功，不能声称 ARM64 运行时或测试通过。若任一架构仍在同类失配处失败，或根因不能唯一归到一个项目锁定点，停止修改，保留日志并回报，不进行盲试或推送。成功时只暂存预期文件，执行 GitNexus 暂存影响检查和 `git diff --cached --check`，提交、推送到 `codex/fix-issue-10-force-attempt-ui`；保持 PR #31 Draft，不合入。

## B. 事项 07：取得 `remove_file` 的底层 Win32 事实后才考虑修复

仅在 A 已停止或完成后进行。固定候选分支=`codex/issue-07-offline-package-cache`，候选 SHA=`a17092ad1c20039d874b99e6e0e2d622ea61f581`，PR #26 base=`codex/v1-integration`、Draft。必读已有交接：`.scratch/windows-initial-setup-workbench/issue-07-windows-11-cache-lock-handoff-a17092a.md`。

在独立 B worktree：

```powershell
git fetch origin codex/issue-07-offline-package-cache
git worktree add --detach ..\azzs-issue07 a17092ad1c20039d874b99e6e0e2d622ea61f581
Set-Location ..\azzs-issue07
git status --short
& .\eng\ensure-windows-sdk.ps1
$vs = "C:\Program Files\Microsoft Visual Studio\18\Enterprise"
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"
& $cmake --preset windows-x64 "-DCMAKE_GENERATOR_INSTANCE=$vs"
& $cmake --build --preset windows-x64-release --target azzs_local_package_cache_adapter_contract
& $ctest --preset windows-x64-release --output-on-failure --verbose -R "^local-package-cache-adapter\.contract$"
```

先不要调整锁算法或清理顺序。只在本地临时副本的 `src/adapters/infrastructure/src/local_package_cache_storage.cpp` 的 `remove_file` 清理路径加入不提交的诊断，重新运行上述 focused CTest，并取得每个失败的：角色化测试路径、失败操作（`symlink_status`、`remove` 或末尾 `exists`）、`error.value()`、`error.category().name()`、`error.message()`、文件后缀（`.partial`/`.complete`/`.payload`/`.complete.tmp`）、调用前文件类型、以及锁句柄是否仍由当前会话持有。禁止公开用户名或真实用户目录。

仅得到 `CacheWriteBeginCode::failed`（`code=4`）或泛化英文 detail 不算底层事实；若观测未到达真正 Windows 错误码，停止并回报观测缺口。只有真实错误唯一指向明显正确、且不改变既有业务合同的单点修复时，才在该候选对应 feature branch 上作修复。修复后运行同一 focused CTest；成功证据必须同时含：

```text
first begin code=0 (acquired)
second begin code=1 (busy)
local-package-cache-adapter.contract ... Passed
```

并确认首次写入生成 `.partial`，完成后只余匹配 `.payload` 与 `.complete`、无 `.complete.tmp`，`read_completed` 为 `found`，未加锁孤立 `.partial` 被清理。满足后才用常规分支 worktree（不是 detached worktree）提交并推送 `codex/issue-07-offline-package-cache`，PR #26 保持 Draft；否则不推送。

## C. 事项 20：设置、UAC 与恢复的真实 Explorer 验证

固定实现分支=`codex/issue-20-initial-settings-catalog`，SHA=`9d4cbe00077a5b8cbda6c10c3d67f4a63077d33f`；实机证据分支=`codex/issue-20-evidence`，现有 SHA=`1cef61d01b604afaa0141375ac51ca17623b39b3`。必读 `.scratch/windows-initial-setup-workbench/issue-20-itbar7layout-windows-handoff.md` 和 `.scratch/windows-initial-setup-workbench/issues/20-initial-settings-catalog.md`。

使用隔离的 Windows 测试账户和可恢复的用户配置，在 C worktree 建好当前实现后，以工作台的正常受控流程逐项验证已实现的两个固定 CLSID 映射：应用前读取状态和 Explorer 可观察菜单样式；执行时记录 UAC/权限提示、确认流程、操作结果和 Explorer 重启要求；重启 Explorer 后读取/观察结果；用工作台恢复原始状态后再次读取/观察，并确认原始状态恢复。不要用管理员命令或任意 `.reg` 导入替代工作台流程。

`ITBar7Layout` 仍是未冻结的来源附加项，不进生产代码：先备份当前值的类型、完整字节和界面效果；仅在隔离账户中按已有交接提及的来源示例测试，重启 Explorer 后记录效果、失败行为和能否恢复原值。不得把二进制值、任意注册表输入或网页内容开放到产品目录/界面。两个 CLSID 已足够时，记录 `ITBar7Layout` 为无需执行的来源附加项；若不够，只提交事实，等待后续将完整字节、适用范围、检测与恢复语义设计为类型化受控能力。

本阶段默认不改代码、不推送。只回报脱敏观察和是否发生 UAC/恢复失败；真实账户名、用户目录、无关注册表内容、屏幕截图中的私人内容一律不得提交或上传。

## D. 事项 29：官方搜狗 16.7 脱敏 UIA 与执行/检测事实

固定目录实现分支=`codex/issue-29-sogou-optimization-catalog`，SHA=`25dabd4d5f8a2aa0f0b7dfff196729a1ba5e988a`；证据分支=`codex/issue-29-evidence`，现有 SHA=`fcf54c9b2eb0472f44704e8bffc797c43d97b0b3`。必读 `docs/research/sogou-optimization-windows-handoff-2026-08-12.md` 和 `.scratch/windows-initial-setup-workbench/issues/29-initial-software-optimization-catalog.md`。

仅从搜狗官方 Windows 来源安装 16.7，使用独立可丢弃测试账户。先记录安装器 SHA-256、文件版本、产品版本、有效签名发布者和安装后的实际版本。不得输入、采集、上传或提交账号、密码、验证码、支付信息、个人资料、用户目录名、真实输入内容或含隐私的整页截图。

当前 Windows 适配器的 16.7 UIA 身份资料尚未冻结，所有执行/检测本应 fail-closed 为 `pending_confirmation`。对目标进程、根窗口以及全部 25 个选项，采集脱敏事实：进程完整路径（必要时仅保留盘符后的稳定产品相对段）、签名发布者、文件/产品版本、PID 与采集时间；UIA 根窗口 PID 和映像身份；窗口类/框架、语言、控件树与稳定的多属性组合（AutomationId、ControlType、Name、LocalizedControlType、框架、父子关系）；每项执行前只读状态、确认目标、执行后只读状态、重新检测、关闭/重启要求和失败原因。PID 重用必须重新绑定；不得单靠标题、文本、AutomationId、前台窗口或坐标。

必须同时记录 fail-closed 负向结果：映像/签名/版本/语言/窗口框架/控件树任一失配，窗口关闭或未知、多个候选窗口、PID 重用、UIA 结构变化/属性冲突、权限不足、设置页不可读、16.7 之外版本、安装器来源或签名不符、安装未完成、执行后读取失败、目标未达到、候选词数量为 `10`/任意字符串/缺失值。候选词仅接受 `three` 至 `nine`。任何负向场景都必须是明确失败、不适用或待确认，不得猜测成功或退回任意命令、脚本、注册表、配置路径、选择器或自由 UI 步骤。

本阶段默认不改代码、不推送。只有上述资料足以为每条规则形成稳定、多属性、可重新检测的受控身份边界时，才将脱敏证据提交到 `codex/issue-29-evidence`；不能冻结就保留 `pending_confirmation`，回报差距，不实现或放宽自动化。

## 完成回报

按 A、B、C、D 分段回报：起始/最终 SHA 与 worktree；是否修改、提交 SHA、远端分支与 PR 链接；逐条实际命令和 x64/ARM64 结果；日志/证据位置；真实 Windows 环境版本；未验证边界和明确停止原因。若 A/B 推送了提交，说明 PR 仍为 Draft 且未合入。没有提交时直接写“无提交、无推送”，不要用计划或推测替代结果。
