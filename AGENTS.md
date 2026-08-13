## Language

项目内所有对话使用中文。

## Agent skills

### Issue tracker

事项和规格说明使用 Git 管理的本地 Markdown 文件，存放于 `.scratch/<feature-slug>/`。详见 `docs/agents/issue-tracker.md`。

### Triage labels

使用默认的五个 triage 标签。详见 `docs/agents/triage-labels.md`。

### Domain docs

使用单上下文布局：根目录 `CONTEXT.md` 与 `docs/adr/`。详见 `docs/agents/domain.md`。

### Orchestration

进行项目总调度、拆分 Codex 会话或子代理、选择模型、控制并发、跨机器交接或跨分支集成时，必须先阅读 `docs/agents/orchestration.md`。
在 Windows 原生环境开始新的总调度会话或接管既有进度时，还必须先阅读 `docs/agents/windows-native-takeover.md`。

### Architecture and code quality

实现或重构界面、核心、平台适配器、目录内容扩展或内置受控能力前，先阅读 `docs/engineering/architecture-and-code-quality.md`。保持单向依赖、状态唯一来源、唯一装配入口和无界面核心测试；普通目录内容不得引入第三方可执行插件。

### WinUI 3 interface and motion

修改 XAML、WinUI 3 交互或动效前，先阅读 `docs/design/winui3-apple-inspired.md`，并在可用时使用 `emil-design-eng` 技能审查频率、目的、可中断性、减少动画与性能。仓库设计文档是项目实现和验收的最终依据。

### Code intelligence

修改代码符号、评估影响、重命名或准备提交时，遵循 `docs/agents/code-intelligence.md`。
