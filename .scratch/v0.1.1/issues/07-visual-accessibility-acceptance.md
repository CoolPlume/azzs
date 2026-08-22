# 0.1.1 视觉、DPI、输入和无障碍验收

Type: task
Status: ready-for-human
Resolution: open
Blocked by: 01, 02, 03, 04, 05, 06
Owner: issue-07
Claimed by: none
Consumers: none
Verification: 在 Windows 11 x64 上执行宽/窄窗口、100%/高 DPI、键盘、触摸、Narrator/屏幕阅读器、减少动画/透明度、高对比度和快速重复导航检查；记录截图/日志、精确提交、环境和未执行边界。
Evidence freshness: 绑定最终 0.1.1 候选提交、Windows/SDK、显示器/DPI、输入方式和主题设置；任何页面头部、侧栏、资源或硬件投影变化后重跑。

## Goal

证明 0.1.1 的用户可见改动在真实 Windows 交互条件下可读、可达、无重叠且不会把静态合同误报为视觉通过。

## Ownership Boundary

本事项只拥有跨页面验收编排、夹具和证据记录；不重新实现页面业务或设计系统。发现缺陷时回到对应 owner 的事项修复，再基于新提交复验。

## Acceptance Criteria

- [ ] 概览、驱动、应用设置及其他导航页面在窄窗和宽窗中标题、命令、状态、列表和错误均不截断或重叠。
- [ ] 设置导航异常路径满足进程存活、焦点保留、中文播报和重试/返回合同。
- [ ] 侧栏 216/248/360 DIP、拖拽、键盘、触摸、compact 和重启持久化可观察。
- [ ] 虚拟/未知硬件在所有硬件表面消失，禁用物理设备仍有明确状态。
- [ ] 键盘 Tab/方向键、触摸命中区和屏幕阅读器名称/状态/错误顺序可完成主要流程。
- [ ] 减少动画、减少透明度和高对比度时功能与反馈不依赖视觉增强；不出现循环或阻塞动画。
- [ ] ARM64、安装生命周期或未具备的设备条件明确标记未执行，不得作为 x64 通过的替代证据。

## References

`V011-ACCEPT-01` 至 `V011-ACCEPT-08`、事项 01 至 06、事项 24、`docs/agents/windows-native-takeover.md`

## Comments
