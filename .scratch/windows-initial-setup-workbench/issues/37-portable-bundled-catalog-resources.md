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
