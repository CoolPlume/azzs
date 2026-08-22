# 仅展示确认存在的物理硬件

Type: task
Status: ready-for-agent
Resolution: open
Blocked by: none
Owner: issue-05
Claimed by: none
Consumers: 06, 07
Verification: 无界面硬件观测/过滤合同覆盖真实、禁用、无驱动、虚拟、软件、VPN、回环、未知和 WMI/SetupAPI 失败夹具；Windows x64 代表性设备人工检查与日志投影核对。
Evidence freshness: 绑定硬件适配器、过滤规则、观测字段、测试夹具和具体 Windows 设备；设备枚举 API、驱动推荐输入或过滤分类变化后重跑。

## Goal

让所有硬件表面只显示实际存在的物理硬件，并为后续驱动推荐提供可审计事实。

## Ownership Boundary

Windows 硬件适配器负责取得原始观测并在单一过滤接缝确认物理性；核心负责把事实投影为硬件概览和状态；页面不得用字符串 substring 复制过滤。OEM 型号由整机摘要 owner 提供，不创建伪设备记录。

## Acceptance Criteria

- [ ] 每个输出记录包含物理存在确认、来源/置信度、当前存在状态和过滤原因（如适用）。
- [ ] 虚拟显示适配器、虚拟网卡、VPN/隧道、回环接口、软件枚举项、Hyper-V/VMware 等虚拟设备以及物理性未知的条目不进入任何硬件列表、摘要、驱动推荐输入或硬件日志摘要。
- [ ] 真实存在但禁用、无驱动或异常的物理设备继续显示，并用中文标注状态。
- [ ] CPU、显卡、主板、网卡等类别只有在物理性被确认时才生成记录；OEM/整机型号只作为摘要。
- [ ] 枚举失败、权限不足或字段冲突时 fail-closed 为“未识别”，不猜测为物理设备；固定驱动入口仍由事项 06 保留。
- [ ] 过滤逻辑集中在适配器/核心边界，页面和驱动目录不建立第二份硬件事实。

## References

`V011-G05`、`V011-HW-01` 至 `V011-HW-08`、ADR-0008、ADR-0009、ADR-0049、事项 13、事项 24

## Comments

- 2026-08-23（依赖图收口）：旧 effort 事项 `.scratch/windows-initial-setup-workbench/issues/13-hardware-overview.md` 与 `.scratch/windows-initial-setup-workbench/issues/24-winui3-design-system.md` 的 `Resolution: completed` 均已核实；这些记录仅作历史参考，不构成当前 0.1.1 阻塞。当前跨 effort 前置已机械收口为 `Blocked by: none`。
