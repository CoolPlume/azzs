# 统一用户可见文案为简体中文

Type: task
Status: ready-for-agent
Resolution: open
Blocked by: none
Owner: issue-02
Claimed by: none
Consumers: 03, 07
Verification: 资源扫描、概览及共享 presentation contract、Windows x64 页面快速反向；逐项记录允许保留的品牌、型号、版本和原始系统/WMI 错误原文。
Evidence freshness: 绑定资源文件、presentation contract、页面文本生成逻辑和构建目录；任一用户可见文案或资源回退变化后重跑。

## Goal

消除概览及其他页面中孤立、未解释的英文用户文案，让普通用户和无障碍用户能完成主要流程。

## Ownership Boundary

本事项拥有资源键、presentation text contract 和共享空/错/加载文案的翻译；页面业务状态和品牌/硬件原始事实由各自 owner 提供。本事项不得改写核心状态或把开发者日志字段作为用户文案。

## Acceptance Criteria

- [ ] 概览标题、阶段摘要、按钮、状态、空状态、错误、确认、工具提示和无障碍名称全部有简体中文资源。
- [ ] 共享运行时创建的按钮/页头也使用资源合同，不直接写入英文常量。
- [ ] 全局用户可见页面完成扫描；孤立英文只允许来自品牌名、型号、版本号、原始系统/WMI 错误或外部厂商名称，并有中文上下文。
- [ ] 资源缺失时使用明确中文回退，不把资源键显示给用户。
- [ ] 文案在窄窗口、较大文本缩放和屏幕阅读器中不截断、不重叠、顺序可理解。

## References

`V011-G02`、`V011-COPY-01` 至 `V011-COPY-05`、事项 03、事项 24、`CONTEXT.md`

## Comments

- 2026-08-23（集成证据核对）：共享页头/资源接缝来自 `96c80e63e1059017f810d7fed2a460b6a39b1d17`，已由普通合并 `da12eb5ea087cf0d9645148c30d0a7a706a837c7` 集成；驱动状态文案来自 `02c52d62b21d2797d87ea13a891878d63f8cfa8d`，经 `ec80d9ed68bf547481adb38c15d0fbfcd4407693` 集成。当前本地候选头为 `80713109b1cd71be19689f0c4fbbc4775d413797`，资源仍有后续中文化工作，事项保持 open。
- 当前审查发现概览 guided-initialization 动态 presentation 仍有英文默认标题、阶段和状态文本，需在 UI 适配器通过资源构造 `GuidedInitializationPresentationText` 后再结票。`draft` 作为协议/测试字段可保留，用户可见的“内置 draft 目录”须使用简体中文上下文。
- 未验证边界：动态概览中文化后的资源回退、完整全局可见文案扫描、窄窗/大文本/屏幕阅读器呈现、ARM64、DPI 和真实 Windows UI 均未验证；品牌、型号、版本及必要原始系统/WMI 错误才可保留原文。
