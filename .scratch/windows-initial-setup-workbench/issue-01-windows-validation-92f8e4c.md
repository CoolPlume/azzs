# 事项 01 Windows 11 25H2 验证记录

日期：2026-08-11  
事项：01 - 建立应用基础与页面框架  
分支：`codex/issue-01-application-foundation`  
原始目标提交：`92f8e4c0f2794eea7c128944af3a4ca6a8e8aaf3`  
最终验证提交：`cd512763bb8f7e65879c01b89f97036ddf095991`

## 结论

- 最终提交的 x64、ARM64 Release 构建均成功，manifest 均为 `result=succeeded`、`source.dirty=false`，并包含 `Azzs.WinUI.exe`。
- x64 core smoke 为 1 个测试、0 个失败；x64、ARM64 portable ZIP 均成功生成并绑定最终提交。
- 最终 x64 portable 在 Windows 11 25H2 x64 实机上完成 UAC、简体中文、七页导航、版本提示、关闭和再次启动验证。
- 后续构建脚本兼容修复提交 `e1574ac2df0b7dbf031840ad14e5d49a5dec5521` 已通过只读 GitHub Actions 的 x64 与 ARM64 检查。
- 本会话没有获得适用于本用途的 WiX 7 Binary Release EULA 明确确认。安装器入口保护门通过；实际 MSI 生成、安装和生命周期验证因条款未确认而阻断。
- 未创建 tag 或 GitHub Release，未上传应用二进制，PR 保持 Draft。

## 主机与会话

| 项目 | 记录 |
| --- | --- |
| 验证主机 | Windows 11 25H2 x64 实机；公开证据不保留本机名称 |
| 会话身份 | 交互登录用户；公开证据不保留本地账户名 |
| Windows | 注册表 `DisplayVersion=25H2`，build `26200.8973`，x64 |
| `Get-ComputerInfo` 兼容字段 | `WindowsProductName=Windows 10 Pro`、`WindowsVersion=2009`、`OsBuildNumber=26200`、`OsArchitecture=64 位`；版本判断采用注册表 `DisplayVersion=25H2` 与 build |
| PowerShell | 构建使用 portable PowerShell `7.6.4`；启动 UI 的调用会话为非管理员（`IsAdministrator=False`） |
| GitHub CLI | 准备阶段未登录；推送前复核已通过 Windows keyring 登录 `CoolPlume`，Git 协议为 HTTPS |

## 锁定工具链

最终两个 build manifest 记录的工具链一致：

| 工具 | 版本 |
| --- | --- |
| Visual Studio 2026 Community Stable | `18.8.2` |
| MSVC | `14.51.36231` |
| C/C++ compiler | `19.51.36252.0` |
| MSBuild | `18.8.2.30814` |
| CMake | `4.3.1-msvc1` |
| Windows SDK release | `10.0.28000.2526` |
| Windows SDK target | `10.0.28000.0` |
| C++ mode / CRT | `/std:c++latest` / `/MT` |
| Windows App SDK | `2.3.1` |
| C++/WinRT | `3.0.260715.1` |

没有放宽版本、关闭检查或通过删除 UAC 处理构建与运行失败。

## 原始失败与修复链

1. 在干净的原始目标提交 `92f8e4c0f2794eea7c128944af3a4ca6a8e8aaf3` 上，x64 core 构建和 smoke 通过，WinUI 构建失败。工具发现逻辑选中了 `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`，其 Windows 目标被错误解析为 `7.0`，MSBuild 报 `MSB8036`。原始失败 manifest 保存为 `out/logs/windows-x64-release-92f8e4c-failed.json`，其中 `result=failed`、原始 commit 正确、`dirty=false`。
2. 提交 `dd7e9bf1819085645db3bfb0b9c67b97203e3640` 收紧 `vswhere` 组件条件，选择同时具备 x64、ARM64、CMake 与 UWP C++ 能力的完整 VS Community 实例。
3. 之后 MIDL 以 `0xC0000135` 失败，确认本机已注册的 SDK `10.0.28000.2526` 缺少 `MidlrtMd.dll`。这是机器 SDK 包损坏，不是版本选择或代码缺陷。使用微软签名的本机缓存 MSI 执行修复，退出码为 0；该 MSI 的 SHA-256 为 `FEDF6E2E90FB42D321B9ED048235BA8464A816DCABD0A17144CE109AB15CC06B`。修复前 manifest 保存为 `out/logs/windows-x64-release-dd7e9bf-sdk-repair-needed.json`。
4. XAML 生成文件曾落入源码目录并令 manifest 记录 dirty。提交 `eba5bd65ad10fa4169010ba2f71629cf88c425ad` 将 WinUI `GeneratedFilesDir` 固定到 `out/obj`；受影响文件的诊断副本保存在忽略目录 `out/logs/source-generated-files-dd7e9bf-before-path-fix`。
5. `eba5bd65ad10fa4169010ba2f71629cf88c425ad` 已能完成双架构构建和 portable 打包，但最终 x64 portable 在 UAC 后于窗口创建阶段崩溃。Application Error 为 `0xC000027B`，内部 HRESULT 为 `0x802B000A`。定向异常边界进一步得到 `Cannot find a Resource with the Name/Key TabViewButtonBackground`；补入 `XamlControlsResources` 后又定位到根 `Window` 的 `x:Uid` 无法赋值 `Window.Title`。
6. 提交 `cd512763bb8f7e65879c01b89f97036ddf095991` 合并 WinUI 框架资源，并改用现有 `ResourceLoader` 设置本地化窗口标题。启动探针由稳定复现 `0xC000027B` 转为在约 1 秒内创建标题为“Windows 初装工作台”的窗口。临时 `[DEBUG-7c41]` 日志与探针代码均已清理，调试日志也已从最终 payload、manifest 和 ZIP 中移除。

GitNexus 在修复提交前报告变更为 4 个文件、1 个符号、0 个受影响 process，风险为 low；`MainWindow` 上游影响为 0。提交后重新建立索引，最终索引 commit 与 `cd512763bb8f7e65879c01b89f97036ddf095991` 一致。FTS 扩展不可用不影响结构索引、impact 或 `detect-changes`。

## 后续托管 CI 复核

验证记录提交后，GitHub Actions run [31456176999](https://github.com/CoolPlume/azzs/actions/runs/31456176999) 暴露了 `vswhere` 组件标识差异：Windows 实机使用 `Microsoft.VisualStudio.ComponentGroup.UWP.VC`，托管镜像注册的是 `Microsoft.VisualStudio.ComponentGroup.UWP.VC.v142`；空查询结果 `[]` 又在解析后被直接取 `[0]`，使两个架构都在真正构建前退出。

提交 `e1574ac2df0b7dbf031840ad14e5d49a5dec5521` 接受这两个组件标识并显式检查解析结果数量。随后 run [31456835674](https://github.com/CoolPlume/azzs/actions/runs/31456835674) 的 x64 Release 与 ARM64 Release 均成功；x64 包含 core smoke，ARM64 完成编译链接。该拉取请求 run 检出的候选合并提交为 `3143080cc65a4d903a60d5cc460c46a1e317cd21`，其父提交分别是集成基线 `caa1658b5e8e6fc79fd959564e1790f99c951357` 与功能分支头 `e1574ac2df0b7dbf031840ad14e5d49a5dec5521`。两个 build manifest 均为 `result=succeeded`、`source.dirty=false`。

## 最终构建与打包

构建前 HEAD 为 `cd512763bb8f7e65879c01b89f97036ddf095991` 且 `git status --porcelain` 无输出。PowerShell 7 在仓库根目录执行：

```powershell
& .\eng\ensure-windows-sdk.ps1
& .\eng\build.ps1 -Architecture x64
& .\eng\build.ps1 -Architecture ARM64
& .\eng\package-portable.ps1 -Architecture x64 -SkipBuild
& .\eng\package-portable.ps1 -Architecture ARM64 -SkipBuild
```

全部命令退出码为 0。完整 transcript 为 `out/logs/windows-11-25h2-handoff.log`。清理调试阶段残留的 `azzs-ui-debug.log` 后，在同一 transcript 中追加重跑 x64 build 和 x64 portable 打包；最终 x64 manifest 和 ZIP 均不含该文件。

### Build manifest

| Manifest | result | commit | dirty | target | `Azzs.WinUI.exe` |
| --- | --- | --- | --- | --- | --- |
| `out/manifests/windows-x64-release.json` | `succeeded` | `cd512763bb8f7e65879c01b89f97036ddf095991` | `false` | `x64/Release` | 包含 |
| `out/manifests/windows-arm64-release.json` | `succeeded` | `cd512763bb8f7e65879c01b89f97036ddf095991` | `false` | `ARM64/Release` | 包含 |

x64 `out/test-results/core-x64-release.xml`：`core.smoke`，tests `1`，failures `0`，disabled `0`，skipped `0`，输出 `core smoke passed`。

### Portable 产物

| 架构 | 产物 | package manifest | 大小（bytes） | SHA-256 |
| --- | --- | --- | ---: | --- |
| x64 | `out/packages/Azzs-standard-x64-portable.zip` | `out/manifests/package-standard-x64-portable.json` | 44,876,881 | `D45A7A93C04BF59F44396A7BF9650E7788136815470AD8797C7E76139298CD5A` |
| ARM64 | `out/packages/Azzs-standard-ARM64-portable.zip` | `out/manifests/package-standard-arm64-portable.json` | 43,690,972 | `278654DC94850B7C506CB856D5CA900C95328469F660D45FABA5905C97EEAFD9` |

两个 package manifest 均为 `kind=portable`，架构正确，`sourceCommit=cd512763bb8f7e65879c01b89f97036ddf095991`，payload 包含 `Azzs.WinUI.exe` 且不含调试日志。

## x64 Windows 11 25H2 实机 UI

最终 x64 ZIP 解压到新的临时目录 `work/ui-validation-cd512763-20260811-1130`，从其中的 `Azzs.WinUI.exe` 启动。调用 PowerShell 为非管理员会话。

1. 自动化回归先仅在调试启动时使用 `RUNASINVOKER` 绕过 UAC，以获得稳定、可重复的 UI Automation 红/绿信号；该模式不作为 UAC 通过证据。七个稳定 `AutomationId` 逐页选择均成功：概览、驱动、系统优化、软件安装、软件优化、历史与日志、应用设置。
2. 每页导航项名称、选中状态和可见内容标题一致，均为简体中文；页面无崩溃，无完全空白内容区。当前页面只有标题属于预期骨架。
3. 窗口标题为“Windows 初装工作台”；首次和再次启动默认选中“概览”，内容标题也是“概览”。
4. Windows 11 25H2 build `26200.8973` 上没有可见“Windows 版本风险”黄色提示。没有伪造旧系统版本；旧版本判定路径由 core smoke 覆盖。
5. 真实入口随后由非管理员 PowerShell 启动。每次启动前出现一次 UAC，由用户确认后应用打开；同一进程内逐页导航到“应用设置”没有再触发 UAC。
6. 在“应用设置”页通过 UI Automation 的窗口关闭操作关闭高完整性窗口；`Get-Process -Name Azzs.WinUI -ErrorAction SilentlyContinue` 无输出。中完整性进程直接发送 `WM_CLOSE` 会被 Windows UIPI 阻止，改用用户等价的 UI Automation `WindowPattern.Close()` 后进程在 5 秒内退出；这不是应用退出失败。
7. 再次真实启动时重新出现 UAC。实机截图目视确认重启后的窗口仍为简体中文、默认“概览”、标题正确、无版本风险提示；最后关闭后再次确认无 `Azzs.WinUI` 进程。

## 安装器入口

本会话没有维护者对 WiX 7 Binary Release EULA 的明确确认或 `-AcceptWixEula` 授权。按要求仅运行一次：

```powershell
& .\eng\package-installer.ps1 -Architecture x64 -SkipBuild
```

命令以仓库预期的条款保护错误退出：`WiX Toolset 7 requires an explicit terms decision. Re-run with -AcceptWixEula only after confirming the WiX 7 terms for this use.`

结论：入口保护门通过，实际 MSI 生成因条款未确认而阻断。没有运行 ARM64 安装器命令，没有生成 x64 或 ARM64 MSI，也没有执行机器级安装、修复、升级、卸载或回滚。

## 未执行与范围边界

- 未执行 ARM64 实机启动；ARM64 仅完成编译、链接和 portable 打包。
- 未执行 4K、225% 显示缩放或混合 DPI 多显示器专项验证。
- 未分别执行纯键盘、纯鼠标、触摸、系统关闭动画、应用减少动画及快速重复导航专项验证；现有 UI Automation 结果不替代这些输入与动效场景。
- 未执行安装生命周期或干净机离线运行验证。
- 未执行旧 Windows 实机；旧版风险路径只采用 core smoke，不伪造系统版本。
- WiX 条款未确认，因此 MSI 生成和安装入口之后的生命周期全部阻断。

历史基线 run [31413000254](https://github.com/CoolPlume/azzs/actions/runs/31413000254) 与修复后 run [31456835674](https://github.com/CoolPlume/azzs/actions/runs/31456835674) 的 x64 Release、ARM64 Release 均成功。

本次未合并 PR、未转 Ready、未修改事项 Resolution、未创建 tag 或 Release、未上传应用二进制。`out/` 继续由 Git 忽略，Git 仅管理本记录和事项评论。
