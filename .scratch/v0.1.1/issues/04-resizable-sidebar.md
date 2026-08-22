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

- 2026-08-23（依赖图收口）：旧 effort 事项 `.scratch/windows-initial-setup-workbench/issues/18-application-settings.md` 的 `Resolution: completed` 已核实；该记录仅作历史参考，不构成当前 0.1.1 阻塞。当前 `Blocked by` 仅保留同 effort 的事项 03。
