# 收紧驱动推荐与无物理设备入口语义

Type: task
Status: ready-for-agent
Resolution: open
Blocked by: 05
Owner: issue-06
Claimed by: none
Consumers: 07
Verification: 驱动页面和概览 presentation contract 覆盖有物理硬件、只有虚拟/未知、检测失败、禁用物理设备及固定入口；核对推荐不触发下载/安装且中文状态准确。
Evidence freshness: 绑定物理硬件契约、驱动推荐规则、页面资源和目录版本；任何推荐 substring、入口资格或驱动边界变化后重跑。

## Goal

让驱动推荐只消费物理硬件事实，同时在没有可确认硬件时提供诚实、可用的外部交接入口。

## Ownership Boundary

本事项拥有推荐资格和无物理设备状态投影；硬件事实由事项 05 提供，目录内容由驱动目录 owner 提供，外部启动仍由核心/Windows 适配器按 ADR-0009 执行。本事项不得下载、匹配或安装驱动包。

## Acceptance Criteria

- [ ] 推荐规则只读取确认物理硬件的类型化字段，不再对任意摘要字符串做 substring 猜测。
- [ ] 有物理 GPU/网卡等设备时只显示相应厂商入口，并说明入口会交接到官方页面/助手。
- [ ] 只有虚拟、软件、VPN、回环或未知设备，或观测失败时，不显示基于硬件的推荐入口；固定驱动获取入口仍保留并说明原因。
- [ ] 禁用但真实的物理设备仍能进入相应推荐，并显示“已禁用/状态异常”等中文事实，不伪报驱动已完成。
- [ ] OEM/整机摘要不会导致重复或虚假的厂商设备推荐。
- [ ] 任何入口都不直接下载或安装具体驱动包，驱动交接状态和用户确认语义保持不变。

## References

`V011-G05`、`V011-DRIVER-01` 至 `V011-DRIVER-06`、ADR-0009、ADR-0049、事项 05、事项 14

## Comments

- 2026-08-23（依赖图收口）：旧 effort 事项 `.scratch/windows-initial-setup-workbench/issues/14-driver-acquisition-page.md` 的 `Resolution: completed` 已核实；该记录仅作历史参考，不构成当前 0.1.1 阻塞。当前 `Blocked by` 仅保留同 effort 的事项 05。
