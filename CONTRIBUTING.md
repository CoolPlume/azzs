# 参与贡献

本仓库仍以规格和设计文档为主。提交实现前，请先确认对应事项已经明确范围、验收条件和依赖。

## 交流语言

中文是项目文档和正式事项的主要语言。GitHub Issue 与 Pull Request 同时接受中文和英文；英文反馈被采纳后，维护者会在对应的 `.scratch/` 票据中记录中文摘要。

## 提交问题或建议

1. 先搜索已有 GitHub Issue 和 `.scratch/windows-initial-setup-workbench/issues/`。
2. 没有对应内容时，可以创建空白 GitHub Issue，说明具体场景、期望行为、已知限制和参考资料。
3. GitHub Issue 是公开反馈入口。确认处理后，由维护者建立或关联 `.scratch/` 票据；规格、范围和实施状态以该票据为准。

不要自行重编号既有票据。只有在维护者明确要求时，Pull Request 才直接新增或调整 `.scratch/` 事项。

## 提交 Pull Request

- 每个 Pull Request 聚焦一个可审查的目标，并链接相关 GitHub Issue 或 `.scratch/` 票据。
- 修改规格时同步核对 `CONTEXT.md`、相关 ADR 和受影响事项，避免同一规则出现相互矛盾的版本。
- 如实列出已经执行、未执行和无法执行的验证；文档检查、源码检查和真实设备验收不能互相替代。
- 不提交凭据、个人绝对路径、本地索引、生成缓存或 GitNexus 生成的 agent 文档与技能。
- 不复制许可不明的第三方代码、资源或文档。

## 贡献许可

提交 Issue 或 Pull Request 即表示你有权提供其中内容，并同意被项目采纳的贡献可以按本仓库的 [MIT License](LICENSE) 发布。项目当前不要求签署 CLA，也不采用 DCO。
