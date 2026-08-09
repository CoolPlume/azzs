# Code intelligence: GitNexus

GitNexus 只生成本地索引。根目录 `.gitnexusrc` 使用纯索引模式，避免工具改写 `AGENTS.md`、生成 `CLAUDE.md` 或安装仓库内技能。

以下示例使用已安装的 `gitnexus` 命令。若维护者本机另有未跟踪的 `.gitnexus/run.cjs`，可把命令开头的 `gitnexus` 替换为 `node .gitnexus/run.cjs`；公开克隆不依赖这个本地 runner。

## 修改代码符号前

1. 运行 `gitnexus status`；索引缺失或过期时，在仓库根目录运行 `gitnexus analyze`。
2. 修改已有函数、类或方法前，运行 GitNexus `impact`，方向为 `upstream`。
3. 向用户报告直接调用者、受影响流程和风险等级。风险为 `HIGH` 或 `CRITICAL` 时，先取得继续授权。
4. 探索陌生代码时优先使用 GitNexus `query` 和 `context`；符号重命名使用 GitNexus `rename`。

只有索引不再过期、影响范围已经报告且高风险改动已获授权，才算完成修改前检查。

## 安全分析

改动涉及外部输入、命令执行、下载、路径或持久化时，使用带 PDG 的索引，并运行 GitNexus `explain` 检查 source-to-sink 路径。工具结果只能证明已索引范围，不能替代真实环境验收。

## 提交前

1. 在最终文件稳定后刷新索引。
2. 运行 `gitnexus detect-changes --scope staged --repo azzs --limit 200`。
3. 同时检查 `git diff --cached --name-status`、`git ls-files --others --exclude-standard`、`git diff --cached --check` 和 `git diff --check`；不能只依赖 GitNexus 的风险等级。
4. 核对每个受影响文件、符号和执行流程都在预期范围内。

只有变更范围完整可见、没有遗漏的未跟踪文件且所有检查通过，才算完成提交前门禁。
