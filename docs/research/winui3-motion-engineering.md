# WinUI 3 动效工程研究：将 Emil Kowalski 原则映射到 Windows 原生能力

- 状态：研究结论
- 研究日期：2026-08-09
- 适用范围：初装工作台的 WinUI 3 界面层
- 来源边界：仅采用微软官方的 WinUI 3、Windows App SDK、Windows UI Composition、输入、无障碍与性能文档

## 1. 结论摘要

本项目可以在 WinUI 3 中落实 Emil Kowalski 强调的“有目的、快速、可被新输入打断、尊重用户、性能稳定”的动效原则，但不能照搬 Web/CSS 实现方式。正确映射是：

1. 高频操作和键盘触发的操作不播放空间位移动画；状态立即更新。
2. 语义与 WinUI 原生控件一致时，优先保留控件自带的主题动画和视觉状态，不重新发明一套。
3. 自定义动效优先使用 Composition 能独立执行的 `Opacity`、`Translation/Offset`、`Scale`、`Rotation`、裁剪等视觉属性，避免持续改变布局尺寸。
4. 动画永远不是业务状态机。业务状态先确定，动画只负责呈现；动画完成事件不得成为安装、优化、下载或导航正确性的前提。
5. 可快速反向操作的界面只允许“接受新目标并立即改向”的动效，不允许把输入排队到上一段动效结束。
6. `ThemeTransition` 用于语义匹配、低频且由平台已有定义的变化；`ConnectedAnimation` 仅用于两个界面确实共享同一视觉对象、且连续性有助理解的偶发导航。
7. 必须读取并监听 `UISettings.AnimationsEnabled`。系统关闭动画时，应用立即关闭非必要动画，不能用应用内开关重新强行开启。
8. 悬停只属于鼠标或具备悬停能力的笔；触摸没有悬停。任何主要功能、文字或风险信息都不能只靠悬停发现。
9. 视觉质量不能凭主观感受验收。进行实机性能验证时，应在 Release 构建中使用 WPR/WPA 的 XAML Frame Analysis 检查慢帧，并在真实输入方式、窗口尺寸和后台高负载场景下验证；缺少这些证据时只能标为“未实机验证”，但不阻止已经满足实现与自动检查门槛的既定发布。

微软把 Windows 动效描述为直接、响应迅速、符合上下文，并建议尽量使用现有 WinUI 控件获得一致性；这与本项目所采用的 Emil 原则方向一致。[M1]

## 2. 原则到 WinUI 3 的映射

| Emil 风格原则 | WinUI 3 对应能力 | 本项目约束 |
| --- | --- | --- |
| 先判断是否需要动画 | WinUI 控件默认视觉状态、`SuppressNavigationTransitionInfo` | 高频、键盘触发、连续勾选、进度数值刷新不播放空间动画 |
| 每段动画必须有目的 | Theme animation、Theme transition、Connected animation | 只允许解释反馈、层级、空间关系或结果；纯装饰不进入首版 |
| 快速且直接 | WinUI 标准时长资源 83/167/250ms | 自定义普通 UI 动效不超过 250ms；例外必须有语义与性能依据 |
| 进入快速响应，退出更快 | Windows 的 direct entrance、direct/gentle exit 曲线 | 进入 167–250ms；退出通常 83–167ms；退出不阻塞新界面交互 |
| 动态交互可以改向 | Composition `StartAnimation`/`StopAnimation`、隐式动画、NaturalMotionAnimation | 新输入直接替换目标；不等待、不排队、不依赖 Completed |
| 弹簧只用于物理感交互 | Composition spring 与 `InteractionTracker` | 默认接近临界阻尼且无可见回弹；只有直接拖动或用户带入速度时才允许轻微回弹 |
| 优先 transform/opacity | Composition Visual / UIElement 的视觉层属性 | 禁止把 `Width`、`Height`、`Margin`、`Padding` 等布局属性作为普通动效路径 |
| 尊重减少动画 | `UISettings.AnimationsEnabled` 与变化事件 | 系统关闭后立即禁用非必要动效，使用即时状态与静态反馈替代 |
| 触摸不伪造 hover | WinUI pointer events、`PointerDeviceType` | 悬停增强只面向实际支持悬停的输入；触摸和键盘都有完整等价入口 |
| 用测量而不是感觉判断性能 | WPR、WPA、XAML Frame Analysis | 进行实机性能验证时，关键交互必须记录帧耗时与慢帧并在 Release 构建中重复测量 |

WinUI 3 提供 83ms、167ms 和 250ms 三个标准控制动画时长资源；Windows 动效指南还给出了按进入、退出和现有元素移动划分的曲线与时长。项目应复用这些资源，而不是在页面内散落任意数字。[M1][M2]

## 3. 动画决策顺序

每个新增动效必须按以下顺序审查：

1. **是否需要动画**：没有信息价值就即时切换。
2. **由谁触发**：键盘或高频导航默认无动画；指针按下只保留极短反馈；偶发浮层或结果可以有动画。
3. **表达什么**：只能表达输入已被接收、空间层级、同一对象的连续性、内容变化或完成结果。
4. **是否有平台现成语义**：能由控件模板、主题动画或主题转场表达时，不自定义。
5. **是否可被重新触发**：若用户能快速反向操作，必须能立即改向或即时结束。
6. **是否尊重系统设置**：动画关闭、能力不足或性能降级时必须有无动画路径。
7. **是否通过性能测量**：没有 Release 构建的帧分析证据，不能把复杂自定义动效的实机性能视为验证通过；缺少证据时按项目规则标为“未实机验证”，不改变实现与自动检查门槛。

微软官方的 XAML 动画库本身以“提示但不打扰”和帮助用户理解状态变化为目标，并建议控件已有行为优先于自定义 Storyboard。[M3][M6]

## 4. 项目动效频率分级

### 4.1 必须即时完成

以下场景不播放位移、缩放、弹簧或页面转场：

- 键盘快捷键触发的任何操作。
- 键盘焦点在列表、导航项和复选项之间移动。
- 主侧边栏的一级页面切换。
- 连续勾选或取消软件、设置、驱动和优化项。
- 下载速度、剩余时间、已下载量、日志行和批次计数的刷新。
- 标准视图与高级视图之间频繁展开的细节值更新。

这些变化仍必须有即时、静态的选中态、焦点态、文本或图标反馈。无动画不等于无反馈。

### 4.2 只允许极短反馈

以下场景可使用 83ms 左右的颜色、透明度或控件原生 pressed 状态：

- 鼠标或触摸按下按钮。
- 鼠标进入可交互表面。
- 小型状态图标从待处理切换为处理中。
- 同一区域内容刷新时的最小交叉淡化。

标准 WinUI 控件已有按下、悬停和焦点反馈时，应保留原生模板行为。只有自定义可点击表面缺少足够反馈时，才增加轻微缩放；不为普通按钮叠加第二套按下动画。[M3]

### 4.3 可以使用标准动画

以下低频场景可以使用 167–250ms 的平台转场或 Composition 动效：

- 偶发的对话框、Flyout、教学提示和错误详情。
- 从汇总进入某一项目的详情层级。
- 展开影响较大的风险说明。
- 有效动效偏好允许时，一个批次完成后的单次勾选与淡入反馈。

### 4.4 首版禁止

- 彩屑、烟花、循环漂浮、装饰性视差和背景持续运动。
- 菜单、列表项、普通按钮的弹跳。
- 为营造“高级感”而使用的长时间模糊、景深或大范围缩放。
- 业务列表的逐项 stagger 入场；多个状态容器一起更新时同步完成。
- 任何阻塞输入或延迟真实结果呈现的庆祝动画。

## 5. 时间、曲线与统一资源

界面层应只有一套集中维护的动效资源，不允许页面自行定义相近但不同的时长和曲线。

| 语义 | 建议时长 | 推荐方式 |
| --- | ---: | --- |
| 即时状态、键盘操作 | 0ms | 直接切换 |
| 按下/悬停微反馈 | 83ms | 平台控件状态或透明度/颜色 |
| 退出、小提示消失 | 83–167ms | 快速淡出或平台退出动画 |
| 小型状态切换 | 167ms | 平台资源或 Composition |
| 偶发浮层、详情进入 | 167–250ms | Theme animation/transition |
| 完成勾选与淡入 | 167–250ms | 仅在有效动效偏好允许时一次性播放、不阻塞 |

工程规则：

- 自定义普通 UI 动效最大 250ms；不得用 333ms 或更长时长作为默认。
- 进入和现有元素移动使用快速响应、末端减速的曲线。
- 退出更短，避免已失效内容继续占据注意力。
- 常量运动（仅当确有必要，例如不确定进度）才使用线性速度。
- 不在页面中复制 cubic-bezier 控制点；统一封装为界面层资源。
- 不用 Web 的 `ease-in/ease-out` 名称直接推断 WinUI 行为，应采用微软给出的 Windows 曲线或平台资源。

微软官方标准资源是 `ControlFasterAnimationDuration` 83ms、`ControlFastAnimationDuration` 167ms、`ControlNormalAnimationDuration` 250ms。[M2]

## 6. 属性与实现层级

### 6.1 选择顺序

1. **原生控件与默认模板**：按下、悬停、焦点、列表增删和标准弹出层首先使用 WinUI 自带行为。
2. **Theme animation / Theme transition**：语义与平台预设一致时使用。
3. **Composition animation**：只有平台预设无法表达，且确实需要可改向、输入驱动或高级视觉关系时使用。
4. **Storyboard**：只用于控件视觉状态或确有依赖属性动画需求的局部场景；不得成为常规页面动效工具。
5. **依赖动画**：默认禁止。若不得不启用 `EnableDependentAnimation`，必须记录不可替代原因、性能测量和无动画回退。

微软指出 Theme animation/transition 比自定义 Storyboard 更一致、更易使用；会占用 UI 线程的 dependent animation 默认不运行，需要开发者显式承担风险。[M6]

### 6.2 优先属性

自定义动效优先限制在以下视觉层属性：

- `Opacity`
- `Translation` 或受控的 `Offset`
- `Scale`
- `Rotation`
- 视觉层可动画的裁剪属性
- 必要且通过能力检测的 Composition effect 参数

不应把以下属性作为普通动画目标：

- `Width`、`Height`
- `Margin`、`Padding`
- 会让父子布局每帧重新计算的网格尺寸或内容尺寸
- 每帧创建、移除大量 XAML 元素的路径
- 无性能证据的 Blur、阴影半径或复杂 effect graph

WinUI 的独立动画可以由合成线程处理；布局相关动画可能成为依赖动画并争用 UI 线程。Composition 动画则面向独立线程和视觉层属性设计。[M6][M7][M15]

### 6.3 XAML 与 Composition 的边界

- XAML 继续拥有可访问结构、布局、焦点、命中测试和控件语义。
- Composition 只增强现有 XAML 元素的呈现，不创建另一套业务界面树。
- 不为了动效把主要文本、按钮或状态移入缺少 XAML 无障碍保证的独立 Visual 岛。
- 使用 handout Visual 时要注意它与 XAML 属性不是双向同步；布局仍以 XAML 为准。
- 采用 `UIElement.StartAnimation` 还是 `ElementCompositionPreview`，由项目锁定的最新稳定 Windows App SDK API 决定；不得仅为动效依赖预览版 SDK。

微软明确说明 WinUI 3 XAML 元素由 Composition 视觉层支持，且 Composition 可用于 spring、expression 等高级动画；同时提醒纯 Visual 内容不具备与 XAML 内容相同的无障碍和体验保证。[M8][M19]

## 7. 可打断、反向与重定向

### 7.1 强制行为

- 用户的新输入始终优先于正在播放的动画。
- 新目标到来时，从当前可见状态向新目标过渡；不得先跳回旧起点。
- 同一元素的同一属性只能有一个动效所有者，避免 Theme、Storyboard 与 Composition 同时争用。
- 快速开关面板、反复进入退出、快速悬停切换时，不创建动画队列。
- 如果当前实现不能平滑改向，优先立即结束并呈现新状态，而不是强行播放不连续动画。
- 动画结束回调只清理临时视觉资源，不能提交业务状态、开始系统操作或决定操作成功。

### 7.2 推荐机制

- 简单状态变化优先使用 Composition 隐式动画，让当前值和最终值形成统一规则。
- 可由用户反向操作的视觉属性使用可替换的 Composition 动画；新输入对同一目标属性调用新动画或停止旧动画。
- 直接拖动、甩动等有速度输入的交互，才使用 `InteractionTracker`、expression animation 或 spring 的 `InitialVelocity`。
- 需要暂停、继续或定位播放进度的非交互动画，可评估 Composition 动画控制器；它不是业务任务暂停机制。

微软官方 Composition API 提供 `StartAnimation`、`StopAnimation` 和播放控制能力；NaturalMotionAnimation 可以声明起点、终点和初始速度，并与输入关联。[M7][M9][M10]

### 7.3 关于“弹簧保持速度”的证据边界

微软文档证明 spring 可使用当前值、终点和初始速度，也展示了改变终点后再次启动的模式；但本文所查官方资料没有承诺任意替换动画时都会自动保留上一段动画的瞬时速度。因此，开发阶段必须实测反向连续性，不能只凭“spring 天然可打断”作出验收结论。[M10]

本项目默认使用接近临界阻尼的 spring（`DampingRatio` 接近 1），普通菜单、列表、对话框和按钮不允许可见过冲。只有直接操控且用户输入带有速度时，才可以在独立评审后使用轻微回弹。微软把 `DampingRatio = 1` 定义为无振荡的临界阻尼。[M10]

## 8. ThemeTransition 与 ThemeAnimation 使用边界

### 8.1 应使用

- 标准 Flyout、Popup、对话框、提示和控件模板已有的状态变化。
- 低频的内容替换、列表增删和重新排列，且动画确实帮助用户理解变化。
- 偶发的详情层级进入与返回，使用与导航语义一致的页面转场。
- 自定义控件的视觉状态，优先通过样式或模板集中定义。

### 8.2 不应使用

- 一级侧边栏的日常导航。
- 键盘连续切换页面或列表项。
- 下载进度、速度、剩余时间和日志持续更新。
- 同一位置只是刷新数值或文本。
- 一个界面已经使用 ConnectedAnimation 时，再叠加默认页面动画。
- 为每个元素实例临时配置不同的 `UIElement.Transitions`。

微软说明 `UIElement.Transitions` 通常应由样式、模板或视觉状态集中设置，而不是在每个直接界面元素上逐个定义。[M18]

`Frame` 默认会使用 `NavigationThemeTransition`。本项目一级导航属于高频操作，应显式使用 `SuppressNavigationTransitionInfo`；偶发的详情 drill-down 才可使用 `DrillInNavigationTransitionInfo`。[M4]

## 9. ConnectedAnimation 使用边界

只在同时满足下列条件时使用：

1. 来源界面与目标界面存在用户能认出的同一对象，例如软件图标或硬件卡片。
2. 该对象的连续移动能明显解释“从列表进入详情”的关系。
3. 导航不是高频键盘工作流。
4. 目标元素能够及时创建，不会为了动画延迟内容或输入。
5. 系统动画开关允许，且性能测量通过。
6. 同一导航已经关闭默认页面转场，不叠加两套空间动画。

下列场景禁止使用：

- 一级标签页、侧边栏、设置分组之间的普通切换。
- 下载/安装/优化批次的状态更新。
- 目标对象不存在、只是视觉相似，或不同对象之间的跳转。
- 目的只是“看起来高级”。
- 减少动画模式、后台高负载降级模式或目标未及时就绪。

微软把 ConnectedAnimation 定义为共享元素在两个视图之间保持上下文的方式，并要求在使用它时关闭冲突的默认页面动画。准备和启动之间不宜超过约 250ms；三秒未启动会被系统丢弃。[M5]

工程上，`TryStart` 失败必须安静降级为已完成导航的静态目标界面，不能回退页面、重试动画或把导航判为失败。

## 10. 系统减少动画与能力降级

### 10.1 唯一有效判定

界面层应建立一个长生命周期的“有效动效偏好”：

- 系统依据：`Windows.UI.ViewManagement.UISettings.AnimationsEnabled`。
- 动态变化：监听 `AnimationsEnabledChanged`，用户修改 Windows 设置后立即生效。
- 应用自身如果以后提供“减少动画”选项，只能进一步减少，不能覆盖系统的关闭决定。
- 有效结果等价于“系统允许，并且应用没有要求进一步减少”。

项目最低版本 Windows 10 22H2 已高于 `AnimationsEnabledChanged` 所需的 Windows 10 2004，因此不需要为缺少该事件设计旧系统分支。[M12]

微软明确要求 WinUI/Composition 应用监听并响应 `UISettings.AnimationsEnabled`，以服从 Windows 设置中的动画偏好。[M11][M12]

### 10.2 系统关闭动画时

- 页面、面板、列表、ConnectedAnimation、spring、视差、位移、缩放和旋转全部即时完成。
- 不使用“短一点的位移动画”冒充关闭动画。
- 仍保留静态焦点、选中态、按下态、状态图标、文本和可访问通知。
- 完成反馈直接显示最终勾选和结果文字，不播放完成动画。
- 不确定进度不能只依赖旋转；同时提供“正在下载/正在安装/正在优化”等文字状态。
- 如果控件自带动画无法由项目可靠控制，应验证平台是否自动服从系统设置；不能未经验证就宣称已支持减少动画。

### 10.3 硬件能力与高负载降级

- 自定义 Acrylic、Blur、阴影或 effect graph 使用前检查 `CompositionCapabilities`。
- 能力不足或效果不够快时，回退为实体背景、清楚边界和无 effect 动效。
- 基础功能、焦点、状态和信息层级不得依赖高级效果。
- 后台正在解析目录、下载、安装或检测硬件时，复杂效果可降级，但不能降低反馈完整性。

微软建议通过 `CompositionCapabilities` 检查效果支持和性能，并为硬件差异设计渐进回退。[M11]

## 11. 鼠标、触摸、笔与键盘

### 11.1 悬停

- 只在实际收到支持悬停的鼠标或笔输入时显示 hover 增强。
- 触摸没有 hover 状态，不因 `PointerEntered` 名称而把触摸接触误判为悬停。
- hover 只增强边框、背景、图标或辅助说明；主要操作、风险、状态和文本始终可见或可通过点击/焦点访问。
- 用户开始触摸后，鼠标专属 UI 应淡出或立即移除。
- hover 频率高，只使用平台状态或 83ms 的颜色/透明度，不使用位移、放大和弹簧跟随。

微软把触摸定义为没有 hover 的二态输入，而鼠标与笔可提供 hover；鼠标专属反馈应在触摸开始后移除。[M13][M14]

### 11.2 按下与焦点

- 指针按下使用 WinUI 控件自带 pressed 状态；自定义表面才补充极短反馈。
- 键盘触发的命令不播放缩放或页面动画，但保留明确的焦点和按下状态。
- 焦点矩形服务键盘，不因鼠标 hover 动效而隐藏键盘焦点。
- 动画不能改变命中目标大小；触摸目标尺寸由布局确定，不用 Scale 动画补偿。

### 11.3 直接操控

本项目首版没有需要复杂手势完成的主要任务。未来若增加拖动排序或可调整面板，应使用 WinUI manipulation/input-driven animation，并同时提供按钮或键盘等价操作。输入驱动动画可在独立线程执行，但使用现有 XAML 控件的部分仍可能受 UI 线程影响，必须测量。[M14][M20]

## 12. 具体界面场景规则

| 场景 | 动效规则 |
| --- | --- |
| 一级侧边栏导航 | 立即切换；关闭 `Frame` 默认页面动画 |
| 列表进入软件/硬件详情 | 默认 drill；只有明确共享对象时才用 ConnectedAnimation |
| 标准/高级视图切换 | 数据与选择立即保持；仅低频新增区块可 167ms 淡入，不移动整页 |
| 勾选软件或优化项 | 选中态即时；不播放列表重排或缩放 |
| 展开风险说明 | 167ms 以内的淡入/裁剪；反向操作立即收起 |
| 下载进度 | 进度值直接更新；不为每次数值刷新补间；未知进度使用平台状态并配文字 |
| 日志追加 | 不逐行入场，不自动闪烁；只维持滚动与可读性 |
| Flyout/对话框 | 使用平台控件默认 Theme animation，不叠加自定义弹簧 |
| 错误出现 | 就地、快速淡入；严重性由图标和文字表达，不靠抖动 |
| 流程完成 | 有效动效偏好允许时单次勾选与 167–250ms 淡入；系统关闭动画时直接显示最终状态；均无彩屑、弹跳、音效或循环 |
| 反复打开/关闭面板 | 同一视觉属性可立即改向；不等待上一段结束 |

## 13. 动效架构与可维护性

为保持低耦合和高内聚，动效属于 WinUI 3 前端呈现层，不进入 C++ 核心领域模型。

- 核心只发布业务状态、进度和可执行命令，不知道动画类型、时长或曲线。
- 界面状态适配器把领域状态映射到视觉状态，动效只发生在视觉状态之间。
- 动效时长、曲线、减少动画、硬件降级和输入方式由单一界面服务统一决定。
- 页面不得各自监听系统动画设置；由集中服务订阅一次并发布有效偏好。
- 每个控件或视觉属性只能有一个动画所有者。
- 测试可把有效动效偏好固定为开或关，验证两条路径得到相同业务结果。
- 更换页面布局、切换标准/高级视图或未来迁移 macOS 时，核心业务接口不因动效变化而改变。

## 14. 性能门槛与验证方法

### 14.1 可测目标

- 用户输入后的首次视觉响应理想值不超过 100ms，最大不超过 200ms。
- 60Hz 设备上的动效目标为连续 60 FPS；每个 XAML frame 应尽量在一个显示刷新周期内完成。
- 不出现可重复的停顿、先停后跳、输入丢失或动画完成后才处理点击。
- 在后台下载、解析目录、写日志和模拟安装状态更新时，导航、滚动、Flyout 与取消操作仍保持响应。
- 关闭系统动画时，所有流程结果与可操作性和开启时一致。

微软建议把首次快速响应设为理想 100ms、最大 200ms，并把连续 60 FPS、无卡顿作为流畅性目标。[M16]

### 14.2 必测场景

1. 鼠标连续切换一级导航。
2. 键盘连续切换导航、列表与复选项。
3. 触摸点击、滚动和打开浮层。
4. 快速反复打开/关闭同一详情或面板。
5. 下载进度、速度、剩余时间与日志同时高频更新。
6. 标准视图和高级视图在大量项目下切换。
7. 窗口快速缩放、窄窗口、4K、225% 显示缩放、常用文本缩放及混合 DPI 多显示器迁移。
8. 后台 CPU/磁盘/网络负载存在时播放允许的偶发动效。
9. Windows 动画开关在应用运行期间从开切到关、再从关切到开。
10. Composition effect 能力不足或被禁用时的实体回退。

### 14.3 工具与证据

- 使用 Release 构建重复运行同一场景，避免只测 Debug。
- 使用 Windows Performance Recorder 记录 CPU Usage 与 XAML Activity。
- 使用 Windows Performance Analyzer 的 XAML Frame Analysis 查看 Interesting Xaml Frames 和 All Xaml Info。
- 按 `Duration` 排序定位最慢的 frame、导航、Flyout 与布局操作。
- 记录测试设备、Windows 版本、Windows App SDK 版本、分辨率、刷新率、文本缩放、输入方式和动画设置。
- “实机性能验证通过”必须附 WPR/WPA 证据；缺少证据时在开发者发布记录中标为“未实机验证”，肉眼顺滑不能代替数据，也不因此阻止已经满足实现与自动检查门槛的既定发布。

微软的 WinUI 3 性能文档说明，慢 frame 会同时延迟画面与输入；WPR/WPA 的 XAML Frame Analysis 可直接定位导航、Flyout、布局和其他慢帧。[M17]

## 15. 动效实现与验证清单

- [ ] 每段自定义动画都有“为何必须动画”的一句话说明。
- [ ] 高频与键盘路径没有位移、缩放、页面或弹簧动画。
- [ ] 一级导航显式抑制默认页面转场。
- [ ] Theme animation/transition 没有与 Composition 或 ConnectedAnimation 争用同一属性。
- [ ] ConnectedAnimation 只用于共享元素，并具备 `TryStart` 失败的静态降级。
- [ ] 所有可反向操作都能立即接受新输入，不排队。
- [ ] 动画完成事件不承担业务状态提交。
- [ ] 自定义普通动效不超过 250ms。
- [ ] 默认 spring 无可见过冲；轻微回弹只存在于直接操控。
- [ ] 没有 Width/Height/Margin/Padding 的持续动画，或例外已附性能证据。
- [ ] `UISettings.AnimationsEnabled` 初始值和运行时变化均生效。
- [ ] 系统关闭动画后，页面、面板、ConnectedAnimation 和 spring 都即时完成。
- [ ] 触摸无需 hover 即可发现并完成全部主要操作。
- [ ] 鼠标、触摸、笔与键盘反馈均可辨，且焦点没有被动效遮盖。
- [ ] 若本次进行实机性能验证，Release 构建已完成关键场景 WPR/WPA XAML Frame Analysis；否则开发者发布证据明确标为“未实机验证”，且未把自动检查冒充实机性能通过。
- [ ] 高负载、能力不足、减少动画和效果关闭时均有清楚的静态回退。

## 16. 官方来源

所有来源访问日期均为 2026-08-09。

| 编号 | 微软官方资料 | 用途 |
| --- | --- | --- |
| M1 | [Motion in Windows](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/motion) | Windows 动效原则、用途、曲线与时长 |
| M2 | [Timing and easing](https://learn.microsoft.com/en-us/windows/apps/design/motion/timing-and-easing) | WinUI 3 标准 83/167/250ms 资源 |
| M3 | [Animations in XAML](https://learn.microsoft.com/en-us/windows/apps/develop/motion/xaml-animation) | Theme animation/transition、控件默认动画与适用场景 |
| M4 | [Page transitions](https://learn.microsoft.com/en-us/windows/apps/develop/motion/page-transitions) | 默认页面动画、drill 与显式抑制 |
| M5 | [Connected animation for Windows apps](https://learn.microsoft.com/en-us/windows/apps/develop/motion/connected-animation) | 共享元素、冲突抑制、启动时限与失败边界 |
| M6 | [Storyboarded animations](https://learn.microsoft.com/en-us/windows/apps/develop/motion/storyboarded-animations) | 独立/依赖动画、UI 线程风险与运行控制 |
| M7 | [Composition animations for WinUI](https://learn.microsoft.com/en-us/windows/apps/develop/composition/composition-animation) | Composition 独立线程、动画类型和视觉层属性 |
| M8 | [XAML and Composition interoperability](https://learn.microsoft.com/en-us/windows/apps/develop/composition/xaml-comp-interop) | WinUI 3 XAML 与 Composition 的直接互操作 |
| M9 | [Natural motion animations](https://learn.microsoft.com/en-us/windows/apps/develop/composition/natural-animations) | 自然运动、起终点、初始速度与输入关联 |
| M10 | [Spring animations](https://learn.microsoft.com/en-us/windows/apps/develop/composition/spring-animations) | 弹簧阻尼、周期、速度与 InteractionTracker |
| M11 | [Tailoring effects and experiences](https://learn.microsoft.com/en-us/windows/apps/develop/composition/composition-tailoring) | 动画设置、效果设置和硬件能力降级 |
| M12 | [UISettings class](https://learn.microsoft.com/en-us/uwp/api/windows.ui.viewmanagement.uisettings) | `AnimationsEnabled`、变化事件及版本历史 |
| M13 | [Mouse interactions](https://learn.microsoft.com/en-us/windows/apps/develop/input/mouse-interactions) | 鼠标悬停、输入反馈及触摸切换 |
| M14 | [Touch interactions developer guide](https://learn.microsoft.com/en-us/windows/apps/develop/input/touch-developer-guide) | 触摸无悬停、pointer/manipulation 与等价反馈 |
| M15 | [Keep the UI thread responsive](https://learn.microsoft.com/en-us/windows/apps/develop/performance/keep-ui-thread-responsive) | UI 线程、渲染线程与 WinUI 3 DispatcherQueue 边界 |
| M16 | [Plan and measure app performance](https://learn.microsoft.com/en-us/windows/apps/develop/performance/planning-measuring-performance) | 响应时间、流畅性目标与测量方法 |
| M17 | [WinUI 3 performance optimization](https://learn.microsoft.com/en-us/windows/apps/develop/performance/winui-perf) | WPR/WPA、XAML Activity 与 XAML Frame Analysis |
| M18 | [UIElement.Transitions property](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.uielement.transitions) | Transition 应由样式、模板与视觉状态集中定义 |
| M19 | [Use the Visual Layer with WinUI XAML](https://learn.microsoft.com/en-us/windows/apps/develop/composition/using-the-visual-layer-with-xaml) | XAML/Visual 分工、handout Visual 与无障碍边界 |
| M20 | [Input-driven animations](https://learn.microsoft.com/en-us/windows/apps/develop/composition/input-driven-animations) | 输入驱动动画、独立线程与 XAML 线程限制 |

## 17. 证据限制

- 本文是官方文档研究与工程规则，不是已完成实现或真实设备验证。
- Composition 独立线程不保证任意界面自动达到 60 FPS；布局、XAML 树变化、资源创建和业务负载仍可能造成慢帧。
- 微软文档中的 API 示例可能随 Windows App SDK 版本演进。实现前必须用项目锁定的最新稳定 SDK 核对 API 可用性，不能默认采用预览 API。
- “可改向”“无卡顿”“尊重减少动画”都必须在实现后通过自动检查和真实 Windows 性能记录确认。
