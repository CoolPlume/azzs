# 补充产品标识与发布文案

Type: task  
Status: ready-for-agent  
Resolution: open
Blocked by: 01
Owner: issue-23
Claimed by: Codex issue-23 task
Consumers: 21, 34, 35
Verification: 对当前规格、八制品名称和候选发布说明进行文案、风险披露与本地化人工审查。
Evidence freshness: 每个发行候选及产品标识、风险政策或最低目标版本变化后重审。

## Goal

根据已经确认的产品标识与发行边界，补齐公开发布所需的正式产品标识和简体中文文案。

## Engineering Work Required

- 起草并审核未签名、无校验值和 SmartScreen 风险说明，保持与 `SEC-04` 至 `SEC-06` 一致。
- 起草并审核最低目标版本及旧版本继续运行风险说明，不暗示跨版本兼容性或实机验证已经完成。

## Acceptance Criteria

- [ ] 应用、标准版 x64/ARM64 安装包与便携包、断网救援版 x64/ARM64 便携包和超大离线版 x64/ARM64 便携包均使用“Windows 初装工作台”与一致的简洁设备准备图形。
- [ ] GitHub 仓库与 Release 使用“Windows 初装工作台：帮助新装 Windows 完成驱动准备、系统优化、常用软件安装和软件优化。”作为产品简介，不把尚未实现的功能、实机验证或 Release 状态写成既成事实。
- [ ] GitHub 发布说明能区分版本、发行形态和 x64/ARM64 架构。
- [ ] 风险说明准确披露未签名和无项目校验信息，不暗示已验证第三方包。
- [ ] 开发者证据中的“未实机验证”不得出现在普通应用界面或面向用户的 GitHub 发布说明中；两者保持独立，面向用户的文案也不得声称已经完成真实设备验证。
- [ ] 首版文案为简体中文，并可纳入后续语言扩展。

## References

`REL-01` 至 `REL-12`、`SEC-04` 至 `SEC-06`、`LANG-01` 至 `LANG-03`、规格第 20 和 21 节

## Comments

- 2026-08-10：Q7 已冻结三档八制品矩阵，Q8/Q10 已分别冻结为“未实机验证如实记录”和“不提供摘要、源码关联或 provenance”。Q21 已确定正式名称为“Windows 初装工作台”，图标方向为简洁设备准备图形，`azzs` 保持仓库标识；Q27 已确定 GitHub 仓库与 Release 使用功能导向的一句话简介。产品决定已经闭合，本事项改为 `ready-for-agent`；风险与最低目标版本文案仍须实际起草和审核，不能把待交付文案误记为待回答问题。
- 2026-08-11：实现基于 `edaafae0add79022116967f3340a45ff4c5e9897`，实现提交为 `d30983cb5748c5e5d8611ed43b4210a02bd62b03`。`release/product-identity.json` 是正式名称、精确简介、八制品显示矩阵、本地化接缝、风险政策与 Release 模板骨架的权威源；README、WinUI `Resources.resw`、PE `app.rc`、WiX DisplayName/ARP 图标和简体中文 Release 模板是当前消费入口，事项 21、34、35 继续作为下游消费者，本提交没有新增三档打包逻辑。
- 2026-08-11：产品标识合同、11 个派生资产的确定性检查、host Debug/Release CTest、XML/JSON 解析、变更 Markdown 路径、`git diff --check` 和 GitNexus staged 影响检测已通过；GitNexus 风险等级为 `HIGH`，已在继续授权后提交。本 PR 的 Windows x64/ARM64 CI 将用于验证 PE 资源编译链接；真实 PE/Explorer/UAC/ARP 显示与 WiX MSI 构建未在本机执行，且没有创建 tag、Release 或制品。
