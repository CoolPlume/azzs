# Issue tracker: Local Markdown

本项目的事项和规格说明存放在 `.scratch/`，并由 Git 进行版本管理。

## 约定

- 每个功能使用一个目录：`.scratch/<feature-slug>/`
- 规格说明位于 `.scratch/<feature-slug>/spec.md`
- 实现事项按票据拆分：`.scratch/<feature-slug>/issues/<NN>-<slug>.md`，从 `01` 开始编号；不要使用合并的票据文件
- 每个票据顶部附近使用 `Status:` 记录 triage 状态（角色字符串见 `triage-labels.md`）
- 评论和对话追加到 `## Comments` 标题下

## 发布事项

当技能要求“发布到问题追踪器”时，在 `.scratch/<feature-slug>/` 下创建文件，必要时先创建目录。

## 获取事项

当技能要求“获取相关票据”时，读取用户指定的路径或票据编号对应的文件。

## Wayfinding 操作

这些操作供 `/wayfinder` 使用。**Map** 是每张票据对应一个子文件的地图：

- Map：`.scratch/<effort>/map.md`，正文包含 Notes、Decisions-so-far、Fog
- Child ticket：`.scratch/<effort>/issues/NN-<slug>.md`，从 `01` 开始编号，正文包含问题；使用 `Type:` 记录 `research`、`prototype`、`grilling` 或 `task`，使用 `Status:` 记录 `claimed` 或 `resolved`
- Blocking：在票据顶部附近使用 `Blocked by: NN, NN`；列出的文件全部为 `resolved` 时票据解除阻塞
- Frontier：扫描 `.scratch/<effort>/issues/`，按编号优先选择未关闭、未阻塞、未认领的票据
- Claim：工作前设置 `Status: claimed` 并保存
- Resolve：在 `## Answer` 下追加答案，设置 `Status: resolved`，再将摘要与链接追加到地图的 Decisions-so-far
