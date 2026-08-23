# 侧边栏宽度、拖拽和持久化

Type: task
Status: ready-for-agent
Resolution: open
Blocked by: 03
Owner: issue-04
Claimed by: none
Consumers: 07
Verification: 偏好服务合同、216/248/360 DIP 边界、鼠标/触摸拖拽、键盘增减、重启持久化、写入失败回退和窄窗 compact 的 Windows x64 自动化/人工快速检查。
Evidence freshness: 绑定 NavigationView 模板、偏好服务、设置键、窗口布局和 Windows/SDK；任一宽度或持久化实现变化后重跑。

## Goal

将当前过长且固定的侧边栏改为默认紧凑、可自由调整并可靠保存的导航区域。

## Ownership Boundary

应用设置偏好服务是侧栏宽度的唯一可写所有者；NavigationView 只投影宽度和导航状态，拖拽手势只提交类型化偏好意图。业务核心、批次、目录和硬件状态不得读取或写入宽度。

## Acceptance Criteria

- [ ] 首次运行默认 `248` DIP，宽度限制为 `216` 至 `360` DIP，任何来源都不能写出边界。
- [ ] 展开状态存在明确拖拽把手；拖拽实时跟手、可中断、可反向，释放后稳定停在边界内。
- [ ] 键盘用户能发现并执行增加/减少宽度，焦点、导航选择和屏幕阅读器名称明确。
- [ ] 读写偏好成功后跨应用重启恢复；读取/写入失败回退 `248`，不覆盖其他设置、不让导航闪退。
- [ ] 窄窗口进入 compact 模式时保留当前页面、选择、焦点和返回路径；窗口恢复宽度后不重置业务状态。
- [ ] 减少动画/高对比度/触摸场景不依赖动画或 hover 才能完成导航。

## References

`V011-G04`、`V011-SIDEBAR-01` 至 `V011-SIDEBAR-07`、ADR-0050、事项 03、事项 18、`CONTEXT.md`

## Comments

- 2026-08-23 07:14:10 +08:00（最终候选证据）：侧栏源提交 `11dfde5b9acb5e62b72e743f16d2b3a503f89d06` 及根字典修复 `65e7e591c0a9df043d1ce284df88663bd7c5a772` 经普通合并 `80713109b1cd71be19689f0c4fbbc4775d413797` 进入候选链；当前远端头为 `d8a4367adc1e322351126e8ab3676d30584475d5`。受影响侧栏合同包含在定向 CTest 10/10 通过结果中，但未完成真实拖拽/键盘/重启 UI 验收。
- 未验证边界：持久化故障回退、窄窗 compact 状态保留、DPI、触摸、屏幕阅读器和 ARM64 均未完成，事项保持 `Resolution: open`。

- 2026-08-23（依赖图收口）：旧 effort 事项 `.scratch/windows-initial-setup-workbench/issues/18-application-settings.md` 的 `Resolution: completed` 已核实；该记录仅作历史参考，不构成当前 0.1.1 阻塞。当前 `Blocked by` 仅保留同 effort 的事项 03。
- 2026-08-23（最终代码验证基线）：侧栏代码验证基线为 `f2748c93a7da18607f4f35bd2d70896630f551f7`；`origin/0.1.1` 为 `744683fef78b6ac2ddaa8f80c585012e3dd20550`，旧 `d8a4367adc1e322351126e8ab3676d30584475d5` 为历史 merge-base。本记录随后通过普通文档合并进入 integration，事项仍保持 `Resolution: open`，`Blocked by: 03` 未变。
