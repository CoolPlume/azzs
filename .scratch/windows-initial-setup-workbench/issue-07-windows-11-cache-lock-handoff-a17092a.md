# 事项 07 Windows 11 缓存适配器一次性交接包

日期：2026-08-12  
事项：07 - 实现离线资源与缓存管理  
用途：在真实 Windows 11 环境取得 x64 缓存适配器失败的底层错误；不作为已通过的 Windows 实机证据。

## 固定候选

| 项目 | 值 |
| --- | --- |
| 仓库 | `CoolPlume/azzs` |
| 分支 | `codex/issue-07-offline-package-cache` |
| 候选 SHA | `a17092ad1c20039d874b99e6e0e2d622ea61f581` |
| 事项起始基线 | `43225e13d2716d182f3dfbee5b800283c1362fd8` |
| PR | [#26](https://github.com/CoolPlume/azzs/pull/26)，base=`codex/v1-integration`，Draft |
| 当前代码状态 | 事项 07 实现已提交；不要 rebase、force push、合入 `integration/main`、tag、Release 或处理 WiX |

候选 SHA 的工作树应保持干净；本文件是本地交接材料，不要为了交接再推送提交，以免再次触发 GitHub Actions。

## 已知 CI 证据

诊断 run：[31611181510](https://github.com/CoolPlume/azzs/actions/runs/31611181510)

| job | 结果 | 证据 |
| --- | --- | --- |
| x64 Release | 失败 | [job 94162610784](https://github.com/CoolPlume/azzs/actions/runs/31611181510/job/94162610784)，Build and test 在 CTest 失败 |
| ARM64 Release | 构建成功，未执行测试 | [job 94162610715](https://github.com/CoolPlume/azzs/actions/runs/31611181510/job/94162610715)，构建、结构化证据和保留日志均成功；工作流配置为 `BUILD_TESTING=OFF` |

x64 失败步骤的精确输出为：

```text
local package cache adapter diagnostic: first begin code=4 detail=stale controlled cache temporary state cannot be cleared
local package cache adapter contract failed: first writer must acquire a typed controlled asset lock
local package cache adapter contract failed: same identity must be locked across write sessions
local package cache adapter contract failed: completion must publish marker and payload without partial
local package cache adapter contract failed: matching marker and payload must be readable
local package cache adapter contract failed: unlocked orphaned partials must be removed
```

`code=4` 是 `CacheWriteBeginCode::failed`，不是底层 Win32 错误码。当前提交只为 `AssetLock::try_acquire` 暴露 Windows 锁打开错误；本次实际失败发生在锁成功之后的临时状态清理路径，`remove_file` 仍把 `std::error_code` 压成了泛化 detail，因此 CI 没有获得真正的 Win32 数值、类别或消息。不能据此猜测单点修复。

x64 证据包中的 manifest 记录：Windows Server 2025/VS2026 runner，`processArchitecture=AMD64`，`imageVersion=20260803.193.1`，测试结果为 24 个测试中 1 个失败；失败测试是 `local-package-cache-adapter.contract`。ARM64 job 仅按工作流配置完成编译链接（`BUILD_TESTING=OFF`），没有执行任何 ARM64 合同测试，不能外推为 ARM64 运行时通过。GitHub artifact 名称为 `windows-x64-evidence` 与 `windows-ARM64-evidence`。

## Windows 11 最小复现

在 Windows 11 25H2、Visual Studio 2026 `18.8.2`、MSVC `14.51.36231`、CMake `4.3.1-msvc1` 和 Windows SDK `10.0.28000.2526` 环境中执行：

```powershell
git fetch origin codex/issue-07-offline-package-cache
git switch --detach a17092ad1c20039d874b99e6e0e2d622ea61f581
git status --short

& .\eng\ensure-windows-sdk.ps1

$vs = "C:\Program Files\Microsoft Visual Studio\18\Enterprise"
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"
& $cmake --preset windows-x64 "-DCMAKE_GENERATOR_INSTANCE=$vs"
& $cmake --build --preset windows-x64-release --target azzs_local_package_cache_adapter_contract
& $ctest --preset windows-x64-release --output-on-failure --verbose -R "^local-package-cache-adapter\.contract$"
```

不需要启动 WinUI、管理员权限、真实网络、网络共享或 U 盘。也可以用 `& .\eng\build.ps1 -Architecture x64` 复现同一失败，但该命令会构建完整 x64 core/WinUI，较慢。

## 一次性底层错误观测

不要先改锁算法或清理顺序。仅在 Windows 本地临时副本中，在 [local_package_cache_storage.cpp](../../src/adapters/infrastructure/src/local_package_cache_storage.cpp) 的 `remove_file` 中打印每一次失败的完整事实，然后运行上面的 focused CTest；不提交、不推送这次观测改动。至少记录：

- `path`（只保留测试根的角色化路径，不公开用户名）；
- `symlink_status`、`remove` 或末尾 `exists` 对应的 `error.value()`；
- `error.category().name()`、`error.message()`；
- 失败的是 `.partial`、`.complete`、`.payload` 还是 `.complete.tmp`；
- 该文件在调用前的 `symlink_status` 类型，以及锁句柄是否仍由当前会话持有。

最小观测输出应类似：

```text
remove_file diagnostic suffix=.partial value=<win32> category=<category> message=<message>
```

其中 `<win32>` 必须是 Windows 错误的真实数值；如果只得到 `code=4` 或固定英文 detail，说明观测仍未到达底层错误，停止修改并回报缺口。

## 预期证据与停止条件

只有在不改变业务合同的单点修复显然对应上述真实错误时，才由 Windows 操作者在本地验证后另行决定是否开新提交。成功证据必须同时包含：

```text
first begin code=0 (acquired)
second begin code=1 (busy)
local-package-cache-adapter.contract ... Passed
```

并确认同一测试根下首次写入生成 `.partial`，完成后仅留下匹配的 `.payload` 与 `.complete`，没有 `.complete.tmp`；`read_completed` 返回 `found`，孤立 `.partial` 清理成功。若真实错误不能唯一指向一个显然正确的单点修复，保留本候选 SHA、PR 和失败证据，不再触发 GitHub CI，转回维护者决定。

## 未验证边界

- 当前只完成 macOS 受控文件系统合同与 GitHub Windows runner 证据；x64 runner 合同失败，ARM64 runner 仅编译链接成功且未运行合同测试。
- 未完成 Windows 11 x64/ARM64 实机上的缓存适配器合同、网络共享、U 盘、介质拔出/恢复、真实网络传输或下载续传验证。
- 事项 08 安装批次真实执行、事项 14 驱动下载和 WiX 均不在本交接范围。
