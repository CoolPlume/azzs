# 使便携发行内置目录自包含

Type: task  
Status: ready-for-agent  
Resolution: open
Blocked by: 01, 03, 19, 25, 29, 31
Owner: issue-37
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
