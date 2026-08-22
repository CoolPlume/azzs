# 事项 24：WinUI 3 设计系统实现证据

## 证据边界

- 基线：`edaafae0add79022116967f3340a45ff4c5e9897`（`origin/codex/v1-integration`）。
- 分支：`codex/issue-24-winui3-design-system`。
- 事项 24 只拥有语义资源、只读呈现模型、可复用呈现控件、输入/无障碍与动效偏好；消费者继续拥有业务命令、状态机、持久化和完成条件。
- 固定夹具只验证呈现合同，不证明事项 06、10、13、14、15、16、18、26、32 的业务流程通过。
- 全任务仅执行一次 WinUI/消费者 Graphify 窄查询；结果确认共享呈现接缝归事项 24，消费者业务所有权不变。既有符号的 GitNexus impact 均为 LOW，无 HIGH/CRITICAL 暂停项。

## 源码入口

- `src/adapters/ui/winui/Themes/DesignSystem.xaml`：浅色、深色、高对比度实体主题资源，以及布局、间距、字体、Fluent 字形、圆角、状态、风险、命令、材质回退与 0/83/167/250ms 动效语义。
- `src/adapters/ui/winui/DesignSystem/Controls/ReadOnlyPresentationSurface.xaml*`：接收 `shared_ptr<const PresentationSnapshot>`，以 WinUI 原生控件投影状态；鼠标、键盘和触摸共用 `Button.Click`，只发出 `PresentationIntent`，不执行或保存业务状态。
- `src/adapters/ui/winui/DesignSystem/motion_preferences.*`：唯一读取和监听 `UISettings.AnimationsEnabled` 的界面层所有者；关闭时取消已登记视觉动效。
- `src/adapters/ui/winui/DesignSystem/Fixtures/DesignSystemFixturePage.xaml*`：不进入正式导航、不读写文件/注册表/网络、不下载、不安装、不修改系统的编译期固定 UI 夹具。
- `tests/ui-design-contract/`：可移植呈现合同测试与 Python 3.9+ XAML/资源/布局/无障碍静态门禁。

## 固定夹具

| 场景 | 固定模型 ID | 验证重点 |
| --- | --- | --- |
| 长中文与逐项选择 | `fixture.long-chinese` | 换行、可扫描、类型化选择意图 |
| 标准/高级同源 | `fixture.shared-view` | 同一快照、同一状态和主要命令；高级只增加低频投影 |
| 四阶段 | `fixture.stage.*` | 驱动、系统优化、软件安装、软件优化固定顺序，仅呈现 |
| 本机试用/恢复/草稿/交接 | `fixture.local-trial` 等 | 完整文字状态、礼貌播报且相同状态不重复播报 |
| 未知进度 | `fixture.unknown-progress` | 不虚构总量、百分比、速度或剩余时间 |
| 错误/等待/失败/待确认/撤回 | `fixture.inline-error` 等 | 严重度、LiveSetting、原位命令与结果定位 |
| 禁用原因 | `fixture.disabled-reason` | 可见正文及按钮 `HelpText`，禁用命令不发意图 |
| 高风险确认 | `fixture.risk-confirmation` | 安全命令默认聚焦；危险命令后置、非默认并带影响说明 |
| 设置/目录编辑器 | `fixture.settings-form`、`fixture.catalog-editor` | 窄窗改为单列；夹具无保存、应用或外部效果 |

## 自动检查

| 检查 | 结果 | 范围 |
| --- | --- | --- |
| `python3 tests/ui-design-contract/check_design_system.py .` | Pass | 资源唯一性/类型、主题实体回退、4.5:1 对比度、圆角、时长/曲线、即时路径、单一动效所有者、Completed 边界、布局动画/scale=0 禁令、页面资源消费、响应式 VSM、AutomationId、LiveSetting、焦点、类型化意图、fixture 无外部效果、XAML 项目元数据与 PCH 边界 |
| `/usr/bin/python3 tests/ui-design-contract/check_design_system.py .` | Pass | macOS 自带 Python 3.9 兼容路径 |
| `xmllint --noout`（全部 WinUI XAML、resw、vcxproj） | Pass | XML 结构 |
| `cmake --preset host-debug` + build + `ctest --preset host-debug` | Pass，3/3 | `core.smoke`、`ui.presentation.contract`、`ui.design.contract` |
| `cmake --preset host-release` + build + `ctest --preset host-release` | Pass，3/3 | Release 可移植核心与两项设计合同 |
| `git diff --check` | Pass | 空白与补丁格式 |
| GitNexus staged `detect-changes` | 完成：34 个文件、266 个符号、18 条受影响流程；Risk `critical` | 全应用呈现系统的预期跨层范围已人工核对，未发现消费者业务、系统修改或持久化越界 |

Draft PR 的 `Windows read-only validation` 才能证明锁定 Windows 工具链下的 x64 Release 构建+CTest 和 ARM64 编译链接；它仍不等于真实设备的人机、图形或性能验收。

## Emil 设计工程审查

仓库设计文档优先于技能示例；首版不新增装饰动效或自定义 Storyboard/Composition。

| Before | After | Why |
| --- | --- | --- |
| 七页复制 `Padding="32,24"`，窄宽策略不统一 | 页面壳统一消费语义资源和 Narrow/Wide VSM | 高频维护点只有一个资源所有者，长文本与文本缩放具有一致回退 |
| fixture 在 XAML 手写状态、命令和无障碍属性 | 可复用控件直接投影不可变模型，并通过原生 `Button.Click` 发类型化意图 | 消除第二套真值，鼠标/键盘/触摸共用平台输入路径 |
| 标准/高级固定双列 | 同一模型分别投影，窄窗堆叠、宽窗双列 | 保留同源状态，同时避免窄窗和高文本缩放挤压 |
| 动效规则只靠页面自律 | 唯一 0/83/167/250ms 语义、快速响应末端减速平台 easing、集中监听系统动画开关 | 高频路径即时，系统关闭后可取消视觉增强，页面不能复制参数 |
| 状态每次重投影都可能再次播报 | 控件在更新前关闭旧 live region，并只对新状态键恢复 Polite/Assertive | 屏幕阅读器获得必要反馈，不被相同状态重复打扰 |
| 高风险与普通命令只靠视觉顺序 | 安全命令成为确认面的默认焦点；危险命令后置、非默认并带 `HelpText` | 可撤销路径优先，键盘与屏幕阅读器均能理解实际影响 |
| 响应式检查只确认存在 `AdaptiveTrigger` | 门禁要求共享视图和设置字段在 Narrow 状态真实换行/换列 | 避免形式上“有 VSM”却仍固定双列 |

结论：源码级 Emil 审查通过。高频操作没有空间/缩放/弹簧动效；偶发视觉增强只能使用集中语义并可取消；无动画路径保留焦点、文字、状态和类型化命令。性能与快速反向的真实 Windows 结论仍需实机证据。

## 未实机验证

当前会话不是 Windows，未跨机往返。以下均保持“未实机验证”：Windows 10 22H2 运行、4K/225% 显示缩放、Windows 文本缩放、混合 DPI/多显示器热插拔、鼠标/键盘/触摸组合、Narrator 或其他屏幕阅读器、高对比度实际主题、运行时开关减少动画/减少透明度、集成/独立/混合图形与材质回退、快速反向、非空白/无重叠视觉检查，以及 WPR/WPA 性能采样。
