# 实现驱动获取页面

Type: task  
Status: ready-for-agent  
Resolution: completed
Blocked by: 03, 07, 08, 12, 13, 24
Owner: issue-14
Claimed by: codex/issue-14-driver-acquisition-evidence
Consumers: 16, 21
Verification: 驱动外部交接核心和 UI 测试，覆盖未识别、返回选择、重启、网络、显示、硬件变化及不伪报完成。
Evidence freshness: 绑定功能合入提交 `c63b6deaa8eaf3adc6d45c3d2dabe5a9cb1af99c`、目录修订和本票记录的 x64 验证；硬件、网络或外部入口行为变化后须重新观察，外部返回后仍以当次重新检测为准。

## Goal

依据硬件概览组织驱动助手和官方厂商页面，同时保持工作台不直接管理具体驱动包的边界。

## Acceptance Criteria

- [x] 显示精选驱动助手入口和固定厂商/OEM 页面入口。
- [x] 硬件识别成功时推荐或高亮相关入口，失败时固定入口仍可用。
- [x] 助手未安装时提供安装，已安装时提供启动。
- [x] 工作台不直接匹配、下载或安装具体驱动包。
- [x] 启动外部驱动操作前保留流程进度并标记“驱动交接中”。
- [x] 外部操作返回或下次启动时由用户选择“已完成”“需要重启”或“暂时跳过”，不自动判定完成；用户选择“已完成”后重新读取必要硬件和网络状态并记录观察结果，但不写成已验证驱动成功。
- [x] 能处理网络、显示和硬件识别变化；用户选择需要重启时建立重启屏障。

## References

`DRV-01` 至 `DRV-03`、`DRV-10` 至 `DRV-15`

## Answer

- 功能提交 `e815f8d8d2b14183cf8a851afbd376afb522f99e` 经 PR [#45](https://github.com/CoolPlume/azzs/pull/45) 普通合入 `codex/v1-integration`，合入提交为 `c63b6deaa8eaf3adc6d45c3d2dabe5a9cb1af99c`。本票的纯文档证据提交不重复执行已绑定到功能提交的构建、合同或 CI。
- Windows 11 x64 本机通过 `driver-acquisition.contract`、`restart-resume.contract`、`ui.presentation.contract` 和 `ui.design.contract`；干净 x64 CMake 全量构建及 WinUI 主机构建成功，生成 `Azzs.WinUI.exe`。GitHub Actions run `31807603375` 的 x64 Release 在功能 PR 上成功。
- Developer Command 环境下的 `cmake --workflow --preset host-guardrails` 编译成功，29 项中 27 项通过。`execution-log.contract` 因本机未准备中完整性 ACL root、`windows-device-data.contract` 因隔离目录符号链接 root 前置条件缺失而未通过；两项均保留为 host 未验证边界，未写作产品通过。
- 未执行真实厂商/OEM 页面或驱动助手的人工 UI/UAC 交接，也未下载、匹配或安装任何具体驱动包；后者是本事项的受控外部交接边界。ARM64 未在本机执行并已延期，自动 CI 的 ARM64 job 不作为 ARM64 验收或八制品完成结论。

## Comments

- 2026-08-10：维护者确认 Q3，首版只做说明、定位、记录和返回检测，不由工作台选择或安装 INF/EXE；ADR-0009 保持有效。产品边界已经完整，本事项改为 `ready-for-agent`；尚未完成的前置只通过 `Blocked by` 表达，不使用 `needs-info` 重复表示。
- 2026-08-14：依据已合入 integration 的功能树、Windows x64 本机合同/构建及 GitHub x64 CI 证据完成结票；host 夹具前置失败、人工外部交接和主动 ARM64 验收均明确保留为未验证或延期，未扩大为成功结论。
