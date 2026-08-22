# 实现 v0.1.0 生产软件安装链

Type: task  
Status: ready-for-agent  
Resolution: completed
Blocked by: none
Owner: issue-38
Consumers: 39, 40
Claimed by: v0.1.0
Verification: 对全部 11 项正式软件目录、受控安装档案、生产组合根与 x64 无界面合同运行定向验证；不得执行真实第三方软件安装。
Evidence freshness: 绑定当前版本分支提交、目录修订、安装档案、来源观察事实、生产适配器与 x64 构建；其中任一项变化后重跑受影响的自动化检查。

## Goal

把现有软件选择、离线缓存和安装批次状态机接入真实的生产适配器，使 `v0.1.0` 的 11 个正式目录项都保留在目录中，并让每个可用项通过受控来源、受控安装档案和结果检测进入可执行安装批次；无法冻结资源的项在自身条目下 fail-closed，不阻断其他项。

## Boundary

本事项拥有生产来源解析、网络观察、软件存在检测、受控下载或托管取得、Windows 安装器启动/观察/终止及结果检测的实现与自动化证据。事项 06、08、09、19 的已完成状态机和 mock 合同保留为基础回归证据，但不证明本事项已经实现。真实 Windows 11 x64 第三方软件安装不在本事项验收内，不能因其尚未执行阻止本事项完成，也不能被 mock 或安装器退出码伪装为通过；它由事项 39 单独拥有。

## Acceptance Criteria

- [x] 生产组合根不再为软件安装路径注入 `UnavailableControlledSourceResolver`、`OfflineNetworkObserver`、`UnavailableSoftwarePresenceDetector` 或 `UnavailableControlledPackageDownloader`；输入未知、网络不可用、来源失效或检测失败时仍按既有语义 fail-closed。
- [x] 生产来源解析只消费正式软件目录声明的原始地址和来源用途，并为每个可执行项目生成完整、可追溯的来源解析结果；无法稳定识别版本、架构、安装包类型或受控安装档案时只暂停当前项目，不猜测地址或静默换源。
- [x] 网络观察、存在检测与受控取得返回真实 Windows 或网络事实；已安装项只作为存在而非最新版本，下载不接收用户在工作台外取得的安装文件，托管来源不伪装为可缓存文件。
- [x] 每个可自动执行的安装器均由受控安装档案明确版本适用性、交互处置、受控参数、完成边界、安装后置行为与结果检测；首进程退出、快捷方式或用户点击都不是成功结论。
- [x] 所有 11 项正式目录软件均保留并有项级运行时结论；可用项有可执行的受控安装档案和稳定来源策略，无法冻结的项明确为项级不可用。ZIP 档案冻结容器大小与 SHA-256、内部成员路径/完整性、实际架构和受控启动方式；稳定版无法可靠判定时只暂停当前项目，x64 缺少可用安装包时可以使用厂商 x86 兼容安装器并记录实际架构，DirectX 保持 `9.29.1974.1` 官方 Web Installer 的固定例外，搜狗不适配的新版本暂停。Cheat Engine 当前因缺少冻结官方资产保持项级不可用。
- [x] `catalog/software-catalog.toml` 的全部 11 项均启用且引用适当安装档案，目录为 `release_state = "release"`，并通过发布级目录与能力校验。
- [x] 新增或调整的内置受控能力遵循架构单向依赖和唯一装配入口；目录内容只声明受控数据，不携带第三方可执行插件或任意命令。
- [x] 自动化合同覆盖每种来源解析/取得机制、ZIP 容器与内部成员校验、网络不可用、存在检测、安装档案启动、交互回退、完成观察、结果检测、单项不可用保留及“安装结果待确认”暂停；测试替身只证明回归行为，证据明确标注未进行真实第三方软件安装。
- [x] x64 构建及与改动相关的无界面/适配器合同通过；未执行的真实安装、ARM64、WiX/MSI 和安装生命周期准确记录为未验证或延期。

## References

ADR-0048，规格第 9、10、12、13、19、20、21 节，`SW-15`、`SW-21` 至 `SW-38`、`SRC-08` 至 `SRC-36`、`CAT-41` 至 `CAT-52`，事项 06、08、09、19、39、40。

## Comments

- 2026-08-21：由 `v0.1.0` 的版本限定 Beta 合同创建。此前未合入的事项 38 草稿把真实 Windows 安装列为本票据结票条件，与本版本授权冲突；本票据将生产实现与真实安装验证拆开，避免用未授权的第三方安装阻断已经授权的 Beta 实现，也不降低后续真实验证门槛。

## Answer

事项 38 已完成。最终候选提交由 build/package manifest 的 `source.commit`/`sourceCommit` 精确绑定；生产组合根已装配 `WindowsRegisteredSourceResolver`、`WindowsControlledPackageDownloader`、`WindowsRegistrySoftwarePresenceDetector`、受控安装下载/档案/启动/结果检测适配器，未再注入 unavailable/mock 生产替身。

- `catalog/software-catalog.toml` 为 `release_state = "release"`，11 项启用且运行时目录保持 11 个唯一 ID；9 项有受控 profile/fact，Cheat Engine 与几何画板保留目录身份并在自身条目下 fail-closed，不阻断其他项目。
- 受控来源与安装档案合同覆盖 8 个固定 installer asset、Office Tool Plus 固定 x64 ZIP（SHA-256 `43ba169e4d07c8e45ed4846d7171bfbc521e8f61efff366112b7c6ef9dae627b`）及成员白名单；ZIP 标记、额外成员、CRC、架构、CDN 文件名、重定向身份和 Content-Range 负例均有无界面证据。
- x64 Release 构建、版本合同、生产目录、来源解析、软件选择、网络观察、存在检测、缓存和安装批次相关合同通过；mock 仅作为回归证据，未执行任何真实第三方软件安装。
- 完整 CTest 中 `execution-log.contract`、`portable.package.contract`、`bundled-catalog-resource.contract`、`windows-device-data.contract` 的失败均为当前宿主未准备 ACL/reparse fixture（包含 `ERROR_PRIVILEGE_NOT_HELD (1314)`），未将其写成产品通过，也未将其升级为事项 38 的实现缺陷。
- ARM64、WiX/MSI、UAC/UI、DPI、安装/Repair/卸载/迁移生命周期、rescue 和 large-offline 仍按发布决策延期；真实 Windows 11 x64 安装由事项 39 保持 open。
