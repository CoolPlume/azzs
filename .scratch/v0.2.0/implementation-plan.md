# 0.2.0 实施计划

状态：已准备交给总调度

确认日期：2026-08-24

## 1. 目标与执行边界

本计划落实已确认的 .scratch/v0.2.0/spec.md、map.md、design-tree.md 和事项 01-06。它不重新打开产品决策，也不把静态合同或旧制品当作新版本验收。

- 实现基线：正式 v0.1.1，精确提交 04eeb5ae8a04038cbb43359bded40504159a358b。
- 实测对照：D:\azzs-codex\local-trial\azzs-0.1.1\f2f1998\Azzs.WinUI.exe；只用于解释 0.1.1 用户可见差异，不得作为 0.2.0 证据。
- 执行位置：D:\azzs-codex\worktrees\v020-*；D:\azzs 主工作树中的现有修改和 _windbg-output_9248_2026-08-23_01-34-58-924.txt 必须保留。
- 当前优先架构：Windows x64；ARM64 源码和目录语义保留，实机证据标为未执行/延期。
- 默认集成目标：codex/v1-integration。禁止 rebase、force push、历史改写、合入 main、创建 tag/Release、上传发行制品或接受 WiX 条款，除非维护者另行明确授权。
- 发布门禁：先形成独立 D:\azzs-codex x64 .exe，再由维护者实际验收；任一 QQ/QQ 音乐安装不稳定就从 0.2.0 可用安装承诺中延期。

总调度使用 GPT-5.6 Terra Max。它可以按依赖创建 GPT-5.6 Terra Medium/High/XHigh/Max 执行会话；实现、定向测试和复杂 Windows 根因不得降到 Luna。执行会话需在独立 worktree 工作，并在交接中报告精确 SHA、验证命令、环境、未验证边界和下一步依赖。

## 2. 阶段总览

| 阶段 | 结果 | 依赖 | 推荐会话 |
| --- | --- | --- | --- |
| 0 | 文档/基线/允许接口核对 | 无 | 总调度，可委派 Terra Medium 事实摘取 |
| 1 | 事项 01：侧栏可调整且可恢复 | 0 | Terra Max 或 XHigh；涉及 WinUI 时保持高推理 |
| 2 | 事项 02：真实 GitHub Release 查询与自动计划 | 0 | Terra Max；网络和状态语义可拆 Terra XHigh |
| 3 | 事项 03：硬件表字段收敛、复制和显示器摘要 | 0 | Terra Max 或 XHigh；Windows 观测变化与 UI 分离 |
| 4 | 事项 04：受控安装能力与 QQ | 0 | Terra Max；安装器/Windows 根因必要时 Terra Max |
| 5 | 事项 05：QQ 音乐 | 4 | Terra Max；来源或自动化不可靠时保持 fail-closed |
| 6 | 事项 06：x64 集成、实机验收和放行记录 | 1-5 | 总调度 + Terra Max/XHigh 验证会话 |
| 7 | 归档与版本交接 | 6 或明确延期 | 总调度机械收口 |

阶段 1-4 在不共享 worktree、不修改同一文件组且事实依赖满足时可以并行；阶段 5 必须等待 4 的安装接口和目录门禁；阶段 6 串行汇总所有前置结果。任何会话中断后先检查 worktree、索引、最新提交、远端分支和 PR，再恢复原会话或重分发最小剩余单元，不重复已经绑定同一 SHA 的成功验证。

## 3. Phase 0：文档发现与基线冻结

### 要做的事

1. 总调度在主工作树仅执行一次基线检查：git fetch origin --prune、git status --short --branch、git rev-parse origin/codex/v1-integration；不要清理或覆盖维护者文件。
2. 阅读 AGENTS.md、CONTEXT.md、docs/agents/orchestration.md、docs/agents/windows-native-takeover.md、docs/agents/windows-workspace-path-policy.md、docs/agents/code-intelligence.md、docs/engineering/architecture-and-code-quality.md，修改 XAML/WinUI 前再阅读 docs/design/winui3-apple-inspired.md。
3. 从 04eeb5a 创建新分支和 linked worktree；不得从当前较早的 codex/v1-integration 直接实现 0.2.0。分支和目录使用可识别 slug，例如 codex/v020-sidebar 与 D:\azzs-codex\worktrees\v020-sidebar。
4. 执行会话先在基线中核对真实符号、测试和组合根，再确定允许修改的文件清单。事实摘取必须报告来源、具体路径/符号、可复制模式位置、置信度和缺口。

### 允许接口与既有模式

- 侧栏：docs/adr/0050-adjustable-sidebar-width-ownership.md、src/application/include/azzs/application/sidebar_width_preferences.hpp、src/adapters/ui/winui/MainWindow.xaml(.h/.cpp) 中的 SidebarWidthPreferences、apply_sidebar_width 和 Thumb 事件；先验证 0.1.1 的运行时接线为何失效，不在页面创建第二个偏好所有者。
- 更新：src/application/include/azzs/application/application_update.hpp 的 ApplicationUpdatePlatform、ApplicationUpdateLifecycle、UpdateSnapshot/UpdateUserIntent，src/adapters/windows/src/windows_application_update_platform.cpp 的平台接缝，StateApplicationUpdateHealthStorage 的持久化模式，及 ADR-0006。自动检查只能复用查询接缝和核心状态所有权，不能在 XAML 定时器里自行实现业务计划。
- 硬件：src/application/include/azzs/application/hardware_overview.hpp 的结构化 HardwareObservation/HardwareDeviceRecord，src/adapters/windows/src/windows_hardware_observer.cpp 的物理性确认，DriversPage.xaml(.h/.cpp) 的投影；删除呈现字段不等于让 UI 重新推断设备类别。
- 安装：src/domain/software-catalog、src/application/installation-batch-runner、src/adapters/windows/src/windows_installation_batch_adapters.cpp、catalog/software-catalog.toml，以及 ADR-0010、0039、0040、0054。目录只能表达稳定 ID、来源和档案引用；参数、选择器和安全边界必须留在项目内置能力。
- 测试：沿用各模块已有 *-contract 目标和 eng/build.ps1/仓库既有 x64 构建入口；具体命令以基线和当前 CI 配置为准，禁止凭空添加不存在的 target 或参数。

### Phase 0 验证

- git show 04eeb5a:<path> 与 git grep 证明每个执行任务引用的接口真实存在。
- git diff --check、git status --short、git worktree list --porcelain。
- 建立基线记录：提交 SHA、分支、worktree 绝对路径、Windows/SDK/toolchain、构建入口和当前未验证项。

### 禁止模式

- 不从当前主工作树复制未确认的 0.1.1 字段实现，不复用 f2f1998 制品作为新证据。
- 不把“打开 GitHub 网页”当作 Release API 查询成功。
- 不在 UI、目录 TOML 或第三方插件中加入脚本、坐标点击、网络抓取或业务状态副本。

## 4. Phase 1：事项 01 侧栏调整

### 要实现

- 找出正式 0.1.1 制品中 Thumb 不可拖拉的真实运行接线问题，修复展开态拖拽连续、可反向、可中断的行为。
- 复用应用设置偏好唯一所有者；范围 216/248/360 DIP，默认 248，非法值/读写失败回退默认；键盘等价调整、双击恢复默认、关闭重开恢复。
- compact 导航不重置当前页面、焦点或业务事实；不把宽度写入页面或批次状态。

### 文档/代码依据

docs/adr/0050-adjustable-sidebar-width-ownership.md；MainWindow.xaml(.h/.cpp) 的现有 Thumb、事件和 SidebarWidthPreferences 模式；WinUI 规则文件和 docs/design/winui3-apple-inspired.md。

### 验证

- 偏好无界面合同：边界、非法值、持久化失败、重建和双击恢复。
- x64 WinUI UI：鼠标拖拽双向、键盘边界、compact、DPI、关闭重开；记录 AutomationId 和窗口尺寸。
- 只运行受影响合同、必要 host guardrails 和一次 x64 构建；成功 SHA 不重复跑。

### 防回归

不得把 Thumb 只做成视觉装饰、用坐标点击模拟拖拽、在页面静态变量缓存第二份宽度，或让动画完成事件写业务状态。

## 5. Phase 2：事项 02 更新查询与自动计划

### 要实现

- 通过 ApplicationUpdatePlatform::query_releases() 接通 GitHub 正式稳定 Release 查询；严格过滤版本、架构、内容版和发行形态，不把预发布/draft/nightly 或不匹配制品当候选。
- 在核心增加关闭/启动时/每日/每周计划、默认启动时、上次检查结果/时间和补偿语义；计划由核心唯一持有，设置页只提交类型化偏好。
- 启动时和运行期到期只读查询；安装、优化、恢复或已有更新事务活动时跳过并记录原因，下一启动/到期窗口补偿。
- 有更新时在设置页和可关闭非打断提示展示版本、日期、摘要与 Release 入口；自动检查绝不下载、替换或切换。手动更新状态机继续遵守 ADR-0006。
- 离线、超时、限流、GitHub 不可达返回清晰类型化状态，不伪报无更新。

### 文档/代码依据

docs/adr/0006-opt-in-self-update.md、application_update.hpp/.cpp、windows_application_update_platform.cpp、state_application_update_health_storage.*、设置页现有更新命令和本规格 4.2。

### 验证

- 无界面合同覆盖成功、无更新、有更新、过滤、离线、超时、限流、忙碌跳过和补偿。
- Windows x64 网络实测绑定 Release 查询时间、当前版本/架构/发行形态和代理环境；设置 UI 检查入口真实返回结果。
- grep/审查确认自动路径没有调用 download_and_replace，没有 UI 定时器第二状态源。

### 防回归

不得用 HTML 抓取代替 API、硬编码“最新版本”、把 GitHub 页面打开当查询成功、静默下载/替换、在活动事务中并发查询或改变签名/发布安全边界。

## 6. Phase 3：事项 03 硬件信息呈现

### 要实现

- 以结构化硬件事实为唯一来源，统一“项目 | 信息”两列表，加入机型和 Windows 版本表头，显示 CPU、主板、内存、显卡、显示器、固态硬盘、机械硬盘、有线/无线网卡、声卡、NPU。
- 删除普通页面的“未分类物理磁盘”、接口/PCIe、NAND 颗粒、资料识别和研究说明；不得破坏 ADR-0049 的物理性 fail-closed 过滤。
- 显示器摘要为型号加括号内可靠的分辨率、Hz、内建/外接事实，省略未知片段和冗余字段名；无事实固定行显示“未识别”，无记录的多实例类别隐藏。
- 让信息值支持鼠标/键盘选择复制，并提供复制本节命令；剪贴板只在 UI 适配器实现，不进入核心状态。

### 文档/代码依据

docs/adr/0049-physical-hardware-confirmation-filter.md、docs/adr/0053-hardware-information-presentation-boundary.md、hardware_overview.hpp/.cpp、windows_hardware_observer.cpp、DriversPage.xaml(.h/.cpp)，以及 f2f1998 对照合同测试。

### 验证

- 核心/投影合同覆盖字段删除、未识别/空多实例、显示器多实例和未知 EDID。
- Windows x64 实机验证多显示器、硬件值复制/整节复制、无未分类磁盘/NAND/资料识别；记录剪贴板文本和设备事实来源。
- grep 页面资源和投影代码，确认不再出现禁用标签或推断字符串；不得以删除 UI 文本掩盖核心事实契约错误。

### 防回归

不要从显示字符串反解析或重新猜测物理类别；不要显示序列号、原始 EDID、唯一标识或把诊断研究说明混入普通页面。

## 7. Phase 4：事项 04 受控安装能力与 QQ

### 要实现

- 建立可复用的三层安装执行闭环：可靠参数化安装 -> 项目内置、可审计的 UI Automation（仅已知安全选项） -> 官方安装界面交由用户处理。
- QQ 作为稳定 ID、普通、默认不勾选、官方 Windows 稳定桌面版目录项；工作台优先解析官方来源、下载、启动、记录生命周期和联合安装结果。
- 安装器可靠性不足、来源失效、版本/身份未知时 fail-closed 并交给用户；不处理账户、注册、验证码、付款、隐私、条款或未知复选框；已知推广默认拒绝。
- 结果由安装器生命周期、软件存在/版本事实和必要后置行为联合确认；仅退出码、缓存存在或进程启动不能代表成功。

### 文档/代码依据

ADR-0010、0039、0040、0054；catalog/software-catalog.toml；controlled_install_profiles.*；installation_batch.*；Windows 安装适配器和既有软件目录合同。

### 验证

- 无界面目录/批次合同：默认不勾选、稳定来源、暂停/恢复/重试、三层回退、待确认状态。
- x64 Windows 真实官方来源下载、启动、安装后存在/版本检测；记录安装器版本、来源、哈希或官方发布事实、退出/后置行为。
- 仅在来源和身份可可靠冻结时启用自动化；否则保留官方界面回退，不把静态目录条目写成可安装。

### 防回归

目录不得携带可执行脚本、路径或选择器；不得随包携带第三方安装器；不得用坐标点击、任意脚本或未经确认的静默参数。

## 8. Phase 5：事项 05 QQ 音乐

### 要实现

- 在事项 04 的共享安装接缝和目录门禁上增加 QQ 音乐稳定 ID、官方 Windows 稳定桌面版来源和安装档案；普通、默认不勾选、不随包携带二进制。
- 复用三层安装和联合结果检测；来源/安装器类型/架构/身份事实不能可靠确认时进入官方界面或待确认，不猜测、不静默换源。

### 文档/代码依据与验证

沿用事项 04 的安装接口、ADR-0054、软件目录/批次合同；新增来源解析、档案和 x64 实机测试必须绑定实际版本、地址、架构和验收时间。先跑静态合同，再进行真实安装；QQ 音乐失败或不稳定立即记录延期，不放宽安全边界。

## 9. Phase 6：集成、x64 验收和放行门槛

### 集成顺序

1. 每个事项在独立 worktree 完成内聚提交、最小充分验证并推送；创建以 codex/v1-integration 为 base 的 Draft PR。
2. 总调度按依赖顺序普通 merge 到 integration，解决冲突后在新 feature head 只重跑会改变结论的 x64 合同/构建；禁止 rebase。
3. 机械扫描 Blocked by，只启动前置已完成的事项；阶段 5 等待 4；本阶段等待 1-5。

### 最终验证

- 在独立 D:\azzs-codex 路径生成 x64 .exe，报告精确构建命令、产物路径、提交 SHA、Windows/SDK/toolchain。
- 运行受影响合同、一次必要 host guardrails、x64 构建和真实 WinUI/网络/剪贴板/QQ/QQ 音乐验收；不重复同 SHA 成功证据。
- 维护者亲自验收侧栏拖拽、更新设置、硬件表/复制和两款软件安装；失败项明确标记延期或待确认。
- 形成证据表，分开记录静态合同、自动化、Windows UI、网络查询、真实安装、CI、ARM64 未执行和发布禁止项。

### 防伪证据

旧 f2f1998 制品、macOS/虚拟机观察、静态 XAML、目录存在或安装器退出码均不能证明当前 Windows x64 行为。

## 10. Phase 7：会话与版本收口

- 总调度在每个执行会话完成交接后检查：是否仍有未提交修改、未推送提交、开放 PR、未交接证据或可恢复阻断；有后续依赖的会话保留，不提前归档。
- 对已完成、已被替代、无未交接工作且不再需要续跑的旧会话调用 Codex 会话归档；运行中的任务、待维护者验收或仍持有关键证据的会话不得归档。
- 0.2.0 完成或明确延期后，归档不再需要的执行会话，并在版本交接记录中列出保留会话及原因。后续版本沿用同一规则。
- 未经维护者另行 OK，不创建 v0.2.0 tag、GitHub Release、发行上传或 WiX 条款接受。

## 11. 总调度启动简报

总调度创建后应直接读取本文件及相关规格/ADR，并执行以下首轮动作：

    git fetch origin --prune
    git status --short --branch
    git rev-parse origin/codex/v1-integration
    git show -s --format='%H %D' 04eeb5ae8a04038cbb43359bded40504159a358b
    git worktree list --porcelain

然后从 04eeb5a 建立 0.2.0 执行 worktree，先完成 Phase 0 基线报告，再按依赖分发 Phase 1-4。总调度不得在 D:\azzs 主工作树直接实现源码，也不得把本计划改写成“已完成”来代替执行证据。

## 12. 完成回报格式

每个执行会话至少回报：

1. 事项、分支、worktree 和继承的精确 SHA。
2. 改动文件范围与最终提交 SHA/PR。
3. 实际运行的验证命令、环境和结果。
4. 未验证边界、失败首因、延期决定和下一依赖。
5. 禁止重复的成功检查，以及是否可以安全归档会话。

总调度最终回报必须把“已实现”“已自动验证”“已通过 Windows x64 实机”“维护者待验收”“延期/未验证”“禁止发布动作”分开陈述。
