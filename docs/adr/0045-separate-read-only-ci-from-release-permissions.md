# 分离早期只读 CI 与后期发布权限

Status: accepted  
Date: 2026-08-10  
Supersedes: ADR-0044  
Related: ADR-0002, ADR-0034, ADR-0037

项目接受已经公开的 `CoolPlume/azzs` 三提交历史，不为追求单一根提交改写已经公开且使用 GitHub noreply 身份的对象。事项 30 先审计并规范现有公开仓库、集成候选和可逆仓库设置，再启用默认只读的 GitHub Actions；事项 01 随后拥有早期 Windows CI 工作流，在公开 `codex/v1-integration` 的同仓库拉取请求、集成分支或 `main` 更新及人工触发时，使用 GitHub 托管 Windows runner 验证 x64 构建与无界面 smoke、ARM64 编译链接。该路径不使用 `pull_request_target`、secrets、写权限或 self-hosted runner，第三方 Action 固定到精确提交，只保留日志、测试报告和构建清单，不在事项 21 前上传可下载应用二进制。

事项 31 只拥有由早期 CI 调用的架构和测试命令，不拥有工作流；事项 34 只在功能完整、八制品和全部发布前置满足后，为最终 beta 发布任务取得最小 `contents: write`，创建准确 tag 和 prerelease。普通构建与检查继续只读，不可信 pull request 或 fork 不得取得写权限或发布凭据。Actions 结果不替代真实设备证据，beta 自动发布也不替代 `v1.0.0` 的独立人工授权。
