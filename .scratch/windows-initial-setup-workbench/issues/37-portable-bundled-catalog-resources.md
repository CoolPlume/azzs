# 使便携发行内置目录自包含

Type: task  
Status: ready-for-agent  
Resolution: open
Blocked by: 01, 03, 19, 25, 29, 31
Owner: issue-37
Claimed by: codex/issue-37-portable-resources
Consumers: 21
Verification: 在无仓库源目录的干净便携候选中运行内置目录加载、内容清单、package manifest 与 portable package contract；确认资源缺失、路径漂移和摘要不匹配均 fail-closed。
Evidence freshness: 绑定当前源码、目录文件、发行内容清单、打包脚本和候选提交；目录模式、资源布局、启动装配路径或清单字段变化后重新生成并验证候选。

## Goal

让标准版、断网救援版和超大离线版的便携发行包携带启动和运行所需的内置目录资源，并在发行包内部以稳定、可验证的部署路径加载，消除通过 `__FILE__` 回溯源码仓库目录的部署依赖。该切片只拥有目录资源的自包含与打包门禁，不拥有目录内容本身的维护发布决定、安装器生命周期或外部 Release。

## Ownership Boundary

本事项只拥有便携发行资源布局、内置目录加载接缝、资源内容清单与对应 package manifest/contract 的改动。事项 03、19、20、25、29 继续拥有目录模型、首版目录内容和发布级目录状态；事项 21 汇总八制品候选门禁。本事项不得把本机试用目录、维护仓库路径或未锁定输入当作发行资源。

## Acceptance Criteria

- [ ] `catalog/software-catalog.toml` 与 `catalog/software-optimization-catalog.toml` 作为发行输入被明确复制或生成到每个便携候选的固定包内路径；构建和运行时不再依赖 `composition_root.cpp` 中通过 `__FILE__` 回溯开发仓库的路径。
- [ ] 便携包在源代码目录不可用、当前工作目录改变且无额外开发运行时的条件下，仍能离线加载三类内置目录；资源缺失、路径越界、架构/版本不匹配或内容摘要不一致时 fail-closed，不自动回退到本机试用目录。
- [ ] `release/artifact-content-manifest.v1.json` 覆盖标准版、断网救援版和超大离线版三个 x64 便携制品的目录资源布局与 x64 内容绑定；`package manifest` 记录规范化仓库相对包路径及其内容绑定，不含 `__FILE__` 派生的绝对路径、盘符或开发机路径。ARM64 与安装版的八制品覆盖继续由事项 21 的长期发行门禁拥有，当前 x64 工作不得写成其已通过。
- [ ] standard、rescue、large-offline 三档便携内容共享同一套内置目录语义；rescue 与 large-offline 的额外输入仍分别受受控制品门禁和 `release_state = "release"` 门禁约束，空输入不得伪造为完整制品。
- [ ] 便携发行合同覆盖资源完整性、ZIP/staging 内容一致性、离线加载以及故意缺失或篡改资源的拒绝路径；该验证不替代 ARM64 实机、安装生命周期或真实设备验收。

## References

`CONTEXT.md` 中的“便携版”“自包含发行包”“x64 临时候选”“标准版”“断网救援版”“超大离线版”“候选资源”、事项 03、19、20、21、25、29、`docs/adr/0030-controlled-rescue-tool-release-boundary.md`、`docs/adr/0037-three-edition-eight-artifact-matrix.md`、`docs/adr/0047-x64-candidate-milestone-boundary.md`、`docs/engineering/architecture-and-code-quality.md`。

## Comments

- 2026-08-17：切片源自发行包审查：`src/composition/windows/composition_root.cpp` 的 `repository_catalog_path()` 与 `repository_optimization_catalog_path()` 通过 `__FILE__` 计算仓库路径，而现有便携打包只复制 Release 输出；内容清单当前只列 x64 的三个便携条目。当前仅建立执行合同，未修改源码、清单或打包脚本，未生成任何制品。
- 2026-08-17：只读静态核对确认，源码实现已由 feature `9559ba73cb1e64a0d11099fcbe6c011e8f50d2fe` 经 integration merge `fd2cec28c1f4d27fd1507e74771632e7575d2485` 落地到当时 integration 基线 `6896ff364f6c7a3a63949b85cbc68cff281cba5d`；ADR-0047 及其合入 `6896ff364f6c7a3a63949b85cbc68cff281cba5d` 只澄清 x64 临时候选范围，不构成制品生成或验收结论。已做源代码、PowerShell、JSON 与 XML 静态检查，但未运行构建、portable contract、打包、EXE 或真实候选；资源拒绝路径的行为级证据仍缺失，因此 `Resolution: open` 不变。
- 2026-08-17：截至本记录的只读静态源码与 Git 核对，PR #81 的 feature 父提交 `a2b275bbddf4429cb46e169b4237fc970daab4f7` 及增量 `9b8f9ca94a6e806c2b4d487a8363caa82edd3162` 已经由 integration merge `cf17c1c0164e5d1b08302c06d926cb635ba4b01d` 合入 `codex/v1-integration`，PR #81 已 merged。包内资源读取器在读取前检查路径链与重解析点，并从同一可信句柄一次性读取，在内存中校验长度和 SHA-256 后交付拥有的不可变字节；后续消费者不再按可变包内路径重开。软件优化目录的初始 `ensure_builtin()` gate 已移动到完整 `WindowsWorkbenchServices` 构造之前，失败仍映射既有 `bundled_catalog_resources` 启动失败；优化目录更新源也消费同一内存字节。软件目录启动仍仅 `restore()`，不自动调用 `preview_built_in()`、`read_built_in()` 或应用 `catalog/software-catalog.toml`；该目录仍为 `release_state = "draft"`，正式发布 gate 及事项 21、25、29 继续阻断其合法启用。新增无界面合同源码覆盖资源缺失、摘要不符、重解析点与校验后路径替换的拒绝/快照语义，但本次没有运行该合同。未运行构建、CTest、EXE、调试器、portable package、真实 x64 候选、ARM64、MSI/WiX、安装生命周期、UI 自动化或 CI；这些静态证据和 PR 合入不构成 ADR-0037 八制品完成或候选通过，资源拒绝路径与候选的行为级证明仍缺失，`Resolution: open` 保持不变。
- 2026-08-17：PR [#92](https://github.com/CoolPlume/azzs/pull/92) 的 feature `eac74b7b038b99308ab206202867e4e4e5227ec6` 已普通合入 `codex/v1-integration`，merge SHA 为 `f650ab57c6e91ef312a5fe32257d9822b09c427f`。`bundled_catalog_resource_reader.cpp` 的 `final_path()` 改为使用不依赖长度查询终止符语义的增长缓冲区，并在 32768 字符上限处 fail-closed；句柄范围确认、普通文件检查、一次性读取、长度和 SHA-256 校验保持不变。仅完成源码、Git 和空白静态核对，未运行 `bundled-catalog-resource-contract`、portable package、构建、EXE、真实候选、ARM64、MSI/WiX、安装生命周期、UI 自动化或 CI，`Resolution: open` 保持不变。
- 2026-08-17：集成分支源码审查增量 `43e1fd9` 让便携复制和独立 verifier 在读取目录资源前拒绝 payload/staging 子树中的 reparse point，并限制 package manifest 输出与内容清单路径。该增量只强化资源自包含与发布边界，未运行资源合同、构建、portable package、EXE、真实候选、ARM64、MSI/WiX、安装生命周期、UI 自动化或 CI；`Resolution: open` 保持不变。
- 2026-08-17：integration 提交 `c34f9ec` 为 `bundled-catalog-resource-contract` 增加不安全相对路径拒绝覆盖，验证绝对路径、`..`、`.` 和空路径均 fail-closed。仅完成源码、Git diff/diff-check 和提交前范围核对，未运行该合同、构建、portable package、EXE、真实候选、ARM64、MSI/WiX、安装生命周期、UI 自动化或 CI；`Status`、`Resolution` 与 `Blocked by` 保持不变。
- 2026-08-17：integration `1ce0206` 修复 portable verifier 的资源循环变量遮蔽，标准 x64 package/verifier 路径已通过；完整 portable contract 在符号链接 fixture 处因宿主无管理员符号链接权限停止。`product.identity.contract`、WinUI async contract、启动装配与核心定向合同通过；未把宿主权限失败写成资源实现通过，ARM64、安装版、EXE、真实候选和安装生命周期仍未验证，`Resolution: open` 保持不变。
- 2026-08-17：integration merge `2b97587` 集成 feature `155871d`，修复独立 portable verifier 丢弃 build manifest `artifacts` 的门禁缺口；现在要求 staging/ZIP 包含 clean Release manifest 中全部非调试输出并匹配字节数，合同新增删除 `Microsoft.UI.Xaml.Controls.dll` 后必须拒绝的反例。D 盘 omission gate 通过并输出缺失构建输出错误；完整 portable contract 在既有符号链接反例处仍因宿主缺少管理员权限停止。未运行完整构建、EXE、ARM64、MSI/WiX 或真实候选，`Resolution: open` 保持不变。
- 2026-08-17：当前基线 `3c0b6cb` 修复 `GetFinalPathNameByHandleW` 返回路径的扩展路径前缀保留，并将相对路径 `/` 分隔符转换为 `\` 后再比较；D 盘 `bundled-catalog-resource-contract` 的有效资源与快照场景通过，仅符号链接夹具因当前账户缺少创建符号链接权限而阻断。`startup-assembly` 与 `core-smoke` 通过；未运行完整 MSBuild/NuGet、真实 EXE、ARM64、WiX/MSI 或安装生命周期，`Status`、`Resolution` 与 `Blocked by` 保持不变。
- 2026-08-17：在 integration `31779f7` 的 D 盘 host-debug 全量 CTest 中，portable package contract 的标准 x64 fixture 场景通过，`bundled-catalog-resource.contract` 的有效资源、摘要拒绝、路径拒绝与快照语义通过；剩余符号链接反例因当前账户无创建符号链接权限停止。未运行 Release 便携候选、真实 EXE、ARM64、MSI/WiX 或安装生命周期，事项 37 的 `Resolution: open` 保持不变。
- 2026-08-18：启动修复 feature `a87174c` 已由普通合并 `5737855` 集成，最新架构门禁登记提交为 `ef894d4`；其中 `bundled_catalog_resources` 的构造期资源接缝与优化目录预检顺序得到静态合同覆盖。x64 host-debug 启动合同 1/1 和 WinUI 异步边界合同通过，但本次没有运行便携资源合同、Release 便携包、真实 EXE、ARM64、MSI/WiX、安装生命周期或 CI；目录资源候选证据仍不足，`Resolution: open` 保持不变。
- 2026-08-18：在源代码等价于 `ef894d4` 的 x64 Release 隔离 worktree 中，`portable.package.contract` 的标准有效包、目录资源完整性、摘要/路径拒绝与快照场景均输出通过；`bundled-catalog-resource.contract` 的正常读取和拒绝路径进入 reparse 反例时停止。两项最终均因宿主没有创建文件/目录符号链接的管理员权限失败，未将该环境缺口写成资源实现通过；未生成并验收三档 Release 便携候选、ARM64、MSI/WiX、安装生命周期或 CI，`Resolution: open` 保持不变。
- 2026-08-18：在 integration `7049f9a571111d5a37a1a2b31de758b9411bde69` 的 AMD64 host-debug 隔离 worktree 中，使用 `D:\azzs-codex\tmp\issue37-resource-contract-20260818` 作为 `TEMP/TMP`，`azzs_bundled_catalog_resource_contract` 增量编译成功。`ctest --preset host-debug --output-on-failure -R bundled-catalog-resource.contract` 的缺失、摘要、路径和快照断言未报告失败；文件与目录 reparse fixture 均因 `CreateSymbolicLinkW` 的非特权尝试和回退尝试返回 `GetLastError=1314 (ERROR_PRIVILEGE_NOT_HELD)` 而停止，合同整体保持失败，未把宿主权限缺口写成资源实现通过；未改系统权限，ARM64、Release 便携候选、真实 EXE、MSI/WiX、安装生命周期和 CI 仍未验证。
- 2026-08-19：feature `codex/issue-37-portable-resources` 在 x64 资源读取接缝增加句柄读取后的锁定字节数复核，并新增实际长度超过 manifest 锁定值时拒绝的无界面合同。x64 Release WinUI/MSVC 构建、`architecture.project-graph`、`core.smoke` 和便携合同的标准资源/manifest 场景通过；资源合同和便携合同的 reparse 反例仍分别受 `ERROR_PRIVILEGE_NOT_HELD (1314)` 与宿主 Administrator 权限阻断。未运行 ARM64、MSI/WiX、人工 UAC、安装生命周期或真实候选，`Resolution: open` 保持不变。
- 2026-08-19：在当前 integration `fcf10922ea794f2e0fcde5e235fcde3417f628e8` 的独立 D 盘核对中，复用已生成且来源提交为其祖先 `85390bf0653c1d5031ac34987e60ca686e9c1ec2` 的 x64 Release 标准便携候选；`eng/verify-portable-package.ps1 -ArtifactId standard-x64-portable` 通过，ZIP `out/packages/Azzs-standard-x64-portable.zip` 为 `46123304` 字节，package manifest 的 `sourceCommit`、目录资源字节数与 SHA-256 均匹配。`bundled-catalog-resource.contract` 以 `ctest --test-dir .../out/build/windows-x64 -C Release -R bundled-catalog-resource.contract --output-on-failure` 运行，缺失、摘要、长度、路径和快照断言通过，但文件与目录符号链接夹具分别以 `CreateSymbolicLinkW` 的 `GetLastError=1314 (ERROR_PRIVILEGE_NOT_HELD)` 停止；该宿主阻断未记为资源实现通过。当前 `release/artifact-content-manifest.v1.json` 的 rescue/large-offline x64 输入仍为空，`eng/package-portable.ps1 -ArtifactId rescue-x64-portable -SkipBuild` 明确拒绝 `has no locked inputs`，因此没有完整三档候选，`Resolution: open` 保持不变；ARM64、安装版、MSI/WiX、真实 EXE、安装生命周期与 CI 未验证。
- 2026-08-19：本轮代码门禁提交 `f7aa1022365144ebcee05e1f4218e9b31ecfd590`（文档收口提交 `4939409fce2c4ce11acff8ac0960d211affb0d22`）的发行门禁源码审查修复了 portable verifier 的 build manifest 反向完整性缺口。内置目录资源和锁定 content input 仍是唯一允许的非 build payload；其余 staging 文件必须出现在 clean build manifest 中，删减 `Microsoft.UI.Xaml.Controls.dll` 的合同夹具现在 fail-closed。该增量只强化目录自包含发行门禁，不构成三档候选、ARM64、安装版、MSI/WiX、安装生命周期或真实设备验收；既有 `ERROR_PRIVILEGE_NOT_HELD (1314)` 宿主阻断和 rescue/large-offline `inputs=[]` 边界保持不变，`Resolution: open` 保持不变。
- 2026-08-20：在独立工作树 `D:\azzs-codex\worktrees\issue37-candidate-audit` 的基线 `0135570a4ea25f646fe48de0b0eb669032cec1b8` 上复核候选资源门禁。`release/artifact-content-manifest.v1.json` 的 `rescue-x64-portable` 与 `large-offline-x64-portable` 均为 `inputs = []`；`catalog/software-catalog.toml` 仍为 `release_state = "draft"`。使用 D 盘临时目录运行 `eng/package-portable.ps1 -ArtifactId rescue-x64-portable -SkipBuild` 与 `-ArtifactId large-offline-x64-portable -SkipBuild`，两者均在 `eng/portable-artifact-content.ps1:633` 以 `has no locked inputs` fail-closed，失败后未留下候选且工作树保持干净。源码和 Git 跟踪文件中没有受控的 `rescue-companion-tool` 或离线二进制输入；打包脚本的两个固定救援目录仍按设计保持空并仅供外部交接。当前 x64 临时候选只能推进 standard，rescue/large-offline 的真实阻断是缺少事项 03/19/25/29/31 所属的已锁定、受控输入与发布状态，不是事项37源码缺陷；未修改状态或验收项，未接受 WiX、未创建 Release/tag。
