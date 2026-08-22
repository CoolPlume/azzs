# Issue tracker: Local Markdown

本项目的事项和规格说明存放在 `.scratch/`，并由 Git 进行版本管理。

## 约定

- 每个功能使用一个目录：`.scratch/<feature-slug>/`
- 规格说明位于 `.scratch/<feature-slug>/spec.md`
- 实现事项按票据拆分：`.scratch/<feature-slug>/issues/<NN>-<slug>.md`，从 `01` 开始编号；不要使用合并的票据文件
- 每个票据顶部附近使用 `Status:` 记录 triage 状态（角色字符串见 `triage-labels.md`）
- 每个票据使用独立的 `Resolution:` 记录生命周期结果，只允许 `open`、`completed` 或 `wontfix`；triage 角色和完成结果不能共用一个字段
- 每个票据使用 `Owner: issue-NN` 标识该 Goal 与验收条件的唯一结票责任事项；实际执行者另用 `Claimed by:`，不得把认领者写进 `Owner:` 或让多个事项共同拥有同一结票责任
- 每个票据使用 `Consumers:` 列出直接下游事项及明确的非阻塞证据消费者；没有消费者时写 `none`，不得用消费者关系暗示新增阻塞边
- 每个票据使用单行 `Verification:` 说明结票所需的可重复验证方法，并用 `Evidence freshness:` 说明证据绑定的提交、目录修订、候选制品、外部观察或环境以及何时必须重跑
- `Blocked by: NN, NN` 只在列出的票据全部为 `Resolution: completed` 时机械解除；前置为 `wontfix` 时，下游事项必须重新评估，不能自动解除
- 评论和对话追加到 `## Comments` 标题下

## 发布事项

当技能要求“发布到问题追踪器”时，在 `.scratch/<feature-slug>/` 下创建文件，必要时先创建目录。

## 获取事项

当技能要求“获取相关票据”时，读取用户指定的路径或票据编号对应的文件。

## Wayfinding 操作

这些操作供 `/wayfinder` 使用。**Map** 是每张票据对应一个子文件的地图：

- Map：`.scratch/<effort>/map.md`，正文包含 Notes、Decisions-so-far、Fog
- Child ticket：`.scratch/<effort>/issues/NN-<slug>.md`，从 `01` 开始编号，正文包含问题；使用 `Type:` 记录 `research`、`prototype`、`grilling` 或 `task`，使用 `Status:` 记录 canonical triage 角色，使用 `Resolution:` 记录结果
- Blocking：在票据顶部附近使用 `Blocked by: NN, NN`；列出的文件全部为 `Resolution: completed` 时票据解除阻塞
- Frontier：扫描 `.scratch/<effort>/issues/`，按编号优先选择 `Resolution: open`、未阻塞且没有 `Claimed by:` 的票据
- Claim：工作前增加 `Claimed by: <agent-or-user>` 并保存，不改变 `Status:` 的 triage 角色
- Resolve：在 `## Answer` 下追加答案，设置 `Resolution: completed`，保留 `Claimed by:` 作为审计记录，再将摘要与链接追加到地图的 Decisions-so-far
