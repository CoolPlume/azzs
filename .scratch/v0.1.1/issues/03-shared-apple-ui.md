# 重整共享按钮和页面头部

Type: task
Status: ready-for-agent
Resolution: open
Blocked by: 24
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

- 2026-08-23（集成证据核对）：共享页头和统一按钮/主题实现提交 `96c80e63e1059017f810d7fed2a460b6a39b1d17` 已通过普通合并 `da12eb5ea087cf0d9645148c30d0a7a706a837c7` 进入候选链，当前本地头为 `80713109b1cd71be19689f0c4fbbc4775d413797`。此前静态检查 `python tests/ui-design-contract/check_design_system.py .` 与 `python tests/ui-design-contract/check_winui_async_contract.py .` 均通过。
- 静态通过不等于视觉验收；仍需在当前候选头执行浅色/深色/高对比度/减少动画、宽窄窗口和键盘/触摸/无障碍快速反向，并记录 `apple-design`、`emil-design-eng` 审查证据。
- 未验证边界：真实 Windows x64 视觉/UIA、DPI、触摸、Narrator、ARM64、安装生命周期和性能/减少透明度实机行为均未验证。
