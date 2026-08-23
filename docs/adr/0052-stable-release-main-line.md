# 正式稳定版本必须进入 main

## 状态

已接受

## 决策

每个非 beta、非 prerelease 的正式版本继续使用一条严格等于语义版本号的长期分支，例如 `0.1.1`。在创建该版本的正式 tag 或 GitHub Release 之前，必须使用普通 `--no-ff` merge 将版本分支合入 `main`，并验证发布提交是 `main` 的祖先。版本分支和历史版本分支均长期保留；发布后清理只针对临时工作分支和已验证可回收的工作树。

beta、alpha、preview 或其他 prerelease 可以不合入 `main`，但必须明确标记为预发行版本，并保留其对应的裸版本分支和 tag/Release 证据。

## 发布门禁

1. 版本分支名称为裸版本号，且与候选提交一致。
2. x64 本地 EXE、最小充分合同和必要 CI 已完成，未验证边界已记录。
3. 非 beta 版本在独立集成工作树从最新 `origin/main` 普通合并版本分支，解决冲突并验证 `git merge-base --is-ancestor <release-commit> origin/main`。
4. 只有在维护者明确授权后，才创建正式 tag、GitHub Release 和发布资产。
5. 发布后不得删除任何历史版本分支；只审计并清理已合并、无开放 PR、无独有提交且工作树干净的临时分支。

## 结果

正式版本的源码默认入口、版本分支和 GitHub Release 形成可追溯链路；beta 不会被误当作 main 上的稳定承诺。
