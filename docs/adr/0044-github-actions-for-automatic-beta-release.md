# 使用 GitHub Actions 自动发布 beta

Status: superseded  
Superseded by: ADR-0045  
Date: 2026-08-10  
Related: ADR-0002, ADR-0034, ADR-0037

首次公开源码仓库基线完成时 GitHub Actions 保持关闭；事项 34 在事项 30 完成、发布工作流通过审查且全部发布前置满足后启用 GitHub Actions。版本化 beta 发布意图进入受保护的 `main` 后，由 GitHub 托管 Windows runner 自动重建候选、执行门禁、创建准确 tag 并发布 prerelease；普通构建与检查默认只读，只有最终发布任务取得最小 `contents: write`，pull request、fork 或其他不可信触发不得取得写权限或发布凭据，也不把公开仓库连接到 self-hosted runner。

该方案让公开源码、自动门禁和 GitHub Release 保持在同一套可审计执行环境，避免本地发布机的长期凭据与外部 CI 的额外信任边界；代价是托管镜像会变化，因此正式任务仍须固定并记录工具与输入版本。Actions 结果不替代真实设备证据，beta 自动发布也不替代 `v1.0.0` 的独立人工授权。
