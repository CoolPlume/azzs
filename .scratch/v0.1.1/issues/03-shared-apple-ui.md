# 重整共享按钮和页面头部

Type: task
Status: ready-for-agent
Resolution: open
Blocked by: none
Owner: issue-03
Claimed by: none
Consumers: 02, 04, 07
Verification: 设计系统和 presentation contract、浅色/深色/高对比度/减少动画夹具、Windows x64 宽窄窗口快速反向；使用 `apple-design` 与 `emil-design-eng` 的 Before/After/Why 审查记录。
Evidence freshness: 绑定主题、控件模板、页头/按钮组件和 WinUI/SDK 版本；共享资源或平台主题变化后重验。

## Goal

让概览和其他页面使用一致、克制、原生 WinUI 3 的 Apple 风格适配，修复复古按钮观感和难看的大标题位置。

## Ownership Boundary

本事项拥有共享样式、页头布局、语义按钮、图标/工具提示、焦点和动效资源；消费者页面拥有命令、状态和业务结果。本事项不复制页面业务判断，不引入新 UI 框架。

## Acceptance Criteria

- [ ] 一级页面头部统一对齐标题、状态摘要和主要命令，标题尺寸与容器匹配，内容区在宽窄窗口均可见且无重叠/截断。
- [ ] 普通、主要、危险和次要命令有清晰层级；熟悉工具命令优先使用 WinUI 图标按钮并提供工具提示，危险命令保留明确文字和确认。
- [ ] 所有页面接入共享样式，不再各自复制颜色、圆角、间距、图标、状态反馈或动效时长。
- [ ] 保留 NavigationView、键盘、触摸和屏幕阅读器的原生语义，不仿制 macOS 窗口。
- [ ] 遵守仓库 0/83/167/250ms 语义动效、系统减少动画/透明度回退、字距为 `0` 和字号不随窗口宽度缩放；动效完成事件不提交业务状态。
- [ ] 页面按钮在按下时即时反馈，任何可中断交互从当前呈现值继续；无循环、庆祝或阻塞性动画。

## References

`V011-G03`、`V011-UI-01` 至 `V011-UI-08`、ADR-0021、事项 24、`docs/design/winui3-apple-inspired.md`、`apple-design`、`emil-design-eng`

## Comments

- 2026-08-23 07:14:10 +08:00（最终候选证据）：共享 UI 源提交 `96c80e63e1059017f810d7fed2a460b6a39b1d17` 经普通合并 `da12eb5ea087cf0d9645148c30d0a7a706a837c7` 进入当前链，当前远端候选头为 `d8a4367adc1e322351126e8ab3676d30584475d5`。`check_design_system.py` 与 `check_winui_async_contract.py` 均通过；已按 `apple-design` 与 `emil-design-eng` 记录静态动效/可访问性审查，未把静态结果写成视觉验收。
- 未验证边界：真实 WinUI 3 宽/窄窗口、DPI、高对比度、减少动画/透明度、触摸和屏幕阅读器视觉/交互仍未完成，事项保持 `Resolution: open`。
- 2026-08-23（最终代码验证基线）：共享 UI 代码验证基线为 `f2748c93a7da18607f4f35bd2d70896630f551f7`；`d8a4367adc1e322351126e8ab3676d30584475d5` 为历史 merge-base。本记录随后通过普通文档合并进入 integration，事项仍保持 `Resolution: open`。

- 2026-08-23（依赖图收口）：旧 effort 事项 `.scratch/windows-initial-setup-workbench/issues/24-winui3-design-system.md` 的 `Resolution: completed` 已核实；该记录仅作历史参考，不构成当前 0.1.1 阻塞。当前跨 effort 前置已机械收口为 `Blocked by: none`。
