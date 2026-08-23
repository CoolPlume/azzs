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

- 2026-08-23 07:14:10 +08:00（最终候选证据）：驱动推荐源提交 `02c52d62b21d2797d87ea13a891878d63f8cfa8d` 经普通合并 `ec80d9ed68bf547481adb38c15d0fbfcd4407693` 进入候选链；当前远端头为 `d8a4367adc1e322351126e8ab3676d30584475d5`。定向驱动/物理硬件/presentation 合同包含在 CTest 10/10 通过结果中，入口仅交接官方页面/助手；未执行真实厂商页面、下载或安装流程。
- 未验证边界：真实 Windows 页面、禁用物理设备样本、ARM64、DPI、安装生命周期和网络交接均未完成，事项保持 `Resolution: open`。

- 2026-08-23（依赖图收口）：旧 effort 事项 `.scratch/windows-initial-setup-workbench/issues/14-driver-acquisition-page.md` 的 `Resolution: completed` 已核实；该记录仅作历史参考，不构成当前 0.1.1 阻塞。当前 `Blocked by` 仅保留同 effort 的事项 05。
- 2026-08-23 07:20:51 +08:00（最终候选证据）：驱动推荐/外部交接合同在 `d8a4367adc1e322351126e8ab3676d30584475d5` 上通过，并随 `7478fa364fea4fc2b4ae5336b1d92e3904b68f5b` 进入 `0.1.1`。真实厂商设备和官方页面交接、网络策略、ARM64 及 UI 无障碍仍未验证；实现不下载或安装驱动。
- 2026-08-23（最终代码验证基线）：驱动推荐代码验证基线为 `f2748c93a7da18607f4f35bd2d70896630f551f7`；`origin/0.1.1` 为 `744683fef78b6ac2ddaa8f80c585012e3dd20550`，旧 `d8a4367adc1e322351126e8ab3676d30584475d5` 为历史 merge-base。本记录随后通过普通文档合并进入 integration，事项仍保持 `Resolution: open`，`Blocked by: 05` 未变。
- 2026-08-23（当前远端事实）：远端 `codex/v1-integration`=`1224b865114ec47b2708fc859eae73d483d2ac6e`，远端 `0.1.1`=`744683fef78b6ac2ddaa8f80c585012e3dd20550`；merge-base=`d8a4367adc1e322351126e8ab3676d30584475d5`。`git rev-list --left-right --count origin/0.1.1...origin/codex/v1-integration` 为 `2/8`（2 behind、8 ahead）。`Resolution: open` 和 `Blocked by: 05` 均保持不变；真实厂商交接、禁用物理设备样本、ARM64、DPI 和安装生命周期仍未验证。
